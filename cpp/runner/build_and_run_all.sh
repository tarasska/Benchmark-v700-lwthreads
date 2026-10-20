#!/usr/bin/env bash
set -euo pipefail

BENCH_YIELD_EVERY=${BENCH_YIELD_EVERY:-100}
PROJECTS_ROOT=${PROJECTS_ROOT:-/home/tskazhenik/tskazhenik/}
NASL_DIR=${NASL_DIR:-$PROJECTS_ROOT/nasl-tools}
BOOST_DIR=${BOOST_DIR:-$PROJECTS_ROOT/nasl-tools/external/boost/}
REPEATS=${REPEATS:-5}
COROUTINES=${COROUTINES:-"1 2 4 8 16 32 64 128 256 512 1024 2048"}
THREADS=${THREADS:-16}
TIME=${TIME:-20000}
ALLOCATOR=${ALLOCATOR:-libjemalloc}
DS=${DS:-"stack_global_lock treiber_stack treiber_stack_fc_opt treiber_stack_yield_only treiber_stack_spin_only elimination_stack elimination_stack_opt"}
RESULTS_DIR=${RESULTS_DIR:-sweep_results}
BUILD_SUFFIX=${BUILD_SUFFIX:-}

if [[ ! "$BENCH_YIELD_EVERY" =~ ^(0|[1-9][0-9]*)$ ]] ||
   (( ${#BENCH_YIELD_EVERY} > 10 )) ||
   (( BENCH_YIELD_EVERY > 2147483647 )); then
    echo "BENCH_YIELD_EVERY must be an integer from 0 to 2147483647" >&2
    exit 2
fi

ARGOBOTS_BUILD="buildArgobots${BUILD_SUFFIX}"
FIBERS_BUILD="buildFibers${BUILD_SUFFIX}"
OS_BUILD="buildOS${BUILD_SUFFIX}"

cd "$(dirname "$0")"


cmake \
    -DNASL_ROOT="$NASL_DIR" \
    -DUSE_ARGOBOTS=ON \
    -DBENCH_YIELD_EVERY="$BENCH_YIELD_EVERY" \
    -DCMAKE_BUILD_TYPE=Release \
    -S ../ \
    -B "../$ARGOBOTS_BUILD"
cmake --build "../$ARGOBOTS_BUILD"

 cmake \
    -DNASL_ROOT="$NASL_DIR" \
    -DBoost_ROOT="$BOOST_DIR" \
    -DUSE_BOOST_FIBERS=ON \
    -DUSE_FIBERS_NUMA=ON \
    -DBENCH_YIELD_EVERY="$BENCH_YIELD_EVERY" \
    -DCMAKE_BUILD_TYPE=Release \
    -S ../ \
    -B "../$FIBERS_BUILD"
cmake --build "../$FIBERS_BUILD"

cmake \
    -DNASL_ROOT="$NASL_DIR" \
    -DUSE_OS=ON \
    -DBENCH_YIELD_EVERY="$BENCH_YIELD_EVERY" \
    -DCMAKE_BUILD_TYPE=Release \
    -S ../ \
    -B "../$OS_BUILD"
cmake --build "../$OS_BUILD"

./run_coroutine_sweep.sh --repeats "$REPEATS" --build-dir "$ARGOBOTS_BUILD" --results-dir "$RESULTS_DIR/abt" --ds "$DS" --coroutines "$COROUTINES" --threads "$THREADS" --time-ms "$TIME" --allocator "$ALLOCATOR"
./run_coroutine_sweep.sh --repeats "$REPEATS" --build-dir "$FIBERS_BUILD" --results-dir "$RESULTS_DIR/fibers" --ds "$DS" --coroutines "$COROUTINES" --threads "$THREADS" --time-ms "$TIME" --allocator "$ALLOCATOR"
./run_coroutine_sweep.sh --repeats "$REPEATS" --build-dir "$OS_BUILD" --results-dir "$RESULTS_DIR/os" --ds "$DS" --coroutines "$COROUTINES" --threads "$THREADS" --time-ms "$TIME" --allocator "$ALLOCATOR"
