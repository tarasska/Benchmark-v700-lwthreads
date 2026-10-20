#!/usr/bin/env bash
# Build and run the three backends for several compile-time yield intervals.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./build_and_run_yield_sweep.sh --periods "0 16 64 256" [options]
  --results-root DIR      Default: sweep_results (relative to runner/)
  --repeats N             Default: 5
  --coroutines "LIST"     Default: 1 2 4 ... 2048
  --threads N             Default: 16
  --time-ms N             Default: 20000
  --ds "LIST"             Data structures passed to build_and_run_all.sh
  --allocator NAME        Default: libjemalloc (without .so)
  --projects-root DIR     Root containing nasl-tools
  --nasl-dir DIR          Override nasl-tools path
  --boost-dir DIR         Override Boost path
  --dry-run               Print planned builds and result paths
EOF
}

periods_input=""
results_root="sweep_results"
dry_run=0
while (( $# )); do
    case "$1" in
        --periods|--results-root|--repeats|--coroutines|--threads|--time-ms|--ds|--allocator|--projects-root|--nasl-dir|--boost-dir)
            if (( $# < 2 )); then echo "Missing value for $1" >&2; exit 2; fi
            option=$1
            value=$2
            case "$option" in
                --periods) periods_input=$value ;;
                --results-root) results_root=$value ;;
                --repeats) REPEATS=$value ;;
                --coroutines) COROUTINES=$value ;;
                --threads) THREADS=$value ;;
                --time-ms) TIME=$value ;;
                --ds) DS=$value ;;
                --allocator) ALLOCATOR=$value ;;
                --projects-root) PROJECTS_ROOT=$value ;;
                --nasl-dir) NASL_DIR=$value ;;
                --boost-dir) BOOST_DIR=$value ;;
            esac
            shift 2 ;;
        --dry-run) dry_run=1; shift ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$periods_input" ]]; then
    echo "--periods is required" >&2
    usage >&2
    exit 2
fi
periods_input=${periods_input//,/ }
read -r -a periods <<< "$periods_input"
if (( ${#periods[@]} == 0 )); then echo "No periods supplied" >&2; exit 2; fi

# CMake accepts the same range; validate before starting any overnight work.
seen=""
for period in "${periods[@]}"; do
    if [[ ! "$period" =~ ^(0|[1-9][0-9]*)$ ]] ||
       (( ${#period} > 10 )) ||
       (( period > 2147483647 )); then
        echo "Invalid BENCH_YIELD_EVERY: $period" >&2
        exit 2
    fi
    if [[ " $seen " == *" $period "* ]]; then
        echo "Duplicate BENCH_YIELD_EVERY: $period" >&2
        exit 2
    fi
    seen="$seen $period"
done
for number in "${REPEATS:-5}" "${THREADS:-16}" "${TIME:-20000}"; do
    if [[ ! "$number" =~ ^[1-9][0-9]*$ ]]; then
        echo "Repeats, threads and time-ms must be positive integers" >&2
        exit 2
    fi
done

REPEATS=${REPEATS:-5}
COROUTINES=${COROUTINES:-"1 2 4 8 16 32 64 128 256 512 1024 2048"}
THREADS=${THREADS:-16}
TIME=${TIME:-20000}
DS=${DS:-"stack_global_lock treiber_stack treiber_stack_fc_opt treiber_stack_yield_only treiber_stack_spin_only elimination_stack elimination_stack_opt"}
ALLOCATOR=${ALLOCATOR:-libjemalloc}
export REPEATS COROUTINES THREADS TIME DS ALLOCATOR
export PROJECTS_ROOT NASL_DIR BOOST_DIR

cd "$(dirname "$0")"
for period in "${periods[@]}"; do
    directory="$results_root/yield_period=$period"
    # Never mix results from a previous run with the current configuration.
    if (( ! dry_run )) && [[ -e "$directory" ]]; then
        echo "Result directory already exists: $directory" >&2
        exit 2
    fi
    printf 'K=%s: builds buildArgobotsYield%s, buildFibersYield%s, buildOSYield%s; results %s/{abt,fibers,os}\n' \
        "$period" "$period" "$period" "$period" "$directory"
done
if (( dry_run )); then exit 0; fi

failures=0
check_results() {
    python3 - "$1" "$2" "$REPEATS" "$DS" "$COROUTINES" <<'PY'
import json
import sys
from pathlib import Path

root, period, repeats, structures, counts = sys.argv[1:]
root = Path(root)
period, repeats = int(period), int(repeats)
missing_or_invalid = []
for backend in ('abt', 'fibers', 'os'):
    for repeat in range(1, repeats + 1):
        for structure in structures.split():
            for count in counts.split():
                result = root / backend / 'output' / f'v{repeat}' / structure / f'result_cops{count}.json'
                try:
                    data = json.loads(result.read_text())
                    if data.get('bench_yield_every') != period:
                        raise ValueError(f"bench_yield_every={data.get('bench_yield_every')!r}")
                except (OSError, ValueError) as error:
                    missing_or_invalid.append(f'{result}: {error}')
for problem in missing_or_invalid:
    print(problem, file=sys.stderr)
if missing_or_invalid:
    sys.exit(1)
print(f'Validated all result JSON files for K={period}')
PY
}
for period in "${periods[@]}"; do
    directory="$results_root/yield_period=$period"
    mkdir -p "$directory"
    echo "Running K=$period; log: $directory/run.log"
    if BENCH_YIELD_EVERY="$period" RESULTS_DIR="$directory" BUILD_SUFFIX="Yield$period" \
       ./build_and_run_all.sh 2>&1 | tee "$directory/run.log" &&
       check_results "$directory" 2>&1 | tee -a "$directory/run.log"; then
        echo "K=$period completed" | tee -a "$directory/run.log"
    else
        echo "K=$period failed; continuing with the next period" | tee -a "$directory/run.log" >&2
        failures=$((failures + 1))
    fi
done
if (( failures )); then
    echo "$failures period(s) failed; inspect their run.log files" >&2
    exit 1
fi
echo "All periods completed under $results_root"
