"""Regression tests for Step 14 analysis and Step 13 request reconciliation."""
import json

import pytest

from analyze_rebuild import (FILES, PREFIX, aggregate, describe, load_run, offered_timeline,
                             pair, percent_change, percentile, write_csv)
from plot_rebuild import comparison, curves
from reconcile_step13 import totals
from run_matrix import WORKLOADS


def test_step13_accounting_excludes_warmup_and_catches_missing_requests():
    def summary(name, workload, measured, warmup, failed):
        return {"run_id": name, "repetition": "1", "workload": workload,
                "attempted_operations": str(measured), "completed_operations": str(measured - failed),
                "failed_operations": str(failed), "warmup_operations": str(warmup)}
    summaries = [summary("a", next(iter(WORKLOADS)), 3, 2, 1), summary("b", "THIN_RANDOM", 1, 0, 0),
                 summary("c", "READ_ONLY_BASELINE", 1, 4, 0)]
    requests = [{"run_id": run, "repetition": "1", "success": success}
                for run, success in (("a", "true"), ("a", "false"), ("a", "true"),
                                     ("b", "true"), ("c", "true"))]
    assert totals(summaries, requests) == {"main": 3, "thin": 1, "baseline": 1, "other": 0,
                                           "measured": 5, "repetitions": 3, "completed": 4, "failed": 1,
                                           "configured_warmup_excluded": 6}
    with pytest.raises(ValueError, match="counts"):
        totals(summaries, requests[:-1])


def test_foreground_timing_and_percentiles():
    window, times = offered_timeline([{"duration_ms": 400, "offered_iops": 0},
                                     {"duration_ms": 101, "offered_iops": 20}])
    assert window == 501_000_000 and times == [400_000_000, 450_000_000, 500_000_000]
    assert percentile([9_000_000, 1_000_000], 50) == 1
    assert percentile([1_000_000] * 19 + [9_000_000], 95) == 1
    assert percentile([1_000_000] * 19 + [9_000_000], 99) == 9
    assert percentile([], 95) is None


