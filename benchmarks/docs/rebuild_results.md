# Measured RAID rebuild scheduling results

The final research audit rederived all 15 loaded p95 pairs from request and checkpoint traces. [Combined research evidence](../../docs/FINAL_RESEARCH_RESULTS.md)

Adaptive pacing reduced foreground p95 against full speed in all five repetitions at each loaded setting in this file-backed RAID 1 study. Fixed throttle produced lower p95 than adaptive at each loaded setting and took substantially longer to finish rebuilding. The tradeoff depends on both the latency objective and the time spent with reduced redundancy.

## Primary comparison

The Release run validated 75 trials, including 60 rebuilds and 15 degraded controls, and all 7,040 scheduled requests completed with correct bytes. Of these, 5,708 completed inside the fixed 1.3 s windows and 4,972 were serviced on degraded arrays with active rebuild workers. All rebuilt arrays finished HEALTHY, all controls stayed DEGRADED, and all original member image hashes matched. Error records and trial diagnostics remain in the run. [Main manifest](../results/rebuild-20261124T131615Z-990308f1936c27d6/manifest.json)

Values below are means of five matched trial metrics. Rebuild time includes durable promotion. A dash indicates no read latency sample or no rebuild. IOPS counts successful completions within the fixed window. p95 and p99 measure scheduled arrival to completion, including late dispatch and the time entering the storage catalog.

| Load (offered IOPS) | Policy | p95 (ms) | p99 (ms) | Read IOPS | Rebuild (s) | Final RAID |
|---|---|---:|---:|---:|---:|---|
| Idle (0) | Full-speed | — | — | 0.0 | 1.70 | HEALTHY |
| Idle (0) | Fixed throttle | — | — | 0.0 | 3.97 | HEALTHY |
| Idle (0) | Adaptive | — | — | 0.0 | 1.86 | HEALTHY |
| Light (40) | Degraded only | 22.1 | 23.1 | 40.0 | — | DEGRADED |
| Light (40) | Full-speed | 38.9 | 40.7 | 39.7 | 1.83 | HEALTHY |
| Light (40) | Fixed throttle | 23.7 | 26.9 | 40.0 | 4.02 | HEALTHY |
| Light (40) | Adaptive | 27.2 | 29.9 | 40.0 | 1.94 | HEALTHY |
| Medium (85) | Degraded only | 21.2 | 22.3 | 84.6 | — | DEGRADED |
| Medium (85) | Full-speed | 452.4 | 452.8 | 56.0 | 1.88 | HEALTHY |
| Medium (85) | Fixed throttle | 21.6 | 22.6 | 84.2 | 4.02 | HEALTHY |
| Medium (85) | Adaptive | 37.0 | 40.9 | 84.6 | 1.96 | HEALTHY |
| Heavy (145) | Degraded only | 25.1 | 27.9 | 143.5 | — | DEGRADED |
| Heavy (145) | Full-speed | 1,081.8 | 1,085.4 | 56.5 | 1.95 | HEALTHY |
| Heavy (145) | Fixed throttle | 317.5 | 328.7 | 115.2 | 3.99 | HEALTHY |
| Heavy (145) | Adaptive | 551.8 | 558.5 | 93.8 | 2.35 | HEALTHY |

### Matched tradeoff

Each percentage is the mean of the five paired relative changes. Negative latency means lower latency. Positive rebuild time means a longer wait for verified promotion. The ranges describe the five paired p95 changes, and the aggregate CSV gives medians and sample SD for trial metrics. [Paired trial data](../results/rebuild-20261124T131615Z-990308f1936c27d6/paired_tradeoffs.csv) · [Aggregate data](../results/rebuild-20261124T131615Z-990308f1936c27d6/policy_aggregates.csv)

| Load | Adaptive p95 vs full | Paired p95 range vs full | Adaptive p99 vs full | Adaptive rebuild time vs full | Adaptive p95 vs fixed | Adaptive rebuild time vs fixed |
|---|---:|---:|---:|---:|---:|---:|
| Light | −26.4% | −52.4% to −9.4% | −24.1% | +8.3% | +15.4% | −51.7% |
| Medium | −91.9% | −94.2% to −88.4% | −91.0% | +4.4% | +71.8% | −51.2% |
| Heavy | −49.0% | −50.6% to −47.5% | −48.5% | +20.3% | +74.8% | −41.1% |

