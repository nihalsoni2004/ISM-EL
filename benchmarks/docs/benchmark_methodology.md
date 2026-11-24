# Benchmark methodology

The final audit independently recomputes both archived aggregate sets from raw samples. It regenerates figures in a separate validation output folder. [Final research results](../../docs/FINAL_RESEARCH_RESULTS.md)

## Experimental question

Measure the implemented storage stack at queue depth 1, then explain differences using observed logical and physical I/O. File-backed members share a host SSD, filesystem, OS cache and failure domain. The study describes software behavior on that shared host. Independent hardware throughput or availability claims require separate physical devices and a controlled hardware study.

## Design

- **Geometry.** RAID 0, 5 and 6 use four equal members with 16-block stripe units. RAID 1 uses two mirrors with a one-block stripe unit. The comparison fixes raw array capacity at 32 MiB and touches the same 1,536 logical blocks (6 MiB). Members are 8 MiB for four-member arrays and 16 MiB for RAID 1. Usable capacities are 32, 16, 24 and 16 MiB respectively for RAID 0, 1, 5 and 6. The runner also accepts equal usable capacity or explicit per-member capacity.
- **Operations.** Random read and write use 4 KiB; sequential read and write use 256 KiB and wrap at the last valid whole request. Mixed I/O uses 4 KiB. A 70% read selection rounds half up separately for warm-up and measured requests. Ratios from 0% to 100% are configurable.
- **Determinism.** MT19937-64 uses independently derived streams for address selection and mixed ordering. Bounded draws use rejection sampling and shuffle uses Fisher–Yates to avoid library-dependent distributions. Seeds are mixed with the one-based repetition. Byte patterns use a separate deterministic function of seed, logical LBA, and write pattern reference. The same workload, geometry, seed, and repetition produce the same request sequence. Timing varies with host conditions.
- **Isolation.** Each repetition creates fresh images and catalogs in a unique run directory. The runner opens storage with an explicit disposable root. One CLI process holds the benchmark-root lock. A repetition's workspace is reclaimed only after its resolved location and contents have been checked. Result files use exclusive creation and remain in run-specific directories.
- **Cache.** OS caching remains enabled. Dataset preparation initializes the tested range before reads and overwrites. RAID writes use their existing completion and flush rules. Warm-up is untimed. Recreating arrays and metadata gives each repetition a defined logical state while the shared host cache can remain warm.
- **Ordering.** The matrix rotates the first RAID level cyclically across the five workloads and records the order. Repetitions for a configuration are consecutive. Rotation reduces systematic placement effects within the matrix while OS scheduling, background host I/O, and cache behavior can still vary.

## Timing and attribution

The runner uses the Step 12 steady clock. Per-request duration begins immediately before the controller or LUN call and ends when it returns or throws. Normal RAID runs retain an open controller. Thin comparisons use the complete provisioning call, so chunk initialization, RAID writes and durable allocation metadata work are included when performed by that call. Read-only rebuild baselines include opening a fresh RAID session and any wait for the catalog in both arms.

The run interval spans the measured request loop. Buffer preparation, sampled validation and timing bookkeeping inside that loop contribute to its elapsed wall time. Setup, warm-up, final persistence checks, rebuild completion waiting, and result file writes have separate boundaries. IOPS uses completed logical requests divided by that interval. Throughput uses completed logical bytes divided by 1,048,576 and by the same interval.

A thread-local capture reuses VirtualDisk telemetry to attribute physical calls to the current foreground request. Physical request fields count attempted disk calls. Physical block and byte fields count completed transfers. A failed logical call can have completed some of its physical suboperations. Validation I/O and background rebuild I/O are outside foreground attribution. For writes, amplification is completed physical write blocks divided by successful logical write blocks. The analysis also reports physical read blocks attributable to writes per successful logical write block.

## Validation and allocation

FULL checks every timed read and reads back each timed write. SAMPLED checks the first, every configured stride, and the final measured operation. Both modes reopen storage and compare expected bytes across their chosen final persistence scan. Write read-backs sit outside per-request timing and physical capture. They can change the cache state between measured requests. NONE records unchecked data and the analysis excludes those repetitions. Failed I/O, allocation discrepancies and validation mismatches are preserved in the raw or summary output.

Thin first-write and overwrite tests use the same shifted chunk groups, request size, order, and patterns. First-write performs one write per previously unallocated group. The warm-up uses distinct groups from the measured phase. Overwrite preallocates every targeted group before warm-up and measurement. A successful allocation initializes 64 KiB per new chunk. Measured allocation growth excludes warm-up and is checked against the new-chunk count. The total time covers allocation and metadata work together.

## Statistics and files

Every attempted measured request contributes an exact duration in nanoseconds to the repetition's raw CSV. The full vector supplies min, max, mean, and nearest-rank p50/p95/p99 using rank `ceil(p × n / 100)`. The Step 12 rolling telemetry ring is separate from the full benchmark sample vector. The configured request count and CSV limits bound memory and disk use. Failed repetitions retain their diagnostic values and are excluded from comparative aggregates.

The analyzer verifies raw sequences, counts, statuses, bytes, physical counters, rates and percentiles against summaries and manifests. It groups by workload, request size, RAID geometry, footprint, seed, warm-up, validation, cache policy, source, build, and host metadata. Reports give means, medians and sample standard deviations across independent repetitions. Plot error bars show sample standard deviation where multiple repetitions exist. These are descriptive comparisons. Claims of statistical significance require an experimental design and inferential analysis.

Within each run directory:

- `benchmark_runs.csv` contains one row per attempted measured request. Logical LBA and block count use 4 KiB blocks. Duration is in nanoseconds. Completed logical and physical bytes are literal bytes. A blank validation field marks an unsampled request. `pattern_ref` identifies deterministic request data. `rebuild_overlap` marks dispatch while the rebuild worker was active after its first checkpoint.
- `benchmark_results.csv` contains one row per repetition. Rates are requests/s and MiB/s. Latency fields are milliseconds, while elapsed, preparation, warm-up and rebuild time are seconds. Allocation fields are bytes, except `new_chunks`. Blank cells represent unavailable or inapplicable values.
- `manifest.json` contains normalized configuration, the UTC run window, geometry, toolchain, host fields, repetition status and filenames. Unavailable host details use null. Each matrix records experiment order and configuration files.
- `benchmark_aggregates.csv`, `benchmark_comparisons.csv`, and `benchmark_exclusions.csv` come from validated run directories. The analysis accepts only explicitly MEASURED records. Step 12's hypothetical device scenarios remain a separate source.

## Read-only rebuild baseline

Matched healthy and concurrent RAID 1 arms use 4 KiB foreground random reads. The concurrent arm fails a member only after healthy setup and warm-up. It starts the existing rebuild, starts foreground requests after the first durable checkpoint, and records request starts during worker activity and the worker's total elapsed time and final durable blocks separately. Thread-local capture excludes worker writes from foreground totals. Catalog serialization, lock waiting and host I/O affect those observations. The baseline is suitable for comparing future read-only rebuild policies under matching settings.

## Step 14 rebuild comparison

Step 14 uses a separate Release benchmark mode with fixed offered-arrival windows, four rebuild conditions, and actual verified reconstruction. Its CSVs account for scheduled requests, queue wait, catalog contention, and measured worker pacing. See the request-count reconciliation and the design used for the new comparison. [Adaptive rebuild methodology](rebuild_methodology.md)
