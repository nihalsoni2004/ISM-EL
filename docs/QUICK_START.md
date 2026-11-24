---
noteId: "4cf0f7d1e80f11f1a150872f649e1a14"
tags: []

---

# Faculty quick start

Open VS Code and select **File > Open Folder**. Choose the software-defined-raid directory, then open **Terminal > New Terminal** and use Windows PowerShell. Keep the working directory at the project root. The process-scoped execution policy below applies to this terminal.

```powershell
Set-Location -LiteralPath 'G:\7th_semProjects\ISM\software-defined-raid'
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force
$demo = 'Faculty'
& .\scripts\demo\00-check-environment.ps1
& .\scripts\demo\01-start-backend.ps1 -Demo $demo
& .\scripts\demo\02-start-frontend.ps1 -Demo $demo
& .\scripts\demo\03-create-demo-storage.ps1 -Demo $demo
```

Open [RAID Console](http://127.0.0.1:5173/) and check **Backend connected** and **Engine ready**. Open [API documentation](http://127.0.0.1:8000/docs) if you need to inspect the live endpoints. The named catalog lives in `build/final-demo/Faculty/storage`. Use a fresh `$demo` name for a fresh run.

Prepare the data shown in the five-minute walkthrough. The failure command changes only the named demo and requires your explicit switch.

```powershell
& .\scripts\demo\05-failure-rebuild-demo.ps1 -Demo $demo -Policy ADAPTIVE -ConfirmFailure
& .\scripts\demo\06-thin-provisioning-demo.ps1 -Demo $demo
& .\scripts\demo\09-research-results-demo.ps1 -SkipFigureRegeneration
```

Follow the [five-minute walkthrough](FACULTY_DEMO_GUIDE.md#quick-demo--5-minutes). It includes the first file upload. Afterward, finish the sample exports, verify the catalog, and stop only the tracked servers.

```powershell
& .\scripts\demo\07-file-management-demo.ps1 -Demo $demo
& .\scripts\demo\10-verify-demo.ps1 -Demo $demo
& .\scripts\demo\stop-demo.ps1 -Demo $demo
```

[Windows setup](setup.md) · [Full command reference](DEMO_COMMANDS.md) · [Troubleshooting](TROUBLESHOOTING.md)

