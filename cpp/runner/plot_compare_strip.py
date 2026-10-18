#!/usr/bin/env python3
"""Three comparison panels with shared numeric limits and separate labeled axes."""
import argparse
import json
from pathlib import Path

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

from plot_compare import (
    AGG_FUNCS, SETUP_STYLES, load_setup, find_repeat_dirs, draw_comparison_axes,
    all_cops_from_setups,
)
from sweep_plot_common import (
    AXIS_LABELS, EXTRA_STATS, add_arguments, load_styles, configure_ds_styles,
    discover_style_names, selected_metric_fields, set_core_count,
)


def load_panels(path, requested_ds, agg):
    config = json.loads(path.read_text())
    labels = config.get("thread_types")
    panels = config.get("panels")
    if (not isinstance(labels, list) or not labels or
            any(not isinstance(label, str) or not label.strip() for label in labels) or
            len(set(labels)) != len(labels) or len(labels) > len(SETUP_STYLES)):
        raise ValueError("thread_types must contain 1–4 unique nonempty labels")
    if not isinstance(panels, list) or len(panels) != 3:
        raise ValueError("panels must contain exactly three panels")
    loaded, style_dirs = [], []
    for index, panel in enumerate(panels, 1):
        if not isinstance(panel, dict):
            raise ValueError(f"Panel {index} must be an object")
        cores, paths = panel.get("cores"), panel.get("paths")
        if type(cores) is not int or cores <= 0:
            raise ValueError(f"Panel {index}: cores must be a positive integer")
        if (not isinstance(paths, list) or len(paths) != len(labels) or
                any(not isinstance(value, str) or not value for value in paths)):
            raise ValueError(f"Panel {index}: paths must match thread_types in length and order")
        setups = []
        for label, value in zip(labels, paths):
            root = path.parent / value
            if not root.is_dir():
                raise ValueError(f"Result directory not found: {root}")
            targets = load_setup(root, requested_ds, agg)
            if not targets:
                raise ValueError(f"No matching results in {root}")
            setups.append((label, targets))
            style_dirs.extend(find_repeat_dirs(root))
        loaded.append((cores, setups))
    configure_ds_styles(discover_style_names(style_dirs))
    return loaded


def draw_strip(panels, field, ylabel):
    if not any(np.isfinite(r.get(field, float("nan")))
               for _, setups in panels for _, targets in setups
               for records in targets.values() for r in records):
        return None
    fig, axes = plt.subplots(1, 3, figsize=(21, 5.5))
    ticks = sorted({tick for _, setups in panels for tick in all_cops_from_setups(setups)})
    try:
        for ax, (cores, setups) in zip(axes, panels):
            set_core_count(cores)
            draw_comparison_axes(ax, setups, field, ylabel)
        # Collect limits only after all data, uncertainty bands and core markers
        # are drawn. Synchronize both limits and ticks, retaining labels per panel.
        xlim = (min(ax.get_xlim()[0] for ax in axes), max(ax.get_xlim()[1] for ax in axes))
        ylim = (min(ax.get_ylim()[0] for ax in axes), max(ax.get_ylim()[1] for ax in axes))
        for ax in axes:
            ax.set_xticks(ticks)
            ax.set_xlim(xlim)
            ax.set_ylim(ylim)
            ax.yaxis.set_major_locator(ticker.MaxNLocator(nbins=6))
        fig.tight_layout()
    finally:
        set_core_count(None)
    return fig


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--panels-config", type=Path, required=True)
    parser.add_argument("--ds", nargs="*", default=[])
    parser.add_argument("--agg", choices=list(AGG_FUNCS), default="mean")
    parser.add_argument("--plot", "--stat", nargs="+", default=["throughput"],
                        choices=["throughput", "work_throughput"] + EXTRA_STATS)
    parser.add_argument("--output-dir", type=Path, default=Path("compare_strip_output"))
    add_arguments(parser)
    args = parser.parse_args()
    try:
        load_styles(args.ds_styles)
        panels = load_panels(args.panels_config, set(args.ds), args.agg)
    except (OSError, ValueError, TypeError, AttributeError) as error:
        parser.error(str(error))
    fields = []
    for plot in ("throughput", "work_throughput"):
        if plot in args.plot:
            fields.append((plot + "_ops_per_sec", AXIS_LABELS[plot]))
    fields.extend(selected_metric_fields(set(args.plot), args.latency_operations))
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for field, ylabel in fields:
        fig = draw_strip(panels, field, ylabel)
        if fig is None:
            print(f"No finite values for {field}; strip skipped")
            continue
        output = args.output_dir / f"compare_strip_{field}.png"
        fig.savefig(output, dpi=300)
        plt.close(fig)
        print("saved:", output)


if __name__ == "__main__":
    main()
