#!/usr/bin/env bash
# overhead.sh -- cost of the LD_PRELOAD interceptor.
#
# WHY clean binaries only: they always terminate, so wall time is comparable
# between the baseline and the instrumented run (a deadlocked run would just hit
# the timeout and measure nothing).  Runs are interleaved baseline/pdp so slow
# drift (thermal, noisy neighbours) hits both modes equally.
# Times `<bin> race $ITER` (same workload both modes).
# Env overrides: N, ITER, BIN_DIR, BINS, LIBPDP, OUT_DIR
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="${BIN_DIR:-$ROOT/tests/bin}"
OUT_DIR="${OUT_DIR:-$ROOT/results}"
LIBPDP="${LIBPDP:-$ROOT/build/libpdp.so}"
BINS="${BINS:-philo_ordered gate}"
ITER="${ITER:-1000}"
N="${N:-${RUNS:-50}}"
[ -f "$LIBPDP" ] || { echo "ERROR: $LIBPDP not found (run make)" >&2; exit 1; }
mkdir -p "$OUT_DIR"
LOG="$(mktemp)"; trap 'rm -f "$LOG"' EXIT   # PDP_LOG goes to a throwaway file
DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"; HOST="$(hostname)"
echo "date,host,binary,mode,iter,run,elapsed_ms" > "$OUT_DIR/overhead.csv"
for bin in $BINS; do
  exe="$BIN_DIR/$bin"; [ -x "$exe" ] || { echo "WARNING: $exe missing" >&2; continue; }
  for ((r = 1; r <= N; r++)); do
    for mode in baseline pdp; do
      t0=$(date +%s%N)
      if [ $mode = pdp ]; then
        : > "$LOG"   # fresh log each run so file growth does not accumulate
        PDP_LOG="$LOG" LD_PRELOAD="$LIBPDP" timeout 30 "$exe" race "$ITER" >/dev/null 2>&1
      else
        timeout 30 "$exe" race "$ITER" >/dev/null 2>&1
      fi
      t1=$(date +%s%N)
      echo "$DATE,$HOST,$bin,$mode,$ITER,$r,$(( (t1 - t0) / 1000000 ))" >> "$OUT_DIR/overhead.csv"
    done
  done
done
echo "wrote $OUT_DIR/overhead.csv" >&2
