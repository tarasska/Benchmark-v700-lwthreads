#!/usr/bin/env python3
"""
plot_compare.py — compare two named benchmark setups on the same plot.

Same DS = same colour. Different setup = different line style.
Accepts paths in the multi-run layout produced by run_coroutine_sweep.sh:
  <results_dir>/v1/<ds_name>/result_cops<N>.json
  <results_dir>/v2/<ds_name>/result_cops<N>.json
  ...

Usage:
  python3 plot_compare.py \\
    --run "yield_wait  /path/to/output_a" \\
    --run "suspend_wait /path/to/output_b" \\
    --agg mean \\
    --ds treiber_stack_fc treiber_stack_fast \\
    --output-dir plots/

  # each --run is "LABEL  PATH" (whitespace-separated)
  # PATH may point at:
  #   - a single-run dir:  PATH/<ds>/result_cops*.json
  #   - a multi-run dir:   PATH/v1/<ds>/result_cops*.json, PATH/v2/...
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
from sweep_plot_common import (
    AXIS_LABELS, EXTRA_FIELDS, EXTRA_STATS, add_arguments, ds_style,
    enrich_metrics, load_styles, selected_metric_fields,
)

# ── style ─────────────────────────────────────────────────────────────────────
# Line style encodes setup/label identity — supports up to 4 setups
SETUP_STYLES = [
    dict(linestyle="solid",       linewidth=2.0),
    dict(linestyle="dashed",      linewidth=2.0),
    dict(linestyle="dashdot",     linewidth=2.0),
    dict(linestyle=(0, (3,1,1,1,1,1)),  linewidth=2.0),  # densely dotted
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
    "mean":   (np.mean,   lambda a: np.std(a, ddof=1),              "±1 std-dev"),
    "median": (np.median, lambda a: (np.percentile(a, 25),
                                     np.percentile(a, 75)),          "IQR"),
    "min":    (np.min,    None, None),
    "max":    (np.max,    None, None),
}


# ── data loading ──────────────────────────────────────────────────────────────

def aggregate(values, agg):
    arr = np.array(values, dtype=float)
    if len(arr) == 0 or not np.isfinite(arr).all():
        return float("nan"), 0.0, 0.0
    func, band_func, _ = AGG_FUNCS[agg]
    center = float(func(arr))
    if band_func is None or len(arr) < 2:
        return center, 0.0, 0.0
    band = band_func(arr)
    if isinstance(band, tuple):
        return center, center - float(band[0]), float(band[1]) - center
    return center, float(band), float(band)


def load_result_file(path, cops):
    with open(path) as f:
        data = json.load(f)
    data["coroutines"] = cops
    enrich_metrics(data)
    run_ms = data.get("max_time_thread_terminate_total", 0) / 1e6
    total  = data.get("sum_num_operations_total", 0)
    work   = data.get("work_iteration", 0)
    data["throughput_ops_per_sec"]      = total / run_ms if run_ms > 0 else 0.0
    data["work_throughput_ops_per_sec"] = work  / run_ms if run_ms > 0 else 0.0
    return data


def find_repeat_dirs(root):
    """Return sorted list of v* sub-dirs, or [root] if none found."""
    runs = sorted(
        (d for d in root.iterdir() if d.is_dir() and RUN_PAT.match(d.name)),
        key=lambda d: d.name,
    )
    return runs if runs else [root]


def load_setup(root, requested_ds, agg):
    """
    Load one setup directory.  Returns:
      { ds_name: [ {coroutines, throughput_ops_per_sec, ..._lo, ..._hi}, ... ] }
    """
    repeat_dirs = find_repeat_dirs(root)

    # raw[ds][cops] = [record, ...]
    raw = {}
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
                cops = int(m.group(1))
                rec  = load_result_file(p, cops)
                raw.setdefault(ds, {}).setdefault(cops, []).append(rec)

    FIELDS = [
        "throughput_ops_per_sec",
        "work_throughput_ops_per_sec",
        "sum_num_pushes_total",
        "sum_num_pops_total",
        "sum_num_fail_pops_total",
        "sum_num_operations_total",
    ]

    FIELDS += EXTRA_FIELDS

    targets = {}
    for ds, cops_map in sorted(raw.items()):
        records = []
        for cops in sorted(cops_map):
            recs = cops_map[cops]
            out  = {"coroutines": cops, "run_count": len(recs)}
            for field in FIELDS:
                vals = [r.get(field, float("nan") if field in EXTRA_FIELDS else 0) for r in recs]
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


def all_cops_from_setups(setups):
    ticks = set()
    for _, targets in setups:
        for records in targets.values():
            for r in records:
                ticks.add(r["coroutines"])
    return sorted(ticks)


def setup_xaxis(ax, ticks):
    ax.set_xscale("log", base=2)
    ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
    ax.set_xticks(ticks)
    ax.set_xlabel(AXIS_LABELS["compare_x"], fontsize=10)


def all_ds_names(setups):
    """Stable-ordered union of DS names across all setups."""
    seen, out = set(), []
    for _, targets in setups:
        for ds in targets:
            if ds not in seen:
                seen.add(ds)
                out.append(ds)
    return out


# ── plots ─────────────────────────────────────────────────────────────────────

def _draw_series(ax, x, y, y_lo, y_hi, color, marker, style, multi, field=None):
    ax.plot(x, y, marker=marker, color=color, markersize=6,
            **style)
    if multi and any(lo > 0 or hi > 0 for lo, hi in zip(y_lo, y_hi)):
        y_arr = np.array(y)
        lower, upper = y_arr - np.array(y_lo), y_arr + np.array(y_hi)
        if field in EXTRA_FIELDS:
            lower = np.maximum(lower, 0)
            if field in ("zero_progress_percent", "jain_fairness"):
                upper = np.minimum(upper, 100 if field == "zero_progress_percent" else 1)
        ax.fill_between(x, lower, upper, alpha=0.10, color=color)


def plot_comparison(setups, field, ylabel, title_base, output_dir, agg, filename):
    """
    Generic comparison plot: one line per (ds × setup) combination.
    Legend split into two sections: colours = DS, styles = setup labels.
    """
    if field in EXTRA_FIELDS and not any(
        np.isfinite(r.get(field, float("nan")))
        for _, targets in setups for records in targets.values() for r in records
    ):
        print(f"Warning: no finite values for {field}; plot skipped.", file=sys.stderr)
        return
    ds_names = all_ds_names(setups)
    multi    = any(
        any(r.get("run_count", 1) > 1 for r in recs)
        for _, targets in setups
        for recs in targets.values()
    )

    fig, ax = plt.subplots(figsize=(10, 5.5))

    for setup_idx, (label, targets) in enumerate(setups):
        style  = SETUP_STYLES[setup_idx % len(SETUP_STYLES)]
        for ds, records in targets.items():
            color  = ds_style(ds)["color"]
            marker = ds_style(ds)["marker"]
            x      = [r["coroutines"]         for r in records]
            y      = [r.get(field, float("nan") if field in EXTRA_FIELDS else 0) for r in records]
            y_lo   = [r.get(field + "_lo", 0)  for r in records]
            y_hi   = [r.get(field + "_hi", 0)  for r in records]
            _draw_series(ax, x, y, y_lo, y_hi, color, marker, style, multi, field)

    # ── legend: DS colours + setup line styles ────────────────────────────────
    colour_handles = [
        mlines.Line2D([], [], color=ds_style(ds)["color"],
                      linewidth=2, marker=ds_style(ds)["marker"],
                      markersize=6, label=ds_style(ds)["label"])
        for ds in ds_names
    ]
    style_handles = [
        mlines.Line2D([], [], color="black", label=label, **SETUP_STYLES[i % len(SETUP_STYLES)])
        for i, (label, _) in enumerate(setups)
    ]

    # Two-column legend: left = DS colours, right = setup styles
    leg1 = ax.legend(handles=colour_handles, loc="upper left",
                     title="data structure", framealpha=0.4, fontsize=9,
                     title_fontsize=8)
    ax.add_artist(leg1)
    ax.legend(handles=style_handles, loc="upper right",
              title="setup", framealpha=0.4, fontsize=9, title_fontsize=8)

    ticks = all_cops_from_setups(setups)
    setup_xaxis(ax, ticks)
    ax.set_ylabel(ylabel, fontsize=10)
    if field in ("zero_progress_percent", "jain_fairness"):
        ax.set_ylim(0, 100 if field == "zero_progress_percent" else 1)
    elif field in EXTRA_FIELDS:
        ax.set_ylim(bottom=0)
    agg_note = " [{}]".format(agg) if multi else ""
    ax.set_title(title_base + agg_note, fontsize=11, fontweight="bold")
    style_ax(ax)
    fig.tight_layout()

    out = output_dir / filename
    fig.savefig(out)
    plt.close(fig)
    print("  saved:", out)


def plot_summary_table(setups, output_dir, agg, field="throughput_ops_per_sec"):
    col = 18
    ds_names  = all_ds_names(setups)
    all_cops  = sorted({r["coroutines"]
                        for _, targets in setups
                        for recs in targets.values()
                        for r in recs})

    # index[setup_label][ds][cops] = record
    index = {}
    for label, targets in setups:
        index[label] = {}
        for ds, recs in targets.items():
            index[label][ds] = {r["coroutines"]: r for r in recs}

    setup_labels = [label for label, _ in setups]
    header_parts = ["  ".join(
        "{:>{}}".format("{}/{}".format(label, ds_style(ds)["label"]), col)
        for label in setup_labels
        for ds in ds_names
    )]
    header = "{:>6}  ".format("cops") + header_parts[0] + "  (ops/s [{}])".format(agg)
    sep    = "-" * len(header)

    rows = []
    for cops in all_cops:
        cells = []
        for label in setup_labels:
            for ds in ds_names:
                r = index[label].get(ds, {}).get(cops)
                if r is None:
                    cells.append("{:>{}}".format("N/A", col))
                else:
                    c  = r.get(field, 0)
                    lo = r.get(field + "_lo", 0)
                    hi = r.get(field + "_hi", 0)
                    if lo > 0 or hi > 0:
                        cell = "{:.0f}(-{:.0f}/+{:.0f})".format(c, lo, hi)
                    else:
                        cell = "{:.0f}".format(c)
                    cells.append("{:>{}}".format(cell, col))
        rows.append("{:>6}  ".format(cops) + "  ".join(cells))

    print("\n" + header)
    print(sep)
    for row in rows:
        print(row)
    print()

    out = output_dir / "comparison_summary.txt"
    with open(out, "w") as f:
        f.write(header + "\n" + sep + "\n" + "\n".join(rows) + "\n")
    print("  saved:", out)


# ── main ──────────────────────────────────────────────────────────────────────

def parse_run(s):
    """Parse 'LABEL /some/path' into (label, Path)."""
    parts = s.split(None, 1)
    if len(parts) != 2:
        raise argparse.ArgumentTypeError(
            "'{}' must be 'LABEL PATH' (whitespace-separated)".format(s))
    return parts[0], Path(parts[1])


ALL_PLOTS = ["throughput", "work_throughput", "combined", "summary"] + EXTRA_STATS


def main():
    parser = argparse.ArgumentParser(
        description="Compare two (or more) named benchmark setups on the same plot.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 plot_compare.py \\
    --run "yield_wait   /results/yield" \\
    --run "suspend_wait /results/suspend" \\
    --agg mean --output-dir plots/

  # filter to specific DS and plots
  python3 plot_compare.py \\
    --run "fc_8cores   /results/a" \\
    --run "fc_16cores  /results/b" \\
    --ds treiber_stack_fc treiber_stack \\
    --plot throughput summary
        """
    )
    parser.add_argument("--run", dest="runs", action="append",
                        type=parse_run, metavar="'LABEL PATH'", required=True,
                        help="Setup to include: 'LABEL /path/to/output' "
                             "(repeat for each setup, up to 4)")
    parser.add_argument("--agg", default="mean", choices=list(AGG_FUNCS),
                        help="Aggregation across repeats (default: mean)")
    parser.add_argument("--ds", nargs="*", default=[],
                        help="DS names to include (default: all found)")
    parser.add_argument("--plot", "--stat", nargs="+", default=ALL_PLOTS,
                        choices=ALL_PLOTS,
                        help="Which plots to generate (default: all)")
    parser.add_argument("--output-dir", type=Path, default=Path("compare_output"),
                        help="Directory for output PNGs (default: compare_output/)")
    add_arguments(parser)
    args = parser.parse_args()
    try:
        load_styles(args.ds_styles)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    if len({label for label, _ in args.runs}) != len(args.runs):
        parser.error("Setup labels must be unique")

    if len(args.runs) > len(SETUP_STYLES):
        print("Warning: more than {} setups — line styles will repeat.".format(
            len(SETUP_STYLES)), file=sys.stderr)

    requested_ds = set(args.ds)

    print("Loading setups:")
    setups = []
    for label, root in args.runs:
        if not root.is_dir():
            print("  ERROR: '{}' not found.".format(root), file=sys.stderr)
            sys.exit(1)
        targets = load_setup(root, requested_ds, args.agg)
        n_points = sum(len(r) for r in targets.values())
        print("  {:20s} → {} DS, {} data points  [{}]".format(
            label, len(targets), n_points, root))
        setups.append((label, targets))

    if not any(targets for _, targets in setups):
        print("No data loaded.", file=sys.stderr)
        sys.exit(1)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    print("\nOutput:", args.output_dir)
    print()

    plot_set = set(args.plot)
    for field, ylabel in selected_metric_fields(plot_set, args.latency_operations):
        plot_comparison(setups, field, ylabel, ylabel + " — setup comparison",
                        args.output_dir, args.agg, f"compare_{field}.png")

    if "throughput" in plot_set:
        plot_comparison(setups,
            field="throughput_ops_per_sec",
            ylabel=AXIS_LABELS["throughput"],
            title_base="DS throughput vs coroutines",
            output_dir=args.output_dir, agg=args.agg,
            filename="compare_throughput.png")

    if "work_throughput" in plot_set:
        plot_comparison(setups,
            field="work_throughput_ops_per_sec",
            ylabel=AXIS_LABELS["work_throughput"],
            title_base="work throughput vs coroutines",
            output_dir=args.output_dir, agg=args.agg,
            filename="compare_work_throughput.png")

    if "combined" in plot_set:
        # DS throughput (solid within style) vs work throughput (one shade lighter)
        # Achieved by overlaying two calls with the same axes
        ds_names = all_ds_names(setups)
        multi = any(
            any(r.get("run_count", 1) > 1 for r in recs)
            for _, targets in setups
            for recs in targets.values()
        )
        fig, ax = plt.subplots(figsize=(10, 5.5))

        for setup_idx, (label, targets) in enumerate(setups):
            base_style  = SETUP_STYLES[setup_idx % len(SETUP_STYLES)]
            # work throughput: same linestyle but thinner and lower alpha marker
            work_style  = dict(base_style, linewidth=1.2, alpha=0.6)
            for ds, records in targets.items():
                color  = ds_style(ds)["color"]
                marker = ds_style(ds)["marker"]
                x      = [r["coroutines"] for r in records]
                for field, style in [("throughput_ops_per_sec",      base_style),
                                     ("work_throughput_ops_per_sec",  work_style)]:
                    y    = [r.get(field, 0)         for r in records]
                    y_lo = [r.get(field + "_lo", 0) for r in records]
                    y_hi = [r.get(field + "_hi", 0) for r in records]
                    _draw_series(ax, x, y, y_lo, y_hi, color, marker, style, multi, field)

        colour_handles = [
            mlines.Line2D([], [], color=ds_style(ds)["color"],
                          linewidth=2, label=ds_style(ds)["label"])
            for ds in ds_names
        ]
        style_handles = [
            mlines.Line2D([], [], color="black", label=label, **SETUP_STYLES[i % len(SETUP_STYLES)])
            for i, (label, _) in enumerate(setups)
        ]
        style_handles += [
            mlines.Line2D([], [], color="black", linewidth=2,  label="DS throughput"),
            mlines.Line2D([], [], color="black", linewidth=1.2, alpha=0.6,
                          label="work throughput"),
        ]

        leg1 = ax.legend(handles=colour_handles, loc="upper left",
                         title="data structure", framealpha=0.4, fontsize=9,
                         title_fontsize=8)
        ax.add_artist(leg1)
        ax.legend(handles=style_handles, loc="upper right",
                  title="setup / metric", framealpha=0.4, fontsize=9,
                  title_fontsize=8)

        ticks = all_cops_from_setups(setups)
        setup_xaxis(ax, ticks)
        ax.set_ylabel(AXIS_LABELS["throughput"], fontsize=10)
        ax.set_title("DS vs work throughput — setup comparison", fontsize=11,
                     fontweight="bold")
        style_ax(ax)
        fig.tight_layout()
        out = args.output_dir / "compare_combined.png"
        fig.savefig(out)
        plt.close(fig)
        print("  saved:", out)

    if "summary" in plot_set:
        plot_summary_table(setups, args.output_dir, args.agg)

    print("\nDone.")


if __name__ == "__main__":
    main()