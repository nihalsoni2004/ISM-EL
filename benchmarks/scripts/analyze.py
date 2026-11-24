"""Validate benchmark artifacts, aggregate repetitions, and plot measured results."""
import argparse
from collections import defaultdict
import csv
from dataclasses import dataclass
import json
import math
from pathlib import Path
import re
import statistics

from run_matrix import BENCH, WORKLOADS

GEOMETRY = ("raid_level", "disk_count", "disk_capacity_mib", "raid_raw_capacity_bytes", "raid_usable_capacity_bytes",
            "stripe_unit_blocks", "capacity_mode", "footprint_blocks", "workload", "request_size_bytes",
            "operation_count", "warmup_operations", "seed", "queue_depth", "mixed_read_percent", "thin_offset_blocks",
            "validation", "validation_stride", "measurement_source", "cache_policy", "build_type", "compiler",
            "rebuild_contention")
PHYSICAL = ("physical_read_requests", "physical_write_requests", "physical_read_blocks", "physical_write_blocks",
            "physical_bytes_read", "physical_bytes_written")
METRICS = ("iops", "throughput_mib_s", "latency_p50_ms", "latency_p95_ms", "latency_p99_ms",
           "write_amplification_blocks", "write_path_read_blocks_per_logical_write_block", "new_chunks",
           "allocation_growth_bytes", "rebuild_seconds", "rebuild_overlap_requests", *PHYSICAL)
SUMMARY = ("schema_version", "run_id", "repetition", "benchmark_name", *GEOMETRY, "elapsed_seconds",
           "preparation_seconds", "warmup_seconds", "attempted_operations", "completed_operations",
           "failed_operations", "logical_bytes", "logical_written_blocks", "iops", "throughput_mib_s", "latency_count",
           "latency_min_ms", "latency_max_ms", "latency_mean_ms", "latency_p50_ms", "latency_p95_ms",
           "latency_p99_ms", *PHYSICAL, "write_amplification_blocks", "new_chunks", "allocated_before_bytes",
           "allocated_after_bytes", "rebuild_seconds", "rebuilt_blocks", "rebuild_total_blocks",
           "rebuild_overlap_requests", "rebuild_state", "validation_status", "status", "error")
RAW = ("schema_version", "run_id", "repetition", "sequence", "benchmark_name", "raid_level", "workload", "operation",
       "request_size_bytes", "logical_lba", "logical_blocks", "pattern_ref", "duration_ns", "success",
       "validation_passed", "logical_bytes_completed", *PHYSICAL, "new_chunks", "rebuild_overlap", "error", "source")


@dataclass
class Dataset:
    valid: list
    excluded: list


def read_csv(path, fields):
    if path.stat().st_size > 512 * 1024 * 1024:
        raise ValueError(f"CSV exceeds the analysis input limit: {path}")
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None or len(reader.fieldnames) != len(fields) or set(reader.fieldnames) != set(fields):
            raise ValueError(f"Unexpected CSV columns: {path}")
        for row in reader:
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"Malformed CSV row: {path}")
            yield row


def count(row, key):
    value = row[key]
    if not re.fullmatch(r"0|[1-9][0-9]{0,19}", value) or int(value) >= 2**64:
        raise ValueError(f"Invalid integer in {key}: {value!r}")
    return int(value)


def number(row, key, optional=False):
    value = row[key]
    if optional and not value:
        return None
    try:
        numeric = float(value)
    except ValueError as error:
        raise ValueError(f"Invalid number in {key}: {value!r}") from error
    if numeric < 0 or not math.isfinite(numeric):
        raise ValueError(f"Invalid number in {key}: {value!r}")
    return numeric


def flag(row, key):
    if row[key] not in ("true", "false"):
        raise ValueError(f"Invalid boolean in {key}")
    return row[key] == "true"


def compare(actual, expected, field):
    equal = actual is expected if actual is None or expected is None else math.isclose(actual, expected, rel_tol=1e-9, abs_tol=1e-10)
    if not equal:
        raise ValueError(f"Summary {field} differs from its raw data")


