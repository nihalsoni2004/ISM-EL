"""Validate schema-1 MEASURED rebuild trials and summarize matched comparisons."""
import argparse
from collections import defaultdict
import csv
import itertools
import json
import math
from pathlib import Path
import statistics

FILES = {
    "foreground_requests.csv": "sequence scheduled_arrival_ns actual_submit_ns operation_start_ns operation_end_ns completion_ns queue_wait_ns operation_duration_ns response_latency_ns logical_lba logical_blocks success validation_passed rebuild_active error",
    "rebuild_progress.csv": "job_id elapsed_ms completed_blocks total_blocks worker_state catalog_hold_ms throttle_wait_ms",
    "scheduler_decisions.csv": "job_id control_time_ms pressure_state configured_delay_ms decision_reason controller_ns",
    "policy_results.csv": "seed offered_iops window_ns foreground_scheduled foreground_submitted foreground_completed foreground_failed foreground_dropped validation_failures foreground_within_window foreground_iops foreground_throughput_mib_s foreground_mean_ms foreground_p50_ms foreground_p95_ms foreground_p99_ms active_foreground_p95_ms active_foreground_p99_ms active_foreground_completed active_exposure_seconds active_iops rebuild_elapsed_seconds rebuild_processing_seconds rebuild_throttle_wait_seconds rebuild_blocks_completed rebuild_mib_s catalog_duty_cycle controller_total_ms state_transitions final_raid_health job_state foreground_validation_passed rebuild_validation_passed source_images_unchanged status error",
}
PREFIX = "schema_version run_id trial repetition policy foreground_load raid_level source"
METRICS = (
    "foreground_p50_ms", "foreground_p95_ms", "foreground_p99_ms", "foreground_iops",
    "foreground_throughput_mib_s", "active_foreground_p95_ms", "active_foreground_p99_ms",
    "active_foreground_completed", "active_exposure_seconds", "active_iops", "rebuild_elapsed_seconds",
    "rebuild_mib_s", "catalog_duty_cycle", "rebuild_processing_seconds", "rebuild_throttle_wait_seconds",
    "controller_total_ms", "state_transitions",
)
PAIRED = ("foreground_p95_ms", "foreground_p99_ms", "foreground_iops",
          "active_foreground_p95_ms", "rebuild_elapsed_seconds", "rebuild_mib_s")


def number(row, field, cast=float):
    value = row[field]
    if value == "":
        return None
    parsed = cast(value)
    if not math.isfinite(parsed) or parsed < 0:
        raise ValueError(f"Invalid {field}")
    return parsed


def same(actual, expected):
    if actual is None or expected is None:
        return actual is expected
    return math.isclose(actual, expected, rel_tol=1e-7, abs_tol=1e-8)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def percentile(samples, percentile_rank):
    """Nearest rank within one trial, converted from nanoseconds to milliseconds."""
    if not samples:
        return None
    return sorted(samples)[math.ceil(len(samples) * percentile_rank / 100) - 1] / 1e6


def csv_rows(path, run_id):
    with path.open(newline="", encoding="utf-8") as file:
        reader = csv.DictReader(file)
        headers = reader.fieldnames or []
        required = set(PREFIX.split()) | set(FILES[path.name].split())
        require(required <= set(headers) and len(headers) == len(set(headers)), f"Invalid header in {path.name}")
        for row in reader:
            require(None not in row and None not in row.values(), f"Malformed row in {path.name}")
            require(row["run_id"] == run_id and row["schema_version"] == "1" and row["source"] == "MEASURED",
                    f"Foreign run, schema or source in {path.name}")
            yield row


