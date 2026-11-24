---
noteId: "4cf0a9b0e80f11f1a150872f649e1a14"
tags: []

---

# Verified Windows PowerShell command reference

All PowerShell fragments run from the project root unless a command says otherwise. Use one named faculty session. Steps that create state can be repeated with a new name. The setup and verification scripts check actual server responses. A prepared session can be reopened with the same name.

## A. Project directory

```powershell
Set-Location -LiteralPath 'G:\7th_semProjects\ISM\software-defined-raid'
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$demo = 'Faculty'
```

Choose this folder in VS Code. The execution policy applies to this PowerShell process.

## B. Environment

```powershell
& .\scripts\demo\00-check-environment.ps1
& .\.venv\Scripts\python.exe --version
& .\build\msvc-debug\Debug\raid_simulator.exe --help
```

The checklist reports CMake, CTest, MSVC, local Python 3.11, Node/npm, simulator version, and the backend port. The CLI help is safe to inspect. Its normal storage commands resolve the catalog from the executable's project location, so use the faculty API scripts for the isolated catalog.

## C–E. Start and stop the applications

```powershell
& .\scripts\demo\01-start-backend.ps1 -Demo $demo
& .\scripts\demo\02-start-frontend.ps1 -Demo $demo
Invoke-RestMethod 'http://127.0.0.1:8000/api/engine/status'
```

