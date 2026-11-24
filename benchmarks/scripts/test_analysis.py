import csv
import json
from pathlib import Path

import pytest

from analyze import GEOMETRY, RAW, SUMMARY, aggregate, load_directory, rank, read_csv
from plot_benchmarks import generate, plot_thin
from run_matrix import plan


def write_rows(path, columns, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)


def fixture(group, *, failed=False, validation="FULL", source="MEASURED"):
    run = group / "run-fixture"
    run.mkdir(parents=True)
    config = {
        "benchmark_name": 'bench, "fixture"', "raid_level": 5, "disk_count": 4,
        "stripe_unit_blocks": 16, "capacity_mode": "PER_MEMBER", "footprint_blocks": 128,
        "workload": "RANDOM_WRITE", "request_size_bytes": 4096, "operation_count": 1,
        "warmup_operations": 0, "seed": 17, "mixed_read_percent": 70, "thin_offset_blocks": 0,
        "validation": validation, "validation_stride": 16, "rebuild_contention": False, "repetitions": 1,
    }
    status = "FAILED" if failed else "COMPLETED"
    checked = "INCOMPLETE" if failed else "NOT_CHECKED" if validation == "NONE" else "PASSED"
    error = "STORAGE_ERROR" if failed else ""
    physical = {"physical_read_requests": 2, "physical_write_requests": 2, "physical_read_blocks": 2,
                "physical_write_blocks": 2 if not failed else 0, "physical_bytes_read": 8192,
                "physical_bytes_written": 8192 if not failed else 0}
    common = {"schema_version": 1, "run_id": run.name, "repetition": 1,
              **{key: config[key] for key in ("benchmark_name", "raid_level", "workload", "request_size_bytes")}}
    row = {
        **common, **{key: config[key] for key in GEOMETRY if key in config}, **physical,
        "disk_capacity_mib": 1, "raid_raw_capacity_bytes": 4 * 1048576, "raid_usable_capacity_bytes": 3 * 1048576,
        "queue_depth": 1, "cache_policy": "HOST_CACHE_ENABLED_PREPARED", "build_type": "Release", "compiler": "compiler",
        "measurement_source": source, "rebuild_contention": "false", "elapsed_seconds": 0.02,
        "preparation_seconds": 0.01, "warmup_seconds": 0, "attempted_operations": 1,
        "completed_operations": 0 if failed else 1, "failed_operations": 1 if failed else 0,
        "logical_bytes": 0 if failed else 4096, "logical_written_blocks": 0 if failed else 1,
        "iops": 0 if failed else 50, "throughput_mib_s": 0 if failed else 4096 / 1048576 / 0.02,
        **{key: 1 for key in ("latency_count", "latency_min_ms", "latency_max_ms", "latency_mean_ms", "latency_p50_ms",
                            "latency_p95_ms", "latency_p99_ms")},
        "write_amplification_blocks": "" if failed else 2, "new_chunks": 0,
        "rebuilt_blocks": 0, "rebuild_total_blocks": 0, "rebuild_overlap_requests": 0,
        "validation_status": checked, "status": status, "error": error,
    }
    raw = {**common, **physical, "sequence": 0, "operation": "WRITE", "logical_lba": 1, "logical_blocks": 1,
           "pattern_ref": 1, "duration_ns": 1_000_000, "success": "false" if failed else "true",
           "validation_passed": "" if failed or validation == "NONE" else "true",
           "logical_bytes_completed": 0 if failed else 4096, "new_chunks": 0, "rebuild_overlap": "false",
           "source": source, "error": error}
    manifest = {"schema_version": 1, "run_id": run.name, "measurement_source": source, "configuration": config,
                "build_type": row["build_type"], "compiler": row["compiler"], "raw_capacity_bytes": row["raid_raw_capacity_bytes"],
                "usable_capacity_bytes": row["raid_usable_capacity_bytes"], "cache_policy": row["cache_policy"],
                "host": {"cpu_identifier": None, "os": "Windows"}, "cmake_version": "test",
                "status": status, "raw_csv": "benchmark_runs.csv", "summary_csv": "benchmark_results.csv",
                "repetitions": [{"repetition": 1, "status": status, "validation_status": checked, "error": error}]}
    (run / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    write_rows(run / "benchmark_results.csv", SUMMARY, [row])
    write_rows(run / "benchmark_runs.csv", RAW, [raw])
    return run


def test_valid_csv_and_full_run_percentiles(tmp_path):
    fixture(tmp_path)
    dataset = load_directory(tmp_path)
    assert not dataset.excluded and len(dataset.valid) == 1
    row = dataset.valid[0]
    assert row["latency_p99_ms"] == 1 and row["benchmark_name"] == 'bench, "fixture"'
    assert row["iops"] == 50 and row["write_amplification_blocks"] == 2
    assert row["write_path_read_blocks_per_logical_write_block"] == 2
    assert rank(list(range(1, 12001)), 99) == 11880


@pytest.mark.parametrize("field,bad", [("iops", "NaN"), ("throughput_mib_s", "-1"),
                                        ("latency_p99_ms", "2"), ("completed_operations", "1.5")])
def test_reject_invalid_or_inconsistent_numbers(tmp_path, field, bad):
    folder = fixture(tmp_path)
    table = folder / "benchmark_results.csv"
    row, = read_csv(table, SUMMARY)
    row[field] = bad
    write_rows(table, SUMMARY, [row])
    with pytest.raises(ValueError):
        load_directory(tmp_path)


@pytest.mark.parametrize("flags", [{"failed": True}, {"validation": "NONE"}])
def test_excludes_unsuccessful_or_unvalidated_repetitions(tmp_path, flags):
    fixture(tmp_path, **flags)
    data = load_directory(tmp_path)
    assert not data.valid and len(data.excluded) == 1
    assert data.excluded[0]["validation_status"] in ("INCOMPLETE", "NOT_CHECKED")


def test_disallows_mixing_model_and_measured_data(tmp_path):
    fixture(tmp_path, source="MODELED")
    with pytest.raises(ValueError, match="MEASURED"):
        load_directory(tmp_path)


def test_aggregate_preserves_geometry_build_cache_and_statistics(tmp_path):
    fixture(tmp_path)
    sample = load_directory(tmp_path).valid[0]
    measurements = [sample, {**sample, "iops": 54}, {**sample, "build_type": "Debug"},
                    {**sample, "cache_policy": "OTHER"}, {**sample, "disk_capacity_mib": "99"}]
    grouped = aggregate(measurements)
    assert len(grouped) == 4
    matching = next(row for row in grouped if row["valid_repetitions"] == 2)
    assert matching["mean_iops"] == matching["median_iops"] == 52
    assert matching["sd_iops"] == pytest.approx(8**0.5)
    assert matching["mean_latency_p99_ms"] == 1


def test_plots_and_empty_selection(tmp_path):
    folder = tmp_path / "inputs"
    fixture(folder)
    current = load_directory(folder).valid[0]
    rows = []
    for workload in ("RANDOM_READ", "RANDOM_WRITE", "SEQUENTIAL_READ", "SEQUENTIAL_WRITE", "MIXED_RW"):
        for level in (0, 1, 5, 6):
            for trial in (0, 1):
                rows.append({**current, "raid_level": str(level), "workload": workload, "iops": 50 + trial,
                             "request_size_bytes": str(262144 if workload.startswith("SEQUENTIAL") else 4096)})
    rows.extend({**current, "workload": mode} for mode in ("THIN_FIRST_WRITE", "THIN_OVERWRITE"))
    output = tmp_path / "plots"
    output.mkdir()
    generate([], output, "empty")
    assert not list(output.iterdir()) and not load_directory(Path(output)).valid
    generate(aggregate(rows), output, "fixture")
    plots = sorted(output.glob("*.png"))
    assert len(plots) == 6 and all(path.read_bytes().startswith(b"\x89PNG\r\n\x1a\n") for path in plots)


def test_matrix_rotates_levels_and_matches_thin_geometry():
    configs = plan(100, 16, 5, 64, 12345)
    assert len(configs) == 24
    assert [configs[index]["raid_level"] for index in range(0, 16, 4)] == [0, 1, 5, 6]
    assert configs[20]["operation_count"] == configs[21]["operation_count"] == 64
    assert configs[-1]["workload"] == "READ_ONLY_BASELINE" and configs[-1]["rebuild_contention"] is True


def test_thin_plot_axis_covers_observed_latency_and_error_bars(tmp_path, monkeypatch):
    folder = tmp_path / "inputs"
    fixture(folder)
    current = load_directory(folder).valid[0]
    thin = aggregate([{**current, "workload": "THIN_FIRST_WRITE", "latency_p99_ms": value} for value in (24, 30)])
    import plot_benchmarks
    observed = []

    def inspect(fig, *args, **kwargs):
        observed.append(fig.axes[0].get_ylim())
        plot_benchmarks.plt.close(fig)

    monkeypatch.setattr(plot_benchmarks, "save", inspect)
    plot_thin(thin, tmp_path / "thin.png", "fixture")
    assert observed[0][0] == 0 and observed[0][1] > 30
