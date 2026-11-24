"""Recompute the archived study evidence and prepare a separate faculty figure set."""
import argparse
from collections import defaultdict
import csv
import json
import math
from pathlib import Path
import statistics
import sys
import tempfile

from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "benchmarks"
sys.path.insert(0, str(BENCH / "scripts"))

from analyze import aggregate, load_directory  # noqa: E402
from analyze_rebuild import load_run, percentile, percent_change  # noqa: E402
from plot_benchmarks import generate  # noqa: E402
from plot_rebuild import plot_study  # noqa: E402
from reconcile_step13 import reconcile  # noqa: E402

MATRIX = "matrix-20261124T123744Z-9e803d3e"
MAIN = "rebuild-20261124T131615Z-990308f1936c27d6"
VARIABLE = "rebuild-20261124T131928Z-659471dc4eeb60ca"
PILOT = "rebuild-20261124T131350Z-f13a023b5045cdac"
SENSITIVITY = "rebuild-20261124T132347Z-e9f54ffe68e6f536"


def require(assertion, message):
    if not assertion:
        raise ValueError(message)


def rows(path):
    with path.open(newline="", encoding="utf-8") as source:
        return list(csv.DictReader(source))


def check_aggregate(recomputed, archived):
    require(len(recomputed) == len(archived), "Step 13 aggregate group counts differ")
    for expected, original in zip(recomputed, archived):
        require(expected.keys() == original.keys(), "Step 13 aggregate schema changed")
        for name, value in expected.items():
            recorded = original[name]
            if isinstance(value, (int, float)):
                match = recorded and math.isclose(float(recorded), value, rel_tol=1e-9, abs_tol=1e-12)
            else:
                match = recorded == ("" if value is None else str(value))
            require(match, f"Archived Step 13 aggregate differs in {name}")


def stats_from_raw(row, requests, progress):
    good = [r for r in requests[row["trial"]] if r["success"] == r["validation_passed"] == "true"]
    latencies = [int(r["completion_ns"]) - int(r["scheduled_arrival_ns"]) for r in good]
    window_ns = int(row["window_ns"])
    state = progress[row["trial"]]
    return {
        "p95_ms": percentile(latencies, 95),
        "p99_ms": percentile(latencies, 99),
        "iops": sum(int(r["completion_ns"]) < window_ns for r in good) / (window_ns / 1e9),
        "completion_seconds": float(state[-1]["elapsed_ms"]) / 1000 if state else None,
    }


