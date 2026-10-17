#!/usr/bin/env bash
# helgrind.sh -- baseline comparison with an established dynamic detector.
#
# Helgrind reports "lock order ... violated" at the moment a thread acquires
# locks in an order inconsistent with earlier acquisitions -- the same
# predictive idea as PDP, so it is the natural tool to compare against.
# `timeout` is required: helgrind serialises threads and a real deadlock can
# still hang the run after the report was already printed.
# Env overrides: BIN_DIR, BINS, OUT_DIR, TIMEOUT_S
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="${BIN_DIR:-$ROOT/tests/bin}"
OUT_DIR="${OUT_DIR:-$ROOT/results}"
BINS="${BINS:-bank sem_sq philo_naive gate philo_ordered}"
TIMEOUT_S="${TIMEOUT_S:-60}"
# seq = sequential run, never hangs: helgrind predicts from lock order alone.
MODE="${MODE:-seq}"
command -v valgrind >/dev/null || { echo "ERROR: valgrind not installed" >&2; exit 1; }
mkdir -p "$OUT_DIR/helgrind"
echo "binary,exit_code,timed_out,lock_order_reported,lock_order_count" > "$OUT_DIR/helgrind.csv"
for bin in $BINS; do
  exe="$BIN_DIR/$bin"; [ -x "$exe" ] || { echo "WARNING: $exe missing" >&2; continue; }
  log="$OUT_DIR/helgrind/$bin.log"
  timeout -k 2 "$TIMEOUT_S" valgrind --tool=helgrind "$exe" "$MODE" >/dev/null 2>"$log"
  rc=$?
  to=no; { [ $rc -eq 124 ] || [ $rc -eq 137 ]; } && to=yes
  n=$(grep -ci "lock order" "$log")
  rep=no; [ "$n" -gt 0 ] && rep=yes
  echo "$bin,$rc,$to,$rep,$n" >> "$OUT_DIR/helgrind.csv"
done
echo "wrote $OUT_DIR/helgrind.csv (logs in $OUT_DIR/helgrind/)" >&2
