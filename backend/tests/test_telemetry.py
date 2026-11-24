import json
import time

import pytest

from test_ipc import frame, invoke


def result(response, code=200):
    assert response.status_code == code, response.text
    return response.json()


def categories(snapshot):
    return {item["category"]: item for item in snapshot["categories"]}


def make_mirror(client):
    for disk_id in ("disk0", "disk1", "disk2"):
        result(client.post("/api/disks", json={"disk_id": disk_id, "capacity_mib": 1}), 201)
    result(client.post("/api/raids", json={
        "array_id": "array0", "raid_level": 1, "members": ["disk0", "disk1"],
    }), 201)


def test_empty_and_populated_telemetry(client):
    initial = result(client.get("/api/telemetry"))
    assert initial["source"] == "MEASURED" and initial["scope"] == "engine_process"
    assert initial["logical_rates"]["iops"] is None
    assert all(row["attempted"] == 0 and row["latency"]["p99_ms"] is None for row in initial["categories"])
    assert all("latency_samples_ns" not in row for row in initial["categories"])

    make_mirror(client)
    result(client.post("/api/pools", json={"pool_id": "pool0", "array_id": "array0", "chunk_blocks": 1}), 201)
    result(client.post("/api/luns", json={"lun_id": "lun0", "pool_id": "pool0", "capacity_mib": 1}), 201)
    reset = result(client.post("/api/telemetry/reset"))
    assert reset["observation"]["generation"] == initial["observation"]["generation"] + 1
    address = "/api/luns/lun0/blocks/0"
    expected = "a6" * 4096
    result(client.put(address, json={"data_hex": expected}))
    assert result(client.get(address))["data_hex"] == expected
    snapshot = result(client.get("/api/telemetry?include_samples=true&event_limit=8"))
    counts = categories(snapshot)
    assert counts["RAID_LOGICAL_READ"]["completed"] == counts["RAID_LOGICAL_WRITE"]["completed"] == 1
    physical = counts["VIRTUAL_DISK_WRITE"]
    assert physical["completed"] == physical["blocks"] == 2 and physical["bytes"] == 8192
    assert physical["origin_counts"]["FOREGROUND"]["completed"] == 2
    assert len(physical["latency_samples_ns"]) == physical["latency"]["sample_count"] == 2
    assert physical["latency"]["p95_ms"] == max(physical["latency_samples_ns"]) / 1_000_000
    assert {event["disk_id"] for event in snapshot["recent_events"] if event["category"] == "VIRTUAL_DISK_WRITE"} == {"disk0", "disk1"}
    assert all(event["array_id"] == "array0" for event in snapshot["recent_events"])
    assert snapshot["observation"]["end_offset_ns"] >= snapshot["observation"]["start_offset_ns"]
    if snapshot["observation"]["seconds"] >= 1:
        assert snapshot["logical_rates"]["iops"] == 2 / snapshot["observation"]["seconds"]
    else:
        assert snapshot["logical_rates"]["iops"] is None
    result(client.post("/api/telemetry/reset"))
    assert categories(result(client.get("/api/telemetry")))["VIRTUAL_DISK_WRITE"]["attempted"] == 0
    assert result(client.get(address))["data_hex"] == expected


def test_rebuild_telemetry_preserves_authoritative_progress(client):
    make_mirror(client)
    result(client.post("/api/raids/array0/fail", json={"disk_id": "disk0"}))
    result(client.post("/api/spares", json={"disk_id": "disk2"}), 201)
    prepared = result(client.post("/api/rebuilds", json={
        "array_id": "array0", "failed_disk_id": "disk0", "spare_disk_id": "disk2",
    }), 201)
    job = prepared["job_id"]
    url = "/api/telemetry/rebuilds/" + job
    assert result(client.get(url))["retained"] is False
    result(client.post("/api/telemetry/reset"))
    result(client.post(f"/api/rebuilds/{job}/start", json={"checkpoint_limit": 1}), 202)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        data = result(client.get(url))
        if data["job"]["status"] == "PAUSED" and not data["job"]["worker_active"]:
            break
        time.sleep(0.03)
    else:
        pytest.fail("Rebuild did not reach its first durable checkpoint")
    assert data["source"] == "MEASURED" and data["raid_level"] == 1
    assert data["job"]["completed_blocks"] == data["job"]["batch_blocks"] == 16
    assert data["remaining_blocks"] == data["job"]["total_blocks"] - 16
    assert data["last_checkpoint_at"] == data["job"]["checkpoint_at"] > 0
    rebuild = categories(data)
    assert rebuild["REBUILD_BATCH"]["completed"] == 1 and rebuild["REBUILD_BATCH"]["blocks"] == 16
    assert rebuild["REBUILD_WRITE"]["bytes"] == 65536 and rebuild["REBUILD_READ"]["completed"] == 17
    assert rebuild["REBUILD_READ"]["origin_counts"]["REBUILD"]["completed"] == 17
    result(client.post("/api/telemetry/reset"))
    cleared = result(client.get(url))
    assert not cleared["retained"] and cleared["job"]["completed_blocks"] == 16
    assert categories(cleared)["REBUILD_BATCH"]["latency"]["sample_count"] == 0


