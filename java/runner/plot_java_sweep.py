#!/usr/bin/env python3
"""
plot_java_sweep.py — plot Java benchmark sweep results.

Reads the layout produced by run_java_sweep.sh:
  results_dir/v1/<DsShortName>/result_cops<T>.json
  results_dir/v2/<DsShortName>/result_cops<T>.json
  ...

The Java result JSON is an array (one element per iteration).
Field mapping vs C++ plot_sweep.py:

  Java                              C++ equivalent
  ─────────────────────────────     ──────────────────────────────────
  result[0].throughput              throughput_ops_per_sec (derived)
  result[0].elapsedTime (s)         max_time_thread_terminate_total/1e6
  result[0].commonStatistic.total   sum_num_operations_total
  result[0].commonStatistic.numAdd  sum_num_pushes_total
  result[0].commonStatistic.numRemove sum_num_pops_total
  result[0].effectiveUpdates        —
  (no work_iteration)               work_throughput_ops_per_sec

The x-axis is thread count (--threads) instead of coroutine count.
Everything else (aggregation, error bands, multi-run, --agg, --ds) is
identical to plot_sweep.py behaviour.

Usage:
  python3 plot_java_sweep.py --results-dir sweep_results
  python3 plot_java_sweep.py --results-dir sweep_results --repeats auto --agg median
  python3 plot_java_sweep.py --results-dir sweep_results --ds LockFreeQueueIntSet
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

# ── style (identical to plot_sweep.py) ────────────────────────────────────────
DS_COLORS  = ["#4C8EDA", "#E06C4B", "#3BAA72", "#9B6DD4",
               "#E0B84B", "#4BC7CE", "#D45E8A", "#7A7A7A", "#991212", "#080808"]
DS_MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*", ".", "h", "1"]
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

RESULT_PAT = re.compile(r"result_cops(\d+)\.json")   # threads, not cops
RUN_PAT    = re.compile(r"^v\d+$")

AGG_FUNCS = {
    "mean":   (np.mean,   lambda a: np.std(a, ddof=1),
                lambda a: np.std(a, ddof=1),          "±1 std-dev"),
    "median": (np.median, lambda a: np.percentile(a, 25),
                lambda a: np.percentile(a, 75),        "IQR"),
    "min":    (np.min,    None, None, None),
    "max":    (np.max,    None, None, None),
}


def aggregate(values, agg):
    arr = np.array(values, dtype=float)
    if len(arr) == 0:
        return float("nan"), 0.0, 0.0
    func, lo_func, hi_func, _ = AGG_FUNCS[agg]
    center = float(func(arr))
    if lo_func is None or len(arr) < 2:
        return center, 0.0, 0.0
    if agg == "median":
        lo_err = center - float(lo_func(arr))
        hi_err = float(hi_func(arr)) - center
    else:
        err    = float(lo_func(arr))
        lo_err = hi_err = err
    return center, lo_err, hi_err


# ── Java-specific result parsing ──────────────────────────────────────────────

def parse_java_result(path, n_threads):
    """
    Parse a Java result JSON file.
    Returns a flat dict with the same field names used in plot_sweep.py
    so all downstream aggregation/plotting code is reusable unchanged.
    """
    with open(path) as f:
        raw = json.load(f)

    # result is an array (one entry per iteration); take the last iteration
    # (earlier ones are warm-up iterations if iterations > 1)
    r = raw[-1]
    cs = r.get("commonStatistic", {})

    elapsed_s = r.get("elapsedTime", 0)

    return {
        # x-axis
        "threads": n_threads,

        # primary metric — Java pre-computes this as total/elapsedTime
        "throughput_ops_per_sec": r.get("throughput", 0.0),

        # raw counters (mirrored field names from C++ for plot reuse)
        "sum_num_operations_total":  cs.get("total", 0),
        "sum_num_pushes_total":      cs.get("numAdd", 0),
        "sum_num_pops_total":        cs.get("numRemove", 0),
        "sum_num_fail_pops_total":   cs.get("failures", 0),
        "effective_updates":         r.get("effectiveUpdates", 0),
        "elapsed_time_s":            elapsed_s,
        "final_size":                r.get("finalSize", 0),

        # per-thread array for boxplot
        "sum_num_operations_by_thread": [
            t.get("total", 0) for t in r.get("threadStatistics", [])
        ] or None,
    }


def load_ds_dir(ds_dir):
    records = []
    for p in sorted(ds_dir.glob("result_cops*.json")):
        m = RESULT_PAT.match(p.name)
        if not m:
            continue
        n_threads = int(m.group(1))
        rec = parse_java_result(p, n_threads)
        records.append(rec)
    records.sort(key=lambda r: r["threads"])
    return records


# ── discovery (same pattern as plot_sweep.py) ─────────────────────────────────

def discover_run_dirs(results_dir, repeat_names):
    if not repeat_names:
        return [(None, results_dir)]
    if repeat_names == ["auto"]:
        runs = sorted(
            (d for d in results_dir.iterdir()
             if d.is_dir() and RUN_PAT.match(d.name)),
            key=lambda d: d.name,
        )
        if not runs:
            return [(None, results_dir)]
        return [(d.name, d) for d in runs]
    result = []
    for name in repeat_names:
        p = results_dir / name
        if p.is_dir():
            result.append((name, p))
        else:
            print("Warning: repeat dir '{}' not found, skipping.".format(p),
                  file=sys.stderr)
    return result


def discover_targets(results_dir, requested, repeat_names, agg):
    run_dirs = discover_run_dirs(results_dir, repeat_names)
    multi    = len(run_dirs) > 1

    raw = {}  # raw[ds][threads] = [record, ...]
    for _label, run_dir in run_dirs:
        for child in sorted(run_dir.iterdir()):
            if not child.is_dir():
                continue
            ds = child.name
            if requested and ds not in requested:
                continue
            for rec in load_ds_dir(child):
                raw.setdefault(ds, {}).setdefault(rec["threads"], []).append(rec)

    SCALAR_FIELDS = [
        "throughput_ops_per_sec",
        "sum_num_operations_total",
        "sum_num_pushes_total",
        "sum_num_pops_total",
        "sum_num_fail_pops_total",
        "effective_updates",
        "elapsed_time_s",
    ]

    targets = {}
    for ds, threads_map in sorted(raw.items()):
        records_out = []
        for n_threads in sorted(threads_map):
            recs = threads_map[n_threads]
            out  = {"threads": n_threads, "run_count": len(recs)}
            for field in SCALAR_FIELDS:
                vals = [r.get(field, 0) for r in recs]
                c, lo, hi = aggregate(vals, agg)
                out[field]          = c
                out[field + "_lo"]  = lo
                out[field + "_hi"]  = hi
            # pool per-thread arrays across runs
            per_thread = []
            for r in recs:
                pt = r.get("sum_num_operations_by_thread")
                if pt:
                    per_thread.extend(pt)
            if per_thread:
                out["sum_num_operations_by_thread"] = per_thread
            out["_raw_recs"] = recs
            records_out.append(out)
        targets[ds] = records_out

    return targets, multi, run_dirs


# ── helpers ───────────────────────────────────────────────────────────────────

def style_ax(ax):
    for spine in ("left", "bottom"):
        ax.spines[spine].set_color(SPINE_COLOR)
    ax.tick_params(colors="#555555")
    ax.yaxis.label.set_color("#333333")
    ax.xaxis.label.set_color("#333333")
    ax.title.set_color("#222222")


def all_thread_ticks(targets):
    ticks = set()
    for recs in targets.values():
        for r in recs:
            ticks.add(r["threads"])
    return sorted(ticks)


def setup_xaxis(ax, ticks):
    if len(ticks) > 1 and ticks[-1] / ticks[0] >= 8:
        ax.set_xscale("log", base=2)
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_xticks(ticks)
    ax.set_xlabel("threads", fontsize=10)


def band_label(agg):
    _, _, _, desc = AGG_FUNCS[agg]
    return desc or ""


def _draw(ax, x, y, y_lo, y_hi, color, marker, multi, **line_kwargs):
    ax.plot(x, y, marker=marker, color=color, markersize=6,
            markeredgewidth=0, **line_kwargs)
    if multi and any(lo > 0 or hi > 0 for lo, hi in zip(y_lo, y_hi)):
        y_arr = np.array(y)
        ax.fill_between(x, y_arr - np.array(y_lo),
                           y_arr + np.array(y_hi),
                        alpha=0.15, color=color)


# ── plots ─────────────────────────────────────────────────────────────────────

def plot_throughput(targets, output_dir, agg, multi, thread_type):
    fig, ax = plt.subplots(figsize=(9, 5))
    for i, (ds, records) in enumerate(targets.items()):
        x    = [r["threads"]                  for r in records]
        y    = [r["throughput_ops_per_sec"]    for r in records]
        y_lo = [r["throughput_ops_per_sec_lo"] for r in records]
        y_hi = [r["throughput_ops_per_sec_hi"] for r in records]
        _draw(ax, x, y, y_lo, y_hi,
              DS_COLORS[i % len(DS_COLORS)],
              DS_MARKERS[i % len(DS_MARKERS)],
              multi, linewidth=2, label=ds)
    ticks = all_thread_ticks(targets)
    setup_xaxis(ax, ticks)
    ax.set_ylabel("throughput  (ops / s)", fontsize=10)
    agg_note = " [{}{}]".format(agg, " " + band_label(agg) if band_label(agg) else "") if multi else ""
    ax.set_title(f"Java throughput vs {thread_type} threads" + agg_note, fontsize=11, fontweight="bold")
    ax.legend(framealpha=0.4, fontsize=9)
    style_ax(ax)
    fig.tight_layout()
    out = output_dir / "java_throughput_vs_threads.png"
    fig.savefig(out);  plt.close(fig)
    print("  saved:", out)


def plot_ops_breakdown(targets, output_dir, agg, multi):
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    ax_add, ax_rem = axes
    for i, (ds, records) in enumerate(targets.items()):
        x     = [r["threads"] for r in records]
        color  = DS_COLORS[i % len(DS_COLORS)]
        marker = DS_MARKERS[i % len(DS_MARKERS)]
        for ax, field, label in [
            (ax_add, "sum_num_pushes_total", ds + " (add)"),
            (ax_rem, "sum_num_pops_total",   ds + " (remove)"),
        ]:
            y    = [r.get(field, 0)         for r in records]
            y_lo = [r.get(field + "_lo", 0) for r in records]
            y_hi = [r.get(field + "_hi", 0) for r in records]
            _draw(ax, x, y, y_lo, y_hi, color, marker, multi,
                  linewidth=2, label=label)
    ticks = all_thread_ticks(targets)
    for ax, title in [(ax_add, "successful adds"), (ax_rem, "successful removes")]:
        setup_xaxis(ax, ticks)
        ax.set_ylabel("total operations", fontsize=10)
        ax.set_title(title, fontsize=11, fontweight="bold")
        ax.legend(framealpha=0.4, fontsize=9)
        style_ax(ax)
    fig.tight_layout()
    out = output_dir / "java_ops_breakdown_vs_threads.png"
    fig.savefig(out);  plt.close(fig)
    print("  saved:", out)


def plot_per_thread_boxplot(targets, output_dir):
    n = len(targets)
    if n == 0:
        return
    fig, axes = plt.subplots(1, n, figsize=(5 * n, 5))
    if n == 1:
        axes = [axes]
    for ax, (i, (ds, records)) in zip(axes, enumerate(targets.items())):
        labels, dists = [], []
        for r in records:
            pt = r.get("sum_num_operations_by_thread")
            if pt:
                dists.append(pt)
                labels.append(str(r["threads"]))
        if not dists:
            ax.set_title("{}\n(no per-thread data)".format(ds))
            continue
        color = DS_COLORS[i % len(DS_COLORS)]
        bp = ax.boxplot(dists, labels=labels, patch_artist=True,
                        medianprops=dict(color="#222222", linewidth=2),
                        whiskerprops=dict(color=SPINE_COLOR),
                        capprops=dict(color=SPINE_COLOR),
                        flierprops=dict(marker="x", color="#B0B0B0", markersize=5))
        for patch in bp["boxes"]:
            patch.set_facecolor(color + "33")
            patch.set_edgecolor(color)
        ax.set_title(ds, fontsize=10, fontweight="bold")
        ax.set_xlabel("OS threads", fontsize=9)
        ax.set_ylabel("ops per thread", fontsize=9)
        style_ax(ax)
    fig.suptitle("per-thread op distribution", fontsize=11, fontweight="bold", y=1.02)
    fig.tight_layout()
    out = output_dir / "java_per_thread_distribution.png"
    fig.savefig(out, bbox_inches="tight");  plt.close(fig)
    print("  saved:", out)


def print_summary(targets, output_dir, agg, multi):
    col      = 18
    ds_names = list(targets.keys())
    all_t    = all_thread_ticks(targets)
    index    = {}
    for ds, recs in targets.items():
        index[ds] = {r["threads"]: r for r in recs}

    agg_note = " [{}]".format(agg) if multi else ""
    header = "{:>8}  ".format("threads") + "  ".join(
        "{:>{}}".format(ds, col) for ds in ds_names
    ) + "  (ops/s" + agg_note + ")"
    sep  = "-" * len(header)
    rows = []
    for t in all_t:
        cells = []
        for ds in ds_names:
            r = index[ds].get(t)
            if r is None:
                cells.append("{:>{}}".format("N/A", col))
            else:
                c  = r["throughput_ops_per_sec"]
                lo = r["throughput_ops_per_sec_lo"]
                hi = r["throughput_ops_per_sec_hi"]
                cell = ("{:.0f}(-{:.0f}/+{:.0f})".format(c, lo, hi)
                        if (lo > 0 or hi > 0) else "{:.0f}".format(c))
                cells.append("{:>{}}".format(cell, col))
        rows.append("{:>8}  ".format(t) + "  ".join(cells))

    print("\n" + header)
    print(sep)
    for row in rows:
        print(row)
    print()

    out = output_dir / "java_summary.txt"
    with open(out, "w") as f:
        f.write(header + "\n" + sep + "\n" + "\n".join(rows) + "\n")
    print("  saved:", out)


# ── main ──────────────────────────────────────────────────────────────────────

ALL_STATS = ["throughput", "ops_breakdown", "per_thread", "summary"]


def main():
    parser = argparse.ArgumentParser(
        description="Plot Java benchmark sweep (thread count sweep).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 plot_java_sweep.py --results-dir sweep_results
  python3 plot_java_sweep.py --results-dir sweep_results --repeats auto --agg median
  python3 plot_java_sweep.py --results-dir sweep_results \\
    --ds LockFreeQueueIntSet NonBlockingFriendlySkipListMap
        """
    )
    parser.add_argument("--results-dir", type=Path, default=Path("sweep_results"))
    parser.add_argument("--repeats", nargs="+", default=[],
                        metavar="DIR_OR_auto")
    parser.add_argument("--agg", default="mean", choices=["mean","median","min","max"])
    parser.add_argument("--ds", nargs="*", default=[])
    parser.add_argument("--output-dir", type=Path, default=None)
    parser.add_argument("--stat", nargs="+", default=ALL_STATS, choices=ALL_STATS)
    parser.add_argument("--thread-type", default="unknown", choices=["unknown", "os", "virtual"])
    args = parser.parse_args()

    if not args.results_dir.is_dir():
        print("Error: --results-dir '{}' not found.".format(args.results_dir),
              file=sys.stderr)
        sys.exit(1)

    targets, multi, run_dirs = discover_targets(
        args.results_dir, set(args.ds), args.repeats, args.agg)

    if not targets:
        print("No DS result directories found.", file=sys.stderr)
        sys.exit(1)

    output_dir = args.output_dir or args.results_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    run_info = ("single-run" if not multi
                else "{} runs  agg={}".format(len(run_dirs), args.agg))
    print("Targets :", ", ".join(
        "{} ({} thread pts)".format(ds, len(recs)) for ds, recs in targets.items()))
    print("Runs    :", run_info)
    print("Output  :", output_dir)
    print()

    stat_set = set(args.stat)
    if "throughput"    in stat_set: plot_throughput(targets, output_dir, args.agg, multi, args.thread_type)
    if "ops_breakdown" in stat_set: plot_ops_breakdown(targets, output_dir, args.agg, multi)
    if "per_thread"    in stat_set: plot_per_thread_boxplot(targets, output_dir)
    if "summary"       in stat_set: print_summary(targets, output_dir, args.agg, multi)

    print("\nDone.")


if __name__ == "__main__":
    main()
