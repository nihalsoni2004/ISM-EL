from pathlib import Path
from tempfile import TemporaryDirectory

import pytest
from fastapi.testclient import TestClient

from backend.app.config import PROJECT_ROOT, Settings
from backend.app.main import create_app


@pytest.fixture
def executable() -> Path:
    exe = PROJECT_ROOT / "build" / "msvc-debug" / "Debug" / "raid_simulator.exe"
    assert exe.is_file(), "Build the msvc-debug preset before running backend tests"
    return exe


@pytest.fixture
def storage_root():
    # The engine opens every directory ancestor to reject Windows reparse points.
    with TemporaryDirectory(prefix="http-test-", dir=PROJECT_ROOT / "build") as directory:
        yield Path(directory)


@pytest.fixture
def settings(storage_root, executable):
    return Settings(executable=executable, storage_root=storage_root, request_timeout=15)


@pytest.fixture
def client(settings):
    with TestClient(create_app(settings), base_url="http://127.0.0.1:8000") as test_client:
        yield test_client