The active-only p95 comparison gives the same direction. At the heavy offer it averaged 1,035 ms for full speed and 552 ms for adaptive, a mean paired change of −46.7%. Full speed served about 127 of 189 offered reads per heavy repetition while the array was still degraded and the worker active. Both paced policies served all 189 in that interval. Heavy adaptive completed 93.8 reads/s within the fixed window, compared with 56.5 for full speed and 115.2 for fixed. Its logical throughput was 0.367 MiB/s, and rebuild throughput was 1.70 MiB/s, between full speed at 2.05 MiB/s and fixed at 1.00 MiB/s.

Full-speed heavy rebuilds held the catalog for 65.9% of elapsed time, adaptive held it for 44.8%, and fixed for 25.5%. Mean reconstruction-batch catalog holds were 13.1, 9.9, and 9.5 ms, respectively. Reads and rebuild jobs overlapped in elapsed time, with real reads serviced between catalog acquisitions. The dominant measured delay under heavy full speed was dispatch backlog, with a mean within-trial queue p95 of 1,075 ms. The corresponding C++ operation p95 after entering the array averaged 0.033 ms. The measurements reveal contention before read service, with the underlying storage and filesystem activity sharing one host SSD.

Adaptive control consumed about 0.13–0.23 ms of measured decision time per main rebuild, roughly 0.01% of elapsed time. This is observation and decision work, separate from telemetry hooks and intentional waits. The 12.7 ms fixed policy achieved about 43% of full-speed idle rebuild throughput in the main experiment. Adaptive used zero delay in every idle trial. Its paired median idle completion change was +0.36%, while its mean was +9.8% after a single +47.8% pair.

## Changing load

In the separate 8 MiB run, adaptive started at zero delay and entered HIGH at 367 ms, shortly after light arrivals began. It reached the 6 ms cap at 800 ms, remained there through heavy traffic and the subsequent light phase, and returned to zero at 2,201 ms once the 200 ms idle criterion was satisfied. The trace recorded two pressure-state changes across 42 updates. That run illustrates both idle recovery and a limitation of the 8 ms target. Even light traffic could hold the controller in HIGH, and the heavy phase left no further headroom. All 133 reads passed validation, and reconstruction made steady checkpoint progress and finished HEALTHY in 3.58 s. This one stimulus shows no repeated state switching and provides no general stability guarantee. [Variable-load manifest](../results/rebuild-20261124T131928Z-659471dc4eeb60ca/manifest.json) · [Controller plot](../plots/rebuild-20261124T131615Z-990308f1936c27d6/adaptive-variable-load.png)

## Fixed-delay sensitivity at 85 offered IOPS

The secondary comparison tested one 1 ms fixed-delay candidate after the primary matrix. It produced a mean p95 of 29.6 ms and completed in 2.43 s. Adaptive averaged 38.3 ms p95 and 2.00 s, and its p95 was lower in three of the five pairs. Adaptive finished 17.8% sooner on a ratio of mean durations. That gap exceeded the predeclared 10% time-match criterion, so equal-duration performance remains unresolved. All ten trials passed validation. [Sensitivity manifest](../results/rebuild-20261124T132347Z-e9f54ffe68e6f536/manifest.json) · [Sensitivity pairs](../results/rebuild-20261124T132347Z-e9f54ffe68e6f536/paired_tradeoffs.csv)

## Interpretation and limits

The file-backed experiment supports H1 descriptively against the full-speed policy under the tested read loads. Each loaded cell has five paired trials. The analysis claims no statistical significance. Host-cache behavior, Windows timing and catalog synchronization, limited sample size, one foreground worker, and the selected pressure thresholds constrain generalization. Foreground traffic was read-only with a fixed offered schedule. Results apply to this host and RAID 1 geometry. RAID 5 and RAID 6 are covered by correctness tests.

## Artifacts

