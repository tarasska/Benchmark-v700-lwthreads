#!/usr/bin/env python3
"""
plot_java_compare.py — compare two or more named Java benchmark setups on the same plot.

Same DS = same colour. Different setup = different line style.
Accepts paths in the multi-run layout produced by run_java_sweep.sh:
  <results_dir>/v1/<DsShortName>/result_cops<N>.json
  <results_dir>/v2/<DsShortName>/result_cops<N>.json
  ...
Single-run dirs (<results_dir>/<DsShortName>/result_cops<N>.json) also work.

Usage:
  python3 plot_java_compare.py \\
    --run "platform_threads  /results/os" \\
    --run "virtual_threads   /results/vt" \\
    --agg mean --num-cores 32 --output-dir plots/

  # select specific DS and plots
  python3 plot_java_compare.py \\
    --run "fc_queue   /results/fc" \\
    --run "lf_queue   /results/lf" \\
    --ds LockFreeQueueIntSet FlatCombiningQueue \\
    --plot throughput summary

  # compare custom map metrics across setups
  python3 plot_java_compare.py \\
    --run "setup_a /results/a" \\
    --run "setup_b /results/b" \\
    --custom-metrics all
"""

import argparse
import json
import re
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.lines as mlines
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# ── style (identical palette to plot_java_sweep.py and plot_compare.py) ───────
DS_COLORS  = ["#4C8EDA", "#E06C4B", "#3BAA72", "#9B6DD4",
               "#E0B84B", "#4BC7CE", "#D45E8A", "#7A7A7A"]
DS_MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]

SETUP_STYLES = [
    dict(linestyle="solid",            linewidth=2.0),
    dict(linestyle="dashed",           linewidth=2.0),
    dict(linestyle="dashdot",          linewidth=2.0),
    dict(linestyle=(0, (3,1,1,1,1,1)), linewidth=2.0),
]

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
RUN_PAT    = re.compile(r"^v\d+$")

AGG_FUNCS = {
    "mean":   (np.mean,   lambda a: np.std(a, ddof=1),
                lambda a: np.std(a, ddof=1),   "±1 std-dev"),
    "median": (np.median, lambda a: np.percentile(a, 25),
                lambda a: np.percentile(a, 75), "IQR"),
    "min":    (np.min,    None, None, None),
    "max":    (np.max,    None, None, None),
}


# ── data loading ──────────────────────────────────────────────────────────────

def aggregate(values, agg):
    arr = np.array(values, dtype=float)
    if len(arr) == 0:
        return float("nan"), 0.0, 0.0
    func, lo_func, hi_func, _ = AGG_FUNCS[agg]
    center = float(func(arr))
    if lo_func is None or len(arr) < 2:
        return center, 0.0, 0.0
    if agg == "median":
        return center, center - float(lo_func(arr)), float(hi_func(arr)) - center
    err = float(lo_func(arr))
    return center, err, err


def parse_java_result(path, n_threads):
    with open(path) as f:
        raw = json.load(f)
    r  = raw[-1]
    cs = r.get("commonStatistic", {})

    record = {
        "threads":                      n_threads,
        "throughput_ops_per_sec":       r.get("throughput", 0.0),
        "sum_num_operations_total":     cs.get("total", 0),
        "sum_num_pushes_total":         cs.get("numAdd", 0),
        "sum_num_pops_total":           cs.get("numRemove", 0),
        "sum_num_fail_pops_total":      cs.get("failures", 0),
        "effective_updates":            r.get("effectiveUpdates", 0),
        "elapsed_time_s":               r.get("elapsedTime", 0),
        "opsPerCombine":                r.get("opsPerCombine",   0.0),
        "nanosPerCombine":              r.get("nanosPerCombine", 0.0),
    }

    # flatten custom map with prefix
    for k, v in r.get("custom", {}).items():
        record["custom." + k] = float(v) if isinstance(v, (int, float)) else 0.0

    return record


def find_repeat_dirs(root):
    runs = sorted(
        (d for d in root.iterdir() if d.is_dir() and RUN_PAT.match(d.name)),
        key=lambda d: d.name,
    )
    return runs if runs else [root]


