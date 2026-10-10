# Operation latency

Latency measurement is enabled by default for the test phase only. It covers each
map operation (`insert`, `remove`, `get`, `contains`, `range_query`) and queue/stack
operation (`push`, `pop`, plus supported reads). Successful and unsuccessful
operations are both included. The interval surrounds the adapter call, excluding
key generation, statistics updates and the artificial `nopCount` delay after push.
`steady_clock` measures wall time, including yields, lock waits and scheduling
while the adapter call is in progress.

Each logical `ThreadLoop` owns its histograms, so Boost fibers and Argobots workers
can yield or migrate without sharing an OS-thread-local measurement. Histograms
are allocated before the timed phase, merged after all workers join, and their
per-worker storage is then released. There are no shared counters on the hot path.

Tune these constants in `latency.h` and rebuild:

- `LATENCY_ENABLED`: set to `false` for a baseline throughput run without timing.
- `LATENCY_MIN_NS`: 10 ns by default.
- `LATENCY_MAX_NS`: 1,000,000,000 ns (one second) by default.
- `LATENCY_BUCKET_RATIO`: 1.01 by default; smaller values increase precision and memory.

Bucket upper bounds grow geometrically, rounded up to integer nanoseconds.
Percentiles use nearest rank and return the containing bucket's upper bound.
Within the configured range, the overestimate is at most about 1% plus 1 ns with
the defaults. Values below 10 ns share the first bucket (reported as 10 ns).
Values above one second have a separate overflow counter. If a percentile falls
in that overflow bucket, it is `null`, rather than an incorrectly clipped one
second. The mean always includes the original, unclipped durations. An operation
with no samples has `null` mean/p95/p99 and count zero.

The existing result-file option writes a `latency_ns` object alongside the existing
statistics. It contains summaries for each operation and `all`, for example:

```json
{
  "latency_ns": {
    "all": {
      "mean": 245.7,
      "p95": 510,
      "p99": 920,
      "count": 10000000,
      "below_min_count": 0,
      "above_max_count": 0
    }
  }
}
```

All durations are nanoseconds. The file contains no samples or bucket arrays.
The `all` percentiles come from merged buckets, not averaged worker percentiles.
Memory is proportional to the number of logical workers and operation types,
not the number of operations: seven histograms per worker, each using one 64-bit
counter per bound. Measurement of every operation adds clock-reading and bucket
lookup overhead; compare with `LATENCY_ENABLED = false` to quantify its effect
on throughput on the target machine.

Standalone histogram checks (no benchmark dependencies required):

```sh
c++ -std=c++17 -O2 -pthread -I cpp/microbench cpp/microbench/tests/latency_test.cpp -o /tmp/latency-test
/tmp/latency-test
```
