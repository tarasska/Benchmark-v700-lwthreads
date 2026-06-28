#!/usr/bin/env python3
"""
plot_sweep.py — compare multiple DS targets across a coroutine sweep.

Directory layout expected (produced by run_coroutine_sweep.sh):
  results_dir/
    config_cops<N>.json          # shared configs (informational)
    <ds_name>/
      result_cops<N>.json        # one per (ds, coroutine_count)
    <ds_name>/
      result_cops<N>.json
    ...

Usage:
  python3 plot_sweep.py --results-dir sweep_results
  python3 plot_sweep.py --results-dir sweep_results --ds treiber_stack_fc treiber_stack
  python3 plot_sweep.py --results-dir sweep_results --stat throughput per_thread
  python3 plot_sweep.py --results-dir sweep_results --output-dir plots/
"""

import argparse
import json
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# ── style ────────────────────────────────────────────────────────────────────

# Distinct colours for up to 8 targets; extend if you have more
DS_COLORS = [
    "#4C8EDA",  # blue
    "#E06C4B",  # orange-red
    "#3BAA72",  # green
    "#9B6DD4",  # purple
    "#E0B84B",  # amber
    "#4BC7CE",  # teal
    "#D45E8A",  # pink
    "#7A7A7A",  # grey
]
# Distinct markers so plots are readable in B&W too
DS_MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]

GRID_COLOR  = "#E8E8E8"
SPINE_COLOR = "#CCCCCC"

plt.rcParams.update({
    "font.family":       "monospace",
    "axes.spines.top":   False,
    "axes.spines.right": False,
    "axes.grid":         True,
    "grid.color":        GRID_COLOR,
    "grid.linewidth":    0.8,
    "figure.dpi":        150,
})

RESULT_PAT = re.compile(r"result_cops(\d+)\.json")


# ── data loading ─────────────────────────────────────────────────────────────

def load_ds(ds_dir):
    """
    Load all result_cops<N>.json from a single DS directory.
    Returns a list of dicts sorted by coroutine count.
    """
    records = []
    for p in sorted(ds_dir.glob("result_cops*.json")):
        m = RESULT_PAT.match(p.name)
        if not m:
            continue
        cops = int(m.group(1))
        with open(p) as f:
            data = json.load(f)
        data["coroutines"] = cops
        run_time_ms = data.get("max_time_thread_terminate_total", 0) / 1e6
        total_ops   = data.get("sum_num_operations_total", 0)
        data["throughput_ops_per_sec"] = (
            total_ops / run_time_ms if run_time_ms > 0 else 0
        )
        records.append(data)
    records.sort(key=lambda r: r["coroutines"])
    return records


def discover_targets(results_dir, requested):
    """
    Returns {ds_name: [records]} for each DS subdirectory that has results.
    If `requested` is non-empty, only those names are included.
    """
    targets = {}
    for child in sorted(results_dir.iterdir()):
        if not child.is_dir():
            continue
        if requested and child.name not in requested:
            continue
        records = load_ds(child)
        if records:
            targets[child.name] = records
    return targets


# ── helpers ──────────────────────────────────────────────────────────────────

def style_ax(ax):
    for spine in ("left", "bottom"):
        ax.spines[spine].set_color(SPINE_COLOR)
    ax.tick_params(colors="#555555")
    ax.yaxis.label.set_color("#333333")
    ax.xaxis.label.set_color("#333333")
    ax.title.set_color("#222222")


def all_coroutine_ticks(targets):
    ticks = set()
    for records in targets.values():
        for r in records:
            ticks.add(r["coroutines"])
    return sorted(ticks)


# ── plots ─────────────────────────────────────────────────────────────────────

def plot_throughput(targets, output_dir):
    fig, ax = plt.subplots(figsize=(9, 5))

    for i, (ds, records) in enumerate(targets.items()):
        x = [r["coroutines"]            for r in records]
        y = [r["throughput_ops_per_sec"] for r in records]
        color  = DS_COLORS[i % len(DS_COLORS)]
        marker = DS_MARKERS[i % len(DS_MARKERS)]
        ax.plot(x, y, marker=marker, linewidth=2, label=ds,
                color=color, markersize=6, markeredgewidth=0)
        ax.fill_between(x, y, alpha=0.06, color=color)

    ticks = all_coroutine_ticks(targets)
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_xticks(ticks)
    ax.set_xlabel("coroutines per thread", fontsize=10)
    ax.set_ylabel("throughput  (ops / s)", fontsize=10)
    ax.set_title("throughput vs coroutines", fontsize=11, fontweight="bold")
    ax.legend(framealpha=0.4, fontsize=9)
    style_ax(ax)
    fig.tight_layout()

    out = output_dir / "throughput_vs_coroutines.png"
    fig.savefig(out)
    plt.close(fig)
    print("  saved:", out)


def plot_push_pop(targets, output_dir):
    fig, axes = plt.subplots(1, 2, figsize=(14, 5), sharey=False)
    ax_push, ax_pop = axes

    for i, (ds, records) in enumerate(targets.items()):
        x       = [r["coroutines"]               for r in records]
        pushes  = [r.get("sum_num_pushes_total", 0) for r in records]
        pops    = [r.get("sum_num_pops_total",   0) for r in records]
        color  = DS_COLORS[i % len(DS_COLORS)]
        marker = DS_MARKERS[i % len(DS_MARKERS)]
        ax_push.plot(x, pushes, marker=marker, linewidth=2, label=ds,
                     color=color, markersize=6)
        ax_pop.plot( x, pops,   marker=marker, linewidth=2, label=ds,
                     color=color, markersize=6)

    ticks = all_coroutine_ticks(targets)
    for ax, title in [(ax_push, "total pushes"), (ax_pop, "total pops")]:
        ax.set_xscale("log", base=2)
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
        ax.set_xticks(ticks)
        ax.set_xlabel("coroutines per thread", fontsize=10)
        ax.set_ylabel("total operations", fontsize=10)
        ax.set_title(title, fontsize=11, fontweight="bold")
        ax.legend(framealpha=0.4, fontsize=9)
        style_ax(ax)

    fig.tight_layout()
    out = output_dir / "push_pop_vs_coroutines.png"
    fig.savefig(out)
    plt.close(fig)
    print("  saved:", out)


