#!/usr/bin/env python3
"""Comparison panels in rows with shared numeric limits and separate labeled axes."""
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


def validate_thread_types(labels, location):
    if (not isinstance(labels, list) or not labels or
            any(not isinstance(label, str) or not label.strip() for label in labels) or
            len(set(labels)) != len(labels) or len(labels) > len(SETUP_STYLES)):
        raise ValueError(f"{location}: thread_types must contain 1–4 unique nonempty labels")


def load_panels(path, requested_ds, agg):
    config = json.loads(path.read_text())
    default_labels = config.get("thread_types")
    if "thread_types" in config:
        validate_thread_types(default_labels, "Config")
    if "rows" in config and "panels" in config:
        raise ValueError("Use either rows or panels, not both")
    if "rows" in config:
        rows = config["rows"]
        if not isinstance(rows, list) or not rows:
            raise ValueError("rows must contain at least one row")
        for index, row in enumerate(rows, 1):
            if not isinstance(row, list) or len(row) not in (2, 3):
                raise ValueError(f"Row {index} must contain two or three panels")
    else:
        panels = config.get("panels")
        if not isinstance(panels, list) or len(panels) != 3:
            raise ValueError("panels must contain exactly three panels")
        rows = [panels]
    loaded, style_dirs = [], []
    for row_index, row in enumerate(rows, 1):
        loaded_row = []
        for panel_index, panel in enumerate(row, 1):
            location = f"Row {row_index}, panel {panel_index}"
            if not isinstance(panel, dict):
                raise ValueError(f"{location} must be an object")
            labels = panel.get("thread_types", default_labels)
            validate_thread_types(labels, location)
            cores, paths = panel.get("cores"), panel.get("paths")
            if type(cores) is not int or cores <= 0:
                raise ValueError(f"{location}: cores must be a positive integer")
            if (not isinstance(paths, list) or len(paths) != len(labels) or
                    any(not isinstance(value, str) or not value for value in paths)):
                raise ValueError(f"{location}: paths must match thread_types in length and order")
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
            loaded_row.append((cores, setups))
        loaded.append(loaded_row)
    configure_ds_styles(discover_style_names(style_dirs))
    return loaded


def draw_strip(rows, field, ylabel):
    panels = [panel for row in rows for panel in row]
    if not any(np.isfinite(r.get(field, float("nan")))
               for _, setups in panels for _, targets in setups
               for records in targets.values() for r in records):
        return None
    row_count = len(rows)
    column_count = max(len(row) for row in rows)
    fig, grid = plt.subplots(row_count, column_count,
                             figsize=(7 * column_count, 5.5 * row_count),
                             squeeze=False)
    axes = []
    for row_index, row in enumerate(rows):
        axes.extend(grid[row_index, :len(row)])
        for ax in grid[row_index, len(row):]:
            fig.delaxes(ax)
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
