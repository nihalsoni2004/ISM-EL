"""Run a rotated Release comparison in a newly reserved results group."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import platform
import subprocess
import uuid

PROJECT = Path(__file__).resolve().parents[2]
BENCH = PROJECT / "benchmarks"
LEVELS = (0, 1, 5, 6)
WORKLOADS = ("RANDOM_READ", "RANDOM_WRITE", "SEQUENTIAL_READ", "SEQUENTIAL_WRITE", "MIXED_RW")


def plan(operations, warmup, repetitions, thin_operations, seed, thin_warmup=8, baseline_operations=160):
    limits = {"max_image_mib": 64, "max_disks": 8, "max_operations": 20000,
              "max_raw_csv_bytes": 16 * 1024 * 1024, "timeout_seconds": 300}
    common = {"schema_version": 1, "capacity_mode": "EQUAL_RAW", "target_capacity_mib": 32,
              "source": "MEASURED", "footprint_blocks": 1536, "queue_depth": 1,
              "seed": seed, "repetitions": repetitions, "validation": "SAMPLED", "limits": limits}
    configs = []
    for index, workload in enumerate(WORKLOADS):
        rotated = LEVELS[index % len(LEVELS):] + LEVELS[:index % len(LEVELS)]
        for level in rotated:
            configs.append({**common, "benchmark_name": f"raid{level}_{workload.lower()}",
                            "raid_level": level, "workload": workload, "mixed_read_percent": 70,
                            "operation_count": operations, "warmup_operations": warmup,
                            "request_size_bytes": 262144 if workload.startswith("SEQUENTIAL") else 4096})
    for workload in ("THIN_FIRST_WRITE", "THIN_OVERWRITE"):
        configs.append({**common, "benchmark_name": f"raid5_{workload.lower()}", "raid_level": 5,
                        "workload": workload, "operation_count": thin_operations,
                        "warmup_operations": thin_warmup, "request_size_bytes": 4096})
    for contention in (False, True):
        configs.append({**common, "benchmark_name": "raid1_rebuild_read" if contention else "raid1_healthy_read",
                        "raid_level": 1, "target_capacity_mib": 16, "workload": "READ_ONLY_BASELINE",
                        "operation_count": baseline_operations, "warmup_operations": min(warmup, 16),
                        "request_size_bytes": 4096, "rebuild_contention": contention})
    return configs


def timestamp():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=PROJECT / "build/msvc-debug/Release/raid_simulator.exe")
    parser.add_argument("--operations", type=int, default=1000)
    parser.add_argument("--warmup", type=int, default=100)
    parser.add_argument("--repetitions", type=int, default=5)
    parser.add_argument("--thin-operations", type=int, default=64)
    parser.add_argument("--thin-warmup", type=int, default=8)
    parser.add_argument("--baseline-operations", type=int, default=160)
    parser.add_argument("--seed", type=int, default=12345)
    parser.add_argument("--scale-note", default="Configured comparison matrix.")
    args = parser.parse_args()
    if not args.exe.is_file():
        parser.error("Build the Release simulator first.")
    if (args.operations <= 0 or args.warmup < 0 or not 1 <= args.repetitions <= 20
            or args.thin_operations <= 0 or args.thin_warmup < 0 or args.thin_operations + args.thin_warmup > 96
            or args.baseline_operations <= 0 or not 0 <= args.seed < 2**64):
        parser.error("Requested workload sizes are invalid or exceed the thin footprint.")
    configs = plan(args.operations, args.warmup, args.repetitions, args.thin_operations, args.seed,
                   args.thin_warmup, args.baseline_operations)
    group = f"matrix-{datetime.now(timezone.utc):%Y%m%dT%H%M%SZ}-{uuid.uuid4().hex[:8]}"
    results = BENCH / "results" / group
    results.mkdir()
    copied_configs = results / "configs"
    copied_configs.mkdir()
    for config in configs:
        path = copied_configs / (config["benchmark_name"] + ".json")
        with path.open("x", encoding="utf-8") as stream:
            json.dump(config, stream, indent=2)
            stream.write("\n")
        subprocess.run([str(args.exe.resolve()), "benchmark", "validate", "--config", str(path),
                        "--output-dir", group], stdout=subprocess.DEVNULL, cwd=PROJECT, check=True)
    print(f"Matrix results {results}", flush=True)
    summary = {"schema_version": 1, "matrix_id": group, "source": "MEASURED", "started_at_utc": timestamp(),
               "python": platform.python_version(), "scale_note": args.scale_note,
               "ordering": "Cyclic rotation of RAID levels across workloads. Repetitions use fresh storage.",
               "planned": [config["benchmark_name"] for config in configs], "executed": []}
    try:
        for index, config in enumerate(configs, 1):
            name = config["benchmark_name"]
            path = copied_configs / (name + ".json")
            process = subprocess.run([str(args.exe.resolve()), "benchmark", "run", "--config", str(path),
                                      "--output-dir", group, "--matrix-id", group, "--matrix-order", str(index),
                                      "--python-version", platform.python_version()],
                                     cwd=PROJECT, capture_output=True, text=True, encoding="utf-8")
            record = json.loads(process.stdout) if process.stdout else {"status": "FAILED", "stderr": process.stderr}
            summary["executed"].append({"order": index, "configuration": name, **record})
            print(f"{index}/{len(configs)} {name} {record['status']}", flush=True)
            if process.returncode:
                raise RuntimeError(process.stderr or f"Failed configuration {name}")
            manifest = json.loads((results / record["run_id"] / "manifest.json").read_text(encoding="utf-8"))
            if manifest["build_type"] != "Release":
                raise RuntimeError("The recorded benchmark build is not Release")
        summary["status"] = "COMPLETED"
    finally:
        summary.setdefault("status", "FAILED")
        summary["finished_at_utc"] = timestamp()
        with (results / "matrix_manifest.json").open("x", encoding="utf-8") as stream:
            json.dump(summary, stream, indent=2)
            stream.write("\n")


if __name__ == "__main__":
    main()
