#!/usr/bin/env bash
# helgrind.sh -- baseline comparison with an established dynamic detector.
#
# Helgrind reports "lock order ... violated" at the moment a thread acquires
# locks in an order inconsistent with earlier acquisitions -- the same
# predictive idea as PDP, so it is the natural tool to compare against.
# `timeout` is required: helgrind serialises threads and a real deadlock can
# still hang the run after the report was already printed.
# Env overrides: BIN_DIR, BINS, OUT_DIR, TIMEOUT_S, FORCE=1 (overwrite existing csv)
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
CSV="$OUT_DIR/helgrind.csv"
if [ -e "$CSV" ] && [ "${FORCE:-0}" != 1 ]; then
  echo "ERROR: $CSV exists; refusing to overwrite (use FORCE=1 or another OUT_DIR)" >&2; exit 1
fi
echo "binary,exit_code,timed_out,lock_order_reported,lock_order_violations,error_summary" > "$CSV"
for bin in $BINS; do
  exe="$BIN_DIR/$bin"; [ -x "$exe" ] || { echo "WARNING: $exe missing" >&2; continue; }
  log="$OUT_DIR/helgrind/$bin.log"
  timeout -k 2 "$TIMEOUT_S" valgrind --tool=helgrind "$exe" "$MODE" >/dev/null 2>"$log"
  rc=$?
  # 124 = timeout; 137 = killed (not necessarily a hang, so only 124 counts as timed out)
  to=no; [ $rc -eq 124 ] && to=yes
  # One line per report: 'Thread #N: lock order "A before B" violated'.  Helgrind
  # reports each lock PAIR once, so a k-cycle does not give k.
  n=$(grep -c 'lock order ".*" violated' "$log")
  es=$(grep -m1 -o 'ERROR SUMMARY: [0-9]* errors' "$log" | grep -o '[0-9]*')
  rep=no; [ "$n" -gt 0 ] && rep=yes
  echo "$bin,$rc,$to,$rep,$n,${es:-NA}" >> "$CSV"
done
echo "wrote $CSV (logs in $OUT_DIR/helgrind/)" >&2
