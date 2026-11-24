# Storage telemetry methodology

The faculty telemetry script generates twelve validated reads and displays MEASURED results and MODELED scenarios. Its optional reset starts a new observation generation. Durable storage state stays available throughout. [Telemetry commands](DEMO_COMMANDS.md#m-telemetry-and-device-scenarios)

## What the measurements describe

**MEASURED** describes elapsed execution time and request counts at C++ storage boundaries. Each virtual disk is a file on the same host volume. File-backed member calls share the host SSD, driver, filesystem, page cache, and failure domain. Cache warmth, buffering, flush behavior, concurrent system load, and the added instrumentation can all affect outcomes. Logical and physical refer to simulator layers. Independent hardware-disk latency, parallel device throughput, and hardware fault tolerance require separate physical devices and a controlled hardware experiment.

**MODELED** describes a deterministic calculation with hypothetical device parameters and explicit arrivals. Treat it as a separate experiment. [Device performance model](performance_model.md)

## Timing boundaries

- **VIRTUAL_DISK_READ** and **VIRTUAL_DISK_WRITE** start before block validation, seeking, and the C runtime read or write. They end when the call returns or throws. A separate flush is outside this boundary. Runtime buffering or seek-triggered work can affect elapsed time.
- **RAID_LOGICAL_READ** and **RAID_LOGICAL_WRITE** cover the controller request, including validation, mapping, parity or reconstruction, member I/O, and any member flush or journal completion required by the controller. Pool calls contribute at this same boundary. A LUN call can cause several logical controller calls, and an unallocated LUN read can return without reaching RAID. A file import or export can issue many controller calls. HTTP, IPC transport, request queueing, and React rendering are outside these timings.
- **REBUILD_READ** and **REBUILD_WRITE** are VirtualDisk requests attributed to a rebuild worker. Source reads, spare read-back, and final verification reads are included. These events are distinct from the ordinary VirtualDisk categories.
- **REBUILD_BATCH** starts after source validation and before reconstruction reads. It ends after the spare write, flush, read-back, and durable checkpoint publication, or at exception unwinding. Catalog acquisition, stage setup, and pauses between batches are outside this boundary.
- **REBUILD_VERIFY** measures the final verification pass one batch at a time. The final verification batch also includes promotion, closing the spare, and saving the completed job record. Verification blocks are inspected blocks. Reconstruction blocks are recorded separately.

All elapsed times use `std::chrono::steady_clock` with integer nanoseconds. Public latency summaries are in milliseconds. One millisecond is one million nanoseconds. An event’s `end_offset_ns` uses the same arbitrary process-local monotonic origin as the observation offsets. A job’s `last_checkpoint_at` is the persisted Unix timestamp in seconds.

## Counting and classification

One finished controller or disk call counts as one attempted request, including rejected or failed calls. Successful operations contribute completed blocks and bytes. A failed call contributes zero completed bytes at its own boundary, including when the underlying runtime may have transferred a prefix. A successful nested disk call still counts if a later RAID flush or logical operation fails. Requested sizes and counts describe what was attempted, even for invalid ranges. Counters saturate at the unsigned 64-bit limit.

Reports keep RAID requests, ordinary physical requests, rebuild physical requests, and rebuild batches separate. To obtain total measured physical reads, combine **VIRTUAL_DISK_READ** and **REBUILD_READ**. Apply the same rule to writes. Grouping by origin adds a breakdown of each category, so adding those breakdowns again would count events twice. The existing per-controller RAID 5 and RAID 6 counters retain their original meaning.

A thread-local scope labels requests **FOREGROUND**, **REBUILD**, or **INTERNAL**. Controller transfers start a foreground context unless an explicit context is in effect. Explicit user disk CLI reads and writes are foreground. Array and file verification and unscoped image maintenance are internal. Rebuild worker reconstruction and final verification carry both job and array IDs. Initial spare blank checks are internal. IDs are limited to safe identifiers. Events carry no paths, payload contents, or raw exception messages.

## Retention, percentiles, and rates

Step 13 benchmarks capture per-request physical counters from these same events on the foreground thread. Exact benchmark percentiles use the full run-scoped vector. [Benchmark timing and sample policy](../benchmarks/docs/benchmark_methodology.md)

The collector retains the most recent 10,000 elapsed samples per operation category, including failures. Counters cover the entire observation generation. Recent event details retain up to 256 completions across categories, with publication order defined by collector lock acquisition. A separate per-job cache retains 32 jobs ordered by most recent activity, with up to 256 samples per rebuild category for each retained job.

To calculate the minimum, maximum, mean, and percentiles, take the retained sample window, sort the values, and use the nearest rank at `ceil(p × n / 100)` for p50, p95, and p99. Each returned summary includes total count, retained sample count, and whether the retained samples cover the entire observation or a recent window. Empty latency fields are null. A one-sample percentile describes that sample alone. [NIST reference on ranks and percentiles](https://www.itl.nist.gov/div898/handbook/prc/section2/prc262.htm)

Foreground logical IOPS equals completed foreground RAID reads and writes divided by observation seconds. Throughput equals completed foreground logical bytes divided by 1,048,576 and by the same observation seconds. The observation begins at NDJSON process startup or at the most recent explicit reset, and includes idle time. Rates are null until the window reaches one second and contains at least one finished foreground logical attempt. These are observation averages. They are unsuitable for treating a few ad hoc requests as a sustained throughput measurement.

To collect samples for analysis, call `GET /api/telemetry?include_samples=true`. Use `event_limit` to request up to 256 recent records. The dashboard’s default snapshot request uses summary statistics. Per-job telemetry returns retained samples and authoritative durable job status. The collector and the durable job are sampled separately. A worker can advance between the two reads.

## Reset, concurrency, and overhead

Adaptive rebuild uses a separate bounded foreground observation window so an ordinary telemetry reset does not change its input. The meter tracks storage-read arrivals, outstanding operations, successful completion timestamps and bytes, and a nearest-rank p95 once at least four successful samples are present. Nested LUN and RAID reads count once. Internal verification, rebuild I/O, telemetry, and management polling do not contribute. The observation scope is the engine process. The study instruments actual dispatch before catalog entry while normal RAID calls enter the meter at the instrumented LUN or RAID method. Work waiting outside those boundaries is not observable to the live controller. [Controller signals and timing](../benchmarks/docs/rebuild_methodology.md)

Telemetry is process-local and starts a new history when the engine restarts. Reset clears only counters, samples, recent events, and per-job telemetry. It increments the observation generation. A scope begun in an earlier generation cannot publish into the new generation, even if it finishes after reset. Nested I/O started after the boundary can enter the new generation independently. Completed rebuild blocks, catalog data, and file bytes retain their existing persistence semantics.

Per-event reporting uses a short mutex-protected update to preallocated buffers. The collector holds no telemetry lock over I/O. Snapshots copy buffers under the collector lock and sort after releasing it. Reset uses the collector lock without performing storage I/O. Clock reads, event copies, synchronization, and serialization still incur overhead. Future experiments should hold telemetry settings, cache conditions, and observed windows consistent across trials.
