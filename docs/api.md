---
noteId: "6f8e3430e7ea11f19fa83bab5b22ac42"
tags: []

---

# Storage API

Faculty startup explicitly selects a named demo catalog. The health response confirms the actual C++ child and catalog before storage requests begin. [Verified PowerShell commands](DEMO_COMMANDS.md)

Run the backend on loopback from the project root. Use Python 3.11, build the Debug simulator, and install the pinned requirements into the project-local environment.

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r backend/requirements.txt
cmake --preset msvc-debug
cmake --build --preset msvc-debug
.\.venv\Scripts\python.exe -m backend.app
```

The Python launcher may be replaced with the installed Python 3.11 executable if its registration is unavailable. The launcher defaults to `http://127.0.0.1:8000`. Open `/docs` for the live OpenAPI schemas, or fetch `/openapi.json`. The equivalent direct command is `.venv\Scripts\python.exe -m uvicorn backend.app.main:app --host 127.0.0.1 --port 8000`. Run one worker.

## Configuration

The process reads environment variables and optionally `backend/.env`. Copy values from [the example](../backend/.env.example) as needed. Explicit environment variables take precedence.

- `RAID_ENGINE_EXECUTABLE` defaults to the Debug build, with Release as a fallback. `RAID_STORAGE_ROOT` defaults to `build/api-storage`, which the backend creates if needed. Explicit roots must already exist. Relative configured paths resolve from the project root. Use a dedicated root for tests and demonstrations.
- `MAX_UPLOAD_MIB` defaults to 100. `ENGINE_REQUEST_TIMEOUT_SECONDS`, `ENGINE_STARTUP_TIMEOUT_SECONDS`, and `ENGINE_SHUTDOWN_TIMEOUT_SECONDS` default to 30, 15, and 15. `APP_PORT` and `LOG_LEVEL` configure the launcher.
- The launcher binds to `127.0.0.1`. The app accepts loopback Host headers and browser mutations from the configured loopback server or Vite on port 5173. This academic simulator has no authentication. Keep the API on a trusted local machine.

## Endpoints

The React console consumes these loopback endpoints through Vite’s `/health` and `/api` proxies. [Dashboard connection, polling, and file handling](dashboard.md)

- **Health** — `GET /`, `GET /health`, `GET /api/version` preserve the starter contracts. `GET /api/health` and `GET /api/engine/status` probe the engine and RAID catalog. They return readiness, process ID, version, and counts by actual array health. An unready probe returns HTTP 503.
- **Disks** — `GET /api/disks`, `GET /api/disks/{disk_id}`, `POST /api/disks`. Create with `disk_id` and integer `capacity_mib`.
- **RAID arrays** — `GET /api/raids`, `GET /api/raids/{array_id}`, `GET /api/raids/{array_id}/status`, `GET /api/raids/{array_id}/members`, `POST /api/raids`, `POST /api/raids/{array_id}/fail`, `POST /api/raids/{array_id}/verify`, `GET /api/raids/{array_id}/map/{lba}`. Create with `array_id`, `raid_level` (0, 1, 5, or 6), ordered `members`, and optional `stripe_unit_blocks`. Failure injection accepts `disk_id`.
- **Spares** — `GET /api/spares`, `POST /api/spares`. Register a blank image with `disk_id`.
- **Rebuild jobs** — `GET /api/rebuilds`, `GET /api/rebuilds/{job_id}`, `POST /api/rebuilds`, and `POST /api/rebuilds/{job_id}/{start|pause|resume}`. Prepare with `array_id`, `failed_disk_id`, and `spare_disk_id`. Start and resume accept an optional `checkpoint_limit` of batches. HTTP 202 acknowledges the worker request. The initial persisted state can still be `PREPARED` or `PAUSED` while the owner starts the worker. Poll status for actual checkpoint counts and `worker_active`.
- **Pools** — `GET /api/pools`, `GET /api/pools/{pool_id}`, `POST /api/pools`, `POST /api/pools/{pool_id}/verify`. Create with `pool_id`, `array_id`, and optional `chunk_blocks`, `max_ratio`, `warning_percent`, and `critical_percent`. Capacities describe allocation in the RAID address space.
- **LUNs** — `GET /api/luns`, `GET /api/luns/{lun_id}`, `POST /api/luns`, and `GET /api/luns/{lun_id}/map/{lba}`. Create with `lun_id`, `pool_id`, `capacity_mib`, and `mode` (`raw` or `file`). `GET` and `PUT /api/luns/{lun_id}/blocks/{lba}` transfer exactly one 4096-byte block in lowercase `data_hex` and require a raw LUN.
- **Files** — `GET /api/files` supports optional `lun_id` filtering. `GET /api/files/{file_id}` inspects metadata. `GET /api/files/{file_id}/verify` verifies SHA-256. `GET /api/luns/{lun_id}/files/verify` reports each completed file's verification. `POST /api/luns/{lun_id}/files` accepts a multipart field named `file`. `GET /api/files/{file_id}/download` streams the verified attachment and sends `X-Content-SHA256`.

Creation returns HTTP 201. Example file upload and download after creating a file-mode LUN.

```powershell
curl.exe -F "file=@sample.bin" http://127.0.0.1:8000/api/luns/lun0/files
curl.exe http://127.0.0.1:8000/api/files/file1/verify
curl.exe --output sample-copy.bin http://127.0.0.1:8000/api/files/file1/download
```

## Rebuild scheduling

