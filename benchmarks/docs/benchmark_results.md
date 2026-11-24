# Step 13 measured benchmark results

The final audit reproduced the archived aggregates and the exact request accounting. [Combined research evidence](../../docs/FINAL_RESEARCH_RESULTS.md)

The Release matrix ran on November 24, 2026 using Windows 10.0.26300 and an AMD Ryzen 7 7735HS, identified from the local system registry. The run manifests record the processor identifier and leave the host drive model and Git revision unavailable. The matrix records the toolchain and Python version with each run. Virtual disk files share the laptop SSD and the Windows host cache.

All 24 configurations completed five repetitions. The analyzer validated 120 repetitions against 27,840 raw request rows and excluded zero. The main comparison used 256 measured and 32 warm-up requests per repetition, a common 6 MiB footprint, equal 32 MiB raw capacity, queue depth 1 and seed 12345. The bounded request count limited measured sequential logical traffic to 2.5 GiB before warm-up or physical amplification. A separate RAID 5 calibration completed five repetitions of 1,000 random writes. [Matrix manifest](../results/matrix-20261124T123744Z-9e803d3e/matrix_manifest.json)

## RAID comparison

Results are means across five independently initialized repetitions. The IOPS standard deviation is the sample SD. The p50, p95, and p99 columns average each repetition's exact percentile.

| Workload | RAID | IOPS | IOPS SD | Logical MiB/s | p50 ms | p95 ms | p99 ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| Random read, 4 KiB | 0 | 262,858.77 | 38,500.69 | 1,026.79 | 0.0035 | 0.0046 | 0.0067 |
| Random read, 4 KiB | 1 | 204,955.15 | 28,997.34 | 800.61 | 0.0041 | 0.0099 | 0.0176 |
| Random read, 4 KiB | 5 | 265,800.31 | 48,196.93 | 1,038.28 | 0.0034 | 0.0053 | 0.0073 |
| Random read, 4 KiB | 6 | 217,348.69 | 30,851.92 | 849.02 | 0.0038 | 0.0083 | 0.0138 |
| Random write, 4 KiB | 0 | 3,445.90 | 77.35 | 13.46 | 0.2778 | 0.3462 | 0.4307 |
| Random write, 4 KiB | 1 | 235.29 | 35.25 | 0.92 | 4.2859 | 5.3662 | 5.8828 |
| Random write, 4 KiB | 5 | 248.81 | 47.22 | 0.97 | 3.9904 | 5.1852 | 7.1621 |
| Random write, 4 KiB | 6 | 259.51 | 10.73 | 1.01 | 3.7726 | 4.5749 | 5.1155 |
| Sequential read, 256 KiB | 0 | 42,085.14 | 2,960.62 | 10,521.29 | 0.0218 | 0.0319 | 0.0462 |
| Sequential read, 256 KiB | 1 | 35,688.93 | 7,631.26 | 8,922.23 | 0.0258 | 0.0451 | 0.0521 |
| Sequential read, 256 KiB | 5 | 39,267.57 | 4,890.34 | 9,816.89 | 0.0232 | 0.0339 | 0.0537 |
| Sequential read, 256 KiB | 6 | 29,627.35 | 1,096.12 | 7,406.84 | 0.0310 | 0.0580 | 0.0715 |
| Sequential write, 256 KiB | 0 | 787.48 | 51.03 | 196.87 | 1.1997 | 1.5184 | 1.6710 |
| Sequential write, 256 KiB | 1 | 277.47 | 8.32 | 69.37 | 3.5126 | 4.2872 | 4.9557 |
| Sequential write, 256 KiB | 5 | 225.91 | 2.11 | 56.48 | 4.3532 | 5.3041 | 5.6921 |
| Sequential write, 256 KiB | 6 | 216.82 | 1.92 | 54.20 | 4.5331 | 5.3047 | 5.7403 |
| Mixed read/write, 4 KiB | 0 | 10,938.78 | 672.43 | 42.73 | 0.0078 | 0.3068 | 0.3588 |
| Mixed read/write, 4 KiB | 1 | 1,006.95 | 47.55 | 3.93 | 0.0102 | 3.4994 | 4.1979 |
| Mixed read/write, 4 KiB | 5 | 1,001.07 | 39.70 | 3.91 | 0.0086 | 3.5072 | 3.9770 |
| Mixed read/write, 4 KiB | 6 | 933.62 | 16.58 | 3.65 | 0.0096 | 3.7948 | 4.2231 |