def make_fixture(directory, policy="DEGRADED_ONLY", foreign_source=False, success=True):
    run_id = "test-fixture"
    prefix = dict(schema_version=1, run_id=run_id, trial=1, repetition=1, policy=policy,
                  foreground_load="DEMO", raid_level=1, source="MEASURED")
    control = policy == "DEGRADED_ONLY"
    done, latency = 10_000_000, 10
    requests = [{**prefix, "sequence": 0, "scheduled_arrival_ns": 0, "actual_submit_ns": 8_000_000,
                 "operation_start_ns": 9_000_000, "operation_end_ns": 9_500_000, "completion_ns": done,
                 "queue_wait_ns": 8_000_000, "operation_duration_ns": 500_000, "response_latency_ns": done,
                 "logical_lba": 12, "logical_blocks": 1, "success": "true", "validation_passed": "true",
                 "rebuild_active": "false" if control else "true", "source": "MODELED" if foreign_source else "MEASURED"}]
    status = "COMPLETED" if success else "FAILED"
    summary = {**prefix, **dict(seed=7, offered_iops=10, window_ns=100_000_000, foreground_scheduled=1,
                              foreground_submitted=1, foreground_completed=1, foreground_failed=0,
                              foreground_dropped=0, validation_failures=0, foreground_within_window=1,
                              foreground_iops=10, foreground_throughput_mib_s=10 / 256, foreground_mean_ms=latency,
                              foreground_p50_ms=latency, foreground_p95_ms=latency, foreground_p99_ms=latency,
                              active_foreground_completed=0 if control else 1, foreground_validation_passed="true",
                              source_images_unchanged="true", final_raid_health="DEGRADED" if control else "HEALTHY",
                              job_state="PREPARED" if control else "COMPLETED", status=status,
                              error="" if success else "CHECKSUM_MISMATCH")}
    progress, decisions = [], []
    if not control:
        summary.update(rebuild_elapsed_seconds=0.024, rebuild_blocks_completed=256, rebuild_validation_passed="true",
                       active_exposure_seconds=.024, active_iops=1 / .024, active_foreground_p95_ms=latency,
                       active_foreground_p99_ms=latency)
        events = [(1, 0, "STARTED"), (4, 64, "RECONSTRUCTION"), (8, 128, "RECONSTRUCTION"),
                  (12, 192, "RECONSTRUCTION"), (16, 256, "RECONSTRUCTION"), (24, 256, "COMPLETED")]
        progress = [{**prefix, "job_id": "rebuild1", "elapsed_ms": t, "completed_blocks": blocks,
                     "total_blocks": 256, "worker_state": event} for t, blocks, event in events]
        decisions = [{**prefix, "job_id": "rebuild1", "control_time_ms": 1, "pressure_state": "IDLE",
                      "configured_delay_ms": 0, "decision_reason": "foreground_idle", "controller_ns": 500}]
    for (name, fields), rows in zip(FILES.items(), (requests, progress, decisions, [summary])):
        write_csv(directory / name, (*PREFIX.split(), *fields.split()), rows)
    manifest = {"run_id": run_id, "schema_version": 1, "source": "MEASURED",
                "configuration": {"seed": 7, "raid_level": 1, "member_capacity_mib": 1, "batch_blocks": 64,
                                  "conditions": [policy], "repetitions": 1,
                                  "loads": [{"name": "DEMO", "phases": [{"duration_ms": 100, "offered_iops": 10}]}]},
                "execution_order": [{"trial": 1, "repetition": 1, "policy": policy, "foreground_load": "DEMO",
                                     "status": status}]}
    (directory / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")


@pytest.mark.parametrize("policy", ("DEGRADED_ONLY", "ADAPTIVE"))
def test_schema_timing_and_active_filter(tmp_path, policy):
    make_fixture(tmp_path, policy=policy)
    _, valid, excluded, _, progress, _ = load_run(tmp_path)
    assert len(valid) == 1 and not excluded
    assert len(progress["1"]) == (6 if policy == "ADAPTIVE" else 0)
    assert valid[0]["active_foreground_p95_ms"] == ("10" if policy == "ADAPTIVE" else "")


def test_failed_trial_is_recorded_but_excluded(tmp_path):
    make_fixture(tmp_path, success=False)
    _, valid, excluded, *_ = load_run(tmp_path)
    assert valid == [] and excluded[0]["reason"] == "CHECKSUM_MISMATCH"


def test_reject_modeled_rows(tmp_path):
    make_fixture(tmp_path, foreign_source=True)
    with pytest.raises(ValueError, match="source"):
        load_run(tmp_path)


def test_paired_direction_matching_and_missing_values():
    def row(policy, load, repetition, p95):
        record = {metric: "" for metric in ("foreground_p99_ms", "foreground_iops", "active_foreground_p95_ms",
                                             "rebuild_elapsed_seconds", "rebuild_mib_s")}
        return {**record, "foreground_load": load, "repetition": repetition, "policy": policy,
                "foreground_p95_ms": str(p95)}
    data = [row("ADAPTIVE", "MEDIUM", 1, 8), row("FULL_SPEED", "MEDIUM", 1, 10),
            row("FULL_SPEED", "MEDIUM", 2, 4), row("FIXED_THROTTLE", "HEAVY", 1, 5)]
    p95 = [p for p in pair(data) if p["metric"] == "foreground_p95_ms"]
    assert len(p95) == 1 and p95[0]["percent_change"] == -20
    assert percent_change(12, 10) == 20 and percent_change(5, None) is None and percent_change(5, 0) is None
    assert describe([1, 3]) == {"n": 2, "mean": 2, "median": 2, "sample_sd": pytest.approx(2**.5)}
    assert describe([])["sample_sd"] is None


def test_graphs_render_from_validated_csv(tmp_path):
    make_fixture(tmp_path)
    _, valid, excluded, *_ = load_run(tmp_path)
    assert not excluded
    rows = aggregate(valid)
    curves(rows, tmp_path)
    comparison(rows, tmp_path)
    pngs = list(tmp_path.glob("*.png"))
    assert len(pngs) == 5 and all(path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n") for path in pngs)

