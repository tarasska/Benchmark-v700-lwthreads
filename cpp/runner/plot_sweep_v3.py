#!/usr/bin/env python3
"""
plot_sweep.py — compare multiple DS targets across a coroutine sweep,
                with optional aggregation across benchmark repeats.

Directory layouts supported:

  Single-run (original):
    results_dir/
      <ds_name>/result_cops<N>.json
      ...

  Multi-run (new):
    results_dir/
      v1/<ds_name>/result_cops<N>.json
      v2/<ds_name>/result_cops<N>.json
      ...

When multiple runs are found the script aggregates values across them
before plotting. The aggregation function is configurable (mean / median /
min / max).  Error bands (±1 std-dev or IQR) are drawn automatically.

Usage:
  # single-run (unchanged behaviour)
  python3 plot_sweep.py --results-dir sweep_results

  # multi-run: discover v* subdirs automatically
  python3 plot_sweep.py --results-dir sweep_results --repeats auto

  # multi-run: explicit repeat dirs
  python3 plot_sweep.py --results-dir sweep_results --repeats v1 v2 v3

  # choose aggregation function
  python3 plot_sweep.py --results-dir sweep_results --repeats auto --agg median

  # filter DS and stats
  python3 plot_sweep.py --results-dir sweep_results --repeats auto \\
    --ds treiber_stack_fc treiber_stack --stat throughput summary
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

# ── style ─────────────────────────────────────────────────────────────────────
DS_COLORS  = ["#4C8EDA", "#E06C4B", "#3BAA72", "#9B6DD4",
               "#E0B84B", "#4BC7CE", "#D45E8A", "#7A7A7A"]
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
RUN_PAT    = re.compile(r"^v\d+$")   # matches v1, v2, v23, …


# ── aggregation functions ─────────────────────────────────────────────────────

AGG_FUNCS = {
    "mean":   (np.mean,   lambda a: np.std(a, ddof=1),       "±1 std-dev"),
    "median": (np.median, lambda a: (np.percentile(a, 25),
                                     np.percentile(a, 75)), "IQR"),
    "min":    (np.min,    None, None),
    "max":    (np.max,    None, None),
}

def aggregate(values, agg):
    """
    Returns (center, lo_err, hi_err) where lo_err/hi_err are distances
    from center to band edge (for fill_between).  Returns (center, 0, 0)
    when bands are not applicable.
    """
    arr = np.array(values, dtype=float)
    if len(arr) == 0:
        return float("nan"), 0.0, 0.0

    func, band_func, _ = AGG_FUNCS[agg]
    center = float(func(arr))

    if band_func is None or len(arr) < 2:
        return center, 0.0, 0.0

    band = band_func(arr)
    if isinstance(band, tuple):          # IQR: (q25, q75)
        lo_err = center - float(band[0])
        hi_err = float(band[1]) - center
    else:                                # std-dev: scalar
        lo_err = hi_err = float(band)

    return center, lo_err, hi_err


# ── data loading ──────────────────────────────────────────────────────────────

def load_ds_dir(ds_dir):
    """Load all result_cops<N>.json from one DS directory -> list of dicts."""
    records = []
    for p in sorted(ds_dir.glob("result_cops*.json")):
        m = RESULT_PAT.match(p.name)
        if not m:
            continue
        cops = int(m.group(1))
        with open(p) as f:
            data = json.load(f)
        data["coroutines"] = cops
        run_ms   = data.get("max_time_thread_terminate_total", 0) / 1e6
        total    = data.get("sum_num_operations_total", 0)
        work     = data.get("work_iteration", 0)
        data["throughput_ops_per_sec"]      = total / run_ms if run_ms > 0 else 0.0
        data["work_throughput_ops_per_sec"] = work  / run_ms if run_ms > 0 else 0.0
        records.append(data)
    records.sort(key=lambda r: r["coroutines"])
    return records


def discover_run_dirs(results_dir, repeat_names):
    """
    Returns a list of (run_label, Path) pairs for the repeat directories.

    - repeat_names == ["auto"]: auto-discover all v* subdirs.
    - repeat_names == []:       single-run mode; returns [(None, results_dir)].
    - otherwise:                use the supplied names as subdirs of results_dir.
    """
    if not repeat_names:
        return [(None, results_dir)]

    if repeat_names == ["auto"]:
        runs = sorted(
            (d for d in results_dir.iterdir()
             if d.is_dir() and RUN_PAT.match(d.name)),
            key=lambda d: d.name
        )
        if not runs:
            print("Warning: --repeats auto found no v* directories; "
                  "falling back to single-run mode.", file=sys.stderr)
            return [(None, results_dir)]
        return [(d.name, d) for d in runs]

    result = []
    for name in repeat_names:
        p = results_dir / name
        if not p.is_dir():
            print(f"Warning: repeat dir '{p}' not found, skipping.", file=sys.stderr)
            continue
        result.append((name, p))
    return result


def discover_targets(results_dir, requested, repeat_names, agg):
    """
    Returns:
      {
        ds_name: [
          {
            "coroutines": N,
            "throughput_ops_per_sec": center,
            "throughput_lo": lo_err,
            "throughput_hi": hi_err,
            "run_count":     K,
            # raw aggregated scalars for other fields:
            "sum_num_pushes_total":  center,
            ...
            # per-run raw values kept for boxplot:
            "_raw": { cops: [record, ...] }
          },
          ...
        ]
      }
    """
    run_dirs = discover_run_dirs(results_dir, repeat_names)
    multi    = len(run_dirs) > 1

    # Collect: ds -> cops -> [record from each run]
    raw = {}   # raw[ds][cops] = [record, ...]

    for _label, run_dir in run_dirs:
        for child in sorted(run_dir.iterdir()):
            if not child.is_dir():
                continue
            ds = child.name
            if requested and ds not in requested:
                continue
            for rec in load_ds_dir(child):
                raw.setdefault(ds, {}).setdefault(rec["coroutines"], []).append(rec)

    if not raw:
        return {}

    SCALAR_FIELDS = [
        "throughput_ops_per_sec",
        "work_throughput_ops_per_sec",
        "work_iteration",
        "sum_num_pushes_total",
        "sum_num_pops_total",
        "sum_num_fail_pops_total",
        "sum_num_operations_total",
        "max_time_thread_terminate_total",
    ]

    targets = {}
    for ds, cops_map in sorted(raw.items()):
        records_out = []
        for cops in sorted(cops_map):
            recs = cops_map[cops]
            out  = {"coroutines": cops, "run_count": len(recs), "_raw_recs": recs}

            for field in SCALAR_FIELDS:
                vals = [r.get(field, 0) for r in recs]
                c, lo, hi = aggregate(vals, agg)
                out[field]          = c
                out[field + "_lo"]  = lo
                out[field + "_hi"]  = hi

            # per-thread distribution: pool all runs together
            per_thread_all = []
            for r in recs:
                pt = r.get("sum_num_operations_by_thread")
                if pt:
                    per_thread_all.extend(pt)
            if per_thread_all:
                out["sum_num_operations_by_thread"] = per_thread_all

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


def all_cops(targets):
    ticks = set()
    for recs in targets.values():
        for r in recs:
            ticks.add(r["coroutines"])
    return sorted(ticks)


def setup_xaxis(ax, ticks):
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_xticks(ticks)
    ax.set_xlabel("coroutines per thread", fontsize=10)


def band_label(agg):
    _, _, desc = AGG_FUNCS[agg]
    return desc or ""


# ── plots ─────────────────────────────────────────────────────────────────────

def plot_throughput(targets, output_dir, agg, multi):
    fig, ax = plt.subplots(figsize=(9, 5))

    for i, (ds, records) in enumerate(targets.items()):
        x      = [r["coroutines"]             for r in records]
        y      = [r["throughput_ops_per_sec"]  for r in records]
        y_lo   = [r["throughput_ops_per_sec_lo"] for r in records]
        y_hi   = [r["throughput_ops_per_sec_hi"] for r in records]
        color  = DS_COLORS[i % len(DS_COLORS)]
        marker = DS_MARKERS[i % len(DS_MARKERS)]

        ax.plot(x, y, marker=marker, linewidth=2, label=ds,
                color=color, markersize=6, markeredgewidth=0)

        if multi and any(lo > 0 or hi > 0 for lo, hi in zip(y_lo, y_hi)):
            y_arr  = np.array(y)
            lo_arr = np.array(y_lo)
            hi_arr = np.array(y_hi)
            ax.fill_between(x, y_arr - lo_arr, y_arr + hi_arr,
                            alpha=0.15, color=color)

    ticks = all_cops(targets)
    setup_xaxis(ax, ticks)
    ax.set_ylabel("throughput  (ops / s)", fontsize=10)

    agg_note = " [{}{}]".format(agg, " " + band_label(agg) if band_label(agg) else "") if multi else ""
    ax.set_title("throughput vs coroutines" + agg_note, fontsize=11, fontweight="bold")
    ax.legend(framealpha=0.4, fontsize=9)
    style_ax(ax)
    fig.tight_layout()

    out = output_dir / "throughput_vs_coroutines.png"
    fig.savefig(out)
    plt.close(fig)
    print("  saved:", out)


def plot_combined_throughput(targets, output_dir, agg, multi):
    """
    One chart per DS: DS throughput (solid) and work throughput (dashed)
    in the same colour so the relationship is immediately visible.
    A single shared legend entry per DS uses a split solid/dashed line patch.
    """
    import matplotlib.lines as mlines

    # Line styles: solid for DS ops, dashed for work
    STYLE_DS   = dict(linestyle="solid",  linewidth=2, markersize=6, markeredgewidth=0)
    STYLE_WORK = dict(linestyle="dashed", linewidth=2, markersize=5, markeredgewidth=0)

    fig, ax = plt.subplots(figsize=(9, 5))
    legend_handles = []

    for i, (ds, records) in enumerate(targets.items()):
        color  = DS_COLORS[i % len(DS_COLORS)]
        marker = DS_MARKERS[i % len(DS_MARKERS)]
        x      = [r["coroutines"] for r in records]

        def _plot_series(field, style):
            y    = [r.get(field, 0)           for r in records]
            y_lo = [r.get(field + "_lo", 0)   for r in records]
            y_hi = [r.get(field + "_hi", 0)   for r in records]
            ax.plot(x, y, marker=marker, color=color, **style)
            if multi and any(lo > 0 or hi > 0 for lo, hi in zip(y_lo, y_hi)):
                y_arr = np.array(y)
                ax.fill_between(x, y_arr - np.array(y_lo),
                                   y_arr + np.array(y_hi),
                                alpha=0.10, color=color)

        _plot_series("throughput_ops_per_sec",      STYLE_DS)
        _plot_series("work_throughput_ops_per_sec",  STYLE_WORK)

        # Combined legend handle: two short lines, same colour
        h = mlines.Line2D([], [], color=color, label=ds,
                          linestyle="solid", linewidth=2)
        legend_handles.append(h)

    # Explain the line styles once via a type legend
    legend_handles += [
        mlines.Line2D([], [], color="black", linestyle="solid",  linewidth=2, label="DS throughput"),
        mlines.Line2D([], [], color="black", linestyle="dashed", linewidth=2, label="work throughput"),
    ]

    ticks = all_cops(targets)
    setup_xaxis(ax, ticks)
    ax.set_ylabel("throughput  (ops / s)", fontsize=10)
    agg_note = " [{}]".format(agg) if multi else ""
    ax.set_title("DS vs work throughput vs coroutines" + agg_note,
                 fontsize=11, fontweight="bold")
    ax.legend(handles=legend_handles, framealpha=0.4, fontsize=9)
    style_ax(ax)
    fig.tight_layout()

    out = output_dir / "combined_throughput_vs_coroutines.png"
    fig.savefig(out)
    plt.close(fig)
    print("  saved:", out)


def plot_push_pop(targets, output_dir, agg, multi):
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    ax_push, ax_pop = axes

    for i, (ds, records) in enumerate(targets.items()):
        x      = [r["coroutines"] for r in records]
        color  = DS_COLORS[i % len(DS_COLORS)]
        marker = DS_MARKERS[i % len(DS_MARKERS)]

        for ax, field, label in [
            (ax_push, "sum_num_pushes_total", "pushes"),
            (ax_pop,  "sum_num_pops_total",   "pops"),
        ]:
            y    = [r.get(field, 0)          for r in records]
            y_lo = [r.get(field + "_lo", 0)  for r in records]
            y_hi = [r.get(field + "_hi", 0)  for r in records]
            ax.plot(x, y, marker=marker, linewidth=2, label=ds,
                    color=color, markersize=6)
            if multi and any(lo > 0 or hi > 0 for lo, hi in zip(y_lo, y_hi)):
                y_arr = np.array(y)
                ax.fill_between(x, y_arr - np.array(y_lo),
                                   y_arr + np.array(y_hi),
                                alpha=0.15, color=color)

    ticks = all_cops(targets)
    for ax, title in [(ax_push, "total pushes"), (ax_pop, "total pops")]:
        setup_xaxis(ax, ticks)
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
                labels.append(str(r["coroutines"]))

        if not dists:
            ax.set_title("{}\n(no per-thread data)".format(ds))
            continue

        color = DS_COLORS[i % len(DS_COLORS)]
        bp = ax.boxplot(
            dists, labels=labels, patch_artist=True,
            medianprops=dict(color="#222222", linewidth=2),
            whiskerprops=dict(color=SPINE_COLOR),
            capprops=dict(color=SPINE_COLOR),
            flierprops=dict(marker="x", color="#B0B0B0", markersize=5),
        )
        for patch in bp["boxes"]:
            patch.set_facecolor(color + "33")
            patch.set_edgecolor(color)

        n_runs = max((r.get("run_count", 1) for r in records), default=1)
        run_note = " (pooled {} runs)".format(n_runs) if n_runs > 1 else ""
        ax.set_title(ds + run_note, fontsize=10, fontweight="bold")
        ax.set_xlabel("coroutines per thread", fontsize=9)
        ax.set_ylabel("ops per thread", fontsize=9)
        style_ax(ax)

    fig.suptitle("per-thread op distribution", fontsize=11, fontweight="bold", y=1.02)
    fig.tight_layout()
    out = output_dir / "per_thread_distribution.png"
    fig.savefig(out, bbox_inches="tight")
    plt.close(fig)
    print("  saved:", out)


def plot_repeats_scatter(targets, output_dir, run_dirs):
    """
    One chart per DS: all individual run values as translucent dots,
    aggregated line on top.  Only generated in multi-run mode.
    """
    for i, (ds, records) in enumerate(targets.items()):
        fig, ax = plt.subplots(figsize=(9, 5))
        color = DS_COLORS[i % len(DS_COLORS)]

        # scatter individual runs
        for r in records:
            cops = r["coroutines"]
            vals = [rec.get("throughput_ops_per_sec", 0)
                    for rec in r["_raw_recs"]]
            ax.scatter([cops] * len(vals), vals,
                       color=color, alpha=0.35, s=20, zorder=2)

        # aggregated line
        x = [r["coroutines"]            for r in records]
        y = [r["throughput_ops_per_sec"] for r in records]
        ax.plot(x, y, color=color, linewidth=2, marker="o",
                markersize=5, zorder=3, label="aggregated")

        ticks = sorted(set(x))
        setup_xaxis(ax, ticks)
        ax.set_ylabel("throughput  (ops / s)", fontsize=10)
        ax.set_title("{} — per-run scatter ({} runs)".format(ds, len(run_dirs)),
                     fontsize=11, fontweight="bold")
        ax.legend(framealpha=0.4, fontsize=9)
        style_ax(ax)
        fig.tight_layout()

        out = output_dir / "repeats_scatter_{}.png".format(ds)
        fig.savefig(out)
        plt.close(fig)
        print("  saved:", out)


def print_summary(targets, output_dir, agg, multi):
    col      = 16
    ds_names = list(targets.keys())
    cops_all = all_cops(targets)
    index    = {}
    for ds, recs in targets.items():
        for r in recs:
            index[(ds, r["coroutines"])] = r

    agg_note = " [{}]".format(agg) if multi else ""
    header = "{:>6}  ".format("cops") + "  ".join(
        "{:>{}}".format(ds, col) for ds in ds_names
    ) + "  (ops/s" + agg_note + ")"
    sep  = "-" * len(header)
    rows = []
    for cops in cops_all:
        vals = []
        for ds in ds_names:
            r = index.get((ds, cops))
            if r is None:
                vals.append("{:>{}}".format("N/A", col))
            else:
                center = r["throughput_ops_per_sec"]
                lo     = r["throughput_ops_per_sec_lo"]
                hi     = r["throughput_ops_per_sec_hi"]
                if multi and (lo > 0 or hi > 0):
                    cell = "{:.0f}(-{:.0f}/+{:.0f})".format(center, lo, hi)
                else:
                    cell = "{:.0f}".format(center)
                vals.append("{:>{}}".format(cell, col))
        rows.append("{:>6}  ".format(cops) + "  ".join(vals))

    print("\n" + header)
    print(sep)
    for row in rows:
        print(row)
    print()

    out = output_dir / "summary.txt"
    with open(out, "w") as f:
        f.write(header + "\n" + sep + "\n" + "\n".join(rows) + "\n")
    print("  saved:", out)


# ── main ──────────────────────────────────────────────────────────────────────

ALL_STATS = ["throughput", "combined_throughput", "push_pop", "per_thread", "repeats_scatter", "summary"]

def main():
    parser = argparse.ArgumentParser(
        description="Compare DS targets across a coroutine sweep, with optional repeat aggregation.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # single-run (unchanged)
  python3 plot_sweep.py --results-dir sweep_results

  # multi-run: auto-discover v1/, v2/, ... subdirs
  python3 plot_sweep.py --results-dir sweep_results --repeats auto

  # multi-run: explicit repeat dirs
  python3 plot_sweep.py --results-dir sweep_results --repeats v1 v2 v3

  # choose aggregation and filter
  python3 plot_sweep.py --results-dir sweep_results --repeats auto \\
    --agg median --ds treiber_stack_fc treiber_stack --stat throughput summary
        """
    )
    parser.add_argument("--results-dir", type=Path, default=Path("sweep_results"),
                        help="Root directory produced by run_coroutine_sweep.sh")
    parser.add_argument("--repeats", nargs="+", default=[],
                        metavar="DIR_OR_auto",
                        help="Repeat subdirs to aggregate (e.g. v1 v2 v3, or 'auto' to discover v* dirs). "
                             "Omit for single-run mode.")
    parser.add_argument("--agg", default="mean", choices=list(AGG_FUNCS),
                        help="Aggregation function across repeats (default: mean)")
    parser.add_argument("--ds", nargs="*", default=[],
                        help="DS names to include (default: all)")
    parser.add_argument("--output-dir", type=Path, default=None,
                        help="Where to save plots (default: --results-dir)")
    parser.add_argument("--stat", nargs="+", default=ALL_STATS, choices=ALL_STATS,
                        help="Which plots to generate")
    args = parser.parse_args()

    if not args.results_dir.is_dir():
        print("Error: --results-dir '{}' not found.".format(args.results_dir),
              file=sys.stderr)
        sys.exit(1)

    result = discover_targets(args.results_dir, set(args.ds),
                              args.repeats, args.agg)
    if not result:
        print("No DS result directories found.", file=sys.stderr)
        sys.exit(1)

    targets, multi, run_dirs = result

    if not targets:
        print("No matching DS data found.", file=sys.stderr)
        sys.exit(1)

    output_dir = args.output_dir or args.results_dir
    output_dir.mkdir(parents=True, exist_ok=True)

    run_info = ("single-run" if not multi
                else "{} runs ({})  agg={}".format(
                    len(run_dirs), ", ".join(l for l, _ in run_dirs), args.agg))
    print("Targets : " + ", ".join(
        "{} ({} cops pts)".format(ds, len(recs)) for ds, recs in targets.items()
    ))
    print("Runs    :", run_info)
    print("Output  :", output_dir)
    print()

    stat_set = set(args.stat)
    if "throughput"      in stat_set: plot_throughput(targets, output_dir, args.agg, multi)
    if "combined_throughput" in stat_set: plot_combined_throughput(targets, output_dir, args.agg, multi)
    if "push_pop"        in stat_set: plot_push_pop(targets, output_dir, args.agg, multi)
    if "per_thread"      in stat_set: plot_per_thread_boxplot(targets, output_dir)
    if "repeats_scatter" in stat_set and multi:
        plot_repeats_scatter(targets, output_dir, run_dirs)
    if "summary"         in stat_set: print_summary(targets, output_dir, args.agg, multi)

    print("\nDone.")


if __name__ == "__main__":
    main()