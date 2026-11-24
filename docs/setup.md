# Windows development setup

For the end-to-end local walkthrough, use the project-root PowerShell startup scripts and a named faculty storage root. The Visual Studio generator configures through `msvc-debug` for both Debug and Release. Build and test each configuration with its own preset. [Quick Start](QUICK_START.md) · [One-command validation](DEMO_COMMANDS.md#o-troubleshooting-and-validation)

## Prerequisites

- Windows 11 and [Visual Studio Code](https://code.visualstudio.com/).
- Visual Studio 2022 or [Visual Studio 2022 Build Tools](https://learn.microsoft.com/en-us/visualstudio/install/install-visual-studio?view=vs-2022). In Visual Studio Installer, select **Desktop development with C++**, the MSVC v143 x64/x86 toolset, and a Windows SDK. [Microsoft's C++ setup guide](https://code.visualstudio.com/docs/cpp/config-msvc/)
- [CMake](https://cmake.org/download/) 3.21 or newer on PATH.
- [Python](https://www.python.org/downloads/windows/) 3.11 or newer with pip. Enable the installer's PATH option or use the installed Python launcher's `py -3.11` command in place of `python`.
- [Node.js](https://nodejs.org/en/download) 22.12 or newer on a supported release line, with npm. [Vite Node.js requirements](https://v7.vite.dev/guide/)
- [Git for Windows](https://git-scm.com/download/win) on PATH.

Confirm tool availability in a fresh PowerShell session.

```powershell
cmake --version
python --version
python -m pip --version
node --version
npm.cmd --version
git --version
```

## C++ and VS Code

In VS Code, use **File → Open Folder** and select `software-defined-raid`, the directory containing `CMakePresets.json`. Install Microsoft's **C/C++** extension for debugging and **CMake Tools** for preset-aware IntelliSense.

The Visual Studio 2022 x64 generator discovers MSVC and runs MSBuild from an ordinary terminal. For direct `cl.exe` commands, open **x64 Native Tools Command Prompt for VS 2022** and run `cl` there. If CMake reports that it cannot find a Visual Studio instance, check the C++ workload and Windows SDK in Visual Studio Installer.

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug --output-on-failure
cmake --build --preset msvc-release
ctest --preset msvc-release --output-on-failure
```

Use **Tasks: Run Task** to select **CMake Configure**, **CMake Build Debug**, **CMake Build Release**, **Run RAID Simulator**, or **Run C++ Tests**. The build tasks first configure the shared Visual Studio solution. The run and test tasks build Debug before launching. Select **Debug RAID Simulator** in **Run and Debug** and press **F5** to build and launch the Debug executable with `--status`.

## Virtual disk CLI

Run `.\build\msvc-debug\Debug\raid_simulator.exe --help` from the project root. The CLI determines the project root by walking up from the executable to the nearest directory containing both `CMakeLists.txt` and `CMakePresets.json`. The terminal's working directory does not select storage. Keep the built executable under the project tree. Images are stored under `data/virtual_disks/` in that root.

- `disk create <id> [capacity-MiB]` creates a new zero-filled image. The CLI accepts 1–1024 MiB and defaults to 100 MiB.
- `disk list` lists supported, block-aligned images in numeric ID order.
- `disk info <id>` reports the image path, exact capacity, block size, block count, and open state in the current process.
- `disk write <id> <lba> <text>` writes UTF-8 text and zeros to the rest of the block. Quote text with spaces. The limit is 4096 encoded bytes.
- `disk read <id> <lba>` prints a hex and printable UTF-8 preview of up to 64 bytes and reports omitted bytes.
- `disk verify <id>` checks image size and alignment and reads the boundary blocks. Verification reports those checks only.

Use a lowercase `disk` prefix followed by an unsigned 64-bit decimal number, with no leading zeros except for `disk0`. Duplicate IDs, path arguments, linked image files, and storage directories reached through reparse points are rejected. Test runs use isolated build directories and create temporary Windows junctions to verify those checks. Use a local Windows volume with junction and hard-link support, such as NTFS, for the full CTest suite.

## RAID CLI

Run the `raid` commands with the same executable. Array IDs use the lowercase `array` prefix followed by a decimal number, with the same numeric rules as disk IDs.

- `raid create <id> raid0 <disk> <disk> [...] [--stripe <blocks>]` creates a stripe set with at least two members. The default is 16 blocks. The stripe unit must divide each member's block count.
- `raid create <id> raid1 <disk> <disk>` creates a two-member mirror. The recorded stripe unit is 1.
- `raid create <id> raid5 <disk> <disk> <disk> [...] [--stripe <blocks>]` creates a rotating XOR parity array. The default is 16 blocks, and the stripe unit must divide the member block count.
- `raid create <id> raid6 <disk> <disk> <disk> <disk> [...] [--stripe <blocks>]` creates a distributed P/Q array with 4–257 members. The same alignment and default stripe-unit rules apply.
- `raid list` lists persisted configurations. `raid info <id>` opens the array and reports geometry and status.
- `raid write <id> <lba> <text>` writes a zero-padded logical block. `raid read <id> <lba>` shows a hex and text preview.
- `raid write-blocks <id> <lba> <count> <text>` repeats one padded payload across 1–4096 consecutive blocks in a single request. With four members and default units, send 48 blocks aligned to 48 for a RAID 5 full row, or 32 blocks aligned to 32 for a RAID 6 full row.
- `raid map <id> <lba>` shows physical coordinates, including data and both parity positions for RAID 6.
- `raid verify <id>` reads mapped samples for RAID 0, compares the full mirrors for RAID 1, and checks every parity row for RAID 5 and RAID 6. Trusted checksums are required to establish end-to-end integrity.
- `raid demo-degraded-read <id> <disk>[,<disk>] <lba>` changes availability within one invocation. RAID 1 and RAID 5 accept one unavailable member. RAID 6 can read with up to two. Writes require all members.
- `raid fail <id> <disk>` persistently marks a current member `FAILED`. Use a dedicated test array. A duplicate request preserves the state and original cause.
- `raid status <id>` reports effective health, counts, tolerance, the pending write marker, and permitted I/O. `raid list` includes health for each array.
- `raid members <id>` shows ordered member states, physical image presence, and causes. An intact image can belong to a failed member. A reappearing image remains offline if its absence was recorded.

Select unused, equal-sized, all-zero images when creating arrays. Creation preserves member bytes. While the metadata owns a disk ID, `disk write` and `disk create` are blocked for that ID. Leave catalog and member files under simulator control. An interrupted write can leave the array `INCONSISTENT` and block logical access. Degraded reads use available redundancy and leave member bytes untouched. All RAID writes require a healthy array.

## Spare and rebuild CLI

- `spare register <disk>` registers a zero-filled, unowned disk. Rebuilds require an exact member-capacity match.
- `spare list` reports capacity, assignment, state, and availability.
- `raid rebuild prepare <array> <failed-member> <spare>` reserves a registered spare and returns a job ID.
- `raid rebuild run <job>` starts the worker and waits in that terminal. `raid rebuild resume <job>` resumes a paused or interrupted job. Either accepts `--batches <count>` to pause after that many newly committed reconstruction batches.
- From another terminal, use `raid rebuild status <job>` to inspect progress and `raid rebuild pause <job>` to request a pause at the next batch boundary. `raid rebuild list` lists jobs, and `raid status <array>` shows array health alongside job progress.

Elapsed time in status measures wall-clock seconds from the first run through the most recent durable checkpoint. It includes pauses before that checkpoint. The final verification pass has its own completion signal in the job state.

Maintain both the job and spare catalog records between runs. If the worker process exits unexpectedly, inspect the job before resuming. Once the lease is released, an unfinished `RUNNING` job reports `INTERRUPTED`. Resume validates the sources and spare, restarts from the last checkpoint, and performs a full verification pass before promoting the spare.

## Provisioning CLI

- `pool create <pool> <array> [--chunk-blocks <count>] [--max-ratio <integer>]` claims one healthy RAID array. Chunk size is fixed per pool, defaults to 16 blocks, and accepts 1–1024 blocks. A maximum ratio of zero means unlimited.
- `pool list`, `pool info <pool>`, and `pool verify <pool>` show state, capacity, and validated allocation metadata.
- `pool watermarks <pool> [<warning-percent> <critical-percent>]` displays or sets the advisory thresholds. Use whole percentages with `1 <= warning < critical <= 100`.
- `lun create <lun> <pool> <capacity-MiB>`, `lun list`, and `lun info <lun>` manage the advertised logical capacity and show allocated chunks.
- `lun read <lun> <lba>`, `lun map <lun> <lba>`, and `lun write <lun> <lba> <text>` inspect or write a logical block. Text writes pad with zeros.
- `lun read-blocks <lun> <lba> <count>` reads up to 4096 contiguous blocks. `lun write-blocks <lun> <lba> <count> <text>` repeats one padded payload over the requested blocks.

For a disposable CLI catalog, place a copy of the built simulator under a directory with its own `CMakeLists.txt` and `CMakePresets.json` root markers. The executable locates the nearest directory with both markers above itself. It creates its own `data` directories there. C++ callers can select an existing absolute root through `StorageEngine::initialize(root)` or the provisioning manager constructor.

```powershell
$demo = Join-Path (Resolve-Path ./build).Path ('provisioning-demo-' + [guid]::NewGuid())
$bin = New-Item -ItemType Directory -Path (Join-Path $demo 'bin') -Force
New-Item -ItemType File -Path (Join-Path $demo 'CMakeLists.txt'), (Join-Path $demo 'CMakePresets.json') | Out-Null
Copy-Item ./build/msvc-debug/Debug/raid_simulator.exe -Destination $bin.FullName
$simulator = Join-Path $bin.FullName 'raid_simulator.exe'
```

Leave metadata under simulator control. Pool creation preserves existing RAID bytes. Use a dedicated backing array because future LUN writes reuse its RAID logical space and initialize whole chunks. Direct RAID reads are diagnostic. Pool-owned arrays reject direct RAID writes. If an interrupted write leaves a reservation, `pool info` shows it separately and the next healthy LUN write can safely reclaim the unpublished chunks. Physical exhaustion rejects an entire request before writing when it requires too many new chunks.

## File management CLI

The build compiles SQLite 3.54.1 from the official amalgamation at `third_party/sqlite/sqlite-amalgamation-3540100.zip`. CMake verifies the archive SHA-256 before extraction. The vendored source supports offline builds in both MSVC configurations. Windows CNG supplies SHA-256. [SQLite release history](https://sqlite.org/changes.html#version_3_54_1)

- `lun create <lun> <pool> <capacity-MiB> --mode file` creates an empty file-managed LUN and initializes its root's SQLite catalog if needed. Existing raw LUNs have no conversion command.
- `file import <lun> <source-path>` imports a regular binary or text file and returns its stable file ID on success.
- `file list [<lun>]` lists file records, including failed imports. `file info <file-id>` shows the exact size, extent, SHA-256 reference, and status.
- `file verify <file-id>` compares stored bytes with the reference. `file verify-all <lun>` checks every completed import and returns nonzero if a check fails.
- `file export <file-id> <destination-path>` verifies and exports exactly the original byte length. The parent directory must exist and the destination must be unused.

Relative paths resolve from the invoking terminal. Use host files outside the configured storage root's `data` directory and keep input and export names distinct. Run the [README demonstration](../README.md#file-import-export-and-integrity) against a disposable catalog, using the isolated executable setup above. If an import is interrupted, inspect its file record and failure message before importing again. Its old extent remains reserved.

## FastAPI backend

From the project root, build the C++ simulator and create the project-local Python 3.11 environment.

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r backend/requirements.txt
.\.venv\Scripts\python.exe -m backend.app
```

If `py -3.11` cannot find a registered interpreter, run the installed Python 3.11 executable with `-m venv .venv`. Open `http://127.0.0.1:8000/docs` for Swagger UI. The original `/health` confirms backend liveness. Use `/api/health` or `/api/engine/status` for storage readiness. The backend starts and stops the simulator itself. Set an existing `RAID_STORAGE_ROOT` for a dedicated lab root or use the default `build/api-storage`. [API reference and configuration](api.md)

VS Code uses the project-local interpreter and offers backend install, run, test, and IPC verification tasks. The **Debug FastAPI Backend** configuration builds Debug first. Run `.\.venv\Scripts\python.exe -m pytest backend/tests -v` from the project root to test API and C++ IPC behavior against temporary roots under `build/`.

## React frontend

Keep the backend running. From the project root, use a second PowerShell terminal.

```powershell
Set-Location frontend
npm.cmd ci
npm.cmd run dev
```

Open `http://127.0.0.1:5173`. Vite forwards same-origin `/health` and `/api` requests to Uvicorn at `127.0.0.1:8000`. The dashboard monitors both backend liveness and storage readiness. Use **Refresh** to update the current page.

Run `npm.cmd run build` from `frontend` to generate `dist/`, and `npm.cmd test` to run the frontend suite. Local production hosting needs the same `/health` and `/api` routes. [Dashboard configuration and browser smoke test](dashboard.md)

The dashboard’s **Telemetry** page is available when FastAPI and the engine are ready. Run `npm.cmd run test:browser` from `frontend` with the Debug simulator built to exercise storage and telemetry on a disposable root. [Browser verification setup](dashboard.md#verify)

## Benchmark workflow

Build the Release simulator, then use the project-local Python environment to install `benchmarks/requirements.txt`. The benchmark CLI runs against its own workspace and writes results under `benchmarks/results`. [Benchmark commands and analysis](../benchmarks/docs/reproduction_guide.md)

Run `benchmark rebuild --config benchmarks/configs/rebuild_comparison.json` with the Release simulator from the project root for the matched Step 14 study. The pilot and changing-load run have separate configurations. [Rebuild experiment commands](../benchmarks/docs/rebuild_reproduction_guide.md)

## Common issues

- Reopen VS Code after installing tools or changing PATH so its terminals and tasks inherit the updated environment.
- If PowerShell blocks `npm.ps1`, use `npm.cmd` as shown above.
- If either service reports an address in use, check which process is listening on port 8000 or 5173. If you change `APP_PORT`, set `VITE_PROXY_TARGET` to the matching backend address before starting Vite.
- If the dashboard shows a connection warning, check `/health` and `/api/engine/status` directly on the backend, then refresh the dashboard.
