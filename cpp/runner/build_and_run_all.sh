YILED_PERIOD=100
PROJECTS_ROOT="/home/tskazhenik/tskazhenik/"
NASL_DIR="$PROJECTS_ROOT/nasl-tools"
BOOST_DIR="$PROJECTS_ROOT/nasl-tools/external/boost/"
REPEATS=5
COROUTINES="1 2 4 8 16 32 64 128 256 512 1024 2048"
THREADS=16
TIME=20000
ALLOCATOR="libjemalloc.so"
DS="stack_global_lock treiber_stack treiber_stack_fc_opt treiber_stack_yield_only treiber_stack_spin_only elimination_stack elimination_stack_opt"


cmake \
    -DNASL_ROOT="$NASL_DIR" \
    -DUSE_ARGOBOTS=ON \
    -DBENCH_YIELD_EVERY=$YILED_PERIOD \
    -DCMAKE_BUILD_TYPE=Release \
    -S ../ \
    -B ../buildArgobots \
    && cmake --build ../buildArgobots

 cmake \
    -DNASL_ROOT="$NASL_DIR" \
    -DBoost_ROOT="$BOOST_DIR" \
    -DUSE_BOOST_FIBERS=ON \
    -DUSE_FIBERS_NUMA=ON \
    -DBENCH_YIELD_EVERY=$YILED_PERIOD \
    -DCMAKE_BUILD_TYPE=Release \
    -S ../ \
    -B ../buildFibers \
    && cmake --build ../buildFibers

cmake \
    -DNASL_ROOT="$NASL_DIR" \
    -DUSE_OS=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -S ../ \
    -B ../buildOS \
    && cmake --build ../buildOS

./run_coroutine_sweep.sh --repeats $REPEATS --build-dir buildArgobots --results-dir "sweep_results/abt" --ds "$DS" --coroutines "$COROUTINES" --threads $THREADS  --time-ms $TIME --allocator $ALLOCATOR
./run_coroutine_sweep.sh --repeats $REPEATS --build-dir buildFibers   --results-dir "sweep_results/fibers" --ds "$DS" --coroutines "$COROUTINES" --threads $THREADS  --time-ms $TIME --allocator $ALLOCATOR
./run_coroutine_sweep.sh --repeats $REPEATS --build-dir buildOS       --results-dir "sweep_results/os" --ds "$DS" --coroutines "$COROUTINES" --threads $THREADS  --time-ms $TIME --allocator $ALLOCATOR
