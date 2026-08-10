#!/usr/bin/env bash
# run_java_sweep.sh
#
# Sweeps thread count for one or more Java DS implementations.
# Uses the same output layout as run_coroutine_sweep.sh:
#   RESULTS_DIR/v<N>/  <ds_short_name>/result_t<T>.json
#
# Usage (from java/ directory):
#   ./run_java_sweep.sh \
#     --ds "queues.lockfree.LockFreeQueueIntSet skiplists.lockfree.NonBlockingFriendlySkipListMap" \
#     --threads "1 2 4 8 16 32" \
#     --time-ms 10000 \
#     --repeats 3 \
#     --results-dir sweep_results

set -euo pipefail

# Kill the entire process group (script + any child gradlew/JVM) on Ctrl+C or SIGTERM.
# Without this, SIGINT only reaches the script and gradlew's JVM keeps running.
_pgid=$(ps -o pgid= $$ | tr -d ' ')
trap 'echo ""; echo "Interrupted — killing process group $_pgid..."; kill -- -"$_pgid" 2>/dev/null; exit 1' INT TERM


# ── defaults ──────────────────────────────────────────────────────────────────
DS_LIST=""
THREADS="1 2 4 8 16"
CORES=$(nproc)
TIME_MS=10000
RANGE=2048
PREFILL_OPS=1024
INSERT_RATIO=0.5
REMOVE_RATIO=0.5
REPEATS=1
CONFIG_DIR="sweep_results/config"
RESULTS_DIR="sweep_results/output"
JAVA_DIR="../"
DATA_MAP_FILE=""
VIRTUAL_THREADS=false

# ── arg parsing ───────────────────────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --ds)            DS_LIST="$2";       shift 2 ;;
    --threads)       THREADS="$2";       shift 2 ;;
    --cores)         CORES="$2";         shift 2 ;;
    --time-ms)       TIME_MS="$2";       shift 2 ;;
    --range)         RANGE="$2";         shift 2 ;;
    --prefill-ops)   PREFILL_OPS="$2";   shift 2 ;;
    --insert-ratio)  INSERT_RATIO="$2";  shift 2 ;;
    --remove-ratio)  REMOVE_RATIO="$2";  shift 2 ;;
    --repeats)       REPEATS="$2";       shift 2 ;;
    --results-dir)   RESULTS_DIR="$2";   shift 2 ;;
    --java-dir)      JAVA_DIR="$2";      shift 2 ;;
    --data-map-file) DATA_MAP_FILE="$2"; shift 2 ;;
    --virtual-threads) VIRTUAL_THREADS=true; shift ;;
    *) echo "Unknown arg: $1" >&2; exit 1 ;;
  esac
done

GRADLEW="$JAVA_DIR/gradlew"
if [[ ! -x "$GRADLEW" ]]; then
  echo "gradlew not found at $GRADLEW — pass --java-dir pointing at the java/ directory"
  exit 1
fi

# ── short name helper: queues.lockfree.LockFreeQueueIntSet → LockFreeQueueIntSet ──
ds_short() { echo "${1##*.}"; }

# ── config generator ──────────────────────────────────────────────────────────
make_config() {
  local T=$1
  cat <<EOF
{
  "iterations": 1,
  "range": $RANGE,
  "detailedStats": true,
  "afterPrefillDuration": 500,
  "afterWarmUpDuration": 0,
  "betweenIterationsDuration": 0,
  "prefill": {
    "numThreads": 1,
    "maxAwaitTime": 0,
    "stopCondition": {
      "commonOperationLimit": $PREFILL_OPS,
      "ClassName": "OperationCounter"
    },
    "threadLoopBuilders": [
      {
        "quantity": 1,
        "threadLoopBuilder": {
          "argsGeneratorBuilder": {
            "distributionBuilder": { "ClassName": "UniformDistributionBuilder" },
            "dataMapBuilder": { "id": 4, "ClassName": "IdDataMapBuilder" },
            "ClassName": "DefaultArgsGeneratorBuilder"
          },
          "numberOfAttempts": 1000000,
          "ClassName": "PrefillInsertThreadLoopBuilder"
        }
      }
    ]
  },
  "warmUp": {
    "numThreads": 0,
    "maxAwaitTime": 0,
    "stopCondition": { "workTime": 0, "ClassName": "Timer" },
    "threadLoopBuilders": []
  },
  "test": {
    "numThreads": $T,
    "maxAwaitTime": 0,
    "stopCondition": { "workTime": $TIME_MS, "ClassName": "Timer" },
    "threadLoopBuilders": [
      {
        "quantity": $T,
        "threadLoopBuilder": {
          "parameters": {
            "insertRatio": $INSERT_RATIO,
            "removeRatio": $REMOVE_RATIO,
            "writeAllsRatio": 0.0,
            "snapshotsRatio": 0.0
          },
          "argsGeneratorBuilder": {
            "distributionBuilder": { "ClassName": "UniformDistributionBuilder" },
            "dataMapBuilder": { "id": 1, "ClassName": "ArrayDataMapBuilder" },
            "ClassName": "DefaultArgsGeneratorBuilder"
          },
          "ClassName": "DefaultThreadLoopBuilder"
        }
      }
    ]
  }
}
EOF
}

echo "Java sweep: [$(echo $DS_LIST | tr ' ' ', ')] × threads=[$(echo $THREADS | tr ' ' ', ')] × repeats=$REPEATS"
echo ""

# ── generate shared configs ────────────────────────────────────────────────────
mkdir -p "$CONFIG_DIR"
mkdir -p "$RESULTS_DIR"
for T in $THREADS; do
  make_config "$T" > "$CONFIG_DIR/config_cops${T}.json"
done

# ── sweep: repeats × DS × threads ─────────────────────────────────────────────
for REP in $(seq 1 "$REPEATS"); do
  echo "── repeat v${REP} ──────────────────────────────────────────"
  for DS in $DS_LIST; do
    SHORT=$(ds_short "$DS")
    DS_OUT="$RESULTS_DIR/v${REP}/${SHORT}"
    mkdir -p "$DS_OUT"
    echo "  $SHORT"

    for T in $THREADS; do
      CONFIG="$CONFIG_DIR/config_cops${T}.json"
      RESULT="$DS_OUT/result_cops${T}.json"
      echo -n "    threads=$T ... "

      set +e
      EXTRA_ARGS=""
      $VIRTUAL_THREADS && EXTRA_ARGS="-virtual-threads"

      "$GRADLEW" -p "$JAVA_DIR" run -q -PmaxCores=$CORES \
        --args="-ds $DS -json-file $(realpath $CONFIG) -result-file $(realpath $RESULT) $EXTRA_ARGS" \
        2>/dev/null
      EXIT=$?
      set -e

      if [[ $EXIT -ne 0 || ! -f "$RESULT" ]]; then
        echo "FAILED (exit $EXIT)"
        continue
      fi

      python3 - <<PYEOF
import json
with open("$RESULT") as f:
    d = json.load(f)
r = d[0]
tput = r['throughput']
total = r['commonStatistic']['total']
t = r['elapsedTime']
print(f"throughput={tput:>12.0f} ops/s  total={total}  time={t:.3f}s")
PYEOF
    done
  done
  echo ""
done

echo "Results in: $RESULTS_DIR/"
echo "Plot with:"
echo "  python3 plot_java_sweep.py --results-dir $RESULTS_DIR"