import asyncio
import hashlib
import importlib
import json
import subprocess
import sys
import time
from dataclasses import replace

from fastapi.testclient import TestClient
import pytest
from starlette.requests import ClientDisconnect

from backend.app.config import PROJECT_ROOT
from backend.app.exchange import Exchange
from backend.app.main import create_app


def json_result(response, status=200):
    assert response.status_code == status, response.text
    return response.json()


def assert_error(response, status, code):
    assert json_result(response, status)["error"]["code"] == code


def make_array(client, *, level=5, size=1):
    count = 4 if level == 5 else 2
    disks = [f"disk{i}" for i in range(count)]
    for name in disks:
        disk = json_result(client.post("/api/disks", json={"disk_id": name, "capacity_mib": size}), 201)
        assert disk["disk_id"] == name and "path" not in disk
    return json_result(client.post("/api/raids", json={
        "array_id": "array0", "raid_level": level, "members": disks,
    }), 201)


def make_pool_lun(client, *, mode="file", name="lun0", pool=True):
    if pool:
        result = json_result(client.post("/api/pools", json={"pool_id": "pool0", "array_id": "array0"}), 201)
        assert result["chunk_bytes"] == 65536
    value = json_result(client.post("/api/luns", json={
        "lun_id": name, "pool_id": "pool0", "capacity_mib": 512, "mode": mode,
    }), 201)
    assert value["mode"] == mode


def test_environment():
    assert sys.version_info[:2] == (3, 11)
    assert sys.prefix != sys.base_prefix
    assert sys.executable.endswith(".venv\\Scripts\\python.exe")
    for module in ("fastapi", "uvicorn", "pydantic", "python_multipart", "httpx", "pytest", "dotenv"):
        importlib.import_module(module)
    settings = json.loads((PROJECT_ROOT / ".vscode/settings.json").read_text())
    assert settings["python.defaultInterpreterPath"] == "${workspaceFolder}/.venv/Scripts/python.exe"


def test_liveness_openapi_and_validation(client):
    assert json_result(client.get("/")) == {"service": "raid-simulator-backend"}
    assert json_result(client.get("/health")) == {"status": "ok", "service": "raid-simulator-backend"}
    assert json_result(client.get("/api/version")) == {"version": "0.1.0"}
    assert json_result(client.get("/api/engine/status"))["ready"]
    assert json_result(client.get("/api/health"))["catalog_accessible"]
    assert json_result(client.get("/api/disks")) == {"disks": []}
    assert client.get("/docs").status_code == 200
    spec = json_result(client.get("/openapi.json"))
    assert len(spec["paths"]) >= 28
    assert "CreateRaid" in spec["components"]["schemas"] and "FileRecord" in spec["components"]["schemas"]
    assert_error(client.post("/api/disks", json={"disk_id": "disk0", "capacity_mib": True}),
                 422, "INVALID_PARAMS")
    assert_error(client.post("/api/disks", json={"disk_id": "disk0", "capacity_mib": 1, "path": "x"}),
                 422, "INVALID_PARAMS")
    assert_error(client.post("/api/disks", content=b"{", headers={"Content-Type": "application/json"}),
                 400, "MALFORMED_JSON")
    assert_error(client.get("/api/disks/diskx"), 422, "INVALID_PARAMS")
    assert_error(client.get("/api/disks/disk999"), 404, "DISK_NOT_FOUND")
    assert_error(client.get("/api/rebuilds/rebuild999"), 404, "REBUILD_NOT_FOUND")


def test_readiness_when_engine_is_absent(settings):
    bad = replace(settings, executable=settings.storage_root / "absent.exe")
    with TestClient(create_app(bad), base_url="http://127.0.0.1:8000") as client:
        assert json_result(client.get("/health"))["status"] == "ok"
        assert json_result(client.get("/api/health"), 503)["ready"] is False
        assert_error(client.post("/api/disks", json={"disk_id": "disk0", "capacity_mib": 1}),
                     503, "ENGINE_UNAVAILABLE")