- **Preservation audit.** All 412 files in the preflight baseline matched after the experiments and regressions. The audit covered 43 files in the original storage root, 251 files across ten earlier demo roots, and 118 earlier benchmark outputs and plots. [SHA-256 audit](../results/rebuild-20261124T131615Z-990308f1936c27d6/preservation_sha256.csv)
- **Regression verification.** CTest passed 94 Debug and 94 Release cases. Backend passed 31, analysis passed 20, and frontend passed 22. The production build and browser smoke passed.
- **Main data** includes [foreground requests](../results/rebuild-20261124T131615Z-990308f1936c27d6/foreground_requests.csv), [durable progress](../results/rebuild-20261124T131615Z-990308f1936c27d6/rebuild_progress.csv), [decisions](../results/rebuild-20261124T131615Z-990308f1936c27d6/scheduler_decisions.csv), [policy results](../results/rebuild-20261124T131615Z-990308f1936c27d6/policy_results.csv), [aggregate statistics](../results/rebuild-20261124T131615Z-990308f1936c27d6/policy_aggregates.csv), [paired changes](../results/rebuild-20261124T131615Z-990308f1936c27d6/paired_tradeoffs.csv), [preservation hashes](../results/rebuild-20261124T131615Z-990308f1936c27d6/preservation_sha256.csv), and [exclusions](../results/rebuild-20261124T131615Z-990308f1936c27d6/analysis_exclusions.csv).
- **Figures** include [foreground p95](../plots/rebuild-20261124T131615Z-990308f1936c27d6/foreground-p95.png), [foreground p99](../plots/rebuild-20261124T131615Z-990308f1936c27d6/foreground-p99.png), [foreground IOPS](../plots/rebuild-20261124T131615Z-990308f1936c27d6/foreground-iops.png), [rebuild time](../plots/rebuild-20261124T131615Z-990308f1936c27d6/rebuild-time.png), [latency–time tradeoff](../plots/rebuild-20261124T131615Z-990308f1936c27d6/latency-time-tradeoff.png), [matched progress](../plots/rebuild-20261124T131615Z-990308f1936c27d6/matched-rebuild-progress.png), [adaptive trace](../plots/rebuild-20261124T131615Z-990308f1936c27d6/adaptive-variable-load.png), and [comparison](../plots/rebuild-20261124T131615Z-990308f1936c27d6/policy-comparison.png).
- **Companion runs** include [pilot manifest](../results/rebuild-20261124T131350Z-f13a023b5045cdac/manifest.json), [pilot per-trial results](../results/rebuild-20261124T131350Z-f13a023b5045cdac/policy_results.csv), [changing-load manifest](../results/rebuild-20261124T131928Z-659471dc4eeb60ca/manifest.json), and [changing-load decisions](../results/rebuild-20261124T131928Z-659471dc4eeb60ca/scheduler_decisions.csv).
- [Methodology and Step 13 reconciliation](rebuild_methodology.md) · [Reproduction commands](rebuild_reproduction_guide.md)

## Implementation inventory

- **New C++ files** include `include/recovery/scheduling/scheduler.h`, `src/recovery/scheduling/scheduler.cpp`, `src/recovery/scheduling/json.h`, `include/benchmark/rebuild_study.h`, and `src/benchmark/rebuild_study.cpp`.
- **Changed C++ files** include `CMakeLists.txt`, `include/recovery/rebuild_job.h`, `include/recovery/rebuild_manager.h`, `src/recovery/rebuild_job.cpp`, `src/recovery/rebuild_manager.cpp`, `src/recovery/rebuild_worker.cpp`, `include/telemetry/telemetry.h`, `src/telemetry/telemetry.cpp`, `src/raid/raid_controller.cpp`, `src/provisioning/provisioning_manager.cpp`, `src/ipc/operations.h`, `src/ipc/operations.cpp`, `src/benchmark/benchmark_writer.cpp`, `src/benchmark_commands.cpp`, and `src/main.cpp`.
- **API and dashboard** changes are in `backend/app/api.py`, `backend/app/schemas.py`, and `frontend/src/pages/Rebuilds.jsx`.
- **Tests** include the new `tests/test_rebuild_scheduler.cpp` and `benchmarks/scripts/test_rebuild_analysis.py`. Updated `tests/CMakeLists.txt`, `backend/tests/test_api.py`, and `frontend/src/tests/dashboard.test.jsx`.
- **Research tooling** adds `benchmarks/scripts/reconcile_step13.py`, `benchmarks/scripts/analyze_rebuild.py`, `benchmarks/scripts/plot_rebuild.py`, and the seven `benchmarks/configs/rebuild_*.json` configurations.
- **Documentation** adds `benchmarks/docs/rebuild_methodology.md`, this report, and `benchmarks/docs/rebuild_reproduction_guide.md`. Updated `README.md`, `docs/architecture.md`, `docs/development_roadmap.md`, `docs/setup.md`, `docs/api.md`, `docs/dashboard.md`, `docs/telemetry_methodology.md`, and `benchmarks/docs/benchmark_methodology.md`.

