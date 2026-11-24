---
noteId: "875e3e51e80f11f1a150872f649e1a14"
tags: []

---

# Faculty demo troubleshooting

## The backend does not start

Enter the project root in Windows PowerShell, enable script execution for the current process, and run the environment checker. This verifies the MSVC workload, local Python 3.11, imports, and the actual Debug executable. Inspect the two backend logs in `build/final-demo/<name>/logs`.

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
& .\scripts\demo\00-check-environment.ps1
Test-Path -LiteralPath .\build\msvc-debug\Debug\raid_simulator.exe
& .\.venv\Scripts\python.exe -m pip check
& .\.venv\Scripts\python.exe -c 'import fastapi, uvicorn; print(fastapi.__version__)'
cmake --preset msvc-debug
cmake --build --preset msvc-debug
```

If the service starts but readiness fails with catalog access denied, stop the tracked session from the same workspace-root terminal. Restart using `01-start-backend.ps1` so the Python supervisor has the verified working directory and an explicit regular demo root. Select a fresh name if the existing demo has no valid identity. Leave Windows ACLs and catalog locks in place. A failed or partial setup remains available for inspection.

## The frontend shows unavailable

Run validation with its workspace-local npm cache if the user-level cache is read-only. A clean install needs the npm registry or the locked tarballs already in that cache. The lockfile stays pinned during validation.

Check both health endpoints from PowerShell. The first tests FastAPI and its C++ child. The second calls the same check through Vite.

```powershell
Invoke-RestMethod 'http://127.0.0.1:8000/api/engine/status'
Invoke-RestMethod 'http://127.0.0.1:5173/api/engine/status'
```

A restart needs the same `-Demo` argument for both startup scripts. The frontend uses the tracked backend port. Use the saved backend and frontend logs when a readiness check fails.

## A port is occupied

```powershell
netstat -ano -p TCP | Select-String ':8000 .*LISTENING|:5173 .*LISTENING'
& .\scripts\demo\00-check-environment.ps1 -Port 18015
& .\scripts\demo\01-start-backend.ps1 -Demo $demo -Port 18015
& .\scripts\demo\02-start-frontend.ps1 -Demo $demo
```

For a named demo already using 8000, use the tracked stop script first. The alternate API is [127.0.0.1:18015/docs](http://127.0.0.1:18015/docs). The Vite dashboard still uses port 5173. Leave unrelated listeners alone. If 5173 is unavailable, use API or PowerShell evidence until its owner releases it.

## A rebuild is too fast, paused or locked

Use `05-failure-rebuild-demo.ps1 -ConfirmFailure -PauseForInspection` on a fresh demo. Its two-batch checkpoint limit reliably exposes a durable pause, even with a fast SSD. At the prompt, inspect the pending job and selected scheduler, then press Enter to resume. If the job already exists, inspect **Rebuild Monitor** or `GET /api/rebuilds/{job_id}`. A PAUSED or INTERRUPTED job can be resumed there after the owner releases its lease. A RUNNING job has an active owner. Use the saved browser checkpoint screen if live timing is awkward.

## A sample upload fails

Check that `lun6` is file-managed and the catalog is ready. `03-create-demo-storage.ps1` creates the deterministic samples. Try the text sample first. `07-file-management-demo.ps1` checks if the sample is already imported, then uses the multipart API and validates the download independently. Inspect backend logs if the response reports capacity, health or integrity errors.

## The benchmark or browser is unavailable

Use `09-research-results-demo.ps1 -SkipFigureRegeneration` to validate and list the archived figures. An optional live trial creates disposable member files and is unnecessary during the live presentation. Without Edge, demonstrate with the PowerShell scripts, actual OpenAPI responses, and existing screenshots. [Command reference](DEMO_COMMANDS.md) · [Measured figures](FINAL_RESEARCH_RESULTS.md)

