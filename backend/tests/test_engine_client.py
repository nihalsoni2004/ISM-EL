import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import replace

import pytest

from backend.app.engine_client import EngineClient
from backend.app.errors import ApiError

FAKE_ENGINE = """
import json, os, sys, time
for line in sys.stdin:
    frame = json.loads(line)
    operation = frame["operation"]
    behavior = frame["params"].get("behavior")
    if behavior == "crash":
        os._exit(8)
    if behavior == "timeout":
        time.sleep(10)
    if behavior == "noisy":
        sys.stderr.write(("a" * 512 + "\\n") * 1500)
        sys.stderr.flush()
    value = {"protocol_version":1} if operation == "engine.ping" else frame["params"]
    if operation == "engine.status":
        value = {"ready": True}
    response = {"request_id": frame["request_id"], "protocol_version": 1, "ok": True, "data": value}
    if behavior == "wrong-id":
        response["request_id"] = "wrong"
    if behavior == "error":
        response = {"request_id": frame["request_id"], "protocol_version": 1, "ok": False,
                    "error":{"code":"DISK_NOT_FOUND","message":"Virtual disk does not exist."}}
    print(json.dumps(response), flush=True)
    if operation == "engine.shutdown":
        break
"""


@pytest.fixture
def fake(settings):
    script = settings.storage_root / "fake_engine.py"
    script.write_text(FAKE_ENGINE, encoding="utf-8")
    config = replace(settings, executable=settings.executable.parent / settings.executable.name,
                     request_timeout=3, shutdown_timeout=1)

    class Fake(EngineClient):
        def _command(self):
            return [sys.executable, "-u", str(script)]

    client = Fake(config)
    assert client.start()["ready"]
    yield client
    client.close()
    assert client.process.poll() is not None


def test_concurrent_matching_and_stderr(fake):
    with ThreadPoolExecutor(max_workers=8) as pool:
        values = list(pool.map(lambda value: fake.request("echo", {"value": value}), range(24)))
    assert values == [{"value": value} for value in range(24)]
    assert fake.request("echo", {"behavior": "noisy"}) == {"behavior": "noisy"}


def test_storage_error_is_mapped_without_breaking_transport(fake):
    with pytest.raises(ApiError) as raised:
        fake.request("disk.info", {"behavior": "error"})
    assert raised.value.status == 404 and raised.value.code == "DISK_NOT_FOUND"
    assert fake.available


@pytest.mark.parametrize("behavior", ["crash", "timeout", "wrong-id"])
@pytest.mark.parametrize("operation", ["disk.create", "telemetry.reset"])
def test_uncertain_mutation_is_never_retried(fake, behavior, operation):
    with pytest.raises(ApiError) as raised:
        fake.request(operation, {"behavior": behavior}, timeout=0.2)
    assert raised.value.code == "OPERATION_OUTCOME_UNKNOWN" and raised.value.uncertain
    assert raised.value.request_id and not fake.available
    with pytest.raises(ApiError) as unavailable:
        fake.request(operation, {})
    assert unavailable.value.code == "ENGINE_UNAVAILABLE"


@pytest.mark.parametrize("operation", ["disk.list", "telemetry.snapshot", "model.simulate"])
def test_read_timeout(fake, operation):
    with pytest.raises(ApiError) as raised:
        fake.request(operation, {"behavior": "timeout"}, timeout=0.2)
    assert raised.value.code == "ENGINE_TIMEOUT" and raised.value.uncertain