def plot_per_thread_boxplot(targets, output_dir):
    """
    One subplot per DS, box per coroutine count.
    Shows how evenly work is spread across threads.
    """
    n = len(targets)
    if n == 0:
        return

    fig, axes = plt.subplots(1, n, figsize=(5 * n, 5), sharey=False)
    if n == 1:
        axes = [axes]

    for ax, (i, (ds, records)) in zip(axes, enumerate(targets.items())):
        labels = []
        dists  = []
        for r in records:
            per_thread = r.get("sum_num_operations_by_thread")
            if not per_thread:
                continue
            dists.append(per_thread)
            labels.append(str(r["coroutines"]))

        if not dists:
            ax.set_title(f"{ds}\n(no per-thread data)")
            continue

        color = DS_COLORS[i % len(DS_COLORS)]
        bp = ax.boxplot(
            dists,
            labels=labels,
            patch_artist=True,
            medianprops=dict(color="#222222", linewidth=2),
            whiskerprops=dict(color=SPINE_COLOR),
            capprops=dict(color=SPINE_COLOR),
            flierprops=dict(marker="x", color="#B0B0B0", markersize=5),
        )
        for patch in bp["boxes"]:
            patch.set_facecolor(color + "33")
            patch.set_edgecolor(color)

        ax.set_title(ds, fontsize=10, fontweight="bold")
        ax.set_xlabel("coroutines per thread", fontsize=9)
        ax.set_ylabel("ops per thread", fontsize=9)
        style_ax(ax)

    fig.suptitle("per-thread op distribution", fontsize=11, fontweight="bold", y=1.02)
    fig.tight_layout()
    out = output_dir / "per_thread_distribution.png"
    fig.savefig(out, bbox_inches="tight")
    plt.close(fig)
    print("  saved:", out)


def print_summary(targets, output_dir):
    col = 14
    ds_names = list(targets.keys())
    all_cops = all_coroutine_ticks(targets)

    # Index records by (ds, cops) for quick lookup
    index = {}
    for ds, records in targets.items():
        for r in records:
            index[(ds, r["coroutines"])] = r

    header = f"{'cops':>6}  " + "  ".join(f"{ds:>{col}}" for ds in ds_names) + "  (ops/s)"
    separator = "-" * len(header)
    rows = []
    for cops in all_cops:
        vals = []
        for ds in ds_names:
            r = index.get((ds, cops))
            vals.append(f"{r['throughput_ops_per_sec']:>{col}.0f}" if r else f"{'N/A':>{col}}")
        rows.append(f"{cops:>6}  " + "  ".join(vals))

    print("\n" + header)
    print(separator)
    for row in rows:
        print(row)
    print()

    out = output_dir / "summary.txt"
    with open(out, "w") as f:
        f.write(header + "\n" + separator + "\n")
        f.write("\n".join(rows) + "\n")
    print("  saved:", out)


# ── main ─────────────────────────────────────────────────────────────────────

ALL_STATS = ["throughput", "push_pop", "per_thread", "summary"]

def main():
    parser = argparse.ArgumentParser(
        description="Compare DS targets across a coroutine sweep.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # plot all DS subdirs found in sweep_results/
  python3 plot_sweep.py --results-dir sweep_results

  # compare only two specific targets
  python3 plot_sweep.py --results-dir sweep_results \\
    --ds treiber_stack_fc treiber_stack_fast

  # only throughput + summary table, save PNGs elsewhere
  python3 plot_sweep.py --results-dir sweep_results \\
    --stat throughput summary --output-dir plots/
        """
    )
    parser.add_argument("--results-dir", type=Path, default=Path("sweep_results"),
                        help="Root directory produced by run_coroutine_sweep.sh")
    parser.add_argument("--ds", nargs="*", default=[],
                        help="DS names to include (default: all subdirs with results)")
    parser.add_argument("--output-dir", type=Path, default=None,
                        help="Where to save plots (default: --results-dir)")
    parser.add_argument("--stat", nargs="+", default=ALL_STATS, choices=ALL_STATS,
                        help="Which plots to generate")
    args = parser.parse_args()

    if not args.results_dir.is_dir():
        print("Error: --results-dir '{}' not found.".format(args.results_dir), file=sys.stderr)
        sys.exit(1)

    targets = discover_targets(args.results_dir, set(args.ds))
    if not targets:
        print("No DS result directories found in", args.results_dir, file=sys.stderr)
        sys.exit(1)

    output_dir = args.output_dir or args.results_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    print("Targets: " + ", ".join(
        "{} ({} points)".format(ds, len(recs)) for ds, recs in targets.items()
    ))
    print("Output : ", output_dir)
    print()

    stat_set = set(args.stat)
    if "throughput" in stat_set: plot_throughput(targets, output_dir)
    if "push_pop"   in stat_set: plot_push_pop(targets, output_dir)
    if "per_thread" in stat_set: plot_per_thread_boxplot(targets, output_dir)
    if "summary"    in stat_set: print_summary(targets, output_dir)

    print("\nDone.")


if __name__ == "__main__":
    main()