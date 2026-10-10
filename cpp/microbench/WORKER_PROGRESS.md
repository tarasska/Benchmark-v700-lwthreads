# Worker progress and fairness

The result JSON already contains `sum_num_operations_by_thread`, indexed by logical
worker ID. No new hot-path counters or yields are needed. The final JSON now also
contains these scalar summaries, computed from that array and the test phase's
configured logical-worker count:

| Field | Meaning |
| --- | --- |
| `worker_count` | All configured logical workers, including those that never ran |
| `zero_progress_workers` | Workers with zero completed adapter operations |
| `zero_progress_percent` | `100 * zero_progress_workers / worker_count` |
| `jain_fairness` | `(sum(n_i))^2 / (worker_count * sum(n_i^2))` |

GSTATS omits trailing zeros in its array. These summaries restore them implicitly
using `worker_count`. The existing array is preserved without adding a duplicate.
Never use its serialized length as the worker count when processing old results;
obtain the count from the experiment configuration instead.

## Line plots

Use the number of logical workers on X, `zero_progress_percent` on Y (0–100%),
and one line per implementation. Lower is better. For four occupied execution
streams, if only their first four workers make progress, the expected points are:

| Logical workers | Zero-progress percent |
| --- | --- |
| 4 | 0 |
| 8 | 50 |
| 16 | 75 |
| 32 | 87.5 |

These are illustrative values, not benchmark measurements. An implementation
where every worker completes an operation has a value of zero at every point.

Also plot `jain_fairness` (0–1, higher is better) to catch workers that complete
only a handful of operations. Equal work per worker gives 1. Four equally busy
workers out of sixteen give 0.25, even if their raw throughput is high. For zero
total operations, Jain is undefined and serialized as `null`; zero-progress
percentage is 100 when workers were configured. With no configured workers,
both metrics are `null`.

Compute these summaries per run, then aggregate repeated runs for each X value.
Do not pool counters across runs: different workers might starve in different
runs, hiding the imbalance. Keep the number of OS execution streams fixed when
comparing different logical-worker counts in a fiber experiment.

These metrics describe completed-operation distribution over the observation
window, not a proof of indefinite starvation or a measure of time spent waiting.
Completed unsuccessful operations (e.g. an empty pop) count as progress too.
Compare workers with equivalent workloads; deliberately different operation mixes
or costs can produce unequal counts without scheduler starvation. A yield after
every adapter call would change the experiment, so none was added.

Standalone tests:

```sh
c++ -std=c++17 -O2 -I cpp/microbench cpp/microbench/tests/worker_progress_test.cpp -o /tmp/worker-progress-test
/tmp/worker-progress-test
```
