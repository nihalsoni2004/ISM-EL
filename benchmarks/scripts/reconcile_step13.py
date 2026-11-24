"""Reconcile Step 13 raw requests and summary rows without changing historical artifacts."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path

from analyze import RAW, SUMMARY, count, load_directory, read_csv
from run_matrix import WORKLOADS


def totals(summaries, raw):
    expected = Counter()
    actual = Counter()
    groups = {}
    warmup = 0
    for row in summaries:
        key = (row["run_id"], row["repetition"])
        if key in groups:
            raise ValueError("Duplicate repetition")
        workload = row["workload"]
        group = "main" if workload in WORKLOADS else "thin" if workload.startswith("THIN_") else (
            "baseline" if workload == "READ_ONLY_BASELINE" else "other")
        groups[key] = group
        for field in ("attempted_operations", "completed_operations", "failed_operations"):
            expected[key, field] = count(row, field)
        warmup += count(row, "warmup_operations")
    breakdown = Counter()
    for row in raw:
        key = (row["run_id"], row["repetition"])
        if key not in groups or row["success"] not in ("true", "false"):
            raise ValueError("Unexpected raw request")
        actual[key, "attempted_operations"] += 1
        actual[key, "completed_operations" if row["success"] == "true" else "failed_operations"] += 1
        breakdown[groups[key]] += 1
    if expected != actual:
        raise ValueError("Raw request counts and summaries disagree")
    return {**{name: breakdown[name] for name in ("main", "thin", "baseline", "other")},
            "measured": sum(breakdown.values()), "repetitions": len(groups),
            "completed": sum(value for (_, field), value in expected.items() if field == "completed_operations"),
            "failed": sum(value for (_, field), value in expected.items() if field == "failed_operations"),
            "configured_warmup_excluded": warmup}


def reconcile(directory):
    validated = load_directory(directory)
    runs = sorted(directory.glob("run-*"))
    data = totals((row for run in runs for row in read_csv(run / "benchmark_results.csv", SUMMARY)),
                  (row for run in runs for row in read_csv(run / "benchmark_runs.csv", RAW)))
    report = {**data, "validated_repetitions": len(validated.valid), "excluded_repetitions": len(validated.excluded)}
    archived = directory / "analysis" / "benchmark_aggregates.csv"
    if archived.is_file():
        with archived.open(newline="", encoding="utf-8") as source:
            aggregates = list(csv.DictReader(source))
        measured = sum(count(row, "valid_repetitions") * count(row, "operation_count") for row in aggregates)
        repetitions = sum(count(row, "valid_repetitions") for row in aggregates)
        if repetitions != len(validated.valid) or measured != sum(int(row["attempted_operations"]) for row in validated.valid):
            raise ValueError("Archived aggregates and validated raw repetitions disagree")
        report["archived_aggregate_requests"] = measured
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path)
    print(json.dumps(reconcile(parser.parse_args().results), indent=2))
