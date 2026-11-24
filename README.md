# Software-Defined RAID & Virtual Provisioning Simulator

For a faculty demonstration, start with the [Quick Start](docs/QUICK_START.md). The [Faculty Demonstration Guide](docs/FACULTY_DEMO_GUIDE.md) covers five-minute and 19-minute walkthroughs. The [final validation](docs/FINAL_VALIDATION_REPORT.md) and [research results](docs/FINAL_RESEARCH_RESULTS.md) link to the test logs, data and measured figures.

Information Storage Management — CS374TFD

This academic project emulates an enterprise storage array on a Windows laptop using file-backed virtual disks. It provides a reproducible way to study RAID capacity and redundancy, thin provisioning, and the effect of rebuild work on application I/O.

## Architecture

FastAPI owns a persistent C++ engine process and exchanges versioned JSON requests over stdin and stdout. The React dashboard uses the REST API to manage virtual disks, RAID, rebuilds, thin provisioning, and files.

The C++ CLI manages file-backed disks and RAID 0, RAID 1, RAID 5, and RAID 6 arrays. Each disk is a raw `.img` file under `data/virtual_disks/`, with 4096-byte blocks addressed from LBA 0. Arrays persist in the storage root's RAID catalog and reopen in later CLI or engine processes.

Virtual disks on the same laptop drive share its hardware failure domain. Their files persist across simulator runs, and they provide no independent hardware fault tolerance.

## Technology stack

- C++17, Visual Studio 2022 MSVC, CMake, and CTest.
- Python 3.11+, FastAPI, and Uvicorn.
- React and Vite.
- The file catalog uses SQLite 3.54.1. Later research tools include NumPy, pandas, and matplotlib.

## Build, run, and test

Open the `software-defined-raid` directory in VS Code or run these commands from that directory in PowerShell.

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-debug
ctest --preset msvc-debug --output-on-failure

