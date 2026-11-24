---
noteId: "875e3e50e80f11f1a150872f649e1a14"
tags: []

---

# Faculty speaking script

**Opening.** “The project models how several disks can provide one logical volume. The goals are flexible capacity, continued access after a recoverable failure, and evidence about the latency cost of restoring redundancy. Each member is a file on this Windows host, and the engine treats it as an indexed sequence of 4 KiB blocks.”

**Architecture.** “The React console displays state and submits operations. FastAPI validates those operations and manages a persistent C++ process using newline-delimited JSON over standard input and output. C++ owns RAID mapping, allocation, and recovery. SQLite tracks file metadata and extent records while the simulator stores the file bytes in logical volumes.”

**RAID.** “RAID 0 stripes data to use all member capacity. Losing a member loses part of the logical address space. RAID 1 writes copies and reads a surviving mirror. RAID 5 rotates XOR parity and reconstructs one missing member. RAID 6 rotates two parity values. P is XOR and Q is a weighted equation over GF(2^8). With independent equations, the engine can reconstruct two missing members. Here is how P and Q change positions between these two actual mappings.”

**Recovery.** “I’m injecting one persistent member failure into the disposable mirror. The array remains readable, and the script checks that a previously written block matches byte for byte. A hot spare gives the job a known replacement. The worker reconstructs, flushes, reads back, and saves a durable checkpoint. This job paused at 32 blocks. On resume it verifies the replacement and promotes it only after the final checks. The live job reaches 100% and the array returns to HEALTHY.”

**Thin provisioning.** “This LUN advertises 24 MiB on a 12 MiB RAID pool. An unallocated read returns zeros. A first write allocates a 64 KiB chunk. Writing to a second address in the same chunk leaves physical allocation unchanged. A write at the next chunk allocates another 64 KiB. These counters describe RAID address space.”

**File integrity.** “The file browser records the names, sizes, extent ranges, states, and SHA-256 digests. The large sample crosses a chunk boundary. The verification computes SHA-256 from saved bytes. Separately, the PowerShell check compares the original and exported bytes and independently hashes both.”

**Telemetry.** “MEASURED shows actual timings and operation counts from C++. Hardware profiles are marked MODELED. They are deterministic predictions from hypothetical seek/service and queue assumptions for the supplied workload.”

**Research.** “Full speed adds zero wait. Fixed throttle waits the same amount between batches. Adaptive uses foreground arrivals, outstanding demand, and observed p95 to choose a bounded delay. The four-condition Release study used matched read schedules and a degraded-only reference. Adaptive improved p95 versus full speed in all 15 loaded pairs. The calibrated fixed throttle improved foreground p95 further while taking substantially longer to restore redundancy. The short fixed-delay sensitivity did not achieve comparable durations, so the equal-duration question is open. The changing-load trace shows adaptive reaching its cap and returning to zero after idle.”

**Close.** “The engine, browser, persistent catalog, and reproducible measurements are all part of the demonstration. The results describe this shared host SSD, Windows cache and read-only RAID 1 study. Five repetitions per cell give descriptive evidence. Independent-drive performance and degraded writes are separate work.”

