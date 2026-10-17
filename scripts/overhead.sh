#!/usr/bin/env bash
# overhead.sh -- cost of the LD_PRELOAD interceptor.
#
# WHY clean binaries only: they always terminate, so wall time is comparable
# between the baseline and the instrumented run.
#
# METHOD
#  * Timing: scripts/runtime_ns.py brackets the child's lifetime with
#    time.perf_counter_ns() (ns resolution, no extra fork inside the window).
#  * WARMUP (default 3) untimed runs per mode per (binary, iter) are discarded
#    (page cache / dynamic loader warm-up of libpdp.so).
#  * Each pair (baseline, pdp) is run in RANDOM order (seeded, SEED recorded) so
#    drift and "second run is warmer" effects hit both modes equally.
#  * ITER is swept (ITERS, default "1000 10000 100000").  Total time is
#    t = fixed_cost + ops * cost_per_op, so the SLOPE over ITER separates
#    per-lock-operation cost from the fixed process/loader/log-open cost.
#  * Every run's exit status is recorded.  rc != 0 (crash, timeout=124) is
#    logged as outcome=error and plot.py excludes it from timing statistics.
#  * lock_ops = number of " ACQ " lines in the PDP log of that pdp run (one per
#    successful acquisition; REQ lines are the matching requests).  The log is
#    written to a throwaway file (path recorded in env, see LOG_DIR).
#    ns_per_lock_op is computed per pdp row as
#        (t_pdp - t_native_of_same_pair) / lock_ops
#    (the native partner is the baseline run of the same pair; blank for
#    baseline rows or when either run failed).
# Env: N (pairs per ITER, default RUNS or 50), ITERS, WARMUP, SEED, BIN_DIR, BINS,
#      LIBPDP, OUT_DIR, TIMEOUT_S, FORCE=1 to overwrite an existing overhead.csv
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
HERE="$ROOT/scripts"
BIN_DIR="${BIN_DIR:-$ROOT/tests/bin}"
OUT_DIR="${OUT_DIR:-$ROOT/results}"
LIBPDP="${LIBPDP:-$ROOT/build/libpdp.so}"
BINS="${BINS:-philo_ordered gate}"
ITERS="${ITERS:-${ITER:-1000 10000 100000}}"
N="${N:-${RUNS:-50}}"
WARMUP="${WARMUP:-3}"
TIMEOUT_S="${TIMEOUT_S:-30}"
SEED="${SEED:-$RANDOM}"
[ -f "$LIBPDP" ] || { echo "ERROR: $LIBPDP not found (run make)" >&2; exit 1; }
mkdir -p "$OUT_DIR"
CSV="$OUT_DIR/overhead.csv"
if [ -e "$CSV" ] && [ "${FORCE:-0}" != 1 ]; then
  echo "ERROR: $CSV exists; refusing to overwrite (use FORCE=1 or another OUT_DIR)" >&2; exit 1
fi
LOG="$(mktemp)"; trap 'rm -f "$LOG"' EXIT   # PDP_LOG goes to a throwaway file
RANDOM=$SEED                                  # seeds bash's RANDOM -> reproducible pair order
DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"; HOST="$(hostname)"
echo "# date=$DATE seed=$SEED n=$N warmup=$WARMUP iters='$ITERS' log=$LOG" >&2
echo "date,host,seed,binary,mode,iter,pair,order,rc,outcome,elapsed_ns,lock_ops,ns_per_lock_op" > "$CSV"

run_one() {   # $1=mode $2=exe $3=iter -> sets RC, NS
  local out
  if [ "$1" = pdp ]; then
    : > "$LOG"
    out=$(PDP_LOG="$LOG" LD_PRELOAD="$LIBPDP" python3 "$HERE/runtime_ns.py" "$TIMEOUT_S" "$2" race "$3")
  else
    out=$(python3 "$HERE/runtime_ns.py" "$TIMEOUT_S" "$2" race "$3")
  fi
  RC="${out% *}"; NS="${out#* }"
}

for bin in $BINS; do
  exe="$BIN_DIR/$bin"; [ -x "$exe" ] || { echo "WARNING: $exe missing" >&2; continue; }
  for iter in $ITERS; do
    for ((w = 0; w < WARMUP; w++)); do run_one baseline "$exe" "$iter"; run_one pdp "$exe" "$iter"; done
    for ((r = 1; r <= N; r++)); do
      if (( RANDOM % 2 )); then modes=(baseline pdp); else modes=(pdp baseline); fi
      declare -A rcs=() nss=() ops=()
      for mode in "${modes[@]}"; do
        run_one "$mode" "$exe" "$iter"
        rcs[$mode]=$RC; nss[$mode]=$NS
        [ "$mode" = pdp ] && ops[pdp]=$(grep -c ' ACQ ' "$LOG")
      done
      for mode in baseline pdp; do
        outcome=ok; [ "${rcs[$mode]}" != 0 ] && outcome=error
        lo=""; nplo=""
        if [ $mode = pdp ]; then
          lo="${ops[pdp]}"
          if [ "${rcs[baseline]}" = 0 ] && [ "${rcs[pdp]}" = 0 ] && [ "$lo" -gt 0 ]; then
            nplo=$(( (nss[pdp] - nss[baseline]) / lo ))
          fi
        fi
        echo "$DATE,$HOST,$SEED,$bin,$mode,$iter,$r,${modes[0]}-first,${rcs[$mode]},$outcome,${nss[$mode]},$lo,$nplo" >> "$CSV"
      done
    done
  done
done
echo "wrote $CSV" >&2
