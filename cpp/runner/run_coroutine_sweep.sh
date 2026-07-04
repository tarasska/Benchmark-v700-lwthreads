#!/usr/bin/env bash
# run_coroutine_sweep.sh
#
# Runs one or more DS targets across a range of coroutine counts.
# Results are stored as:  RESULTS_DIR/<ds_name>/result_cops<N>.json
# Configs are shared and stored as: RESULTS_DIR/config_cops<N>.json
#
# Usage:
#   ./run_coroutine_sweep.sh \
#     --ds "treiber_stack_fc treiber_stack_fast treiber_stack" \
#     --coroutines "1 2 4 8 16 32 64" \
#     --threads 4 --time-ms 10000
#
# All targets must support the StackThreadLoopBuilder (USE_STACK_OPERATIONS).

set -euo pipefail

# ── defaults ──────────────────────────────────────────────────────────────────
BUILD_DIR="build"
DS_LIST="treiber_stack_fc_sleep treiber_stack_fc treiber_stack_fast treiber_stack stack_nasl_mcs"
RECLAIM="debra"
COROUTINES="1 2 4 8 16 32 64"
THREADS=4
REPEATS=5
TIME_MS=10000
RANGE=2048
PREFILL_OPS=1024
PUSH_RATIO=0.5
POP_RATIO=0.5
ALLOCATOR="libmimalloc"
RESULTS_COMMON_DIR="sweep_results"
LIB_DIR="../lib"

# ── arg parsing ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)   BUILD_DIR="$2";   shift 2 ;;
    --repeats)     REPEATS="$2";     shift 2 ;;
    --ds)          DS_LIST="$2";     shift 2 ;;
    --reclaim)     RECLAIM="$2";     shift 2 ;;
    --coroutines)  COROUTINES="$2";  shift 2 ;;
    --threads)     THREADS="$2";     shift 2 ;;
    --time-ms)     TIME_MS="$2";     shift 2 ;;
    --range)       RANGE="$2";       shift 2 ;;
    --push-ratio)  PUSH_RATIO="$2";  shift 2 ;;
    --pop-ratio)   POP_RATIO="$2";   shift 2 ;;
    --allocator)   ALLOCATOR="$2";   shift 2 ;;
    --results-dir) RESULTS_COMMON_DIR="$2"; shift 2 ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

CONFIG_DIR="$RESULTS_COMMON_DIR/config"
OUTPUT_DIR="$RESULTS_COMMON_DIR/output"
BIN_DIR="../build"

# ── validate that all binaries exist before starting ─────────────────────────
echo "Checking binaries..."
for DS in $DS_LIST; do
  BINARY="$BIN_DIR/${DS}.${RECLAIM}"
  if [[ ! -x "$BINARY" ]]; then
    echo "  MISSING: $BINARY"
    echo "  Build with: cmake --build $BUILD_DIR --target ${DS}.${RECLAIM}"
    exit 1
  fi
  echo "  OK: $BINARY"
done
echo ""

mkdir -p "$RESULTS_COMMON_DIR"
mkdir -p "$CONFIG_DIR"
mkdir -p "$OUTPUT_DIR"

