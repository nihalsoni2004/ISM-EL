"""Publication plots for the measured, validated benchmark aggregates."""
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from run_matrix import WORKLOADS

LABELS = dict(zip(WORKLOADS, (
    "4 KiB random read", "4 KiB random write", "256 KiB sequential read",
    "256 KiB sequential write", "4 KiB mixed read/write",
)))
PALETTE = ("#176783", "#397f69", "#bf6b2a", "#80599a")
INK = "#153044"


def style(axis, units):
    axis.spines[["top", "right"]].set_visible(False)
    axis.spines[["left", "bottom"]].set_color("#92a4ad")
    axis.set_axisbelow(True)
    axis.grid(axis="y", color="#e0e8ed", linewidth=0.7)
    axis.set_ylim(bottom=0)
    axis.set_ylabel(units)
    axis.tick_params(colors=INK, labelsize=9)


def save(fig, path, title, caption, bottom=0.13):
    fig.suptitle(title, x=0.075, y=0.985, ha="left", fontsize=17, fontweight="bold", color=INK)
    fig.text(0.075, 0.018, caption, color="#405869", fontsize=8, ha="left")
    fig.subplots_adjust(left=0.075, right=0.98, top=0.88, bottom=bottom, hspace=0.58, wspace=0.28)
    fig.savefig(path, dpi=220, facecolor="white")
    plt.close(fig)


def plot_raid(rows, path, metric, title, units, matrix):
    groups = []
    for workload in WORKLOADS:
        selected = sorted((row for row in rows if row["workload"] == workload and row[f"mean_{metric}"] is not None),
                          key=lambda row: int(row["raid_level"]))
        if not selected:
            continue
        definitions = {(row["capacity_mode"], row["footprint_blocks"], row["queue_depth"], row["request_size_bytes"],
                        row["cache_policy"], row["build_type"], row["host_key"], row["seed"], row["operation_count"],
                        row["warmup_operations"], row["mixed_read_percent"]) for row in selected}
        if len(definitions) != 1 or len({row["raid_level"] for row in selected}) != len(selected):
            raise ValueError("Plot requires one comparable result per RAID level and workload")
        groups.append((workload, selected))
    if not groups:
        return

    ncols = min(3, len(groups))
    nrows = (len(groups) + ncols - 1) // ncols
    fig, axes = plt.subplots(nrows, ncols, figsize=(4.25 * ncols, 3.4 * nrows + 0.5), squeeze=False)
    for axis, (workload, selected) in zip(axes.flat, groups):
        style(axis, units)
        rates = []
        for index, row in enumerate(selected):
            mean, deviation = row[f"mean_{metric}"], row[f"sd_{metric}"]
            raid = int(row["raid_level"])
            color = PALETTE[(0, 1, 5, 6).index(raid)]
            axis.bar(index, mean, width=0.6, color=color, yerr=deviation, capsize=4, edgecolor="none",
                     error_kw={"elinewidth": 1, "ecolor": INK})
            top = mean + (deviation or 0)
            rates.append(top)
            label = f"{mean:,.0f}" if mean >= 100 else f"{mean:.3g}"
            axis.annotate(label, (index, top), xytext=(0, 5), textcoords="offset points",
                          ha="center", color=INK, fontsize=8)
        axis.set_ylim(0, max(rates) * 1.23 if max(rates) else 1)
        axis.set_xticks(range(len(selected)), [f"RAID {row['raid_level']}" for row in selected])
        axis.set_title(LABELS[workload], loc="left", color=INK, fontsize=11, fontweight="semibold")
    for axis in list(axes.flat)[len(groups):]:
        axis.set_visible(False)
    save(fig, path, title, f"MEASURED  |  Means of validated repetitions; error bars are sample SD  |  {matrix}")


def plot_thin(rows, path, matrix):
    selected = sorted((row for row in rows if row["workload"] in ("THIN_FIRST_WRITE", "THIN_OVERWRITE")),
                      key=lambda row: row["workload"])
    if not selected:
        return
    if len({row["workload"] for row in selected}) != len(selected):
        raise ValueError("Thin latency plot requires a single matched comparison")
    if len({(row["raid_level"], row["disk_count"], row["disk_capacity_mib"], row["stripe_unit_blocks"],
             row["footprint_blocks"], row["request_size_bytes"], row["thin_offset_blocks"], row["cache_policy"],
             row["build_type"], row["seed"], row["operation_count"], row["warmup_operations"]) for row in selected}) != 1:
        raise ValueError("Thin latency plot requires matched request, geometry, and run settings")
    fig, axis = plt.subplots(figsize=(7.6, 4.3))
    style(axis, "Write latency (ms)")
    top = 0
    for group, pct in enumerate((50, 95, 99)):
        for index, row in enumerate(selected):
            x = group - 0.18 + index * 0.36
            label = "First allocation" if row["workload"] == "THIN_FIRST_WRITE" else "Allocated overwrite"
            mean = row[f"mean_latency_p{pct}_ms"]
            deviation = row[f"sd_latency_p{pct}_ms"]
            axis.bar(x, mean, 0.30, color=PALETTE[index], yerr=deviation, capsize=4,
                     error_kw={"ecolor": INK, "elinewidth": 1}, label=label if group == 0 else None)
            maximum = mean + (deviation or 0)
            top = max(top, maximum)
            axis.annotate(f"{mean:.2f}", (x, maximum), xytext=(0, 5), textcoords="offset points",
                          ha="center", color=INK, fontsize=8)
    axis.set_ylim(0, top * 1.28 if top else 1)
    axis.set_xticks(range(3), ("p50", "p95", "p99"))
    axis.set_xlabel("Full-repetition percentile")
    axis.legend(frameon=False, ncols=2, loc="upper left")
    save(fig, path, "Thin provisioning write latency",
         f"MEASURED  |  RAID {selected[0]['raid_level']}, {int(selected[0]['request_size_bytes']) // 1024} KiB requests  |  Means ± sample SD  |  {matrix}",
         bottom=0.19)


def generate(rows, output, matrix):
    for filename, key, title, units in (
        ("raid_iops.png", "iops", "Foreground RAID request rate", "Completed requests/s"),
        ("raid_throughput.png", "throughput_mib_s", "Foreground RAID logical throughput", "Logical MiB/s"),
        ("raid_p95.png", "latency_p95_ms", "Foreground RAID p95 request latency", "p95 latency (ms)"),
        ("raid_p99.png", "latency_p99_ms", "Foreground RAID p99 request latency", "p99 latency (ms)"),
        ("raid_write_amplification.png", "write_amplification_blocks", "RAID physical write amplification", "Written blocks per logical write block"),
    ):
        plot_raid(rows, output / filename, key, title, units, matrix)
    plot_thin(rows, output / "thin_write_latency.png", matrix)