def load_setup(root, requested_ds, agg):
    """
    Load one setup root directory, aggregating across v* repeat subdirs.
    Returns {ds_name: [{threads, field, field_lo, field_hi, ...}, ...]}
    """
    repeat_dirs = find_repeat_dirs(root)

    raw = {}   # raw[ds][n_threads] = [record, ...]
    for rdir in repeat_dirs:
        for child in sorted(rdir.iterdir()):
            if not child.is_dir():
                continue
            ds = child.name
            if requested_ds and ds not in requested_ds:
                continue
            for p in sorted(child.glob("result_cops*.json")):
                m = RESULT_PAT.match(p.name)
                if not m:
                    continue
                n_threads = int(m.group(1))
                rec = parse_java_result(p, n_threads)
                raw.setdefault(ds, {}).setdefault(n_threads, []).append(rec)

    # collect every custom.* key seen
    custom_keys = sorted({
        k for threads_map in raw.values()
        for recs in threads_map.values()
        for rec in recs for k in rec if k.startswith("custom.")
    })

    FIELDS = [
        "throughput_ops_per_sec",
        "sum_num_operations_total",
        "sum_num_pushes_total",
        "sum_num_pops_total",
        "sum_num_fail_pops_total",
        "effective_updates",
        "opsPerCombine",
        "nanosPerCombine",
    ] + custom_keys

    targets = {}
    for ds, threads_map in sorted(raw.items()):
        records = []
        for n_threads in sorted(threads_map):
            recs = threads_map[n_threads]
            out  = {"threads": n_threads, "run_count": len(recs)}
            for field in FIELDS:
                vals = [r.get(field, 0) for r in recs]
                c, lo, hi = aggregate(vals, agg)
                out[field]          = c
                out[field + "_lo"]  = lo
                out[field + "_hi"]  = hi
            records.append(out)
        targets[ds] = records
    return targets


# ── helpers ───────────────────────────────────────────────────────────────────

def style_ax(ax):
    for spine in ("left", "bottom"):
        ax.spines[spine].set_color(SPINE_COLOR)
    ax.tick_params(colors="#555555")
    ax.yaxis.label.set_color("#333333")
    ax.xaxis.label.set_color("#333333")
    ax.title.set_color("#222222")


def all_thread_ticks(setups):
    ticks = set()
    for _, targets in setups:
        for records in targets.values():
            for r in records:
                ticks.add(r["threads"])
    return sorted(ticks)


def fig_width(ticks, base=10.0, per_tick=0.55, minimum=7.0):
    return max(minimum, base + max(0, len(ticks) - 8) * per_tick)


def setup_xaxis(ax, ticks):
    if len(ticks) > 1 and ticks[-1] / ticks[0] >= 8:
        ax.set_xscale("log", base=2)
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_xticks(ticks)
    ax.tick_params(axis="x", labelrotation=45 if len(ticks) > 10 else 0)
    ax.set_xlabel("threads", fontsize=10)


def draw_core_vline(ax, num_cores):
    if num_cores is None:
        return
    ax.axvline(x=num_cores, color="#888888", linewidth=1.2,
               linestyle="dotted", zorder=1)
    ylim = ax.get_ylim()
    ax.text(num_cores, ylim[1] - (ylim[1] - ylim[0]) * 0.03,
            " {} cores".format(num_cores),
            color="#666666", fontsize=8, va="top", ha="left")


def all_ds_names(setups):
    seen, out = set(), []
    for _, targets in setups:
        for ds in targets:
            if ds not in seen:
                seen.add(ds)
                out.append(ds)
    return out


def all_custom_keys_from_setups(setups):
    return sorted({
        k for _, targets in setups
        for recs in targets.values()
        for r in recs for k in r if k.startswith("custom.")
    })


def _draw_series(ax, x, y, y_lo, y_hi, color, marker, style, multi):
    ax.plot(x, y, marker=marker, color=color, markersize=6,
            markeredgewidth=0, **style)
    if multi and any(lo > 0 or hi > 0 for lo, hi in zip(y_lo, y_hi)):
        y_arr = np.array(y)
        ax.fill_between(x, y_arr - np.array(y_lo),
                           y_arr + np.array(y_hi),
                        alpha=0.10, color=color)


# ── generic comparison plot ───────────────────────────────────────────────────

