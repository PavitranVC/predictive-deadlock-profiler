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
trap 'exit 130' INT TERM   # Ctrl-C ends the demo (timeout runs children in another process group)
# Colours only on a terminal (avoid ^[[1;36m in pipes/recordings).
if [ -t 1 ]; then C1=$'\033[1;36m'; C2=$'\033[33m'; C3=$'\033[1;32m'; C0=$'\033[0m'; else C1=; C2=; C3=; C0=; fi

# step N "title" "what to notice": coloured banner, one-line note, then pause.
step() {
  printf '\n%s=== Step %s: %s ===%s\n' "$C1" "$1" "$2" "$C0"
  printf '%sNotice: %s%s\n' "$C2" "$3" "$C0"
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
if ! kill -0 "$BGPID" 2>/dev/null; then
  echo "(bank slow did not hang this time; re-run the demo)"
elif ! command -v gdb >/dev/null 2>&1; then
  echo "(gdb not installed: sudo apt install gdb)"
else
  # Yama ptrace_scope>0 forbids attaching to a non-child process unless root.
  GDB="gdb"
  PTS="$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0)"
  if [ "$PTS" != 0 ] && [ "$(id -u)" != 0 ]; then
    if command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then GDB="sudo -n gdb"
    else GDB=""; echo "(ptrace_scope=$PTS blocks gdb attach. Fix: sudo sysctl kernel.yama.ptrace_scope=0   -- reverts on reboot)"; fi
  fi
  if [ -n "$GDB" ]; then
    if timeout 20 $GDB -batch -p "$BGPID" -ex 'thread apply all bt' >"$TMP/gdb.txt" 2>&1 \
       && grep -q 'Thread' "$TMP/gdb.txt"; then
      grep -E 'Thread |pthread_mutex_lock|__lll_lock_wait|teller' "$TMP/gdb.txt"
    else
      echo "(gdb attach failed; skipping)"
    fi
  fi
fi
kill -9 "$BGPID" 2>/dev/null; wait "$BGPID" 2>/dev/null; BGPID=""

# ---- 3. predict from the clean run ----------------------------------------
step 3 "PDP predicts from the clean run" "a lock-order cycle A->B / B->A is reported, plus the F(R) fix suggestion."
timeout 30 build/pdp run -o "$TMP/bank.log" tests/bin/bank seq
timeout 30 build/pdp analyze "$TMP/bank.log"

# ---- 4. confirm -----------------------------------------------------------
step 4 "Confirm the prediction" "a concurrent run really deadlocks; compare with the baseline hang rate."
timeout --foreground 120 build/pdp confirm --log "$TMP/bank.log" --baseline -n 10 ./tests/bin/bank race 1

# ---- 5. fixed version -----------------------------------------------------
step 5 "Fixed version is clean" "no cycle predicted, and the slow schedule now finishes."
FIXED=tests/bin/bank_fixed
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
# Prefer the WSL2 run, fall back to the cloud run; empty array => message below.
pngs=(results/wsl2/*.png)
[ ${#pngs[@]} -eq 0 ] && pngs=(results/cloud/*.png)
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
printf '\n%sDemo complete.%s\n' "$C3" "$C0"
exit 0
