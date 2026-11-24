---
noteId: "97e233a0e7ef11f19fa83bab5b22ac42"
tags: []

---

# RAID Console

Use the [faculty walkthrough](FACULTY_DEMO_GUIDE.md) for exact click paths and the verified 1440, 1024 and 390 px browser coverage. A supervised demo start uses the existing Vite proxy and the backend port recorded for that named session.

The dashboard manages the same catalog as the CLI and FastAPI. Run it on a trusted local machine. Use a disposable storage root when demonstrating persistent member failures or reconstruction.

## Start the console

From the project root, build the simulator and use the existing project-local Python 3.11 environment. See [Windows setup](setup.md) for first-time prerequisites.

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug
.\.venv\Scripts\python.exe -m backend.app
```

In a second PowerShell terminal, start Vite.

```powershell
Set-Location frontend
npm.cmd ci
npm.cmd run dev
```

Open [RAID Console](http://127.0.0.1:5173/). Vite forwards relative `/health` and `/api` requests to FastAPI on port 8000. When changing the backend port, set `APP_PORT` for FastAPI and `VITE_PROXY_TARGET` to the matching backend origin before starting Vite. Put local Vite values in `frontend/.env.local` or set them in the Vite terminal.

Use `npm.cmd run build` to generate `frontend/dist`. Serve the build with a local same-origin proxy for `/health` and `/api`. `VITE_API_BASE_URL` is an optional build-time prefix, with a trailing slash allowed. If using a separate origin, that origin must permit the dashboard under the existing localhost Host, Origin, and CORS rules. The Vite development proxy supplies those paths during local development. Keep secrets in backend configuration.

## Architecture

```text
React Dashboard
      |
      v
FastAPI REST API
      |
      v
Persistent NDJSON IPC
      |
      v
C++ Storage Engine
      |
      +-- Virtual Disks
      +-- RAID 0 / 1 / 5 / 6
      +-- Failure Management
      +-- Hot Spares / Rebuilding
      +-- Thin Provisioning
      +-- File Management
      |
      v
Persistent Images + Metadata + SQLite
```

React Router uses hash routes so detail links and browser navigation work with static hosting. The shared shell supplies navigation, status, operation notifications, and manual refresh. Page modules own their inventory and detail views. Shared components cover capacity, block progress, forms, accessible dialogs, tables, and query states. Lucide supplies icons. TanStack Query caches resource reads and forwards cancellation signals. A centralized API client handles JSON errors, bounded requests, verified binary downloads, and multipart uploads. [REST contracts](api.md#endpoints)

## Pages

- **Overview** reports live disk and array inventories, health counts, pool allocation, logical provisioned capacity, imported files, and a snapshot of rebuilds.
- **Virtual Disks** creates disk images and inspects block geometry and ownership.
- **RAID Arrays** creates RAID 0, 1, 5, and 6 arrays from existing disks, verifies array consistency, displays member states, and maps one LBA at a time. The visualization displays backend-reported data, mirror, P, and Q roles. Failure injection requires explicit confirmation and persists the member state while preserving the image file.
- **Hot Spares** registers existing unassigned blank disks and displays assignments and availability. The engine validates final eligibility.
- **Rebuild Monitor** prepares a job, reserves the selected spare, and starts, pauses, or resumes reconstruction. Start and resume accept an optional checkpoint batch limit. Progress uses durable physical block counts. A pause request is acknowledged before the worker reaches its checkpoint.
- **Storage Pools** claims and verifies a backing RAID array. Capacity shows allocated, reserved, and free RAID address space. Utilization includes allocated and reserved bytes. Host filesystem use is separate.
- **Logical Volumes** creates raw and file-managed LUNs, displays provisioned and allocated space, and inspects logical chunk mappings. The engine applies the pool’s maximum provisioning ratio.
- **File Explorer** imports binary files into file-managed LUNs, shows import state and extents, verifies SHA-256, and downloads verified bytes under their original filename.
- **System** displays backend, engine, and catalog states, reported versions and process ID, and supported RAID levels.
- **Telemetry** shows MEASURED counters, recent-window latency summaries, foreground rates, and durable rebuild progress. It refreshes measurements every four seconds on the active page. Reset requires confirmation and starts a new observation generation. In the separate MODELED panel, select a profile, inspect its assumptions, and run or edit a bounded scenario. [Telemetry methodology](telemetry_methodology.md) · [Device model](performance_model.md)

## Connection, progress, and mutation handling

Rebuild Monitor offers Full-speed, Fixed throttle, and Adaptive when preparing a job. Fixed throttle accepts an inter-batch delay. Adaptive accepts the foreground p95 target and maximum inter-batch delay. A job's scheduling panel displays the engine-reported pressure, delay, outstanding reads, and recent measured decisions beside durable block progress. A checkpoint pause or restart can clear the live decision while retaining the job configuration. [Measured policy comparison](../benchmarks/docs/rebuild_results.md)

The shell checks `GET /health` for FastAPI liveness and `GET /api/engine/status` for engine and catalog readiness. The check runs every 12 seconds and on manual refresh. Cached inventory is labeled when the service disconnects. Storage writes are disabled until readiness returns.

Overview and Rebuild Monitor poll the job collection every 1.6 seconds while a job is running or has an active worker. Polling stops after the last worker finishes or pauses, when the page changes, or when the service is unavailable. Successful actions refresh the affected queries. Other inventory reads are cached for 10 seconds. Requests are deduplicated and foreground tasks suspend polling while work is pending.

The console locks foreground operations until the prior request settles. Consequential changes require confirmation. An uncertain write blocks further mutations in the current browser tab and survives a reload. Refresh the affected view, inspect the resource, then select **I inspected the resource** to unlock operations. This includes timeouts or network interruptions after a request may have been sent.

## Binary exchange and limits

The upload uses multipart field `file` and reports browser transfer progress when available. After transfer, **Processing and verifying** describes the backend work. Successful import is reported after the REST response. Download receives the verified binary stream, uses the original attachment filename, and displays the `X-Content-SHA256` header when provided. The download is buffered in the browser before saving. File verification compares the digest of the stored bytes against the import reference. The latest verification result is available within the current dashboard session.

The REST interface supplies one-block mapping and checkpoint timestamps. Finish times, a protocol-version field, historical events, spare file identity, and hardware metrics are unavailable through this interface. File deletion, arbitrary failed-member recovery, and LUN resize are unsupported. Raw LUN block I/O remains available through the documented API. [Storage API](api.md)

## Verify

From `frontend` run the React Testing Library tests and build.

```powershell
npm.cmd test
npm.cmd run build
npm.cmd run test:browser
```

The browser smoke test uses installed Microsoft Edge by default. Set `RAID_BROWSER_EXECUTABLE` for another installed Chromium browser. Free ports 5173 and 18011 before starting. The script starts and stops both services, creates fresh isolated storage under `build/step12-demo-*`, and writes viewport screenshots, fixture files, logs, and JSON verification records there. It drives RAID mapping, provisioning, file transfer and integrity checks, and rebuild pause/resume. It also verifies measured I/O on all four RAID levels and runs the same modeled scenario with each profile.

Run the backend and C++ regression suites from the project root.

```powershell
.\.venv\Scripts\python.exe -m pytest backend/tests -v
ctest --preset msvc-debug --output-on-failure
ctest --preset msvc-release --output-on-failure
```
