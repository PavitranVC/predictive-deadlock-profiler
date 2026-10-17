#!/usr/bin/env bash
# Live viva demo for PDP (Predictive Deadlock Profiler).
# Walks through: clean test passes -> same program hangs when slowed ->
# PDP predicts the deadlock from the clean run -> confirm -> fix -> filter -> chart.
# Usage: scripts/demo.sh          (pauses at each step)
#        DEMO_NOPAUSE=1 scripts/demo.sh   (non-interactive)
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
TMP="$(mktemp -d)"
BGPID=""
cleanup() { [ -n "$BGPID" ] && kill -9 "$BGPID" 2>/dev/null; rm -rf "$TMP"; }
trap cleanup EXIT

# step N "title" "what to notice": coloured banner, one-line note, then pause.
step() {
  printf '\n\033[1;36m=== Step %s: %s ===\033[0m\n' "$1" "$2"
  printf '\033[33mNotice: %s\033[0m\n' "$3"
  if [ "${DEMO_NOPAUSE:-0}" != 1 ]; then read -rp "[enter]" _; fi
}

# ---- 0. build -------------------------------------------------------------
step 0 "Build" "everything builds from source with plain make."
make -s || { echo "build failed"; exit 1; }

# ---- 1. clean run ---------------------------------------------------------
step 1 "Testing says it's fine" "the sequential run exits 0, so a normal test suite would pass."
timeout 10 tests/bin/bank seq; echo "exit status: $?"

# ---- 2. slow version hangs ------------------------------------------------
step 2 "...but a slower schedule hangs" "threads are asleep at 0% CPU: blocked on locks, not spinning."
tests/bin/bank slow & BGPID=$!
sleep 1
echo "--- top (per thread) ---"
timeout 5 top -b -n1 -H -p "$BGPID" | head -12
echo "--- gdb backtraces (filtered) ---"
if command -v gdb >/dev/null 2>&1 && timeout 20 gdb -batch -p "$BGPID" -ex 'thread apply all bt' >"$TMP/gdb.txt" 2>&1 \
   && grep -q 'Thread' "$TMP/gdb.txt"; then
  grep -E 'Thread |pthread_mutex_lock|__lll_lock_wait|teller' "$TMP/gdb.txt"
else
  echo "(gdb unavailable or ptrace not permitted; skipping)"
fi
kill -9 "$BGPID" 2>/dev/null; wait "$BGPID" 2>/dev/null; BGPID=""

# ---- 3. predict from the clean run ----------------------------------------
step 3 "PDP predicts from the clean run" "a lock-order cycle A->B / B->A is reported, plus the F(R) fix suggestion."
timeout 30 build/pdp run -o "$TMP/bank.log" tests/bin/bank seq
timeout 30 build/pdp analyze "$TMP/bank.log"

# ---- 4. confirm -----------------------------------------------------------
step 4 "Confirm the prediction" "a concurrent run really deadlocks; compare with the baseline hang rate."
timeout 120 build/pdp confirm --log "$TMP/bank.log" --baseline -n 10 ./tests/bin/bank race 1

# ---- 5. fixed version -----------------------------------------------------
step 5 "Fixed version is clean" "no cycle predicted, and the slow schedule now finishes."
FIXED=tests/bin/bank_fixed
for _ in 1 2 3 4 5; do [ -x "$FIXED" ] && break; sleep 1; done
if [ -x "$FIXED" ]; then
  timeout 30 build/pdp run -o "$TMP/fixed.log" "$FIXED" seq
  timeout 30 build/pdp analyze "$TMP/fixed.log"
  timeout 10 "$FIXED" slow; echo "bank_fixed slow exit status: $? (124 would mean hang)"
else
  echo "(tests/bin/bank_fixed not built yet; skipping)"
fi

# ---- 6. gate lock ---------------------------------------------------------
step 6 "Gate lock is not a false alarm" "the cycle is FILTERED because both orders are held under gate_G."
timeout 30 build/pdp run -o "$TMP/gate.log" tests/bin/gate seq
timeout 30 build/pdp analyze "$TMP/gate.log"

# ---- 7. chart -------------------------------------------------------------
step 7 "Experiment results" "hang rate depends on scheduler, cores and load (95% Wilson CIs)."
shopt -s nullglob
pngs=(results/*.png)
if [ ${#pngs[@]} -eq 0 ]; then
  echo "No charts: run scripts/run_experiment.sh first (then python3 scripts/plot.py)."
else
  ls -1 "${pngs[@]}"
  chart="${pngs[0]}"
  for p in "${pngs[@]}"; do case "$p" in *hangrate*) chart="$p"; break;; esac; done
  if   command -v wslview  >/dev/null 2>&1; then wslview  "$chart" >/dev/null 2>&1 &
  elif command -v xdg-open >/dev/null 2>&1 && [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then xdg-open "$chart" >/dev/null 2>&1 &
  else echo "Open this file: $ROOT/$chart"; fi
fi
printf '\n\033[1;32mDemo complete.\033[0m\n'
exit 0
