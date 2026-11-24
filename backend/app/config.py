from __future__ import annotations

import os
import stat
from dataclasses import dataclass
from pathlib import Path

from dotenv import load_dotenv

PROJECT_ROOT = Path(__file__).resolve().parents[2]
MIB = 1024 * 1024


def local_path(value: str | Path) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        candidate = PROJECT_ROOT / candidate
    return Path(os.path.abspath(candidate))


def regular_path(path: Path, *, directory: bool) -> os.stat_result:
    info = path.lstat()
    reparse = getattr(info, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT
    correct_type = stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode)
    if reparse or not correct_type or (not directory and info.st_nlink != 1):
        raise ValueError(f"Unsafe local {'directory' if directory else 'file'}: {path}")
    return info


def checked_directory(path: Path, *, create: bool = False) -> None:
    for part in [*reversed(path.parents), path]:
        try:
            regular_path(part, directory=True)
        except FileNotFoundError:
            if not create:
                raise
            part.mkdir(exist_ok=True)
            regular_path(part, directory=True)


def positive_env(name: str, default: int, limit: int) -> int:
    value = int(os.environ.get(name, str(default)))
    if not 1 <= value <= limit:
        raise ValueError(f"{name} must be between 1 and {limit}")
    return value


@dataclass(frozen=True)
class Settings:
    executable: Path
    storage_root: Path
    max_upload_bytes: int = 100 * MIB
    request_timeout: float = 30
    startup_timeout: float = 15
    shutdown_timeout: float = 15
    port: int = 8000
    log_level: str = "INFO"
    create_storage_root: bool = False

    @property
    def allowed_origins(self) -> tuple[str, ...]:
        return tuple(f"http://{host}:{port}" for host in ("127.0.0.1", "localhost")
                     for port in sorted({5173, self.port}))

    @classmethod
    def from_environment(cls) -> Settings:
        load_dotenv(PROJECT_ROOT / "backend" / ".env")
        if os.environ.get("APP_HOST", "127.0.0.1") != "127.0.0.1":
            raise ValueError("The backend launcher only binds to 127.0.0.1")
        build = PROJECT_ROOT / "build" / "msvc-debug"
        debug = build / "Debug" / "raid_simulator.exe"
        exe = os.environ.get("RAID_ENGINE_EXECUTABLE") or (debug if debug.is_file() else
                                                           build / "Release" / "raid_simulator.exe")
        root = os.environ.get("RAID_STORAGE_ROOT")
        log_level = os.environ.get("LOG_LEVEL", "INFO").upper()
        if log_level not in {"DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"}:
            raise ValueError("Invalid LOG_LEVEL")
        return cls(
            executable=local_path(exe), storage_root=local_path(root or PROJECT_ROOT / "build" / "api-storage"),
            max_upload_bytes=positive_env("MAX_UPLOAD_MIB", 100, 4096) * MIB,
            request_timeout=positive_env("ENGINE_REQUEST_TIMEOUT_SECONDS", 30, 3600),
            startup_timeout=positive_env("ENGINE_STARTUP_TIMEOUT_SECONDS", 15, 300),
            shutdown_timeout=positive_env("ENGINE_SHUTDOWN_TIMEOUT_SECONDS", 15, 300),
            port=positive_env("APP_PORT", 8000, 65535), log_level=log_level, create_storage_root=not bool(root),
        )

    def validate_paths(self) -> None:
        if not self.executable.is_absolute() or not self.storage_root.is_absolute():
            raise ValueError("Engine and storage paths must be absolute")
        checked_directory(self.executable.parent)
        regular_path(self.executable, directory=False)
        checked_directory(self.storage_root, create=self.create_storage_root)
