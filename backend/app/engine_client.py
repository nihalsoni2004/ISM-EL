from __future__ import annotations

import json
import logging
import queue
import subprocess
import threading
import time
import uuid
from concurrent.futures import Future

from .config import Settings
from .errors import ApiError

LOG = logging.getLogger(__name__)
PROTOCOL = 1
MAX_REQUEST = 32 * 1024
MAX_RESPONSE = 8 * 1024 * 1024
MUTATIONS = frozenset({
    "disk.create", "raid.create", "raid.fail", "raid.verify", "spare.register",
    "rebuild.prepare", "rebuild.start", "rebuild.pause", "rebuild.resume",
    "pool.create", "lun.create", "lun.write", "file.import",
    "telemetry.reset",
})


class TransportLost(Exception):
    pass


class EngineClient:
    def __init__(self, settings: Settings) -> None:
        self.settings = settings
        self.process: subprocess.Popen[bytes] | None = None
        self._state = threading.Lock()
        self._serial = threading.Lock()
        self._outgoing: queue.Queue[bytes | None] = queue.Queue(maxsize=2)
        self._threads: list[threading.Thread] = []
        self._pending: tuple[str, Future] | None = None
        self._available = False
        self._closing = False

    @property
    def available(self) -> bool:
        with self._state:
            return bool(self._available and self.process and self.process.poll() is None and not self._closing)

    def _command(self) -> list[str]:
        return [str(self.settings.executable), "--serve-json", "--storage-root", str(self.settings.storage_root)]

    def start(self) -> dict:
        if self.process is not None:
            raise RuntimeError("Engine client already started")
        self.settings.validate_paths()
        child = subprocess.Popen(
            self._command(), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            cwd=self.settings.storage_root, shell=False,
        )
        with self._state:
            self.process = child
            self._available = True
        for name, target in (("stdout", self._read_stdout), ("stderr", self._read_stderr),
                             ("stdin", self._write_stdin)):
            thread = threading.Thread(target=target, name=f"raid-engine-{name}", args=(child,), daemon=True)
            self._threads.append(thread)
            thread.start()
        deadline = time.monotonic() + self.settings.startup_timeout
        try:
            hello = self.request("engine.ping", timeout=self.settings.startup_timeout)
            if not isinstance(hello, dict) or hello.get("protocol_version") != PROTOCOL:
                raise ApiError("ENGINE_UNAVAILABLE", "C++ engine protocol is incompatible.")
            return self.request("engine.status", timeout=max(0.001, deadline - time.monotonic()))
        except Exception:
            self._poison()
            raise

    def _poison(self) -> None:
        with self._state:
            self._available = False
            if self._pending and not self._pending[1].done():
                self._pending[1].set_exception(TransportLost())

    def _read_stdout(self, child: subprocess.Popen[bytes]) -> None:
        assert child.stdout
        try:
            while line := child.stdout.readline(MAX_RESPONSE + 2):
                if len(line) > MAX_RESPONSE + 1 or not line.endswith(b"\n"):
                    raise TransportLost("invalid response frame")
                response = json.loads(line)
                if not isinstance(response, dict) or type(response.get("protocol_version")) is not int or (
                    response["protocol_version"] != PROTOCOL or type(response.get("ok")) is not bool
                ):
                    raise TransportLost("invalid response envelope")
                if response["ok"]:
                    if "data" not in response:
                        raise TransportLost("missing response data")
                else:
                    error = response.get("error")
                    if not isinstance(error, dict) or not all(isinstance(error.get(key), str)
                                                               for key in ("code", "message")):
                        raise TransportLost("invalid storage error")
                with self._state:
                    if self._pending is None or self._pending[0] != response.get("request_id"):
                        raise TransportLost("unmatched response ID")
                    future = self._pending[1]
                    if not future.done():
                        future.set_result(response)
        except (OSError, ValueError, TransportLost) as error:
            LOG.warning("Engine stdout stopped (%s)", type(error).__name__)
        finally:
            self._poison()

    @staticmethod
    def _read_stderr(child: subprocess.Popen[bytes]) -> None:
        assert child.stderr
        try:
            while chunk := child.stderr.readline(4096):
                LOG.info("C++ engine: %s", chunk.decode("utf-8", errors="replace").rstrip())
        except OSError:
            LOG.debug("Engine diagnostics pipe closed")

    def _write_stdin(self, child: subprocess.Popen[bytes]) -> None:
        assert child.stdin
        try:
            while (payload := self._outgoing.get()) is not None:
                child.stdin.write(payload)
                child.stdin.flush()
        except OSError:
            self._poison()
        finally:
            try:
                child.stdin.close()
            except OSError:
                pass

    def request(self, operation: str, params: dict | None = None, *, timeout: float | None = None,
                closing: bool = False) -> dict | list:
        budget = self.settings.request_timeout if timeout is None else timeout
        deadline = time.monotonic() + budget
        if not self._serial.acquire(timeout=budget):
            raise ApiError("ENGINE_TIMEOUT", "Timed out waiting for the engine queue.")
        request_id = uuid.uuid4().hex
        try:
            payload = json.dumps({
                "request_id": request_id, "protocol_version": PROTOCOL, "operation": operation,
                "params": params if params is not None else {},
            }, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n"
            if len(payload) > MAX_REQUEST + 1:
                raise ApiError("REQUEST_TOO_LARGE", "Request exceeds the IPC size limit.")
            future: Future = Future()
            with self._state:
                if not self._available or not self.process or self.process.poll() is not None or (
                    self._closing and not closing
                ):
                    raise ApiError("ENGINE_UNAVAILABLE", "The C++ engine is unavailable.")
                self._pending = (request_id, future)
                self._outgoing.put_nowait(payload)
            try:
                response = future.result(timeout=max(0, deadline - time.monotonic()))
            except (TimeoutError, TransportLost) as error:
                self._poison()
                code = "ENGINE_TIMEOUT" if isinstance(error, TimeoutError) else "ENGINE_UNAVAILABLE"
                message = "The C++ engine did not return a usable response."
                if operation in MUTATIONS:
                    code = "OPERATION_OUTCOME_UNKNOWN"
                    message = "The operation may have committed. Inspect storage state before retrying."
                raise ApiError(code, message, request_id=request_id, uncertain=True) from error
            if not response["ok"]:
                detail = response["error"]
                raise ApiError(detail["code"], detail["message"], request_id=request_id)
            return response["data"]
        finally:
            with self._state:
                if self._pending and self._pending[0] == request_id:
                    self._pending = None
            self._serial.release()

    def close(self) -> None:
        child = self.process
        if child is None:
            return
        with self._state:
            if self._closing:
                return
            self._closing = True
            can_request = self._available
        grace = self.settings.shutdown_timeout
        if child.poll() is None and can_request:
            try:
                self.request("engine.shutdown", timeout=grace, closing=True)
            except ApiError as error:
                LOG.warning("Engine shutdown RPC ended with %s", error.code)
        self._outgoing.put_nowait(None)
        try:
            child.wait(timeout=grace)
        except subprocess.TimeoutExpired:
            LOG.warning("Engine exceeded shutdown grace; persisted rebuild checkpoints remain authoritative")
            child.terminate()
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=3)
        self._poison()
        for thread in self._threads:
            thread.join(timeout=3)
        for stream in (child.stdin, child.stdout, child.stderr):
            if stream and not stream.closed:
                stream.close()
