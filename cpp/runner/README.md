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
Unconfigured targets receive distinct colors allocated in sorted identifier order
from the full set of result directories, before `--ds` filtering. All explicitly
configured colors are reserved first. The palette is extended instead of wrapping
when it runs out. Explicitly assigning the same color to two targets is allowed.
Adding/removing result directories or changing the set of compared setups can
change automatic colors; use the JSON file to keep colors fixed across different
experiments. Distinct RGB values do not guarantee easy visual distinction for
very large numbers of lines.

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


## CPU core reference line

`plot_sweep_v3.py` and `plot_compare.py` accept `--cores 16` to draw a gray
dashed vertical line at X=16 with a `Число ядер: 16` annotation. Omit the option to
leave the graphs unchanged. The value must be a positive integer.

```sh
python3 plot_sweep_v3.py --results-dir sweep_results --cores 16 --stat throughput latency
python3 plot_compare.py --run "A /results/a" --run "B /results/b" --cores 16
```

The reference is included on numeric-X charts (throughput, latency, progress,
scatter and combined plots); the categorical per-thread boxplot is unchanged.
It marks the supplied X value directly: no conversion from coroutines per
carrier to total logical workers is performed.


## Publication export

`plot_sweep_v3.py` and `plot_compare.py` save PNG figures at 300 dpi, including
latency and worker-progress charts. A 9 × 5 inch figure produces 2700 × 1500
pixels (tight bounding-box cropping may change the dimensions). PNG preserves
text and lines without JPEG compression artifacts. Keep the placed image size
no larger than its export size to retain at least 300 effective dpi.


## Russian publication labels

The sweep and comparison plots use Russian axis and legend labels. X is
`Число потоков`; the DS legend is `Структура данных` and the setup legend is
`Вид потоков`. Figure and subplot titles are omitted for use with article captions.
DS display names still come from the style JSON, and setup names from `--run`;
these user-provided names are preserved. Edit `AXIS_LABELS`, `OPERATION_LABELS`
and `LATENCY_LABELS` in `sweep_plot_common.py` to customize metric labels.

## Comparison grid

Use `plot_compare_strip.py` to create rows of two or three comparison panels.
The metric, aggregation, DS selection and style file are shared CLI options.
Result paths, core counts and thread-type labels may differ per panel.

Copy `compare_strip.example.json` and edit its paths:

```json
{
  "thread_types": ["Boost Fibers", "Argobots"],
  "rows": [
    [
      {"cores": 4, "paths": ["results/row1/4cores/boost", "results/row1/4cores/argobots"]},
      {"cores": 8, "paths": ["results/row1/8cores/boost", "results/row1/8cores/argobots"]},
      {"cores": 16, "paths": ["results/row1/16cores/boost", "results/row1/16cores/argobots"]}
    ],
    [
      {"cores": 4, "thread_types": ["Boost shared_work", "Argobots single pool"], "paths": ["results/row2/4cores/boost", "results/row2/4cores/argobots"]},
      {"cores": 8, "thread_types": ["Boost shared_work"], "paths": ["results/row2/8cores/boost"]}
    ]
  ]
}
```

The root `thread_types` supplies default legend labels. A panel can override
them with its own `thread_types`; the root field can be omitted when every panel
defines its own. Each panel's `paths` must match its effective `thread_types`
in length and order. Line styles follow this order within each panel. Paths are relative to the JSON file's
directory (absolute paths also work). Each path supports either single results
or `v1/`, `v2/`, etc. repeats. Every row contains two or three panels. The old
top-level `panels` array with exactly three panels remains supported.

```sh
python3 plot_compare_strip.py \
  --panels-config compare_strip.example.json \
  --ds-styles ds_styles.example.json \
  --plot throughput latency_p99 jain_fairness \
  --latency-operations all --agg median --output-dir strip_plots
```

Each selected metric produces a separate grid, e.g.
`compare_strip_throughput_ops_per_sec.png`. Supported metrics: throughput,
work throughput, latency mean/p95/p99 and the progress/fairness scalars.
The single-plot `combined` and text `summary` options are not part of this mode.

Each panel retains its axes, tick labels and legends, without a title. X uses
base-2 log scaling; Y is linear. X/Y limits and tick positions are identical
across all panels and include all data, uncertainty bands and core markers.
Colors and thread-type line styles are consistent throughout the grid. Missing
metrics remain gaps; a metric missing from all panels is skipped.

Each tile is 2100 × 1650 pixels at 300 dpi; a row with two panels leaves the
last cell empty when another row has three. Russian labels and JSON-configured
DS appearance are shared with `plot_compare.py`.

## Overnight yield-period sweep

`build_and_run_yield_sweep.sh` runs the Argobots, Boost Fibers and OS builds for
each compile-time `BENCH_YIELD_EVERY` value. Run it from any directory:

```sh
./cpp/runner/build_and_run_yield_sweep.sh \
  --periods "0 16 64 256" --results-root sweep_results \
  --repeats 5 --threads 16 --time-ms 20000 \
  --nasl-dir /path/to/nasl-tools --boost-dir /path/to/boost \
  --dry-run

nohup ./cpp/runner/build_and_run_yield_sweep.sh \
  --periods "0 16 64 256" --results-root sweep_results \
  --repeats 5 --threads 16 --time-ms 20000 \
  --nasl-dir /path/to/nasl-tools --boost-dir /path/to/boost \
  > overnight.log 2>&1 &
```

Paths under a relative `--results-root` are relative to `cpp/runner/`. For
K=64, results live in `sweep_results/yield_period=64/abt/output/v1/<ds>/`,
`fibers/output/v1/<ds>/` and `os/output/v1/<ds>/`. Pass the backend's `output`
directory to `plot_compare.py`. Each K gets its own three build directories and
`yield_period=K/run.log`. The OS build also receives the same K.

The wrapper rejects duplicate K values and existing result directories before
starting. It checks that all expected JSON files exist and contain the selected
`bench_yield_every` value after each period, then
continues to the next K even if one period fails. A nonzero final exit code
signals incomplete results. Other supported options include `--ds`,
`--coroutines`, `--allocator` (name without `.so`) and `--projects-root`.