# ── generate shared config JSONs (one per coroutine count) ────────────────────
make_config() {
  local COPS=$1
  cat <<EOF
{
    "prefill": {
        "numThreads": $THREADS,
        "coroutines": $COPS,
        "stopCondition": {
            "ClassName": "OperationCounter",
            "commonOperationLimit": $PREFILL_OPS
        },
        "threadLoopBuilders": [
            {
                "quantity": $THREADS,
                "coroutines": $COPS,
                "threadLoopBuilder": {
                    "ClassName": "PrefillInsertThreadLoopBuilder",
                    "argsGeneratorBuilder": {
                        "ClassName": "DefaultArgsGeneratorBuilder",
                        "dataMapBuilder": {
                            "ClassName": "IdDataMapBuilder",
                            "id": 4
                        },
                        "distributionBuilder": {
                            "ClassName": "UniformDistributionBuilder"
                        }
                    },
                    "numberOfAttempts": 1
                }
            }
        ]
    },
    "range": $RANGE,
    "test": {
        "numThreads": $THREADS,
        "coroutines": $COPS,
        "stopCondition": {
            "ClassName": "Timer",
            "workTime": $TIME_MS
        },
        "threadLoopBuilders": [
            {
                "quantity": $THREADS,
                "coroutines": $COPS,
                "threadLoopBuilder": {
                    "ClassName": "StackThreadLoopBuilder",
                    "argsGeneratorBuilder": {
                        "ClassName": "DefaultArgsGeneratorBuilder",
                        "dataMapBuilder": {
                            "ClassName": "ArrayDataMapBuilder",
                            "id": 1
                        },
                        "distributionBuilder": {
                            "ClassName": "UniformDistributionBuilder"
                        }
                    },
                    "parameters": {
                        "pushRatio": $PUSH_RATIO,
                        "popRatio": $POP_RATIO,
                        "insertRatio": 0.1,
                        "removeRatio": 0.1,
                        "rqRatio": 0.0
                    }
                }
            }
        ]
    },
    "warmUp": {
        "numThreads": 0,
        "coroutines": 0,
        "stopCondition": {
            "ClassName": "Timer",
            "workTime": 5000
        }
    }
}
EOF
}

echo "Generating shared configs in $RESULTS_COMMON_DIR/..."
for COPS in $COROUTINES; do
  make_config "$COPS" > "$CONFIG_DIR/config_cops${COPS}.json"
done

# ── sweep: targets × coroutine counts ────────────────────────────────────────
echo "Starting sweep: [$(echo $DS_LIST | tr ' ' ', ')] × coroutines=[$(echo $COROUTINES | tr ' ' ', ')]"
echo "Threads: $THREADS  |  Time: ${TIME_MS}ms  |  push=$PUSH_RATIO pop=$POP_RATIO"
echo ""
for ITER in $(seq 1 $REPEATS); do
    for DS in $DS_LIST; do
        DS_OUT="$OUTPUT_DIR/v$ITER/$DS"
        mkdir -p "$DS_OUT"
        echo "── $DS ──────────────────────────────────────"
        BINARY="$BIN_DIR/${DS}.${RECLAIM}"

        for COPS in $COROUTINES; do
            CONFIG_FILE="$CONFIG_DIR/config_cops${COPS}.json"
            RESULT_FILE="$DS_OUT/result_cops${COPS}.json"

            echo -n "  coroutines=$COPS ... "

            set +e
            LD_PRELOAD="${LIB_DIR}/${ALLOCATOR}.so" \
                "$BINARY" \
                -json-file "$CONFIG_FILE" \
                -result-file "$RESULT_FILE" \
                2>/dev/null
            EXIT_CODE=$?
            set -e

            if [[ $EXIT_CODE -ne 0 ]]; then
                echo "FAILED (exit $EXIT_CODE)"
                continue
            fi

            if [[ ! -f "$RESULT_FILE" ]]; then
                echo "FAILED (no result file)"
                continue
            fi

            python3 - <<PYEOF
import json
with open("$RESULT_FILE") as f:
    d = json.load(f)
total = d.get("sum_num_operations_total", 0)
ms    = d.get("max_time_thread_terminate_total", 1) / 1e6
tput  = total / ms if ms > 0 else 0
pushes = d.get("sum_num_pushes_total", 0)
pops   = d.get("sum_num_pops_total", 0)
print(f"throughput={tput:>10.0f} ops/s   pushes={pushes}  pops={pops}")
PYEOF
        done
    echo ""
    done
done
echo "All results saved to: $OUTPUT_DIR/"
echo ""
echo "Plot with:"
echo "  python3 plot_sweep.py --results-dir $OUTPUT_DIR"