"""Shared metric extraction, labels and DS appearance for both sweep plotters."""
import hashlib
import json
import math
import sys
from pathlib import Path

import matplotlib.colors as colors
from matplotlib.markers import MarkerStyle
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# Edit axis labels here; used by both entry points.
AXIS_LABELS = {
    "x": "coroutines per thread",
    "throughput": "throughput (ops / s)",
    "operations": "total operations",
    "per_thread": "ops per thread",
    "latency": "latency (ns)",
    "zero_progress_percent": "workers without completed operations (%)",
    "jain_fairness": "Jain fairness index",
    "zero_progress_workers": "workers without completed operations",
    "worker_count": "logical workers",
}
LATENCY_OPERATIONS = ["all", "insert", "remove", "get", "contains", "range_query", "push", "pop"]
PROGRESS_FIELDS = ["zero_progress_percent", "jain_fairness", "zero_progress_workers", "worker_count"]
LATENCY_FIELDS = [f"latency_{op}_{stat}_ns" for op in LATENCY_OPERATIONS for stat in ("mean", "p95", "p99")]
EXTRA_FIELDS = PROGRESS_FIELDS + LATENCY_FIELDS
EXTRA_STATS = ["latency", "latency_mean", "latency_p95", "latency_p99"] + PROGRESS_FIELDS
PALETTE = ["#4C8EDA", "#E06C4B", "#3BAA72", "#9B6DD4", "#E0B84B", "#4BC7CE", "#D45E8A", "#7A7A7A"]
MARKERS = ["o", "s", "^", "D", "v", "P", "X", "*"]
DS_STYLES = {}


def load_styles(path):
    """JSON maps exact benchmark DS identifiers to label/color/marker overrides."""
    styles = {} if path is None else json.loads(Path(path).read_text())
    if not isinstance(styles, dict):
        raise ValueError("DS style file must be a JSON object")
    for name, style in styles.items():
        if not isinstance(style, dict) or set(style) - {"label", "color", "marker"}:
            raise ValueError(f"Invalid style for {name}: use label, color, marker")
        if "label" in style and not isinstance(style["label"], str):
            raise ValueError(f"Invalid label for {name}")
        if "color" in style and not colors.is_color_like(style["color"]):
            raise ValueError(f"Invalid color for {name}")
        if "marker" in style:
            MarkerStyle(style["marker"] or "")
    DS_STYLES.clear()
    DS_STYLES.update(styles)


def ds_style(name):
    # Stable defaults even when --ds filters or directory discovery order change.
    index = int.from_bytes(hashlib.sha256(name.encode()).digest()[:4], "big")
    result = dict(label=name, color=PALETTE[index % len(PALETTE)], marker=MARKERS[index % len(MARKERS)])
    result.update(DS_STYLES.get(name, {}))
    if result["marker"] is None:
        result["marker"] = ""
    return result


def finite_number(value):
    return float(value) if isinstance(value, (int, float)) and math.isfinite(value) else float("nan")


def enrich_metrics(data):
    latency = data.get("latency_ns") or {}
    for op in LATENCY_OPERATIONS:
        summary = latency.get(op) or {}
        for stat in ("mean", "p95", "p99"):
            data[f"latency_{op}_{stat}_ns"] = finite_number(summary.get(stat))

    # Old JSON can be reconstructed only when its full worker count is known.
    # Never infer that count from a GSTATS array with trimmed trailing zeros.
    count = data.get("worker_count")
    counters = data.get("sum_num_operations_by_thread")
    if isinstance(count, int) and count >= 0 and isinstance(counters, list):
        if len(counters) > count or any(not isinstance(n, int) or n < 0 for n in counters):
            raise ValueError("Invalid per-worker operation counters")
        counters = counters + [0] * (count - len(counters))
        data["sum_num_operations_by_thread"] = counters
        zero = counters.count(0)
        squares = sum(n * n for n in counters)
        data.setdefault("zero_progress_workers", zero)
        data.setdefault("zero_progress_percent", 100 * zero / count if count else None)
        data.setdefault("jain_fairness", sum(counters) ** 2 / (count * squares) if squares else None)
    for field in PROGRESS_FIELDS:
        data[field] = finite_number(data.get(field))


def add_arguments(parser):
    parser.add_argument("--ds-styles", type=Path, help="JSON file mapping DS IDs to label/color/marker")
    parser.add_argument("--latency-operations", nargs="+", choices=LATENCY_OPERATIONS, default=["all"],
                        help="Operation types for latency plots (default: all = combined operations)")


def plot_extra_metrics(targets, output_dir, stats, operations, agg=None, multi=False):
    fields = [(field, AXIS_LABELS[field]) for field in PROGRESS_FIELDS if field in stats]
    for stat in ("mean", "p95", "p99"):
        if "latency" in stats or f"latency_{stat}" in stats:
            fields.extend((f"latency_{op}_{stat}_ns", f"{op}: {stat} {AXIS_LABELS['latency']}")
                          for op in operations)
    ticks = sorted({r["coroutines"] for records in targets.values() for r in records})
    for field, ylabel in fields:
        if not any(math.isfinite(r.get(field, float("nan"))) for records in targets.values() for r in records):
            print(f"Warning: no finite values for {field}; plot skipped.", file=sys.stderr)
            continue
        fig, ax = plt.subplots(figsize=(9, 5))
        for ds, records in targets.items():
            x = [r["coroutines"] for r in records]
            y = np.array([r.get(field, float("nan")) for r in records])
            if not np.isfinite(y).any():
                continue
            style = ds_style(ds)
            ax.plot(x, y, linewidth=2, markersize=6, **style)
            if multi:
                lo = np.array([r.get(field + "_lo", 0) for r in records])
                hi = np.array([r.get(field + "_hi", 0) for r in records])
                lower, upper = y - lo, y + hi
                lower = np.maximum(lower, 0)
                if field in ("zero_progress_percent", "jain_fairness"):
                    upper = np.minimum(upper, 100 if field == "zero_progress_percent" else 1)
                ax.fill_between(x, lower, upper, color=style["color"], alpha=0.15)
        ax.set_xscale("log", base=2)
        ax.set_xticks(ticks)
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
        ax.set_xlabel(AXIS_LABELS["x"])
        ax.set_ylabel(ylabel)
        ax.set_title(ylabel + (f" [{agg} across runs]" if multi else ""))
        if field in ("zero_progress_percent", "jain_fairness"):
            ax.set_ylim(0, 100 if field == "zero_progress_percent" else 1)
        else:
            ax.set_ylim(bottom=0)
        ax.legend(framealpha=0.4)
        fig.tight_layout()
        out = output_dir / f"{field}_vs_coroutines.png"
        fig.savefig(out)
        plt.close(fig)
        print("  saved:", out)
