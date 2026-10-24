#!/usr/bin/env bash
# One-command viva run: preflight, then scripts/demo.sh, then a 4-line summary.
# Usage: scripts/present.sh [--nopause] [--check]
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 1
CHECK=0
for a in "$@"; do
  case "$a" in
    --nopause) export DEMO_NOPAUSE=1 ;;
    --check)   CHECK=1 ;;
    *) echo "usage: $0 [--nopause] [--check]"; exit 2 ;;
  esac
done

# ok "label" cmd...: print PASS/FAIL for one preflight line, stop on FAIL.
ok() {
  local label="$1"; shift
  if "$@" >/dev/null 2>&1; then printf 'PASS  %s\n' "$label"
  else printf 'FAIL  %s\n' "$label"; exit 1; fi
}
have() { command -v "$1" >/dev/null 2>&1; }
ptrace_ok() {
  local v; v="$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo 0)"
  [ "$v" = 0 ] || [ "$(id -u)" = 0 ] && return 0
  sudo sysctl kernel.yama.ptrace_scope=0 >/dev/null
}
chart() { compgen -G "results/wsl2/hangrate_*.png" >/dev/null || compgen -G "results/cloud/hangrate_*.png" >/dev/null; }

for t in gcc make python3 gdb; do ok "$t present" have "$t"; done
ok "make -s && make test" bash -c 'make -s && make test'
ok "ptrace_scope = 0 (gdb can attach)" ptrace_ok
ok "results/{wsl2,cloud}/hangrate_*.png exists" chart
[ "$CHECK" = 1 ] && { echo "Preflight OK."; exit 0; }

LOG="$(mktemp)"
START=$SECONDS
summary() {
  local steps s4 s6
  steps=$(grep -c '^=== Step ' "$LOG" 2>/dev/null); steps=${steps:-0}
  # Slice the log per step so a word in one step cannot satisfy another.
  s4=NO; sed -n '/^=== Step 4:/,/^=== Step 5:/p' "$LOG" | grep -q CONFIRMED && s4=YES
  s6=NO;   sed -n '/^=== Step 6:/,/^=== Step 7:/p' "$LOG" | grep -q 'FILTERED' && s6=YES
  printf '\nSteps reached:        %s of 8 (0-7)\n' "$steps"
  printf 'Step 4 CONFIRMED:     %s\n' "$s4"
  printf 'Step 6 FILTERED:      %s\n' "$s6"
  printf 'Total time:           %ss\n' "$((SECONDS - START))"
  rm -f "$LOG"
}
trap summary EXIT
trap 'exit 130' INT TERM

# demo.sh prints colours only on a terminal; through a pipe it is plain text.
scripts/demo.sh 2>&1 | tee "$LOG"
