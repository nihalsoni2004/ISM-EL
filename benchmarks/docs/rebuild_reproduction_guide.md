# Reproduce the rebuild policy study

Run from the repository root in PowerShell. Build and test the storage engine first. The study uses the project-local Python analysis environment and the existing benchmark dependencies.

```powershell
cmake --preset msvc-debug
cmake --build --preset msvc-release
ctest --preset msvc-release --output-on-failure -R '^rebuild_scheduler_'
$sim = '.\build\msvc-debug\Release\raid_simulator.exe'
$python = '.\.venv\Scripts\python.exe'
& $python -m pip install -r benchmarks/requirements.txt
& $python benchmarks/scripts/reconcile_step13.py benchmarks/results/matrix-20261124T123744Z-9e803d3e
& $sim benchmark rebuild --config benchmarks/configs/rebuild_pilot.json
$main = (& $sim benchmark rebuild --config benchmarks/configs/rebuild_comparison.json | ConvertFrom-Json).directory
$variable = (& $sim benchmark rebuild --config benchmarks/configs/rebuild_variable.json | ConvertFrom-Json).directory
& $python benchmarks/scripts/analyze_rebuild.py $main
& $python benchmarks/scripts/analyze_rebuild.py $variable
$plots = Join-Path benchmarks/plots (Split-Path $main -Leaf)
& $python benchmarks/scripts/plot_rebuild.py $main --variable $variable --output $plots
& $python -m pytest benchmarks/scripts -q
```

The three single-policy configs are `rebuild_full_speed.json`, `rebuild_fixed_throttle.json`, and `rebuild_adaptive.json`. Use the comparison config for matching across policies. Keep a study on one host and Release build and avoid concurrent benchmarks for comparable conditions.

The secondary medium-load sensitivity is available as `rebuild_matched_time_sensitivity.json`. It uses the same Release command and analysis script to compare adaptive with a single 1 ms fixed-delay candidate. The target was a mean completion-time difference within 10%.

Every execution creates its own result directory and disposable trial workspace under `benchmarks/workspaces`. Guards reject reparse points and hold a workspace lease against another benchmark. The experiment is bounded to 1–16 MiB member images, one foreground worker, 500 offered IOPS, at most ten repetitions, 100,000 scheduled reads across a study, and an 8,192-event trace per trial. The CLI caps individual trials at 30 seconds with cooperative shutdown and records deadline failures. Underlying Windows operations can add time to a deadline. Analysis and plotting use exclusive output creation. Run the CLI again for a new run directory.

[Experimental design and accounting](rebuild_methodology.md) · [Recorded results](rebuild_results.md)
