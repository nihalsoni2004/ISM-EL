from __future__ import annotations

import logging
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request, Response
from fastapi.exceptions import RequestValidationError
from fastapi.middleware.cors import CORSMiddleware
from starlette.concurrency import run_in_threadpool
from starlette.datastructures import Headers
from starlette.exceptions import HTTPException
from starlette.responses import JSONResponse

from .api import router
from .config import Settings
from .engine_client import EngineClient
from .errors import ApiError
from .schemas import EngineStatus, ErrorResponse

SERVICE = "raid-simulator-backend"
VERSION = "0.1.0"
LOG = logging.getLogger(__name__)


class LocalRequests:
    def __init__(self, app, settings: Settings) -> None:
        self.app = app
        self.settings = settings

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return
        headers = Headers(scope=scope)
        host = headers.get("host", "").split(":", 1)[0].lower()
        if host not in {"127.0.0.1", "localhost"}:
            await self.reject(ApiError("INVALID_REQUEST", "Use a loopback Host header."), scope, receive, send)
            return
        if scope["method"] in {"GET", "HEAD", "OPTIONS"}:
            await self.app(scope, receive, send)
            return
        origin = headers.get("origin")
        if origin is not None and origin not in self.settings.allowed_origins:
            await self.reject(ApiError("ORIGIN_FORBIDDEN", "This request origin is not allowed."),
                              scope, receive, send)
            return
        upload = scope["method"] == "POST" and scope["path"].startswith("/api/luns/") and (
            scope["path"].endswith("/files") or scope["path"].endswith("/files/")
        )
        limit = self.settings.max_upload_bytes + 65536 if upload else 32768
        try:
            declared = headers.getlist("content-length")
            if declared and (len(declared) != 1 or not declared[0].isascii() or not declared[0].isdecimal()):
                raise ValueError
            if declared and int(declared[0]) > limit:
                await self.reject(ApiError("REQUEST_TOO_LARGE", "The request body exceeds its limit."),
                                  scope, receive, send)
                return
        except ValueError:
            await self.reject(ApiError("INVALID_REQUEST", "Invalid Content-Length."), scope, receive, send)
            return
        consumed = 0

        async def bounded_receive():
            nonlocal consumed
            message = await receive()
            consumed += len(message.get("body", b""))
            if consumed > limit:
                raise HTTPException(413, "Request body exceeds its limit")
            return message

        await self.app(scope, bounded_receive, send)

    @staticmethod
    async def reject(error, scope, receive, send):
        await JSONResponse(error.body(), status_code=error.status)(scope, receive, send)


def create_app(settings: Settings | None = None) -> FastAPI:
    config = settings if settings is not None else Settings.from_environment()

    @asynccontextmanager
    async def lifespan(application: FastAPI):
        client = EngineClient(config)
        application.state.engine = client
        application.state.settings = config
        application.state.catalog_ready = False
        try:
            status = EngineStatus.model_validate(await run_in_threadpool(client.start))
            application.state.catalog_ready = status.ready
        except Exception as error:
            LOG.error("Storage engine startup unavailable: %s", error)
        try:
            yield
        finally:
            application.state.catalog_ready = False
            await run_in_threadpool(client.close)

    application = FastAPI(title="RAID Simulator Backend", version=VERSION, lifespan=lifespan)
    application.add_middleware(CORSMiddleware, allow_origins=list(config.allowed_origins),
                               allow_methods=["GET", "POST", "PUT"], allow_headers=["Content-Type"],
                               expose_headers=["Content-Disposition", "X-Content-SHA256"])
    application.add_middleware(LocalRequests, settings=config)

    @application.exception_handler(ApiError)
    async def storage_error(_request: Request, error: ApiError):
        return JSONResponse(error.body(), status_code=error.status)

    @application.exception_handler(RequestValidationError)
    async def invalid_parameters(_request: Request, error: RequestValidationError):
        if any(item["type"] == "json_invalid" for item in error.errors()):
            return JSONResponse(ApiError("MALFORMED_JSON", "The request is not valid JSON.").body(),
                                status_code=400)
        return JSONResponse(ApiError("INVALID_PARAMS", "Request parameters failed validation.").body(),
                            status_code=422)

    @application.exception_handler(HTTPException)
    async def invalid_http(_request: Request, error: HTTPException):
        code = "REQUEST_TOO_LARGE" if error.status_code == 413 else "INVALID_REQUEST"
        return JSONResponse(ApiError(code, "The HTTP request could not be processed.").body(),
                            status_code=error.status_code)

    @application.get("/")
    def root() -> dict[str, str]:
        return {"service": SERVICE}

    @application.get("/health")
    def health() -> dict[str, str]:
        return {"status": "ok", "service": SERVICE}

    @application.get("/api/version")
    def version() -> dict[str, str]:
        return {"version": VERSION}

    @application.get("/api/health", response_model=EngineStatus, responses={503: {"model": EngineStatus}})
    @application.get("/api/engine/status", response_model=EngineStatus, responses={503: {"model": EngineStatus}})
    def engine_status(request: Request, response: Response):
        state = request.app.state
        try:
            status = EngineStatus.model_validate(state.engine.request("engine.status"))
        except ApiError as error:
            status = EngineStatus(engine_available=state.engine.available, catalog_accessible=False,
                                  ready=False, error_code=error.code)
        state.catalog_ready = status.ready
        if not status.ready:
            response.status_code = 503
        return status

    application.include_router(router)
    return application


app = create_app()
