from __future__ import annotations

import errno
import logging
import os
import re
import secrets
from pathlib import Path
from typing import BinaryIO
from urllib.parse import quote

from anyio import CancelScope
from fastapi import UploadFile
from starlette.concurrency import run_in_threadpool
from starlette.responses import StreamingResponse

from .config import checked_directory, regular_path
from .errors import ApiError

LOG = logging.getLogger(__name__)
BUFFER_BYTES = 64 * 1024
RESERVED = re.compile(r"CON|NUL|PRN|AUX|CONIN\$|CONOUT\$|COM[0-9¹²³]|LPT[0-9¹²³]",
                      re.ASCII | re.IGNORECASE)


def checked_name(value: str | None) -> str:
    if (not value or len(value.encode("utf-8")) > 240 or value.endswith((".", " "))
            or any(ord(c) < 32 or ord(c) == 127 or c in '<>:"/\\|?*' for c in value)
            or RESERVED.fullmatch(value.split(".", 1)[0].rstrip(" "))):
        raise ApiError("INVALID_REQUEST", "Expected a safe upload basename.")
    return value


def storage_failure(error: Exception) -> ApiError:
    LOG.warning("Exchange path or I/O rejected: %s", error)
    if isinstance(error, OSError) and (error.errno == errno.ENOSPC or getattr(error, "winerror", 0) == 112):
        return ApiError("OUT_OF_SPACE", "The local exchange directory is full.")
    return ApiError("STAGING_REJECTED", "The exchange file or directory is unsafe or unavailable.")


class ExchangeDownload(StreamingResponse):
    def __init__(self, file: BinaryIO, cleanup, **kwargs) -> None:
        self.file = file
        self.cleanup = cleanup
        super().__init__(iter(lambda: file.read(BUFFER_BYTES), b""), **kwargs)

    def finish(self) -> None:
        try:
            self.file.close()
        finally:
            self.cleanup()

    async def __call__(self, scope, receive, send) -> None:
        try:
            await super().__call__(scope, receive, send)
        finally:
            # A failed send or disconnect must also close and remove the staging file.
            with CancelScope(shield=True):
                await run_in_threadpool(self.finish)


class Exchange:
    """One operation owns one random directory. Clients supply only a display basename."""

    def __init__(self, root: Path, direction: str, name: str) -> None:
        self.token = secrets.token_hex(16)
        self.directory = root / "ipc_exchange" / direction / self.token
        self.path = self.directory / name
        try:
            checked_directory(self.directory.parent, create=True)
            self.directory.mkdir()
            checked_directory(self.directory)
        except (OSError, ValueError) as error:
            raise storage_failure(error) from error

    def cleanup(self) -> None:
        try:
            checked_directory(self.directory)
            try:
                regular_path(self.path, directory=False)
            except FileNotFoundError:
                pass
            else:
                self.path.unlink()
            self.directory.rmdir()
        except (OSError, ValueError) as error:
            LOG.warning("Could not clean exchange token %s: %s", self.token, error)

    def copy_upload(self, upload: UploadFile, limit: int) -> None:
        if upload.size is not None and upload.size > limit:
            raise ApiError("REQUEST_TOO_LARGE", "The upload exceeds the configured file size limit.")
        try:
            checked_directory(self.directory)
            with self.path.open("xb") as target:
                received = 0
                while chunk := upload.file.read(BUFFER_BYTES):
                    received += len(chunk)
                    if received > limit:
                        raise ApiError("REQUEST_TOO_LARGE", "The upload exceeds the configured file size limit.")
                    target.write(chunk)
        except (OSError, ValueError) as error:
            raise storage_failure(error) from error

    def stream_download(self, *, name: str, size: int, checksum: str) -> StreamingResponse:
        try:
            checked_directory(self.directory)
            checked = regular_path(self.path, directory=False)
            stream = self.path.open("rb")
            try:
                handle = os.fstat(stream.fileno())
                if (checked.st_dev, checked.st_ino, checked.st_size) != (handle.st_dev, handle.st_ino, size):
                    raise ValueError("Export changed before streaming")
            except BaseException:
                stream.close()
                raise
        except (OSError, ValueError) as error:
            raise storage_failure(error) from error

        return ExchangeDownload(
            stream, self.cleanup,
            media_type="application/octet-stream",
            headers={
                "Content-Length": str(size),
                "Content-Disposition": f"attachment; filename*=UTF-8''{quote(name, safe='')}",
                "X-Content-SHA256": checksum,
                "X-Content-Type-Options": "nosniff",
            },
        )