def test_loopback_and_content_limits(settings):
    limited = replace(settings, max_upload_bytes=1024)
    with TestClient(create_app(limited), base_url="http://127.0.0.1:8000") as client:
        assert_error(client.get("/health", headers={"Host": "evil.example"}), 400, "INVALID_REQUEST")
        assert_error(client.post("/api/disks", json={"disk_id": "disk0", "capacity_mib": 1},
                                 headers={"Origin": "https://evil.example"}), 403, "ORIGIN_FORBIDDEN")
        cors = client.options("/api/disks", headers={"Origin": "http://127.0.0.1:5173",
                                                      "Access-Control-Request-Method": "POST"})
        assert cors.headers["access-control-allow-origin"] == "http://127.0.0.1:5173"
        for size in (2048, 70000):
            response = client.post("/api/luns/lun0/files", files={"file": ("sample.bin", b"x" * size)})
            assert_error(response, 413, "REQUEST_TOO_LARGE")
        assert_error(client.post("/api/luns/lun0/files", files={"file": ("../escape.bin", b"x")}),
                     400, "INVALID_REQUEST")
        assert_error(client.post("/api/luns/lun0/files", files={"file": ("COM¹.txt", b"x")}),
                     400, "INVALID_REQUEST")
        body = iter((b"--boundary\r\nContent-Disposition: form-data; name=\"file\"; filename=\"a.bin\"\r\n\r\n",
                     b"x" * 70000, b"\r\n--boundary--\r\n"))
        assert_error(client.post("/api/luns/lun0/files", content=body,
                                 headers={"Content-Type": "multipart/form-data; boundary=boundary"}),
                     413, "REQUEST_TOO_LARGE")


def test_upload_download_and_restart(settings):
    content = bytes(range(256)) * 617 + b"\0\x01"
    digest = hashlib.sha256(content).hexdigest()
    with TestClient(create_app(settings), base_url="http://127.0.0.1:8000") as client:
        assert make_array(client)["capacity_bytes"] == 3 * 1024**2
        assert_error(client.post("/api/disks", json={"disk_id": "disk0", "capacity_mib": 1}),
                     409, "DUPLICATE_RESOURCE")
        assert json_result(client.get("/api/disks/disk0"))["block_count"] == 256
        assert json_result(client.get("/api/raids/array0"))["health"] == "HEALTHY"
        assert json_result(client.get("/api/raids"))["arrays"][0]["array_id"] == "array0"
        assert len(json_result(client.get("/api/raids/array0/map/0"))["locations"]) == 2
        assert len(json_result(client.get("/api/raids/array0/members"))["members"]) == 4
        make_pool_lun(client)
        assert json_result(client.get("/api/pools/pool0"))["allocated_bytes"] == 0
        assert json_result(client.post("/api/pools/pool0/verify"))["passed"]
        assert json_result(client.get("/api/pools"))["pools"][0]["pool_id"] == "pool0"
        assert json_result(client.get("/api/luns"))["luns"][0]["lun_id"] == "lun0"
        assert json_result(client.get("/api/luns/lun0"))["mode"] == "file"
        assert json_result(client.get("/api/luns/lun0/map/0"))["raid_lba"] is None
        assert_error(client.put("/api/luns/lun0/blocks/0", json={"data_hex": "00" * 4096}),
                     409, "INVALID_RESOURCE_STATE")
        uploaded = json_result(client.post("/api/luns/lun0/files", files={"file": ("payload.bin", content)}), 201)
        assert uploaded["sha256"] == digest
        file_id = uploaded["file_id"]
        assert not list((settings.storage_root / "ipc_exchange/uploads").iterdir())
        assert json_result(client.get(f"/api/files/{file_id}"))["size_bytes"] == len(content)
        assert json_result(client.get(f"/api/files/{file_id}/verify"))["actual_sha256"] == digest
        assert json_result(client.get("/api/luns/lun0/files/verify"))["files"][0]["passed"]
        assert json_result(client.post("/api/raids/array0/verify"))["passed"]
        first_pid = json_result(client.get("/api/engine/status"))["process_id"]
    with TestClient(create_app(settings), base_url="http://127.0.0.1:8000") as restarted:
        assert json_result(restarted.get("/api/engine/status"))["process_id"] != first_pid
        assert json_result(restarted.get("/api/files?lun_id=lun0"))["files"][0]["file_id"] == file_id
        result = restarted.get(f"/api/files/{file_id}/download")
        assert result.status_code == 200 and result.content == content
        assert result.headers["X-Content-SHA256"] == digest
        assert result.headers["Content-Type"] == "application/octet-stream"
        assert not list((settings.storage_root / "ipc_exchange/downloads").iterdir())


