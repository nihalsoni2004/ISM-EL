"""Generate Step 14 research figures from validated, actual C++ measurements."""
import argparse
import itertools
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

from analyze_rebuild import aggregate, load_run, number

LABELS = {"FULL_SPEED": "Full-speed", "FIXED_THROTTLE": "Fixed throttle", "ADAPTIVE": "Adaptive",
          "DEGRADED_ONLY": "Degraded only"}
COLORS = {"FULL_SPEED": "#225e91", "FIXED_THROTTLE": "#af4d12", "ADAPTIVE": "#087762", "DEGRADED_ONLY": "#646978"}
CURVES = (
    ("foreground_p95_ms", "foreground-p95.png", "Foreground response p95 (ms)"),
    ("foreground_p99_ms", "foreground-p99.png", "Foreground response p99 (ms)"),
    ("foreground_iops", "foreground-iops.png", "Completed foreground IOPS"),
    ("rebuild_elapsed_seconds", "rebuild-time.png", "Rebuild completion time (s)"),
)


def style(axis, xlabel, ylabel):
    axis.set(xlabel=xlabel, ylabel=ylabel)
    axis.grid(axis="y", alpha=.25)
    axis.spines[["top", "right"]].set_visible(False)


def save(figure, directory, name):
    figure.savefig(directory / name, dpi=180, bbox_inches="tight", facecolor="white")
    plt.close(figure)


def curves(rows, figure_dir):
    for metric, filename, ylabel in CURVES:
        fig, axis = plt.subplots(figsize=(7.4, 4.6), layout="constrained")
        for policy, label in LABELS.items():
            points = sorted((r for r in rows if r["policy"] == policy and r["metric"] == metric
                             and r["mean"] is not None and r["offered_iops"] is not None),
                            key=lambda row: row["offered_iops"])
            if points:
                axis.errorbar([r["offered_iops"] for r in points], [r["mean"] for r in points],
                              yerr=[r["sample_sd"] or 0 for r in points], color=COLORS[policy],
                              label=label, marker="o", markersize=4, capsize=4)
        axis.set_ylim(bottom=0)
        style(axis, "Scheduled foreground reads per second", ylabel)
        axis.set_title("MEASURED · Mean of trial metrics with sample SD", loc="left", fontweight="bold")
        if axis.get_legend_handles_labels()[0]:
            axis.legend(frameon=False)
        save(fig, figure_dir, filename)


def tradeoff(valid, destination):
    fig, axis = plt.subplots(figsize=(7.4, 4.7), layout="constrained")
    markers = itertools.cycle(("o", "D", "^", "s", "P"))
    rates = sorted({number(row, "offered_iops") for row in valid if number(row, "offered_iops") not in (None, 0)})
    for rate, marker in zip(rates, markers):
        for policy, label in LABELS.items():
            points = [(number(row, "rebuild_elapsed_seconds"), number(row, "foreground_p95_ms"))
                      for row in valid if row["policy"] == policy and number(row, "offered_iops") == rate]
            points = [(x, y) for x, y in points if x is not None and y is not None]
            if points:
                axis.scatter(*zip(*points), marker=marker, alpha=.7, color=COLORS[policy],
                             label=f"{label} · {rate:g} IOPS", s=24)
    style(axis, "Rebuild completion time (s)", "Foreground response p95 (ms)")
    axis.set_title("MEASURED · Latency and completion time by trial", loc="left", fontweight="bold")
    if axis.get_legend_handles_labels()[0]:
        axis.legend(ncol=2, fontsize=8, frameon=False)
    save(fig, destination, "latency-time-tradeoff.png")


def progress_plot(valid, progress, destination):
    fig, axis = plt.subplots(figsize=(7.4, 4.6), layout="constrained")
    candidates = [r for r in valid if r["repetition"] == "1" and r["policy"] != "DEGRADED_ONLY"
                  and number(r, "offered_iops") is not None]
    rate = max((number(row, "offered_iops") for row in candidates), default=None)
    for row in candidates:
        if number(row, "offered_iops") != rate:
            continue
        events = [event for event in progress[row["trial"]] if event["worker_state"] != "VERIFICATION"]
        axis.step([number(event, "elapsed_ms") / 1000 for event in events],
                  [100 * number(event, "completed_blocks") / number(event, "total_blocks") for event in events],
                  where="post", color=COLORS[row["policy"]], label=LABELS[row["policy"]])
        axis.scatter([number(events[-1], "elapsed_ms") / 1000], [100], color=COLORS[row["policy"]], s=21)
    axis.set_ylim(0, 106)
    style(axis, "Time since RUNNING checkpoint (s)", "Durable reconstructed blocks (%)")
    axis.set_title(f"MEASURED · Matched progress, repetition 1 ({rate:g} offered IOPS)" if rate is not None
                   else "MEASURED · No eligible matched progress", loc="left", fontweight="bold")
    if axis.get_legend_handles_labels()[0]:
        axis.legend(frameon=False)
    save(fig, destination, "matched-rebuild-progress.png")