def rank(sorted_samples, percentage):
    return sorted_samples[(percentage * len(sorted_samples) + 99) // 100 - 1]


def validate_repetition(summary, raw, manifest, rep):
    if count(summary, "schema_version") != 1 or summary["measurement_source"] != "MEASURED":
        raise ValueError("Only schema-1 MEASURED summaries are accepted")
    if summary["queue_depth"] != "1" or summary["raid_level"] not in ("0", "1", "5", "6"):
        raise ValueError("Unsupported storage geometry or queue depth")
    cfg = manifest["configuration"]
    manifest_fields = {"benchmark_name": cfg["benchmark_name"], "raid_level": cfg["raid_level"],
                       "disk_count": cfg["disk_count"], "stripe_unit_blocks": cfg["stripe_unit_blocks"],
                       "footprint_blocks": cfg["footprint_blocks"], "workload": cfg["workload"],
                       "request_size_bytes": cfg["request_size_bytes"], "operation_count": cfg["operation_count"],
                       "warmup_operations": cfg["warmup_operations"], "seed": cfg["seed"],
                       "mixed_read_percent": cfg["mixed_read_percent"], "thin_offset_blocks": cfg["thin_offset_blocks"],
                       "validation_stride": cfg["validation_stride"], "validation": cfg["validation"],
                       "capacity_mode": cfg["capacity_mode"], "build_type": manifest["build_type"],
                       "compiler": manifest["compiler"], "cache_policy": manifest["cache_policy"],
                       "raid_raw_capacity_bytes": manifest["raw_capacity_bytes"],
                       "raid_usable_capacity_bytes": manifest["usable_capacity_bytes"],
                       "rebuild_contention": json.dumps(cfg["rebuild_contention"])}
    for key, expected in manifest_fields.items():
        if summary[key] != str(expected):
            raise ValueError(f"Summary field {key} diverges from manifest")
    if (summary["status"] != rep["status"] or summary["validation_status"] != rep["validation_status"]
            or summary["error"] != rep["error"] or summary["status"] not in ("COMPLETED", "FAILED")
            or summary["validation_status"] not in ("PASSED", "FAILED", "INCOMPLETE", "NOT_CHECKED")):
        raise ValueError("Invalid repetition status")
    if (count(summary, "disk_capacity_mib") * count(summary, "disk_count") * 1048576
            != count(summary, "raid_raw_capacity_bytes")):
        raise ValueError("Inconsistent raw capacity")
    elapsed = number(summary, "elapsed_seconds")
    number(summary, "preparation_seconds")
    number(summary, "warmup_seconds")
    capacity = count(summary, "footprint_blocks")
    request_size = count(summary, "request_size_bytes")
    total = defaultdict(int)
    durations = []
    for index, row in enumerate(raw):
        if (row["source"] != "MEASURED" or count(row, "sequence") != index or row["schema_version"] != "1"
                or any(row[field] != summary[field] for field in ("run_id", "repetition", "benchmark_name", "raid_level", "workload"))
                or row["operation"] not in ("READ", "WRITE") or row["validation_passed"] not in ("", "true", "false")):
            raise ValueError("Invalid measured raw operation")
        blocks = count(row, "logical_blocks")
        if not blocks or blocks * 4096 != request_size or count(row, "request_size_bytes") != request_size:
            raise ValueError("Inconsistent request size")
        if count(row, "logical_lba") + blocks > capacity or (row["operation"] == "READ" and count(row, "pattern_ref") != 0):
            raise ValueError("Out-of-range request or invalid pattern reference")
        count(row, "pattern_ref")
        success = flag(row, "success")
        completed = count(row, "logical_bytes_completed")
        if completed != (request_size if success else 0):
            raise ValueError("Logical transfer bytes disagree with operation outcome")
        if summary["status"] == "COMPLETED" and (not success or row["error"] or row["validation_passed"] == "false"):
            raise ValueError("Completed repetition contains a failed request or validation")
        total["completed_operations"] += int(success)
        total["logical_bytes"] += completed
        total["logical_written_blocks"] += blocks if success and row["operation"] == "WRITE" else 0
        total["write_reads"] += count(row, "physical_read_blocks") if row["operation"] == "WRITE" else 0
        total["rebuild_overlap_requests"] += flag(row, "rebuild_overlap")
        total["new_chunks"] += count(row, "new_chunks")
        durations.append(count(row, "duration_ns"))
        for field in PHYSICAL:
            total[field] += count(row, field)
        if (count(row, "physical_bytes_read") != count(row, "physical_read_blocks") * 4096
                or count(row, "physical_bytes_written") != count(row, "physical_write_blocks") * 4096):
            raise ValueError("Inconsistent measured physical byte totals")

    total["attempted_operations"] = len(raw)
    total["failed_operations"] = len(raw) - total["completed_operations"]
    total["latency_count"] = len(raw)
    for field in (*PHYSICAL, "logical_bytes", "logical_written_blocks", "new_chunks", "rebuild_overlap_requests"):
        total.setdefault(field, 0)
    for key, expected in total.items():
        if key != "write_reads" and count(summary, key) != expected:
            raise ValueError(f"Invalid raw total for {key}")
    if len(raw) > count(summary, "operation_count") or (summary["status"] == "COMPLETED" and len(raw) != count(summary, "operation_count")):
        raise ValueError("Unexpected number of measured requests")
    if sum(durations) / 1e9 > elapsed + 1e-8:
        raise ValueError("Request durations exceed serial observation time")

    compare(number(summary, "iops", True), total["completed_operations"] / elapsed if elapsed else None, "IOPS")
    compare(number(summary, "throughput_mib_s", True), total["logical_bytes"] / 1048576 / elapsed if elapsed else None, "throughput")
    written = total["logical_written_blocks"]
    compare(number(summary, "write_amplification_blocks", True), total["physical_write_blocks"] / written if written else None, "write amplification")
    samples = sorted(durations)
    stats = {f"latency_p{percent}_ms": rank(samples, percent) / 1e6 if samples else None for percent in (50, 95, 99)}
    stats.update({"latency_min_ms": min(samples) / 1e6 if samples else None,
                  "latency_max_ms": max(samples) / 1e6 if samples else None,
                  "latency_mean_ms": statistics.mean(samples) / 1e6 if samples else None})
    for field, expected in stats.items():
        compare(number(summary, field, True), expected, field)

    before, after = (count(summary, field) if summary[field] else None
                     for field in ("allocated_before_bytes", "allocated_after_bytes"))
    growth = after - before if before is not None and after is not None else None
    if summary["status"] == "COMPLETED" and summary["workload"].startswith("THIN_"):
        if growth is None or growth != total["new_chunks"] * 65536:
            raise ValueError("Thin allocation growth disagrees with chunk counts")
    rebuilt, planned = (count(summary, field) for field in ("rebuilt_blocks", "rebuild_total_blocks"))
    rebuild_seconds = number(summary, "rebuild_seconds", True)
    if summary["status"] == "COMPLETED" and flag(summary, "rebuild_contention"):
        if not rebuild_seconds or not rebuilt or rebuilt != planned or not total["rebuild_overlap_requests"] or summary["rebuild_state"] != "COMPLETED":
            raise ValueError("Rebuild baseline lacks durable completion or observed overlap")

    values = {metric: number(summary, metric, True) for metric in METRICS if metric in summary}
    values.update({"allocation_growth_bytes": growth,
                   "write_path_read_blocks_per_logical_write_block": total["write_reads"] / written if written else None})
    return {**summary, **values, "host_key": json.dumps(manifest["host"], sort_keys=True),
            "git_commit": manifest.get("git_commit") or "unknown", "cmake_version": manifest["cmake_version"]}


def load_directory(directory):
    items = Dataset([], [])
    ids = set()
    for folder in sorted(directory.glob("run-*")):
        if not folder.is_dir():
            raise ValueError(f"Expected run directory: {folder}")
        manifest = json.loads((folder / "manifest.json").read_text(encoding="utf-8"))
        if (manifest["schema_version"] != 1 or manifest["run_id"] != folder.name
                or manifest["measurement_source"] != "MEASURED" or manifest["raw_csv"] != "benchmark_runs.csv"
                or manifest["summary_csv"] != "benchmark_results.csv" or folder.name in ids):
            raise ValueError(f"Invalid or duplicate MEASURED run manifest: {folder}")
        ids.add(folder.name)
        raw = defaultdict(list)
        for row in read_csv(folder / manifest["raw_csv"], RAW):
            raw[count(row, "repetition")].append(row)
        repetitions = {row["repetition"]: row for row in manifest["repetitions"]}
        if (len(repetitions) != len(manifest["repetitions"]) or sorted(repetitions) != list(range(1, len(repetitions) + 1))
                or not repetitions or manifest["status"] not in ("COMPLETED", "FAILED")
                or (manifest["status"] == "COMPLETED" and (
                    len(repetitions) != manifest["configuration"]["repetitions"] or any(rep["status"] != "COMPLETED" for rep in repetitions.values())))):
            raise ValueError("Invalid manifest repetition coverage")
        seen = set()
        for summary in read_csv(folder / manifest["summary_csv"], SUMMARY):
            rep = count(summary, "repetition")
            if summary["run_id"] != folder.name or rep in seen or rep not in repetitions:
                raise ValueError("Unknown or duplicate repetition")
            seen.add(rep)
            result = validate_repetition(summary, raw.pop(rep, []), manifest, repetitions[rep])
            if result["status"] == "COMPLETED" and result["validation_status"] == "PASSED":
                items.valid.append(result)
            else:
                items.excluded.append({key: result[key] for key in ("run_id", "repetition", "status", "validation_status", "error")})
        if raw or seen != set(repetitions):
            raise ValueError("Raw, summary, and manifest repetition coverage differ")
    return items


GROUP = (*GEOMETRY, "benchmark_name", "host_key", "git_commit", "cmake_version")


def aggregate(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[tuple(row[field] for field in GROUP)].append(row)
    results = []
    for key, repetitions in sorted(groups.items()):
        result = dict(zip(GROUP, key))
        result["valid_repetitions"] = len(repetitions)
        for metric in METRICS:
            values = [row[metric] for row in repetitions if row[metric] is not None]
            result[f"mean_{metric}"] = statistics.mean(values) if values else None
            result[f"median_{metric}"] = statistics.median(values) if values else None
            result[f"sd_{metric}"] = statistics.stdev(values) if len(values) > 1 else None
        results.append(result)
    return results


def csv_output(path, fields, rows):
    with path.open("x", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results", type=Path, help="One benchmark matrix group containing run directories")
    parser.add_argument("--label", default="analysis", help="New analysis directory name within the group")
    args = parser.parse_args()
    if not re.fullmatch(r"[a-zA-Z0-9_-]{1,24}", args.label):
        parser.error("Use a short path-safe label.")
    directory = args.results.resolve(strict=True)
    if directory.parent != (BENCH / "results").resolve():
        parser.error("Use a result group directly under benchmarks/results.")
    loaded = load_directory(directory)
    if not loaded.valid:
        parser.error("The selection has no valid measured repetitions.")
    results = aggregate(loaded.valid)
    output = directory / args.label
    figures = BENCH / "plots" / (directory.name + "-" + args.label)
    if output.exists() or figures.exists():
        parser.error("Choose a new analysis label to preserve earlier artifacts.")
    output.mkdir()
    figures.mkdir()
    csv_output(output / "benchmark_aggregates.csv", list(results[0]), results)
    csv_output(output / "benchmark_comparisons.csv", (
        "raid_level", "workload", "rebuild_contention", "request_size_bytes", "valid_repetitions",
        "mean_iops", "sd_iops", "mean_throughput_mib_s", "mean_latency_p50_ms", "mean_latency_p95_ms",
        "mean_latency_p99_ms", "mean_write_amplification_blocks", "mean_write_path_read_blocks_per_logical_write_block",
        "mean_new_chunks", "mean_allocation_growth_bytes", "mean_rebuild_seconds", "mean_rebuild_overlap_requests",
    ), results)
    csv_output(output / "benchmark_exclusions.csv", ("run_id", "repetition", "status", "validation_status", "error"), loaded.excluded)
    from plot_benchmarks import generate
    generate(results, figures, directory.name)
    print(f"Validated {len(loaded.valid)} repetitions across {len(results)} groups. Excluded {len(loaded.excluded)}.")
    print(output)
    print(figures)


if __name__ == "__main__":
    main()