def plot_comparison(setups, field, ylabel, title_base, output_dir, agg,
                    filename, num_cores=None):
    """
    One line per (ds × setup). Colour = DS, line style = setup.
    Silently skips if no setup has non-zero data for the field.
    """
    has_data = any(
        any(r.get(field, 0) != 0 for r in recs)
        for _, targets in setups
        for recs in targets.values()
    )
    if not has_data:
        print("  skip (no data): {}".format(field))
        return

    ds_names = all_ds_names(setups)
    ds_index = {ds: i for i, ds in enumerate(ds_names)}
    multi    = any(
        any(r.get("run_count", 1) > 1 for r in recs)
        for _, targets in setups
        for recs in targets.values()
    )

    ticks = all_thread_ticks(setups)
    fig, ax = plt.subplots(figsize=(fig_width(ticks), 5.5))

    for setup_idx, (label, targets) in enumerate(setups):
        style = SETUP_STYLES[setup_idx % len(SETUP_STYLES)]
        for ds, records in targets.items():
            di     = ds_index[ds]
            color  = DS_COLORS[di % len(DS_COLORS)]
            marker = DS_MARKERS[di % len(DS_MARKERS)]
            x      = [r["threads"]          for r in records]
            y      = [r.get(field, 0)        for r in records]
            y_lo   = [r.get(field + "_lo", 0) for r in records]
            y_hi   = [r.get(field + "_hi", 0) for r in records]
            _draw_series(ax, x, y, y_lo, y_hi, color, marker, style, multi)

    colour_handles = [
        mlines.Line2D([], [], color=DS_COLORS[ds_index[ds] % len(DS_COLORS)],
                      linewidth=2,
                      marker=DS_MARKERS[ds_index[ds] % len(DS_MARKERS)],
                      markersize=6, label=ds)
        for ds in ds_names
    ]
    style_handles = [
        mlines.Line2D([], [], color="black", label=label, **style)
        for (label, _), style in zip(setups, SETUP_STYLES)
    ]

    leg1 = ax.legend(handles=colour_handles, loc="upper left",
                     title="data structure", framealpha=0.4, fontsize=9,
                     title_fontsize=8)
    ax.add_artist(leg1)
    ax.legend(handles=style_handles, loc="upper right",
              title="setup", framealpha=0.4, fontsize=9, title_fontsize=8)

    setup_xaxis(ax, ticks)
    ax.set_ylabel(ylabel, fontsize=10)
    agg_note = " [{}]".format(agg) if multi else ""
    ax.set_title(title_base + agg_note, fontsize=11, fontweight="bold")
    draw_core_vline(ax, num_cores)
    style_ax(ax)
    fig.tight_layout()

    out = output_dir / filename
    fig.savefig(out)
    plt.close(fig)
    print("  saved:", out)


# ── summary table ─────────────────────────────────────────────────────────────

def plot_summary_table(setups, output_dir, agg,
                       field="throughput_ops_per_sec"):
    col       = 20
    ds_names  = all_ds_names(setups)
    all_t     = sorted({r["threads"]
                        for _, targets in setups
                        for recs in targets.values()
                        for r in recs})

    index = {}
    for label, targets in setups:
        index[label] = {ds: {r["threads"]: r for r in recs}
                        for ds, recs in targets.items()}

    setup_labels = [label for label, _ in setups]
    header = "{:>8}  ".format("threads") + "  ".join(
        "{:>{}}".format("{}/{}".format(lbl, ds), col)
        for lbl in setup_labels for ds in ds_names
    ) + "  (ops/s [{}])".format(agg)
    sep  = "-" * len(header)
    rows = []
    for t in all_t:
        cells = []
        for lbl in setup_labels:
            for ds in ds_names:
                r = index.get(lbl, {}).get(ds, {}).get(t)
                if r is None:
                    cells.append("{:>{}}".format("N/A", col))
                else:
                    c, lo, hi = (r.get(field, 0),
                                 r.get(field + "_lo", 0),
                                 r.get(field + "_hi", 0))
                    cell = ("{:.0f}(-{:.0f}/+{:.0f})".format(c, lo, hi)
                            if (lo > 0 or hi > 0) else "{:.0f}".format(c))
                    cells.append("{:>{}}".format(cell, col))
        rows.append("{:>8}  ".format(t) + "  ".join(cells))

    print("\n" + header)
    print(sep)
    for row in rows:
        print(row)
    print()

    out = output_dir / "java_comparison_summary.txt"
    with open(out, "w") as f:
        f.write(header + "\n" + sep + "\n" + "\n".join(rows) + "\n")
    print("  saved:", out)


# ── main ──────────────────────────────────────────────────────────────────────

def parse_run(s):
    parts = s.split(None, 1)
    if len(parts) != 2:
        raise argparse.ArgumentTypeError(
            "'{}' must be 'LABEL PATH' (whitespace-separated)".format(s))
    return parts[0], Path(parts[1])


ALL_PLOTS = ["throughput", "ops_breakdown", "combining", "custom", "summary"]