WORKLOAD = [
    {"device": 0, "arrival_ms": 0.0, "operation": "READ", "bytes": 4096, "access": "RANDOM"},
    {"device": 0, "arrival_ms": 0.01, "operation": "WRITE", "bytes": 4096, "access": "RANDOM"},
    {"device": 1, "arrival_ms": 0.0, "operation": "READ", "bytes": 4096, "access": "RANDOM"},
    {"device": 1, "arrival_ms": 0.0, "operation": "WRITE", "bytes": 8192, "access": "SEQUENTIAL", "depends_on": [0, 2]},
]


def test_profiles_simulation_and_limits(client):
    listing = result(client.get("/api/model/profiles"))
    assert listing["source"] == "MODELED"
    assert [row["profile"] for row in listing["profiles"]] == ["HDD_7200", "SATA_SSD", "NVME_SSD"]
    settings = listing["profiles"][0]["parameters"]
    request = {"profile": "HDD_7200", "devices": 2, "workload": WORKLOAD}
    url = "/api/model/simulate"
    data = result(client.post(url, json=request))
    assert data == result(client.post(url, json=request)) and data["source"] == "MODELED"
    assert data["parameters"] == settings
    read, queued, parallel, dependent = data["requests"]
    assert read["service_ms"] == pytest.approx(8.5 + 1000 * (4096 / 1048576) / 180)
    assert read["start_ms"] == parallel["start_ms"] == 0
    assert queued["start_ms"] == read["completion_ms"] and queued["queue_wait_ms"] > 0
    assert dependent["effective_arrival_ms"] == max(read["completion_ms"], parallel["completion_ms"])
    custom = result(client.post(url, json={**request, "parameters": {**settings, "queue_depth": 2}}))
    assert custom["requests"][1]["queue_wait_ms"] == 0

    wrong_row = [
        {"device": 2}, {"arrival_ms": -1}, {"arrival_ms": "0"}, {"bytes": 0},
        {"operation": "TRIM"}, {"depends_on": [0]}, {"unexpected": True},
    ]
    for changes in wrong_row:
        bad = {**request, "workload": [{**WORKLOAD[0], **changes}]}
        assert result(client.post(url, json=bad), 422)["error"]["code"] == "INVALID_PARAMS"
    assert result(client.post(url, json={**request, "workload": WORKLOAD * 17}), 422)["error"]["code"] == "INVALID_PARAMS"
    assert result(client.post(url, json={**request, "parameters": {**settings, "read_mib_per_second": 0}}), 422)
    assert result(client.post(url, content=b" " * 32769, headers={"Content-Type": "application/json"}), 413)
    assert result(client.get("/api/telemetry?event_limit=257"), 422)
    assert result(client.post("/api/model/simulate", content=json.dumps({
        **request, "workload": [{**WORKLOAD[0], "arrival_ms": float("nan")}],
    }), headers={"Content-Type": "application/json"}), 422)


def test_telemetry_and_model_unavailable_after_engine_exit(client):
    child = client.app.state.engine.process
    child.terminate()
    child.wait(timeout=5)
    for response in (client.get("/api/telemetry"), client.post("/api/telemetry/reset"), client.get("/api/model/profiles")):
        assert result(response, 503)["error"]["code"] == "ENGINE_UNAVAILABLE"


def test_telemetry_and_model_over_ndjson(executable, storage_root):
    work = {"profile": "SATA_SSD", "devices": 2, "workload": WORKLOAD}
    replies, _ = invoke(executable, storage_root, b"".join([
        frame("before", "telemetry.snapshot", {"include_samples": True, "event_limit": 5}),
        frame("model", "model.simulate", work),
        frame("repeat", "model.simulate", work),
        frame("invalid", "model.simulate", {**work, "workload": [{**WORKLOAD[0], "access": "OTHER"}]}),
        frame("deps", "model.simulate", {**work, "workload": [WORKLOAD[0], {**WORKLOAD[1], "depends_on": [0, 0]}]}),
        frame("bad-options", "telemetry.snapshot", {"include_samples": 1}),
        frame("reset", "telemetry.reset"),
        frame("after", "telemetry.snapshot"),
        frame("bye", "engine.shutdown"),
    ]))
    assert [row["request_id"] for row in replies] == ["before", "model", "repeat", "invalid", "deps", "bad-options", "reset", "after", "bye"]
    assert replies[1]["data"] == replies[2]["data"] and replies[1]["data"]["source"] == "MODELED"
    assert all(row["error"]["code"] == "INVALID_PARAMS" for row in replies[3:6])
    before, reset, after = (replies[index]["data"] for index in (0, 6, 7))
    assert before["source"] == reset["source"] == after["source"] == "MEASURED"
    assert reset["observation"]["generation"] == before["observation"]["generation"] + 1
    assert reset["observation"]["generation"] == after["observation"]["generation"]
    assert all(row["attempted"] == 0 for row in after["categories"])