def test_raw_blocks_and_degraded_file_access(client):
    make_array(client)
    make_pool_lun(client)
    make_pool_lun(client, mode="raw", name="lun1", pool=False)
    value = "74" + "00" * 4095
    assert not json_result(client.get("/api/luns/lun1/blocks/0"))["mapped"]
    assert json_result(client.put("/api/luns/lun1/blocks/0", json={"data_hex": value}))["new_chunks"] == 1
    assert json_result(client.get("/api/luns/lun1/blocks/0"))["data_hex"] == value
    uploaded = json_result(client.post("/api/luns/lun0/files", files={"file": ("a.bin", b"recoverable")}), 201)
    assert json_result(client.post("/api/raids/array0/fail", json={"disk_id": "disk0"}))["health"] == "DEGRADED"
    assert json_result(client.get("/api/raids/array0/status"))["reads_permitted"]
    assert client.get(f"/api/files/{uploaded['file_id']}/download").content == b"recoverable"
    assert_error(client.post("/api/luns/lun0/files", files={"file": ("new.bin", b"blocked")}),
                 409, "ARRAY_DEGRADED")


def test_out_of_space_and_reparse_exchange(client, settings):
    make_array(client, level=1)
    make_pool_lun(client)
    assert_error(client.post("/api/luns/lun0/files", files={"file": ("large.bin", b"x" * (1024**2 + 1))}),
                 507, "OUT_OF_SPACE")
    assert json_result(client.get("/api/files"))["files"][0]["status"] == "FAILED"
    alias = settings.storage_root / "ipc_exchange/downloads"
    outside = settings.storage_root / "outside"
    outside.mkdir()
    result = subprocess.run(["cmd.exe", "/d", "/c", "mklink", "/J", str(alias), str(outside)],
                            capture_output=True, timeout=10)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    try:
        assert_error(client.get("/api/files/file1/download"), 400, "STAGING_REJECTED")
        assert not any(outside.iterdir())
    finally:
        subprocess.run(["cmd.exe", "/d", "/c", "rmdir", str(alias)], check=True, timeout=10)


def wait_for_job(client, job_id, target, seconds=20):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        job = json_result(client.get(f"/api/rebuilds/{job_id}"))
        assert job["status"] != "FAILED", job
        if job["status"] == target and not job["worker_active"]:
            return job
        time.sleep(0.03)
    raise AssertionError(f"Rebuild did not reach {target}: {job}")


def test_async_rebuild_checkpoint_pause_resume(client):
    make_array(client, level=1, size=8)
    json_result(client.post("/api/raids/array0/fail", json={"disk_id": "disk0"}))
    json_result(client.post("/api/disks", json={"disk_id": "disk2", "capacity_mib": 8}), 201)
    assert json_result(client.post("/api/spares", json={"disk_id": "disk2"}), 201)["state"] == "SPARE"
    assert json_result(client.get("/api/spares"))["spares"][0]["available"]
    job = json_result(client.post("/api/rebuilds", json={
        "array_id": "array0", "failed_disk_id": "disk0", "spare_disk_id": "disk2",
    }), 201)
    assert job["status"] == "PREPARED"
    job_id = job["job_id"]
    route = f"/api/rebuilds/{job_id}"
    assert json_result(client.post(route + "/start", json={"checkpoint_limit": 3}), 202)["worker_active"]
    partial = wait_for_job(client, job_id, "PAUSED")
    assert partial["completed_blocks"] == 48 and 0 < partial["progress_percent"] < 100
    assert json_result(client.post(route + "/pause"), 202)["status"] == "PAUSED"
    assert json_result(client.post(route + "/resume"), 202)["worker_active"]
    assert json_result(client.post(route + "/pause"), 202)["pause_requested"]
    wait_for_job(client, job_id, "PAUSED")
    json_result(client.post(route + "/resume"), 202)
    done = wait_for_job(client, job_id, "COMPLETED")
    assert done["progress_percent"] == 100 and done["completed_blocks"] == done["total_blocks"]
    assert json_result(client.get("/api/raids/array0/status"))["health"] == "HEALTHY"
    assert json_result(client.get("/api/rebuilds"))["jobs"][0]["status"] == "COMPLETED"


def test_api_blocks_mutation_after_child_exit(client):
    child = client.app.state.engine.process
    child.terminate()
    child.wait(timeout=5)
    assert not json_result(client.get("/api/engine/status"), 503)["ready"]
    assert_error(client.post("/api/disks", json={"disk_id": "disk0", "capacity_mib": 1}),
                 503, "ENGINE_UNAVAILABLE")


