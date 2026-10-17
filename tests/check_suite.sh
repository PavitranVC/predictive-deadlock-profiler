#!/bin/bash
# check_suite.sh - ground-truth programs: PDP must predict from a clean seq run.
cd "$(dirname "$0")/.." || exit 1
fail() { echo "FAIL: $1"; exit 1; }

# analyze <prog>: runs prog in seq mode under PDP; sets OUT and RC from the analyzer.
analyze() {
    local log=tests/bin/$1.suite.log
    ./build/pdp run -o "$log" "./tests/bin/$1" seq >/dev/null || fail "$1: seq run failed"
    OUT=$(./build/pdp analyze "$log" --no-color); RC=$?
}
# want <prog> <exit code> <text>...
want() {
    local p=$1 rc=$2; shift 2
    analyze "$p"
    [ "$RC" -eq "$rc" ] || fail "$p: exit $RC, want $rc"
    for text in "$@"; do
        grep -qF -- "$text" <<<"$OUT" || fail "$p: report lacks '$text'"
    done
}

want bank 2 "REAL potential deadlocks" "acct_a -> acct_b -> acct_a"
want sem_sq 2 "REAL potential deadlocks" "S -> Q -> S"

want philo_naive 2 "REAL potential deadlocks"
# the first cycle line "[1] a -> b -> ... -> a" must name 5 distinct locks (4 arrows + return)
arrows=$(grep -m1 '^  \[1\]' <<<"$OUT" | grep -o ' -> ' | wc -l)
[ "$arrows" -eq 5 ] || fail "philo_naive: cycle has $arrows arrows, want 5 (5 locks)"

want gate 0 "FILTERED" "gate lock gate_G" "No deadlock potential"
grep -q "REAL" <<<"$OUT" && fail "gate reported REAL"

want philo_ordered 0 "No deadlock potential"
grep -qE "REAL|FILTERED" <<<"$OUT" && fail "philo_ordered reports a cycle"

# the hang is real: bank slow never finishes, gate slow always does
timeout 3 tests/bin/bank slow >/dev/null; [ $? -eq 124 ] || fail "bank slow should hang (124)"
timeout 5 tests/bin/gate slow >/dev/null; [ $? -eq 0 ] || fail "gate slow should exit 0"

# race mode must not crash: 0 (finished) or 124 (hung) are both fine
for p in bank sem_sq gate philo_naive philo_ordered; do
    timeout 5 tests/bin/$p race 200 >/dev/null; rc=$?
    [ $rc -eq 0 ] || [ $rc -eq 124 ] || fail "$p race: exit $rc"
done
echo PASS