def main(output, figures):
    matrix = BENCH / "results" / MATRIX
    main_path = BENCH / "results" / MAIN
    variable_path = BENCH / "results" / VARIABLE
    totals = reconcile(matrix)
    step13 = load_directory(matrix)
    step13_aggregates = aggregate(step13.valid)
    for label in ("analysis", "verified"):
        check_aggregate(step13_aggregates, rows(matrix / label / "benchmark_aggregates.csv"))
    calibration = reconcile(BENCH / "results" / "step13-calibration")
    require(not step13.excluded and totals["completed"] == 27840 and totals["validated_repetitions"] == 120,
            "Step 13 archived cohort differs")
    thin = {row["workload"]: {field: row[field] for field in ("mean_iops", "mean_latency_p95_ms", "mean_new_chunks")}
            for row in step13_aggregates if row["workload"].startswith("THIN_")}

    manifest, valid, excluded, requests, progress, decisions = load_run(main_path)
    require(manifest["build_type"] == "Release" and len(valid) == 75 and not excluded,
            "Step 14 main cohort differs")
    require(sum(bool(d) for d in decisions.values()) == 60 and
            all(any(row["pressure_state"] == "HIGH" for row in decisions[t["trial"]])
                for t in valid if t["policy"] == "ADAPTIVE" and t["foreground_load"] == "HEAVY"),
            "Step 14 scheduling decision traces differ")
    actual = {name: len(load_run(BENCH / "results" / run)[1])
              for name, run in {"pilot": PILOT, "variable": VARIABLE, "sensitivity": SENSITIVITY}.items()}
    require(actual == {"pilot": 11, "variable": 1, "sensitivity": 10}, "Companion trial coverage differs")
    raw = {(r["repetition"], r["foreground_load"], r["policy"]): stats_from_raw(r, requests, progress)
           for r in valid}
    archived_pairs = {(t["repetition"], t["foreground_load"], t["baseline"], t["metric"]): t
                      for t in rows(main_path / "paired_tradeoffs.csv")}
    pairs = defaultdict(list)
    for (rep, load, policy), adaptive in raw.items():
        if policy != "ADAPTIVE" or load == "IDLE":
            continue
        comparison = {"repetition": int(rep), "adaptive": adaptive}
        for baseline in ("FULL_SPEED", "FIXED_THROTTLE"):
            reference = raw[(rep, load, baseline)]
            changes = {metric: percent_change(value, reference[metric]) for metric, value in adaptive.items()}
            for metric, column in {"p95_ms": "foreground_p95_ms", "p99_ms": "foreground_p99_ms",
                                   "iops": "foreground_iops", "completion_seconds": "rebuild_elapsed_seconds"}.items():
                archived = archived_pairs[(rep, load, baseline, column)]["percent_change"]
                require(archived and math.isclose(changes[metric], float(archived), abs_tol=1e-6),
                        "Step 14 paired raw and archived changes differ")
            comparison[baseline] = changes
        pairs[load].append(comparison)
    paired_stats = {}
    for load in ("LIGHT", "MEDIUM", "HEAVY"):
        trials = pairs[load]
        require(len(trials) == 5 and all(t["FULL_SPEED"]["p95_ms"] < 0 for t in trials),
                f"Step 14 p95 pairing differs for {load}")
        paired_stats[load] = {baseline: {
            metric: {"mean_percent": statistics.mean(values), "sd_percent": statistics.stdev(values),
                     "range_percent": [min(values), max(values)]}
            for metric in ("p95_ms", "p99_ms", "iops", "completion_seconds")
            for values in [[t[baseline][metric] for t in trials]]}
            for baseline in ("FULL_SPEED", "FIXED_THROTTLE")}
    reads = sum(int(r["foreground_completed"]) for r in valid)
    healthy = sum(r["final_raid_health"] == "HEALTHY" for r in valid)
    require(reads == 7040 and healthy == 60 and
            all(int(r["foreground_failed"]) == int(r["foreground_dropped"]) == 0 for r in valid),
            "Step 14 data integrity or foreground coverage differs")
    step13_names = ("raid_iops.png", "raid_throughput.png", "raid_p95.png", "raid_p99.png",
                    "raid_write_amplification.png", "thin_write_latency.png")
    step14_names = ("foreground-p95.png", "foreground-p99.png", "foreground-iops.png", "rebuild-time.png",
                    "latency-time-tradeoff.png", "matched-rebuild-progress.png", "adaptive-variable-load.png",
                    "policy-comparison.png")
    sets = ((MATRIX + "-verified", step13_names), (MAIN, step14_names))
    for name, plots in sets:
        for file in plots:
            with Image.open(BENCH / "plots" / name / file) as png:
                require(png.format == "PNG" and min(png.size) >= 600, f"Invalid archived figure {file}")
                png.verify()

    report = {"source": "MEASURED", "step13": {**totals, "calibration": calibration, "thin": thin},
              "step14": {"validated_trials": len(valid), "excluded": len(excluded), "validated_reads": reads,
                         "healthy_rebuilds": healthy, "degraded_controls": len(valid) - healthy,
                         "loaded_pairs": sum(map(len, pairs.values())), "companion_trials": actual,
                         "paired_statistics": paired_stats, "paired_from_raw": dict(pairs)},
              "archived_figures": [f"benchmarks/plots/{name}/{plot}" for name, plots in sets for plot in plots]}
    with (output / "research-audit.json").open("x", encoding="utf-8") as file:
        json.dump(report, file, indent=2, allow_nan=False)
    if figures:
        step13_plots = output / "raid-performance"
        step13_plots.mkdir()
        generate(step13_aggregates, step13_plots, MATRIX)
        plot_study(main_path, variable_path, output / "rebuild-scheduling")
    print(f"Step 13: {totals['completed']:,} validated measured requests, {len(step13.valid)} repetitions.")
    print(f"Step 14: {len(valid)} validated trials, {reads:,} successful reads, {healthy} healthy rebuilds.")
    for load, baselines in paired_stats.items():
        print(f"{load}: adaptive p95 vs full-speed {baselines['FULL_SPEED']['p95_ms']['mean_percent']:+.1f}%, "
              f"elapsed {baselines['FULL_SPEED']['completion_seconds']['mean_percent']:+.1f}%; "
              f"p95 vs fixed {baselines['FIXED_THROTTLE']['p95_ms']['mean_percent']:+.1f}%.")
    for path in report["archived_figures"]:
        print(path)
    print(f"Research evidence: {output}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--skip-figures", action="store_true", help="Still check the archived PNGs")
    args = parser.parse_args()
    if args.output is None:
        parent = ROOT / "build/final-validation"
        parent.mkdir(parents=True, exist_ok=True)
        folder = Path(tempfile.mkdtemp(prefix="research-", dir=parent))
    else:
        folder = args.output
        folder.mkdir(parents=False)
    main(folder, not args.skip_figures)