def offered_timeline(phases):
    elapsed = 0
    timeline = []
    for phase in phases:
        duration = phase["duration_ms"] * 1_000_000
        rate = phase["offered_iops"]
        if rate:
            for index in range((phase["duration_ms"] * rate + 999) // 1000):
                timeline.append(elapsed + index * 1_000_000_000 // rate)
        elapsed += duration
    return elapsed, timeline


def group_rows(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[row["trial"]].append(row)
    return groups


def validate_trial(row, requests, progress, decisions, config, phases, identity_ok):
    control = row["policy"] == "DEGRADED_ONLY"
    window_ns, expected = offered_timeline(phases)
    integer = lambda field: number(row, field, int)
    require(identity_ok, "trial metadata differs from manifest or raw rows")
    require(integer("seed") == config["seed"] and integer("raid_level") == config["raid_level"],
            "inconsistent configuration")
    offer = phases[0]["offered_iops"] if len(phases) == 1 else None
    require(integer("offered_iops") == offer and integer("window_ns") == window_ns, "observation window mismatch")
    require(len(requests) == integer("foreground_scheduled") == len(expected), "scheduled count mismatch")
    response, active = [], []
    submitted = failed = dropped = errors = within = active_within = 0
    addresses = []
    rebuild_ns = None if row["rebuild_elapsed_seconds"] == "" else round(number(row, "rebuild_elapsed_seconds") * 1e9)
    for index, (request, scheduled) in enumerate(zip(requests, expected)):
        val = lambda field: number(request, field, int)
        require(val("sequence") == index and val("scheduled_arrival_ns") == scheduled and
                val("logical_blocks") == 1, "mismatched arrival schedule or request shape")
        addresses.append(val("logical_lba"))
        start, end, submit, done = (val(field) for field in
                                    ("operation_start_ns", "operation_end_ns", "actual_submit_ns", "completion_ns"))
        success, valid, overlap = (request[field] for field in ("success", "validation_passed", "rebuild_active"))
        require(success in ("true", "false") and valid in ("", "true", "false") and overlap in ("true", "false"),
                "invalid request flags")
        errors += valid == "false"
        if submit is None:
            require(done is None and request["error"] and success == "false", "silent scheduled-request drop")
            dropped += 1
            continue
        submitted += 1
        require(submit >= scheduled and val("queue_wait_ns") == submit - scheduled,
                "incorrect queue timing")
        require(done is not None and done >= submit and val("response_latency_ns") == done - scheduled,
                "incorrect response timing")
        if success == "false":
            require(request["error"], "foreground error missing")
            failed += 1
            continue
        require(start is not None and end is not None and submit <= start <= end <= done and
                val("operation_duration_ns") == end - start, "incorrect read timing")
        response.append(done - scheduled)
        within += done < window_ns
        if overlap == "true":
            require(not control and (rebuild_ns is None or start <= rebuild_ns), "healthy or control read marked active")
            active.append(done - scheduled)
            active_within += rebuild_ns is not None and done < min(window_ns, rebuild_ns)
    counts = {"foreground_submitted": submitted, "foreground_completed": len(response),
              "foreground_failed": failed, "foreground_dropped": dropped, "validation_failures": errors,
              "foreground_within_window": within, "active_foreground_completed": len(active)}
    require(all(integer(key) == value for key, value in counts.items()), "raw and summary counts disagree")
    require(row["status"] == "COMPLETED", row["error"] or "runner marked trial failed")
    require(not (failed or dropped or errors) and all(r["validation_passed"] == "true" for r in requests)
            and row["source_images_unchanged"] == row["foreground_validation_passed"] == "true",
            "integrity or completion validation failed")
    duration = window_ns / 1e9
    exposure = min(window_ns, rebuild_ns) / 1e9 if rebuild_ns else None
    for key, value in {
        "foreground_iops": within / duration, "foreground_throughput_mib_s": within / 256 / duration,
        "foreground_mean_ms": statistics.mean(response) / 1e6 if response else None,
        "foreground_p50_ms": percentile(response, 50), "foreground_p95_ms": percentile(response, 95),
        "foreground_p99_ms": percentile(response, 99), "active_foreground_p95_ms": percentile(active, 95),
        "active_foreground_p99_ms": percentile(active, 99), "active_exposure_seconds": exposure,
        "active_iops": active_within / exposure if exposure else None,
    }.items():
        require(same(number(row, key), value), f"raw and summary {key} disagree")

    if control:
        require(not progress and not decisions and rebuild_ns is None and row["final_raid_health"] == "DEGRADED"
                and row["job_state"] == "PREPARED" and row["rebuild_validation_passed"] == "",
                "degraded control contained rebuild activity")
    else:
        total = config["member_capacity_mib"] * 256
        batch = config["batch_blocks"]
        require(rebuild_ns and row["final_raid_health"] == "HEALTHY" and row["job_state"] == "COMPLETED" and
                row["rebuild_validation_passed"] == "true" and integer("rebuild_blocks_completed") == total,
                "rebuild failed validation")
        require(len(progress) >= 3 and progress[0]["worker_state"] == "STARTED" and
                progress[-1]["worker_state"] == "COMPLETED", "missing rebuild boundaries")
        times = [number(event, "elapsed_ms") for event in progress]
        require(times == sorted(times) and same(times[-1], rebuild_ns / 1e6), "invalid rebuild progress timing")
        durable = [number(event, "completed_blocks", int) for event in progress if event["worker_state"] == "RECONSTRUCTION"]
        require(durable == [*range(batch, total, batch), total], "duplicate or skipped rebuild checkpoint")
        require(bool(decisions), "missing pacing decision trace")
        require(all(event["job_id"] == progress[0]["job_id"] for event in progress + decisions), "trace job changed")
    return tuple(addresses)


def load_run(directory):
    directory = Path(directory)
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8"))
    require(manifest["schema_version"] == 1 and manifest["source"] == "MEASURED", "Expected MEASURED schema-1 manifest")
    config = manifest["configuration"]
    profiles = {load["name"]: load["phases"] for load in config["loads"]}
    by_file = {name: group_rows(csv_rows(directory / name, manifest["run_id"])) for name in FILES}
    requests, progress, decisions, summaries = by_file.values()
    execution = {str(row["trial"]): row for row in manifest["execution_order"]}
    require(len(execution) == len(manifest["execution_order"]) and execution.keys() == summaries.keys() and
            all(group.keys() <= summaries.keys() for group in (requests, progress, decisions)),
            "trial IDs in manifest and CSVs disagree")
    expected = {(rep, policy, load) for rep in range(1, config["repetitions"] + 1)
                for policy in config["conditions"] for load, phases in profiles.items()
                if policy != "DEGRADED_ONLY" or any(phase["offered_iops"] for phase in phases)}
    seen = set()
    valid, excluded, sequences = [], [], defaultdict(list)
    for trial, rows in summaries.items():
        require(len(rows) == 1, "duplicate trial summary")
        row = rows[0]
        rep, policy, load = int(row["repetition"]), row["policy"], row["foreground_load"]
        key = (rep, policy, load)
        require(key not in seen and key in expected, "unexpected or duplicate experiment cell")
        seen.add(key)
        fields = ("repetition", "policy", "foreground_load", "status")
        identity_ok = all(str(execution[trial][field]) == row[field] for field in fields)
        for dataset in (requests, progress, decisions):
            identity_ok = identity_ok and all(all(raw[field] == row[field] for field in
                                                 ("repetition", "policy", "foreground_load", "raid_level"))
                                              for raw in dataset[trial])
        try:
            addresses = validate_trial(row, requests[trial], progress[trial], decisions[trial], config, profiles[load], identity_ok)
        except (ValueError, TypeError) as error:
            excluded.append({"trial": trial, "repetition": rep, "policy": policy,
                             "foreground_load": load, "reason": str(error)})
        else:
            valid.append(row)
            sequences[rep, load].append(addresses)
    require(seen == expected, "missing predeclared experiment cell")
    require(all(len(set(addresses)) == 1 for addresses in sequences.values()), "unmatched address traces across policies")
    return manifest, valid, excluded, requests, progress, decisions


def describe(values):
    return {"n": len(values), "mean": statistics.mean(values) if values else None,
            "median": statistics.median(values) if values else None,
            "sample_sd": statistics.stdev(values) if len(values) > 1 else None}


def aggregate(valid):
    groups = defaultdict(list)
    for row in valid:
        groups[row["policy"], row["foreground_load"]].append(row)
    return [{"policy": policy, "foreground_load": load, "metric": metric,
             "offered_iops": number(rows[0], "offered_iops", int), "trials": len(rows),
             **describe([value for row in rows if (value := number(row, metric)) is not None])}
            for (policy, load), rows in sorted(groups.items()) for metric in METRICS]


def percent_change(value, reference):
    return 100 * (value - reference) / reference if value is not None and reference else None


def pair(valid):
    lookup = {(row["repetition"], row["foreground_load"], row["policy"]): row for row in valid}
    changes = []
    for (rep, load, policy), row in sorted(lookup.items()):
        if policy != "ADAPTIVE":
            continue
        for baseline, metric in itertools.product(("FULL_SPEED", "FIXED_THROTTLE"), PAIRED):
            other = lookup.get((rep, load, baseline))
            if other:
                value, reference = number(row, metric), number(other, metric)
                changes.append({"repetition": rep, "foreground_load": load, "baseline": baseline, "metric": metric,
                                "adaptive": value, "baseline_value": reference,
                                "percent_change": percent_change(value, reference)})
    return changes


def write_csv(path, fields, rows):
    with path.open("x", encoding="utf-8", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def export(directory):
    manifest, valid, excluded, *_ = load_run(directory)
    output = Path(directory)
    write_csv(output / "policy_aggregates.csv", "policy foreground_load metric offered_iops trials n mean median sample_sd".split(),
              aggregate(valid))
    write_csv(output / "paired_tradeoffs.csv", "repetition foreground_load baseline metric adaptive baseline_value percent_change".split(),
              pair(valid))
    write_csv(output / "analysis_exclusions.csv", "trial repetition policy foreground_load reason".split(), excluded)
    print(f"{manifest['run_id']} MEASURED: {len(valid)} validated, {len(excluded)} excluded trials")
    return not excluded and bool(valid)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    raise SystemExit(0 if export(parser.parse_args().run) else 1)