.\build\msvc-debug\Debug\raid_simulator.exe --help
.\build\msvc-debug\Debug\raid_simulator.exe --version
.\build\msvc-debug\Debug\raid_simulator.exe --status
.\build\msvc-debug\Debug\raid_simulator.exe --self-test
```

The `msvc-release` build and test presets use the same configuration tree and write Release executables under `build/msvc-debug/Release`.

## Virtual disk example

The standalone storage CLI uses the catalog beside this project. Use the [named faculty demo](docs/QUICK_START.md) for an isolated rehearsal that includes persistent failure injection.

Run from the project root. Use a free disk ID when you want to create a new image.

```powershell
$simulator = ".\build\msvc-debug\Debug\raid_simulator.exe"
& $simulator disk create disk0 100
& $simulator disk list
& $simulator disk info disk0
& $simulator disk write disk0 10 "Hello ISM"
& $simulator disk read disk0 10
& $simulator disk verify disk0
```

Each CLI call starts a new process. The write replaces one whole block and zero-pads after the UTF-8 text. Verification opens the disk, validates its size and alignment, and reads the first and last blocks. End-to-end content integrity requires trusted checksums.

For argument details, environment setup, and web service commands, see [Windows setup](docs/setup.md).

## RAID example

From the project root, choose unused disk and array IDs. The commands below use four fresh 100 MiB images.

```powershell
$simulator = ".\build\msvc-debug\Debug\raid_simulator.exe"
3..6 | ForEach-Object { & $simulator disk create "disk$_" 100 }
& $simulator raid create array0 raid0 disk3 disk4
& $simulator raid create array1 raid1 disk5 disk6
& $simulator raid list
& $simulator raid info array0
& $simulator raid write array0 20 "Hello RAID"
& $simulator raid map array0 20
& $simulator raid read array0 20
& $simulator raid write array1 20 "Hello mirrors"
& $simulator raid verify array1
& $simulator raid demo-degraded-read array1 disk5 20
```

RAID 0 stripes 16-block units across its ordered members by default. With two members, logical LBA 20 maps to physical LBA 4 on the second member. RAID 1 writes logical LBA 20 to physical LBA 20 on both members. Two 100 MiB disks yield 200 MiB usable in RAID 0 and 100 MiB in RAID 1.

Creation requires unassigned, equal-sized, entirely zero-filled disks. Raw CLI writes and replacement of missing images are blocked for array-owned IDs. An incomplete mirror or parity write leaves a persistent marker and blocks logical access until recovery. The degraded-read command changes availability only for that invocation.

## RAID 5 example

With the same executable, use four unused 100 MiB disks. The following IDs are examples.

```powershell
7..10 | ForEach-Object { & $simulator disk create "disk$_" 100 }
& $simulator raid create array2 raid5 disk7 disk8 disk9 disk10
& $simulator raid info array2
& $simulator raid write array2 20 "Hello RAID5"
& $simulator raid read array2 20
& $simulator raid map array2 20
& $simulator raid write-blocks array2 48 48 "Full stripe"
& $simulator raid demo-degraded-read array2 disk8 20
& $simulator raid demo-degraded-read array2 disk10 20
& $simulator raid verify array2
```

Four 100 MiB members provide 400 MiB raw and 300 MiB usable. One rotating 16-block unit per row holds XOR parity, leaving 48 logical blocks per complete row. A single-block update reads old data and parity and writes their replacements. An aligned full-row write computes parity from the new data and skips the old-block reads. RAID 5 can reconstruct data with one unavailable member. With two unavailable members the array rejects reads.

## RAID 6 example

Use four unused 100 MiB images. The following IDs are examples.

```powershell
11..14 | ForEach-Object { & $simulator disk create "disk$_" 100 }
& $simulator raid create array3 raid6 disk11 disk12 disk13 disk14
& $simulator raid list
& $simulator raid info array3
& $simulator raid write array3 20 "Hello RAID6"
& $simulator raid map array3 20
& $simulator raid read array3 20
& $simulator raid write-blocks array3 32 32 "Full RAID6 row"
& $simulator raid demo-degraded-read array3 disk12,disk11 20
& $simulator raid verify array3
```

Four members provide 400 MiB raw and 200 MiB usable. A row contains two 16-block data units, one XOR parity unit (P), and one weighted parity unit (Q). Q uses arithmetic over GF(2^8) with polynomial `0x11D`. The two independent parity equations recover any two unavailable members' data within the session simulation. Writes and complete P/Q verification require all members.

## Persistent member failures

Use a dedicated test array. The following example creates a mirror from unused images.

```powershell
& $simulator disk create disk15 1
& $simulator disk create disk16 1
& $simulator raid create array4 raid1 disk15 disk16
& $simulator raid write array4 20 "Persistent failure test"
& $simulator raid fail array4 disk15
& $simulator raid status array4
& $simulator raid members array4
& $simulator raid read array4 20
```

`raid fail` records a persistent state transition in the catalog. A failed member can still have an intact image, and the new state survives each CLI invocation. RAID 0 tolerates zero unavailable members, RAID 1 and RAID 5 tolerate one, and RAID 6 tolerates two. Degraded arrays serve reads within that tolerance. Writes require a healthy array. An unresolved write marker blocks logical access regardless of redundancy.

## Hot-spare rebuilds

Use a fresh disk of the same capacity as the failed member. This example assumes a dedicated two-member test array, an unused `disk29`, and a failed `disk27` in `array8`. Use the job ID returned by `prepare`.

```powershell
& $simulator disk create disk29 1
& $simulator spare register disk29
& $simulator spare list
& $simulator raid rebuild prepare array8 disk27 disk29
& $simulator raid rebuild run rebuild1 --batches 2
& $simulator raid rebuild status rebuild1
& $simulator raid rebuild resume rebuild1
& $simulator raid status array8
& $simulator raid verify array8
```

A degraded read returns the requested data using surviving members. A rebuild writes an entire missing member to a reserved spare. Redundancy improves only after the spare passes verification and becomes an active member. A RAID 6 array with two unavailable members remains degraded after the first replacement and becomes healthy after the second.

### Workload-aware rebuild scheduling

Rebuild jobs can use full-speed, fixed-delay, or adaptive pacing. The adaptive policy tracks recent foreground reads and adjusts a bounded inter-batch delay. Choose the policy in Rebuild Monitor or through the storage API. The reproducible RAID 1 study measures foreground response and verified rebuild completion on disposable file-backed arrays. [Results](benchmarks/docs/rebuild_results.md) · [Reproduction](benchmarks/docs/rebuild_reproduction_guide.md)

## Thin provisioning

A storage pool owns one healthy RAID array. LUNs share its available chunks and have independent logical addresses. Creating a LUN consumes no pool chunks. An unwritten logical chunk reads as zeros, and its first write allocates one 64 KiB chunk by default. Further writes in the same chunk reuse that allocation.

Use an isolated test catalog and a dedicated array. The example assumes a new, healthy `array12`. Run `disk list`, `raid list`, `pool list`, and `lun list` to confirm the IDs before creating anything.

```powershell
& $simulator pool create pool0 array12
& $simulator lun create lun0 pool0 1024
& $simulator lun create lun1 pool0 512
& $simulator lun read lun0 100
& $simulator lun write lun0 10 "Hello Thin"
& $simulator lun write lun0 11 "Same chunk"
& $simulator lun write lun0 32 "New chunk"
& $simulator lun write lun1 10 "Independent LUN"
& $simulator lun map lun0 10
& $simulator lun map lun1 10
& $simulator pool info pool0
& $simulator pool verify pool0
```

With 300 MiB of usable RAID capacity this example provisions 1,536 MiB of logical capacity. The four writes consume three chunks, or 192 KiB of the pool. Pool capacity counts whole chunks in the RAID logical address space. Host filesystem usage also depends on the member images and the filesystem.

Overprovisioning is unlimited by default. Use `--max-ratio <integer>` on pool creation to cap it. Warning and critical thresholds default to 70% and 90% and can be changed with `pool watermarks`. When space is exhausted, existing mappings remain writable on a healthy backing array. See the [setup guide](docs/setup.md) for the isolation, health, and recovery policies.

## File import, export, and integrity

Create a new file-managed LUN in a dedicated pool. This example assumes `pool3` exists in a disposable catalog and `lun5` is unused.

```powershell
& $simulator lun create lun5 pool3 512 --mode file
& $simulator file import lun5 "./sample.txt"
& $simulator file list lun5
& $simulator file info file1
& $simulator file verify file1
& $simulator file export file1 "./sample-export.txt"
& $simulator file verify-all lun5
```

Use the file ID returned by import. Each file's bytes live in a RAID-backed LUN extent. SQLite records placement, state, the verified SHA-256 reference, and operation outcomes. Exports verify the checksum before publishing and require a destination that does not already exist. [File management and storage recovery](docs/architecture.md#file-catalog-and-raid-backed-file-storage)

## Run the storage API

From the project root after building the simulator, use Python 3.11 and the project-local environment.

```powershell
py -3.11 -m venv .venv
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r backend/requirements.txt
.\.venv\Scripts\python.exe -m backend.app
```

Open `http://127.0.0.1:8000/docs` for the API and `/api/health` for engine readiness. The default API data root is `build/api-storage`. [API reference](docs/api.md) · [Windows setup](docs/setup.md)

## Run the dashboard

Keep the backend running. In a second terminal, run from the project root.

```powershell
Set-Location frontend
npm.cmd ci
npm.cmd run dev
```

Open `http://127.0.0.1:5173`. The console manages virtual disks, RAID arrays, spares, rebuilds, pools, volumes, and files. [Dashboard guide and tests](docs/dashboard.md)

## Roadmap

Step 12 adds measured C++ telemetry and configurable hypothetical device scenarios to the dashboard and API. The measured counters describe file-backed simulator I/O. The modeled calculations assume independent devices with explicit arrivals and dependencies. [Telemetry methodology](docs/telemetry_methodology.md) · [Performance model](docs/performance_model.md)

Step 13 adds isolated, repeatable RAID and thin-provisioning benchmarks with raw requests, repetition summaries and measured plots. [Benchmark results](benchmarks/docs/benchmark_results.md) · [Methodology](benchmarks/docs/benchmark_methodology.md) · [Reproduction guide](benchmarks/docs/reproduction_guide.md)