def main():
    parser = argparse.ArgumentParser(
        description="Compare Java benchmark setups on the same plot.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 plot_java_compare.py \\
    --run "platform  /results/os" \\
    --run "virtual   /results/vt" \\
    --agg mean --num-cores 32

  # specific DS + only throughput and summary
  python3 plot_java_compare.py \\
    --run "fc  /results/fc" \\
    --run "lf  /results/lf" \\
    --ds LockFreeQueueIntSet FlatCombiningQueue \\
    --plot throughput summary

  # compare custom map metrics across setups
  python3 plot_java_compare.py \\
    --run "a /results/a" --run "b /results/b" \\
    --custom-metrics all
        """
    )
    parser.add_argument("--run", dest="runs", action="append",
                        type=parse_run, metavar="'LABEL PATH'", required=True,
                        help="Setup: 'LABEL /path/to/output' (repeat for each, up to 4)")
    parser.add_argument("--agg", default="mean", choices=list(AGG_FUNCS),
                        help="Aggregation across repeats (default: mean)")
    parser.add_argument("--ds", nargs="*", default=[],
                        help="DS short names to include (default: all found)")
    parser.add_argument("--plot", nargs="+", default=ALL_PLOTS,
                        choices=ALL_PLOTS,
                        help="Which plots to generate (default: all)")
    parser.add_argument("--num-cores", type=int, default=None, metavar="N",
                        help="Draw a vertical reference line at N threads")
    parser.add_argument("--output-dir", type=Path, default=Path("java_compare_output"))
    parser.add_argument("--custom-metrics", nargs="+", default=[],
                        metavar="NAME",
                        help="Fields from the 'custom' JSON map to compare. "
                             "Use 'all' for every field found, or name specific "
                             "fields (e.g. meanOpsPerCombine stdNanosPerCombine)")
    args = parser.parse_args()

    if len(args.runs) > len(SETUP_STYLES):
        print("Warning: more than {} setups — styles will repeat.".format(
            len(SETUP_STYLES)), file=sys.stderr)

    requested_ds = set(args.ds)

    print("Loading setups:")
    setups = []
    for label, root in args.runs:
        if not root.is_dir():
            print("  ERROR: '{}' not found.".format(root), file=sys.stderr)
            sys.exit(1)
        targets = load_setup(root, requested_ds, args.agg)
        n_pts = sum(len(r) for r in targets.values())
        print("  {:20s} → {} DS, {} thread pts  [{}]".format(
            label, len(targets), n_pts, root))
        setups.append((label, targets))

    if not setups:
        print("No data loaded.", file=sys.stderr)
        sys.exit(1)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    print("\nOutput:", args.output_dir)
    print()

    plot_set = set(args.plot)

    if "throughput" in plot_set:
        plot_comparison(setups,
            field="throughput_ops_per_sec",
            ylabel="throughput  (ops / s)",
            title_base="throughput vs threads",
            output_dir=args.output_dir, agg=args.agg,
            filename="java_compare_throughput.png",
            num_cores=args.num_cores)

    if "ops_breakdown" in plot_set:
        for field, ylabel, fname in [
            ("sum_num_pushes_total", "total adds",    "java_compare_adds.png"),
            ("sum_num_pops_total",   "total removes", "java_compare_removes.png"),
        ]:
            plot_comparison(setups, field=field, ylabel=ylabel,
                            title_base=ylabel + " vs threads",
                            output_dir=args.output_dir, agg=args.agg,
                            filename=fname, num_cores=args.num_cores)

    if "combining" in plot_set:
        for field, ylabel, fname in [
            ("opsPerCombine",   "ops per combining round",
             "java_compare_ops_per_combine.png"),
            ("nanosPerCombine", "ns per combining round",
             "java_compare_nanos_per_combine.png"),
        ]:
            plot_comparison(setups, field=field, ylabel=ylabel,
                            title_base=ylabel + " vs threads",
                            output_dir=args.output_dir, agg=args.agg,
                            filename=fname, num_cores=args.num_cores)

    if "custom" in plot_set and args.custom_metrics:
        all_custom = all_custom_keys_from_setups(setups)
        if args.custom_metrics == ["all"]:
            selected = all_custom
        else:
            selected = [
                "custom." + n if not n.startswith("custom.") else n
                for n in args.custom_metrics
            ]
            missing = [f for f in selected if f not in all_custom]
            if missing:
                print("Warning: custom fields not found:",
                      ", ".join(m.replace("custom.", "") for m in missing),
                      file=sys.stderr)

        if selected:
            print("Custom metrics ({} fields):".format(len(selected)))
        for field in selected:
            short = field.replace("custom.", "")
            safe  = re.sub(r"[^A-Za-z0-9_\-]", "_", short)
            plot_comparison(setups, field=field,
                            ylabel=short,
                            title_base=short + " vs threads",
                            output_dir=args.output_dir, agg=args.agg,
                            filename="java_compare_custom_{}.png".format(safe),
                            num_cores=args.num_cores)

    if "summary" in plot_set:
        plot_summary_table(setups, args.output_dir, args.agg)

    print("\nDone.")


if __name__ == "__main__":
    main()
