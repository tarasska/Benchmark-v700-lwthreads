# External yield interval

`BENCH_YIELD_EVERY` is a CMake cache setting, compiled into every benchmark target:

```sh
# From the repository root; keep the backend/dependency options of your build.
cmake -S cpp -B build-y64 -DUSE_ARGOBOTS=ON -DBENCH_YIELD_EVERY=64
cmake --build build-y64 --target treiber_stack.debra
```

Use separate build directories for different intervals, or reconfigure and rebuild
an existing directory. The default is `0` (external yield disabled). Positive K
means one external yield after every K completed adapter operations per logical
worker. Accepted range: 0–2147483647. Unlike a hardcoded compile definition, a cache
setting can be changed from the command line without editing CMakeLists.txt.
For the legacy Makefile, append `-DBENCH_YIELD_EVERY=64` to its `xargs` flags; an
enabled build must link its backend's NASL yield implementation.

The hook applies to map and queue/stack operations, including unsuccessful
operations, in prefill, warmup and test phases. Each phase creates new workers
with fresh counters. We count adapter operations rather than `step()` calls:
one prefill step can perform multiple insert attempts. The counter belongs to
the logical worker, so fiber migration does not mix workers' progress.

The existing `nasl::core::yield()` backend is used:

- Boost Fibers: `boost::this_fiber::yield()`.
- Argobots: `ABT_thread_yield()`.
- OS threads: `std::this_thread::yield()`.

Yield occurs after the adapter call, latency recording and operation accounting
(and after queue push's existing artificial nop delay). It is outside the latency
interval, but its overhead and scheduling delays affect total throughput. K=0
compiles out the hook's counting and yield call. External yielding is independent
of `LATENCY_ENABLED` and does not change a structure's internal backoff/yields.
Yield is an opportunity for scheduling, not a guarantee that every ready worker
runs before the caller resumes. It also cannot help an operation stuck inside an
adapter that never yields or returns.

Result JSON includes `bench_yield_every` even when latency measurement is disabled.
Do not aggregate different K values as repeats of the same experiment. Put them
in separate result roots and compare them using named setups, for example:

```sh
python3 cpp/runner/plot_compare.py \
  --run "K0 /results/k0" --run "K64 /results/k64" \
  --plot throughput latency_p99 zero_progress_percent jain_fairness
```

Standalone counter test (repeat with K=0, 1, 64, 256):

```sh
c++ -std=c++17 -DBENCH_YIELD_EVERY=64 -I cpp/microbench \
  cpp/microbench/tests/periodic_yield_test.cpp -o /tmp/periodic-yield-test
/tmp/periodic-yield-test
```
