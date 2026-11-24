# Reproduce the benchmark suite

Work from the project root in PowerShell. Build the Release configuration for measurements and use the project-local Python 3.11 environment for analysis.

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-release --parallel 4
.\.venv\Scripts\python.exe -m pip install -r benchmarks/requirements.txt
```

## Inspect a single run

```powershell
$simulator = ".\build\msvc-debug\Release\raid_simulator.exe"
& $simulator benchmark list-presets
& $simulator benchmark validate --config benchmarks/configs/raid5_random_write.json
& $simulator benchmark dry-run --config benchmarks/configs/raid5_random_write.json --output-dir raid5-trial
& $simulator benchmark run --config benchmarks/configs/raid5_random_write.json --output-dir raid5-trial
```

Use the returned run ID with `benchmark info <run-id> --output-dir raid5-trial`. The output group is one safe name under `benchmarks/results`. The dry run reports nominal image space including any spare, logical traffic, queue depth, operation counts and the CSV budget. The CLI uses an exclusive benchmark lock and one fresh repetition workspace at a time. It checks free space before creating images and uses a cooperative deadline between storage calls and rebuild batches. A synchronous host I/O call can overrun the deadline before control returns.

## Run the rotated matrix

```powershell
.\.venv\Scripts\python.exe benchmarks/scripts/run_matrix.py --operations 256 --warmup 32 --repetitions 5 --seed 12345
```

The script also runs 64 measured requests and 8 warm-up requests in each thin arm, plus 160 measured reads and up to 16 warm-up reads in each rebuild baseline arm. The main-workload defaults are 1,000 measured operations and 100 warm-up operations when those flags are omitted. Keep the configuration, seed, observation policy, build type and recorded host conditions with every comparison. Schedule the matrix with a stable host workload. Use `--scale-note` to record the reason for a reduced request budget.

Each matrix creates a new group under `benchmarks/results`. Use its printed path as input to the analysis.

```powershell
.\.venv\Scripts\python.exe benchmarks/scripts/analyze.py benchmarks/results/<matrix-id>
```

The analyzer writes aggregate and comparison tables under `<matrix-id>/analysis` and plots under `benchmarks/plots/<matrix-id>-analysis`. Use a new `--label` for additional analysis outputs. The six publication PNGs cover IOPS, throughput, p95, p99, write amplification and thin write latency.

## Verify the implementation

```powershell
cmake --build --preset msvc-debug
ctest --preset msvc-debug --output-on-failure
ctest --preset msvc-release --output-on-failure
.\.venv\Scripts\python.exe -m pytest benchmarks/scripts/test_analysis.py -q
```

The benchmark workflow runs independently from the normal dashboard backend. [Methodology and CSV units](benchmark_methodology.md)
