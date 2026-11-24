---
noteId: "4cf0f7d0e80f11f1a150872f649e1a14"
tags: []

---

# Faculty demonstration guide

**Software-Defined RAID & Virtual Provisioning Simulator** · Information Storage Management (CS374TFD)

Use the [quick start](QUICK_START.md) to bring up the named disposable catalog. Keep the PowerShell window and [RAID Console](http://127.0.0.1:5173/) visible. On a narrow screen, open the navigation menu before selecting a page and scroll wide tables within their panels. The labels and sample IDs below come from the setup script.

## Quick demo — 5 minutes

1. **Overview**. Confirm **Engine ready**, show the four healthy arrays and the completed rebuild. Use **RAID Arrays > array6**, enter LBA `0` in **RAID block mapping**, choose **Map block**, and repeat for `2`. Point to P and Q moving across disks.
2. **Storage Pools > pool5**. Point to the 12 MiB physical pool. In **Logical Volumes > lun5**, show its 24 MiB logical size and map LBA `16`. The setup steps in Quick Start already produced two allocated chunks.
3. **File Explorer > Upload file**. Choose `lun6` and `faculty note.txt` from the demo's `samples` directory. Submit, open the resulting file, then choose **Verify SHA-256**. If the file was already imported, open its existing record and verify it.
4. Open the [measured p95 figure](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/foreground-p95.png). State that adaptive lowered p95 against full speed on the tested RAID 1 setup, while the longer fixed throttle produced lower p95. Show [the time tradeoff](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/latency-time-tradeoff.png) alongside it.

The file upload is the only mutation in this quick walkthrough. Use the prepared named demo. If live navigation fails, use [the command reference](DEMO_COMMANDS.md) and the archive figures.

## Full demo — 19 minutes

### Introduction and architecture · 3 minutes

Open **Overview**, then **System**. Explain the distinction between the **Backend connected** and **Engine ready** indicators. Follow the [architecture diagram](architecture.md#final-system-diagrams) from React to FastAPI, then the persistent C++ child, and down to file-backed member images and SQLite metadata. These are read-only views. If the dashboard is unavailable, show `GET /api/engine/status` and the diagram.

### Disks, RAID, and parity · 3 minutes

For a fresh catalog, run the two startup scripts and then run `03-create-demo-storage.ps1 -Demo $demo`. Show the reported arrays and actual block size. Open **Virtual Disks** and select `disk61`. Show capacity, block size and array use. Open **RAID Arrays** and compare `array0`, `array1`, `array5` and `array6`. Array details show usable capacity and members. Choose **Verify array** where shown, then map LBAs `0` and `2` on `array6`. Explain striping, mirrors, distributed XOR and the independent Q equation. Setup writes new images and metadata within the named demo. For CLI/API fallback, run `04-raid-demo.ps1 -Demo $demo`.

### Failure, rebuild and durable progress · 3 minutes

Run the following in the named disposable catalog only.

```powershell
& .\scripts\demo\05-failure-rebuild-demo.ps1 -Demo $demo -Policy ADAPTIVE -ConfirmFailure -PauseForInspection
```

Watch HEALTHY → DEGRADED in the terminal and the matching 4 KiB degraded read. At the prompt, open **Hot Spares**, then **Rebuild Monitor > rebuild1**. Show the replacement reservation, **PAUSED**, **32/4096** durable blocks for this fixture, and **Adaptive** under **Rebuild scheduling**. Press Enter in PowerShell to resume. The job advances and ends **COMPLETED**. In **RAID Arrays > array1**, show **HEALTHY** and the promoted `disk13`.

The script persists the failure and reconstructs the spare. If presenting GUI mutations from an untouched fixture instead, choose **RAID Arrays > array1 > Member to fail > disk11 > Simulate failure > Confirm failure**. Register `disk13` under **Hot Spares > Register spare**. Then use **Rebuild Monitor > Prepare rebuild**, select `array1`, `disk11`, `disk13` and **Adaptive**. Choose **Start rebuild**, enter `2` in **Pause after checkpoint batches (optional)**, confirm, then **Resume rebuild**. Use the scripted route for the verified degraded read and final evidence file. If progress is too fast live, use the checkpoint limit or the recorded paused screen.

### Thin provisioning · 2 minutes

Before file imports, open **Storage Pools > pool5** and **Logical Volumes > lun5**. Run `06-thin-provisioning-demo.ps1 -Demo $demo`. It checks unallocated zeros and writes LBAs `0`, `1` and `16`. Return to **Logical Volumes > lun5 > Logical block mapping** and choose **Map block** at those LBAs. Explain why the first two addresses share a chunk, why LBA 16 uses a second chunk, and why 24 MiB of address space can sit on a 12 MiB pool. Actual allocation changes 0 → 64 → 64 → 128 KiB. The writes are confined to demo blocks. If the GUI is unavailable, keep the script's mapping table on screen.

### File storage · 2 minutes

Use **File Explorer > Upload file**, select `lun6` and `faculty note.txt`, then submit. Open the uploaded file and show its file ID, extent, byte size and SHA-256. Choose **Verify SHA-256** and **Download**. Run `07-file-management-demo.ps1 -Demo $demo` to import remaining samples and independently compare all three exports. The 73,729-byte binary example exceeds one allocation chunk. The demo writes import data and separate export files. If browser upload fails, the same script uses multipart HTTP with the generated files.

### Telemetry · 2 minutes

Run `08-telemetry-demo.ps1 -Demo $demo` and open **Telemetry**. In measured operations, show logical and physical counts, sample p95/p99, and the rebuild job selected in rebuild telemetry. Under **Device performance model**, select an HDD profile and **Run model scenario**. Repeat for NVMe. The script also checks SATA. Explain that the model predicts service/queue behavior for supplied requests and the measured panel summarizes real C++ operations. The script adds bounded reads. Reset only when you choose **Reset measurements > Confirm reset** or supply `-ConfirmReset`.

### Research contribution and conclusion · 4 minutes

Run `09-research-results-demo.ps1 -SkipFigureRegeneration` and open the archived p95, rebuild time, and changing-load figures from [Final research results](FINAL_RESEARCH_RESULTS.md). Show all three loaded conditions and the degraded-only reference. Compare p95 with completion time and explain the capped controller. Mention the five repetitions per cell, shared SSD, cache, read-only workload and unresolved equal-duration comparison. End with `10-verify-demo.ps1 -Demo $demo`, then stop the tracked services when finished. Research presentation reads the archive. The optional short live run uses fresh disposable images and separate outputs.

[Troubleshooting and fallbacks](TROUBLESHOOTING.md) · [Speaking script](DEMO_SPEAKING_SCRIPT.md) · [Viva questions](VIVA_QA.md)

## Rehearsal evidence

The faculty catalog was started on port 8000, stopped, and reopened on port 18015 behind the same Vite URL. The second engine process reverified four healthy arrays, two pools, three LUNs, the promoted adaptive rebuild and three exact file exports. A separate fixed-throttle run waited at the interactive checkpoint and resumed after Enter. [Restart verification](../build/final-validation/faculty-walkthrough.json) · [Adaptive rebuild evidence](<../build/final-demo/Faculty Step15/rebuild-evidence.json>) · [Fixed-throttle pause](<../build/final-demo/Faculty Pause/interactive-pause.json>)

Edge exercised the real API, file transfer, measured telemetry, modeled scenarios, mobile navigation, and disconnect recovery at 1440, 1024 and 390 px. [Screenshot review](../build/final-validation/browser-review/browser-demo-SHdloQ/screenshot-review.png) · [Browser verification](../build/final-validation/browser-review/browser-demo-SHdloQ/verification.json)

