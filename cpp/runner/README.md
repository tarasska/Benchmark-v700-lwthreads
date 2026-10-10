# Sweep plots

`plot_sweep.py` plots a single sweep; `plot_sweep_v3.py` also aggregates repeated
sweeps (`v1/`, `v2/`, etc.). Both require `matplotlib` and `numpy`.

```sh
python3 plot_sweep.py --results-dir sweep_results \
  --stat throughput latency zero_progress_percent jain_fairness \
  --ds-styles ds_styles.example.json --output-dir plots

python3 plot_sweep_v3.py --results-dir sweep_results --repeats auto --agg median \
  --stat latency_p95 latency_p99 zero_progress_percent jain_fairness \
  --latency-operations all push pop --ds-styles ds_styles.example.json \
  --output-dir plots
```

## Names, colors and markers

Copy `ds_styles.example.json` and use the exact benchmark directory identifiers
as keys. All properties are optional:

```json
{
  "treiber_stack": {
    "label": "Treiber stack",
    "color": "#4C8EDA",
    "marker": "o"
  },
  "treiber_stack_fc": {
    "label": "Flat combining",
    "color": "darkorange",
    "marker": null
  }
}
```

`null` or `""` disables the marker; omitted marker selects a default. Colors accept
Matplotlib color names and hex strings. Marker examples: `o`, `s`, `^`, `D`, `x`.
Settings apply to old and new plots, titles and legends. `--ds` still selects the
original identifiers; labels do not rename result directories or output files.
Configured appearance is independent of filtering, ordering and repeat count.
Unconfigured targets receive deterministic defaults based on their identifiers.

Edit `AXIS_LABELS` in `sweep_plot_common.py` to change X and Y labels for both
scripts. X remains the `N` from `result_cops<N>.json`, as in the original scripts;
changing a label does not convert per-thread counts into total logical workers.

## New metrics

- `--stat latency`: three separate mean/p95/p99 plots, each with one line per DS.
- `--stat latency_mean latency_p95 latency_p99`: select individual latency plots.
- `--latency-operations all`: combined operation latency (default).
  Use e.g. `--latency-operations push pop` for separate operation plots.
  Other choices: `insert remove get contains range_query`.
- `--stat zero_progress_percent`: percentage of logical workers without completed
  operations, Y = 0–100%; lower is better.
- `--stat jain_fairness`: fairness index, Y = 0–1; higher is better.
- `--stat zero_progress_workers worker_count`: absolute worker counts.

Latency is in nanoseconds. Example output filenames:
`latency_all_p99_ns_vs_coroutines.png`, `latency_pop_mean_ns_vs_coroutines.png`,
`zero_progress_percent_vs_coroutines.png`, `jain_fairness_vs_coroutines.png`.
Omitting `--stat` generates existing plots plus the new metrics when available.

Missing metrics and JSON `null` (including latency percentiles above the histogram
range) create gaps, never zeros. Plots with no finite points are skipped with a
warning. Existing result files still support their original plots. Latency cannot
be reconstructed from throughput. Fairness can be reconstructed from operation
counters if `worker_count` is available; otherwise it stays missing because
GSTATS trims trailing zero counters. With a known count these zeros are also
restored for the existing distribution plot.

In v3, each new scalar is aggregated across runs using `--agg`, with the same
standard-deviation/IQR bands as throughput. Fairness is computed per run, never
from pooled operation counts. Aggregated p99 means the mean/median/etc. of each
run's p99, **not** the p99 of all pooled operations (raw histograms are unavailable).
If any contributing run lacks a finite metric, that point is missing rather than
silently averaging only the successful measurements. Bands are clipped to the
metric's physical range. Existing `summary.txt` remains a throughput table.

Tests (from repository root):

```sh
python3 -m unittest discover -s cpp/runner/tests -v
```

## Comparing named setups

`plot_compare.py` supports the same metrics and DS style JSON. Color and marker
identify a data structure; solid/dashed/etc. lines identify the named setup.
Each setup may contain a single run or automatically discovered `v1/`, `v2/`, etc.

```sh
python3 plot_compare.py \
  --run "yield /results/with-yield" \
  --run "no-yield /results/without-yield" \
  --plot throughput latency zero_progress_percent jain_fairness \
  --latency-operations all push pop \
  --ds-styles ds_styles.example.json --agg median --output-dir compare_plots
```

`--stat` is an alias for `--plot`. All new metrics, missing-value rules and
per-run aggregation semantics described above apply. Outputs include
`compare_latency_all_p99_ns.png`, `compare_zero_progress_percent.png` and
`compare_jain_fairness.png`. Existing throughput, combined and summary outputs
remain available. Setup labels must be unique.

Edit `AXIS_LABELS["compare_x"]` for the comparison X axis;
`AXIS_LABELS["work_throughput"]` controls the work-throughput Y label. Other
Y labels are shared with the sweep scripts. Setup line styles remain editable
in `SETUP_STYLES` inside `plot_compare.py`.