@pytest.mark.parametrize("configuration", [
    {"policy": "FIXED_THROTTLE", "fixed_delay_us": 2100},
    {"policy": "ADAPTIVE", "maximum_delay_us": 7300},
])
def test_rebuild_scheduling_api(settings, configuration):
    address = "http://127.0.0.1:8000"
    with TestClient(create_app(settings), base_url=address) as first:
        listing = json_result(first.get("/api/rebuild-policies"))
        assert {policy["policy"] for policy in listing["policies"]} == {"FULL_SPEED", "FIXED_THROTTLE", "ADAPTIVE"}
        make_array(first, level=1, size=2)
        json_result(first.post("/api/raids/array0/fail", json={"disk_id": "disk0"}))
        json_result(first.post("/api/disks", json={"disk_id": "disk2", "capacity_mib": 2}), 201)
        json_result(first.post("/api/spares", json={"disk_id": "disk2"}), 201)
        request = {"array_id": "array0", "failed_disk_id": "disk0", "spare_disk_id": "disk2"}
        for invalid in ({"policy": "UNKNOWN"}, {"maximum_delay_us": -1},
                        {"maximum_delay_us": 400, "minimum_delay_us": 500},
                        {"target_read_p95_us": 1.5}, {"control_interval_ms": 300}):
            assert_error(first.post("/api/rebuilds", json={**request, "scheduling": invalid}), 422, "INVALID_PARAMS")
        job = json_result(first.post("/api/rebuilds", json={**request, "scheduling": configuration}), 201)
        job_id = job["job_id"]
        path = f"/api/rebuilds/{job_id}"
        saved = job["scheduling"]
        assert all(saved[key] == value for key, value in configuration.items())
        json_result(first.post(path + "/start", json={"checkpoint_limit": 2}), 202)
        paused = wait_for_job(first, job_id, "PAUSED")
        trace = json_result(first.get(path + "/scheduler"))
        assert trace["configuration"] == saved and trace["source"] == "MEASURED"
        assert trace["completed_blocks"] == paused["completed_blocks"] == 32
        assert trace["current"] is None and trace["decisions"]
        assert trace["decisions"][0]["delay_us"] == configuration.get("fixed_delay_us", 0)
        json_result(first.post(path + "/resume"), 202)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            active = json_result(first.get(path))
            if active["status"] == "RUNNING" and active["completed_blocks"] >= 48:
                break
            time.sleep(.01)
        else:
            raise AssertionError("Resumed worker did not publish another checkpoint")
        child = first.app.state.engine.process
        child.terminate()
        child.wait(timeout=5)
    with TestClient(create_app(settings), base_url=address) as restarted:
        recovered = json_result(restarted.get(path))
        assert recovered["scheduling"] == saved and recovered["status"] == "INTERRUPTED"
        assert recovered["completed_blocks"] >= active["completed_blocks"]
        json_result(restarted.post(path + "/resume"), 202)
        done = wait_for_job(restarted, job_id, "COMPLETED")
        assert done["scheduling"] == saved and done["progress_percent"] == 100
        assert json_result(restarted.get("/api/raids/array0/status"))["health"] == "HEALTHY"


def test_graceful_shutdown_checkpoints_active_rebuild(settings):
    address = "http://127.0.0.1:8000"
    with TestClient(create_app(settings), base_url=address) as first:
        make_array(first, level=1, size=16)
        json_result(first.post("/api/raids/array0/fail", json={"disk_id": "disk0"}))
        json_result(first.post("/api/disks", json={"disk_id": "disk2", "capacity_mib": 16}), 201)
        json_result(first.post("/api/spares", json={"disk_id": "disk2"}), 201)
        job = json_result(first.post("/api/rebuilds", json={
            "array_id": "array0", "failed_disk_id": "disk0", "spare_disk_id": "disk2",
        }), 201)
        path = "/api/rebuilds/" + job["job_id"]
        assert json_result(first.post(path + "/start"), 202)["worker_active"]
    assert first.app.state.engine.process.returncode == 0
    with TestClient(create_app(settings), base_url=address) as restarted:
        checkpoint = json_result(restarted.get(path))
        assert checkpoint["status"] == "PAUSED" and not checkpoint["worker_active"]
        assert checkpoint["completed_blocks"] < checkpoint["total_blocks"]


def test_download_removes_staging_on_disconnect(storage_root):
    exchange = Exchange(storage_root, "downloads", "payload.bin")
    exchange.path.write_bytes(b"abcd")
    response = exchange.stream_download(name="file.bin", size=4, checksum=hashlib.sha256(b"abcd").hexdigest())

    async def disconnected():
        raise AssertionError("ASGI 2.4 stream should detect disconnect from send")

    async def send(message):
        if message["type"] == "http.response.body":
            raise OSError("client disconnected")

    with pytest.raises(ClientDisconnect):
        asyncio.run(response({"type": "http", "asgi": {"spec_version": "2.4"}}, disconnected, send))
    assert not exchange.directory.exists()
