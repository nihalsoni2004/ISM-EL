import json
import subprocess


def invoke(executable, root, lines):
    args = [str(executable), "--serve-json", "--storage-root", str(root)]
    result = subprocess.run(args, input=lines, capture_output=True, timeout=25)
    assert result.returncode == 0, result.stderr.decode(errors="replace")
    return [json.loads(line) for line in result.stdout.splitlines()], result.stderr


def frame(request_id, operation, params=None, version=1):
    return json.dumps({"request_id": request_id, "operation": operation,
                       "protocol_version": version, "params": params if params is not None else {}}).encode() + b"\n"


def test_persistent_protocol_and_rejection(executable, storage_root):
    replies, stderr = invoke(executable, storage_root, b"".join([
        frame("hello", "engine.ping"),
        b"{bad json\n",
        frame("future", "engine.version", version=99),
        frame("unknown", "disk.delete"),
        frame("wrong", "disk.create", {"disk_id": "disk1", "capacity_mib": True}),
        frame("missing", "disk.info", {"disk_id": "disk1"}),
        frame("token", "file.import", {"lun_id": "lun0", "token": "../outside", "name": "file.txt"}),
        b" " * (32 * 1024 + 5) + b"\n",
        b"[" * 30 + b"]" * 30 + b"\n",
        frame("alive", "engine.status"),
        frame("bye", "engine.shutdown"),
    ]))
    assert [item["request_id"] for item in replies] == [
        "hello", None, "future", "unknown", "wrong", "missing", "token", None, None, "alive", "bye",
    ]
    assert all(item["protocol_version"] == 1 for item in replies)
    assert [item["error"]["code"] for item in replies[1:9]] == [
        "MALFORMED_JSON", "PROTOCOL_VERSION_UNSUPPORTED", "UNKNOWN_OPERATION", "INVALID_PARAMS",
        "DISK_NOT_FOUND", "INVALID_PARAMS", "REQUEST_TOO_LARGE", "INVALID_REQUEST",
    ]
    assert replies[0]["data"]["process_id"] == replies[-2]["data"]["process_id"]
    assert replies[-2]["data"]["ready"] and replies[-1]["data"]["stopped"]
    assert b"Storage dispatch" in stderr


def test_eof_does_not_dispatch_an_unterminated_mutation(executable, storage_root):
    replies, _ = invoke(executable, storage_root, frame("before", "disk.list") +
                        frame("incomplete", "disk.create", {"disk_id": "disk1", "capacity_mib": 1}).rstrip())
    assert len(replies) == 1 and replies[0]["data"] == []
    assert not (storage_root / "data/virtual_disks/disk1.img").exists()