The backend prints actual readiness and a process ID at [API documentation](http://127.0.0.1:8000/docs). The frontend starts the existing npm dev script and probes the Vite proxy at [RAID Console](http://127.0.0.1:5173/). After the walkthrough, use the tracked stop command. Both services shut down through their own lifecycle handlers.

```powershell
& .\scripts\demo\stop-demo.ps1 -Demo $demo
```

## F–G. Disks, RAID, mapping, and verification

```powershell
& .\scripts\demo\03-create-demo-storage.ps1 -Demo $demo
& .\scripts\demo\04-raid-demo.ps1 -Demo $demo
. .\scripts\demo\common.ps1
$session = Get-Demo $demo
$Api = Connect-Demo $session
Invoke-DemoApi GET 'disks'
Invoke-DemoApi GET 'disks/disk61'
Invoke-DemoApi GET 'raids'
Invoke-DemoApi GET 'raids/array6'
Invoke-DemoApi GET 'raids/array6/status'
Invoke-DemoApi GET 'raids/array6/map/0'
Invoke-DemoApi GET 'raids/array6/map/2'
Invoke-DemoApi POST 'raids/array6/verify'
```

Setup creates 13 images and four arrays through `POST /api/disks` and `POST /api/raids`, as well as the two pools and three LUNs used below. The RAID script prints members, geometry, actual placement, and verification. Compare the RAID 6 P and Q disk IDs at the two logical addresses to explain parity rotation.

## H–J. Failure, spare, and rebuild

```powershell
& .\scripts\demo\05-failure-rebuild-demo.ps1 -Demo $demo -Policy ADAPTIVE -ConfirmFailure -PauseForInspection
Invoke-DemoApi GET 'raids/array1/status'
Invoke-DemoApi GET 'spares'
Invoke-DemoApi GET 'rebuild-policies'
Invoke-DemoApi GET 'rebuilds/rebuild1'
Invoke-DemoApi GET 'rebuilds/rebuild1/scheduler'
```

The script persists failure on `disk11` once, validates the degraded read, registers `disk13`, and prepares, starts, pauses, resumes and verifies the rebuild. Use `FIXED_THROTTLE` or `FULL_SPEED` in a fresh fixture to demonstrate those policies. At the prompt the durable checkpoint is visible in **Rebuild Monitor > rebuild1**. Press Enter to resume. Omit `-PauseForInspection` for unattended execution.

For a separate manual run, the mutation order is `POST raids/array1/fail`, `POST spares`, `POST rebuilds`, then `POST rebuilds/{job_id}/start`, `POST rebuilds/{job_id}/pause`, and `POST rebuilds/{job_id}/resume`. The script uses a two-batch checkpoint limit so the pause is observable even on a fast machine. Inspect the actual request bodies in [the verified recovery script](../scripts/demo/05-failure-rebuild-demo.ps1) or the live OpenAPI page.

## K. Thin provisioning

```powershell
& .\scripts\demo\06-thin-provisioning-demo.ps1 -Demo $demo
Invoke-DemoApi GET 'pools/pool5'
Invoke-DemoApi GET 'luns/lun5'
Invoke-DemoApi GET 'luns/lun5/map/16'
(Invoke-DemoApi GET 'luns/lun5/blocks/16').mapped
```

Setup creates a 24 MiB Raw LUN in a 12 MiB physical pool. The thin script uses `PUT luns/lun5/blocks/{lba}` for LBAs 0, 1 and 16 and reads back the exact bytes. It verifies zero-fill, one allocation, a same-chunk overwrite, and a second allocation. Run it before file import to observe the pool start at zero.

## L. Files

```powershell
& .\scripts\demo\07-file-management-demo.ps1 -Demo $demo
Invoke-DemoApi GET 'files?lun_id=lun6'
Invoke-DemoApi GET 'files/file1'
Invoke-DemoApi GET 'files/file1/verify'
Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $session.Path 'exports/faculty note.txt')
```

The file script uploads three deterministic bytestrings through the multipart API, writes exports only to new names, and compares bytes and independently computed hashes. It also exercises a missing record returning HTTP 404. In a fresh scripted import, `file1` is the text sample. If a GUI upload came first, use the ID printed beside the actual filename.

## M. Telemetry and device scenarios

```powershell
& .\scripts\demo\08-telemetry-demo.ps1 -Demo $demo
Invoke-DemoApi GET 'telemetry?include_samples=true&event_limit=24'
Invoke-DemoApi GET 'telemetry/rebuilds/rebuild1'
Invoke-DemoApi GET 'model/profiles'
```

The telemetry script generates twelve actual reads, prints MEASURED counters and percentiles, and submits the same deterministic request scenario to HDD, SATA SSD, and NVMe SSD MODELED profiles. For an intentional telemetry reset, add `-ConfirmReset`. That command resets observation history while retaining the array and durable rebuild state.

## N. Benchmarks and figures

```powershell
& .\scripts\demo\09-research-results-demo.ps1
& .\scripts\demo\09-research-results-demo.ps1 -SkipFigureRegeneration -LiveTrial -LegacyTrial
```

Run the second command only if a short live comparison is useful. It runs a single adaptive RAID 1 trial and one 16-operation RAID 5 trial. The latter checks CLI `validate`, `dry-run`, `run`, and `info` before running analysis. Both use new output directories. Normal research presentation rechecks archived CSVs and manifests and regenerates separate figures under `build/final-validation`. See [verified plots](FINAL_RESEARCH_RESULTS.md#figures-and-reproduction). The optional `-OpenPlots` flag asks Windows to open three archived adaptive figures in the registered viewer.

## O. Troubleshooting and validation

```powershell
netstat -ano -p TCP | Select-String ':8000 .*LISTENING|:5173 .*LISTENING'
Get-Command cmake.exe, node.exe, npm.cmd
Test-Path -LiteralPath .\build\msvc-debug\Debug\raid_simulator.exe
& .\.venv\Scripts\python.exe -m pip check
& .\scripts\demo\10-verify-demo.ps1 -Demo $demo
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\validate-all.ps1
```

Unused ports produce no netstat match. Stop the tracked frontend before starting full validation, which needs ports 5173 and 18011 for its isolated Edge smoke test. Use `-Port 18015` on the backend startup script if 8000 is occupied. The frontend follows the tracked port automatically. Engine and service logs remain under the named session. [Detailed fixes and fallbacks](TROUBLESHOOTING.md)