def adaptive_trace(directory, variable_run):
    manifest, valid, excluded, requests, progress, decisions = load_run(variable_run)
    if len(valid) != 1 or excluded or valid[0]["policy"] != "ADAPTIVE":
        raise ValueError("Adaptive trace requires one validated variable-load trial")
    row = valid[0]
    phases = manifest["configuration"]["loads"][0]["phases"]
    events = decisions[row["trial"]]
    fig, axes = plt.subplots(3, 1, figsize=(8.8, 7.8), sharex=True, layout="constrained")
    elapsed = 0
    for i, phase in enumerate(phases):
        end = elapsed + phase["duration_ms"] / 1000
        for axis in axes:
            axis.axvspan(elapsed, end, color="#e2e8f0", alpha=.56 if i % 2 == 0 else .2)
        if i:
            for axis in axes:
                axis.axvline(elapsed, color="#8693a0", lw=.7)
        axes[0].text((elapsed + end) / 2, .94, f"{phase['offered_iops']} IOPS",
                     transform=axes[0].get_xaxis_transform(), ha="center", va="top", fontsize=8)
        elapsed = end
    t = [number(event, "control_time_ms") / 1000 for event in events]
    latency = [number(event, "foreground_p95_ms") for event in events]
    axes[0].plot(t, [float("nan") if v is None else v for v in latency], ".-", color=COLORS["FULL_SPEED"],
                 label="Observed foreground p95")
    raw = requests[row["trial"]]
    axes[0].scatter([number(req, "completion_ns") / 1e9 for req in raw],
                    [number(req, "response_latency_ns") / 1e6 for req in raw], s=6, alpha=.28,
                    color=COLORS["DEGRADED_ONLY"], label="Individual response")
    axes[0].legend(frameon=False, loc="upper right", bbox_to_anchor=(1, .79), fontsize=8)
    axes[1].step(t, [number(event, "configured_delay_ms") for event in events], where="post", color=COLORS["ADAPTIVE"])
    changes = [0, *(i for i in range(1, len(events)) if events[i]["pressure_state"] != events[i-1]["pressure_state"])]
    axes[1].scatter([t[i] for i in changes], [number(events[i], "configured_delay_ms") for i in changes],
                    s=20, color=COLORS["ADAPTIVE"], label="Pressure state changed")
    axes[1].legend(frameon=False, loc="upper right", fontsize=8)
    checkpoints = progress[row["trial"]]
    axes[2].step([number(event, "elapsed_ms") / 1000 for event in checkpoints],
                 [number(event, "completed_blocks") for event in checkpoints], color=COLORS["ADAPTIVE"], where="post")
    for axis, label in zip(axes, ("Latency (ms)", "Configured delay (ms)", "Durable blocks")):
        style(axis, "", label)
        axis.set_ylim(bottom=0)
    axes[-1].set_xlabel("Time since RUNNING checkpoint (s)")
    fig.suptitle("MEASURED · Adaptive response to changing foreground load", fontweight="bold")
    save(fig, directory, "adaptive-variable-load.png")


def comparison(rows, directory):
    chosen = ("foreground_p95_ms", "foreground_p99_ms", "foreground_iops", "rebuild_elapsed_seconds")
    lookup = {(r["policy"], r["foreground_load"], r["metric"]): r for r in rows}
    groups = sorted({(r["offered_iops"], r["foreground_load"]) for r in rows if r["offered_iops"] is not None})
    cells, colors = [], []
    for rate, load in groups:
        for policy in LABELS:
            if (policy, load, chosen[0]) not in lookup:
                continue
            values = [lookup[policy, load, metric] for metric in chosen]
            cells.append([load.title(), LABELS[policy], *(f"{r['mean']:.2f}" if r["mean"] is not None else "—" for r in values)])
            colors.append(["#edf3f8" if len(cells) % 2 else "#ffffff"] * 6)
    if not cells:
        raise ValueError("Policy comparison needs a constant-rate study")
    fig, axis = plt.subplots(figsize=(8.7, .31 * len(cells) + 1.5), layout="constrained")
    axis.set_axis_off()
    axis.set_title("MEASURED · Mean across validated repetitions", loc="left", fontweight="bold", pad=16)
    matrix = axis.table(cellText=cells, colLabels=["Load", "Policy", "p95 (ms)", "p99 (ms)", "Read IOPS", "Rebuild (s)"],
                        cellColours=colors, colColours=["#d0e2ed"] * 6, cellLoc="left", colLoc="left",
                        colWidths=[.12, .23, .16, .16, .16, .16], bbox=[0, 0, 1, 1])
    matrix.auto_set_font_size(False)
    matrix.set_fontsize(9)
    for (row, _), cell in matrix.get_celld().items():
        cell.set_edgecolor("white")
        if row == 0:
            cell.set_text_props(weight="bold")
    save(fig, directory, "policy-comparison.png")


def plot_study(run, variable, output):
    _, valid, excluded, _, progress, _ = load_run(run)
    if not valid:
        raise ValueError("No validated trials to plot")
    output = Path(output)
    output.mkdir(exist_ok=False)
    plt.rcParams.update({"font.family": "DejaVu Sans", "axes.labelsize": 10, "axes.titlesize": 12})
    rows = aggregate(valid)
    curves(rows, output)
    tradeoff(valid, output)
    progress_plot(valid, progress, output)
    adaptive_trace(output, variable)
    comparison(rows, output)
    if excluded:
        print(f"Plotted {len(valid)} valid trials. See the exclusions CSV for {len(excluded)} excluded trials.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("--variable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    plot_study(args.run, args.variable, args.output)