- `GET /api/rebuild-policies` returns supported policies, default values, and controller version.
- `POST /api/rebuilds` accepts an optional `scheduling` object. For example, submit `{"policy":"FIXED_THROTTLE","fixed_delay_us":12700}` or `{"policy":"ADAPTIVE","target_read_p95_us":8000,"maximum_delay_us":6000}` in that field. The prepare and status responses return the persisted configuration. Existing requests default to full speed.
- `GET /api/rebuilds/{job_id}/scheduler` returns the persisted configuration, actual durable block counts, and up to 24 recent process-local decisions. `current` is null when the worker has no active decision. Unavailable p95 and foreground observations are null. The endpoint returns MEASURED data and the decision history restarts with the process or the resumed worker.

Policy configuration uses integer microseconds for delays and latency targets and integer milliseconds for control, idle, and observation intervals. Delay bounds are 0–50,000 µs for adaptive and 1–50,000 µs for fixed. The control interval must be within the observation and idle windows. The selected configuration persists through resume and interruption. The NDJSON equivalents are `rebuild.policies` and `rebuild.scheduler`, and `rebuild.prepare` accepts the same optional object.

## NDJSON and errors

The measurement and modeling extensions use the existing version 1 envelope and EngineClient serialization.

- `GET /api/telemetry` calls `telemetry.snapshot`. The `MEASURED` payload contains the monotonic observation window, per-category counters, origin breakdown, latency summaries, and foreground logical rates. Set `include_samples=true` for retained nanosecond samples, or `event_limit=0–256` for recent event details.
- `POST /api/telemetry/reset` calls `telemetry.reset` and returns the new observation window. It clears process-local measurements. An uncertain outcome follows the existing mutation reconciliation flow.
- `GET /api/telemetry/rebuilds/{job_id}` calls `telemetry.rebuild`. The response pairs durable job progress and checkpoint time with retained per-job counters and samples.
- `GET /api/model/profiles` calls `model.profiles` and returns `MODELED` presets and bounds.
- `POST /api/model/simulate` calls `model.simulate` and returns `MODELED` services, waits, starts, and completions. This endpoint computes a scenario without mutating storage. JSON validation and the 32 KiB body limit apply. [Model request and limits](performance_model.md#limits-and-api)


FastAPI starts the child with `raid_simulator.exe --serve-json --storage-root <absolute-path>`. You can also invoke that command directly. Each UTF-8 newline-terminated request carries a caller-unique ID, an operation, and its parameter object.

```json
{"request_id":"req-001","protocol_version":1,"operation":"raid.list","params":{}}
```

The engine echoes the ID and protocol version in a success envelope with `ok: true` and `data`, or an error envelope with `ok: false` and `error.code` and `error.message`. Protocol version 1 may be omitted in a request. An explicit unsupported version returns `PROTOCOL_VERSION_UNSUPPORTED`. Malformed or oversized frames without a reliably parsed ID use a null response ID. An unterminated frame at EOF is discarded. The request limit is 32 KiB and the response limit is 8 MiB. The build pins [nlohmann/json 3.12.0](https://github.com/nlohmann/json/releases/tag/v3.12.0) and verifies the vendored header's release hash.

HTTP storage errors use `{"error":{"code":"...","message":"...","request_id":"..."}}`. The request ID is included for IPC failures where one was assigned. Examples include 404 for missing resources, 409 for duplicates, incompatible RAID states, or checksum mismatch, 413 for body limits, 422 for invalid parameters, 503 for engine or lock unavailability, 504 for timeouts or uncertain mutations, and 507 for exhausted storage.

The backend drains stdout and stderr independently and serializes ordinary IPC requests. A queued request that times out before submission has no effect. A submitted mutation whose outcome cannot be established returns `OPERATION_OUTCOME_UNKNOWN`. The backend retires that connection and performs no implicit retries. Restart the backend deliberately, then reconcile disk, RAID, pool, LUN, file, or rebuild status before sending the mutation again. Durable records and rebuild leases remain authoritative.

## File exchange and shutdown

Large binary content travels on disk through engine-controlled paths under `ipc_exchange/uploads` and `ipc_exchange/downloads` in the storage root. Requests carry random exchange tokens and a validated upload basename. The backend checks directories and file attributes. The engine restricts the same component names and uses its existing pinned path and reparse-point checks. An HTTP upload body and its file are size-limited. Streaming copies use bounded buffers. Downloads start after the C++ export checksum passes.

Normal completion and ordinary errors clean up exchange files. Successful downloads clean up after response streaming. Ambiguous operations can retain their exchange directories until the child is stopped and the operation is reconciled. Keep the exchange tree under the simulator's control.

On graceful stop the backend stops serving storage requests and requests engine shutdown. The engine asks its managed rebuild workers to stop at safe boundaries and joins them, including on stdin EOF. After the configured shutdown grace the backend can terminate its own child. A stale running job is recovered from its durable checkpoint by the existing rebuild lease protocol.

Windows metadata replacement tolerates short-lived sharing denials with a bounded retry on the same staged record. A continuing denial still fails the operation and leaves the prior checkpoint authoritative.

Foreground IPC admits one request at a time. Long file transfers and RAID verification occupy that queue. The C++ process can run up to four managed rebuild jobs, subject to the storage catalogs and rebuild leases. HTTP and IPC times include queueing and host filesystem work. They are not modeled hardware latencies.

## Verification

```powershell
ctest --preset msvc-debug --output-on-failure
ctest --preset msvc-release --output-on-failure
.\.venv\Scripts\python.exe -m pytest backend/tests -v
```