The sequential-read rates reflect a prepared, cache-enabled 6 MiB working set. The random-write results show variation among the redundant arrays. RAID 6 averaged more random-write IOPS than RAID 1 in this run even though RAID 6 issued more physical calls per logical write. RAID 1 random-write IOPS had a sample SD of 35.25, while RAID 6 had a sample SD of 10.73. Request timing includes the existing flush and journal completion behavior. These measurements do not isolate how much time each stage contributed.

## Physical amplification

For a single-block random write, the measured physical read and write blocks per logical write block were:

- RAID 0 recorded **0 reads and 1 write**.
- RAID 1 recorded **0 reads and 2 writes**.
- RAID 5 recorded **2 reads and 2 writes**.
- RAID 6 recorded **3 reads and 3 writes**.

For 256 KiB sequential writes, RAID 6 requests covered two complete 32-block logical stripe rows and averaged **0 physical read blocks and 2 physical write blocks** per logical write block. RAID 5 used 48-block logical rows and averaged **0.998 reads and 1.666 writes**. The 64-block requests can combine complete and partial RAID 5 rows.

## Thin provisioning

The matched RAID 5 arms issued 64 measured 4 KiB writes after eight warm-up writes per repetition. First allocation averaged **42.16 IOPS**, **25.87 ms p95** and **28.26 ms p99**. Allocated overwrite averaged **63.93 IOPS**, **17.00 ms p95** and **20.50 ms p99**.

First writes allocated 64 new 64 KiB chunks per repetition, increasing pool allocation by **4 MiB**. Overwrites allocated zero chunks. Each first write averaged **32 physical read blocks and 32 physical write blocks** per logical write block. The overwrite averaged **2 and 2**. These results include provisioning and metadata work on the user-visible write path.

## Read-only rebuild baseline

The matched RAID 1 arms issued 160 measured 4 KiB reads per repetition. Healthy reads averaged **150.34 IOPS**, **0.587 MiB/s**, **8.13 ms p95** and **9.05 ms p99**. With a rebuild active, they averaged **59.96 IOPS**, **0.234 MiB/s**, **20.73 ms p95** and **23.31 ms p99**.

All 160 measured requests per concurrent repetition began while the worker was active. Each rebuild completed all **2,048 physical blocks**. Mean worker elapsed time was **3.474 seconds**. The foreground stream was read-only, and its counters excluded the worker's writes. Use the matched baselines for a future rebuild-policy experiment under the same geometry and readiness rules.

## Artifacts

- [Raw CSVs, summary CSVs and per-run manifests](../results/matrix-20261124T123744Z-9e803d3e/)
- [All aggregate statistics](../results/matrix-20261124T123744Z-9e803d3e/verified/benchmark_aggregates.csv) and [compact comparisons](../results/matrix-20261124T123744Z-9e803d3e/verified/benchmark_comparisons.csv)
- [RAID IOPS](../plots/matrix-20261124T123744Z-9e803d3e-verified/raid_iops.png), [logical throughput](../plots/matrix-20261124T123744Z-9e803d3e-verified/raid_throughput.png), [p95 latency](../plots/matrix-20261124T123744Z-9e803d3e-verified/raid_p95.png), and [p99 latency](../plots/matrix-20261124T123744Z-9e803d3e-verified/raid_p99.png)
- [physical write amplification](../plots/matrix-20261124T123744Z-9e803d3e-verified/raid_write_amplification.png) and [thin write latency](../plots/matrix-20261124T123744Z-9e803d3e-verified/thin_write_latency.png)

[Experimental methodology](benchmark_methodology.md) · [Reproduction commands](reproduction_guide.md)
