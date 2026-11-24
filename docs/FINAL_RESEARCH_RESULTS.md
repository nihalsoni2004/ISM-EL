---
noteId: "4dd59600e81011f1a150872f649e1a14"
tags: []

---

# Final research results

**Research question.** Can workload-aware inter-batch pacing improve foreground read responsiveness during RAID reconstruction, and what does it cost in time to verified promotion?

**H1.** Under foreground-read load, adaptive pacing reduces scheduled-arrival-to-completion p95 relative to full-speed rebuilding, with a measurable rebuild-time tradeoff. **H0.** A reproducible p95 improvement does not occur in the matched comparison. The primary result is descriptive.

## Experimental environment and method

The experiments used the Release C++ simulator, MSVC 19.44, Windows 10.0.26300, and an AMD Ryzen 7 7735HS. Virtual members used the same filesystem and host SSD. The host cache remained enabled and the actual drive model is unavailable in the manifests. Each repetition used fresh files. Phase schedules, seeds, configuration, execution order and toolchain are saved with the data.

Step 13 compared RAID 0, 1, 5 and 6 at equal 32 MiB raw capacity with a common 6 MiB footprint, queue depth one and seed 12345. The compared workloads include sequential and random reads and writes, a mixed workload, thin first writes and overwrites, and a separate read-only rebuild baseline. It measures latency inside the C++ operation and reports completed operations/s, logical MiB/s, physical calls and blocks, and physical written blocks per logical write block.

Step 14 used RAID 1 with two 4 MiB members, a same-size spare, a 4 KiB stripe unit, 16 physical blocks per batch, and 4 KiB random foreground reads. Five repetitions per condition used seed 14024 with matched offers/address streams and rotated condition order. The 1.3-second scheduled windows offered 0, 40, 85 or 145 IOPS following the pilot. The degraded-only control has no worker. Full speed adds no intentional delay. Fixed throttle used 12.7 ms between batches. Adaptive used a 0–6 ms delay, in 1 ms steps, from arrival rate, outstanding reads, and observed p95. Both paced policies wait outside the catalog.

Step 14 reports end-to-end foreground latency from scheduled arrival to completion, so late dispatch contributes to the tail. It counts IOPS within the fixed observation window, with active-rebuild exposure also reported. Rebuild duration runs from the durable RUNNING record to completion after reconstruction, verification, and promotion. Per-trial p95/p99 are nearest-rank quantiles. Means, medians and sample SD summarize trial metrics. Percent changes are computed per matched pair before averaging.

## Rechecked evidence

The Step 13 reconciliation agrees with the raw rows, the manifests, per-run summaries, and both archived aggregate sets. There are **27,840** completed measured operations in **120** valid repetitions, including 25,600 main, 640 thin, and 1,600 baseline. There were zero failed operations. There are 3,440 warm-up requests excluded. The separate calibration has 5,000 completed operations. Revalidated metrics include nanosecond request durations, exact percentiles, IOPS, MiB/s, physical request and block counts, and amplification. Thin first allocation averaged **42.16 IOPS** and **25.87 ms p95**. Allocated overwrite averaged **63.93 IOPS** and **17.00 ms p95**. [Step 13 results](../benchmarks/docs/benchmark_results.md)

The Step 14 recheck rederived read p95/p99 and IOPS from the request rows, completion time from final progress events, and percentage changes from the matched trial keys. It validated **75** main trials, **7,040** successful and byte-validated reads, **60** verified healthy rebuilds, and **15** degraded controls. No reads failed or dropped. It also revalidated the 11 pilot, one variable-load and ten sensitivity trials. All **15** loaded adaptive/full-speed pairs lowered p95. [Step 14 report and raw evidence](../benchmarks/docs/rebuild_results.md)

| Foreground load | Adaptive p95 change vs full speed | Paired p95 range | Adaptive p99 change vs full speed | Rebuild-time change vs full speed |
|---|---:|---:|---:|---:|
| Light, 40 IOPS | −26.4% | −52.4% to −9.4% | −24.1% | +8.3% |
| Medium, 85 IOPS | −91.9% | −94.2% to −88.4% | −91.0% | +4.4% |
| Heavy, 145 IOPS | −49.0% | −50.6% to −47.5% | −48.5% | +20.3% |

At heavy load, adaptive averaged 551.8 ms p95 and 2.35 s to verified promotion, while the calibrated fixed throttle averaged 317.5 ms and 3.99 s. The fixed arm reduced p95 more at all tested loads, and adaptive restored redundancy sooner. The 1 ms fixed-delay sensitivity at 85 IOPS still had a 17.8% completion gap, exceeding the 10% match criterion. Equal-duration superiority remains unresolved. In the changing-load trial, adaptive reached its 6 ms bound and later returned to zero when idle.

## Correctness, threats and conclusion

The research runner verifies all foreground bytes, original member digests, expected monotonic checkpoints, the replacement image, final logical contents and parity/mirror consistency. It excludes failed data integrity or performance trials from aggregates and retains raw errors. All main results were validated. The study shows reduced foreground delay through workload-sensitive pacing relative to full speed. It does not establish significance with five repetitions per condition. Shared caching, catalog serialization, Windows timers, one foreground worker, a recoverable RAID 1 read workload and chosen thresholds all limit generalization. RAID 5 and RAID 6 are covered by correctness tests. Independent-device and degraded-write performance remain unmeasured.

## Figures and reproduction

The figure set consists of fourteen MEASURED PNGs. The validation runner checks the archived PNGs and rebuilds figures from the validated inputs in a separate output folder. MODELED profile scenarios appear only in the model workflow.

- **RAID performance** includes [IOPS](../benchmarks/plots/matrix-20261124T123744Z-9e803d3e-verified/raid_iops.png), [throughput](../benchmarks/plots/matrix-20261124T123744Z-9e803d3e-verified/raid_throughput.png), [p95](../benchmarks/plots/matrix-20261124T123744Z-9e803d3e-verified/raid_p95.png), [p99](../benchmarks/plots/matrix-20261124T123744Z-9e803d3e-verified/raid_p99.png), [write amplification](../benchmarks/plots/matrix-20261124T123744Z-9e803d3e-verified/raid_write_amplification.png), and [thin first-write versus overwrite](../benchmarks/plots/matrix-20261124T123744Z-9e803d3e-verified/thin_write_latency.png).
- **Adaptive research** includes [p95](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/foreground-p95.png), [p99](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/foreground-p99.png), [read IOPS](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/foreground-iops.png), [rebuild duration](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/rebuild-time.png), [latency/time tradeoff](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/latency-time-tradeoff.png), [durable progress](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/matched-rebuild-progress.png), [changing-load pacing](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/adaptive-variable-load.png), and [policy comparison](../benchmarks/plots/rebuild-20261124T131615Z-990308f1936c27d6/policy-comparison.png).

[Step 13 reproduction](../benchmarks/docs/reproduction_guide.md) · [Step 14 reproduction](../benchmarks/docs/rebuild_reproduction_guide.md) · [Faculty commands](DEMO_COMMANDS.md)

