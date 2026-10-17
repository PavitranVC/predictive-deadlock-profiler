#!/bin/bash
# check_suite.sh - ground-truth programs: PDP must predict from a clean seq run.
cd "$(dirname "$0")/.." || exit 1
fail() { echo "FAIL: $1"; exit 1; }

# analyze <prog>: runs prog in seq mode under PDP; sets OUT and RC from the analyzer.
analyze() {
    local log=tests/bin/$1.suite.log
    timeout 10 ./build/pdp run -o "$log" "./tests/bin/$1" seq >/dev/null; [ $? -eq 0 ] || fail "$1: seq run failed or timed out"
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
# the first cycle line "[1] a -> b -> ... -> a" has 5 arrows (a->b->c->d->e->a)
# and its 5 names (the last one repeats the first) must be distinct.
line=$(grep -m1 '^  \[1\]' <<<"$OUT")
arrows=$(grep -o ' -> ' <<<"$line" | wc -l)
[ "$arrows" -eq 5 ] || fail "philo_naive: cycle has $arrows arrows, want 5"
names=$(sed 's/^ *\[1\] *//' <<<"$line" | sed 's/ -> /\n/g' | head -n 5 | sort -u | wc -l)
[ "$names" -eq 5 ] || fail "philo_naive: cycle names $names distinct locks, want 5"
grep -q 'fork0' <<<"$line" || fail "philo_naive: cycle does not name fork0"

want gate 0 "FILTERED" "gate lock gate_G" "No deadlock potential" "3 locks, 6 edges"
grep -q "REAL" <<<"$OUT" && fail "gate reported REAL"

want philo_ordered 0 "No deadlock potential" "5 locks, 5 edges"
grep -qE "REAL|FILTERED" <<<"$OUT" && fail "philo_ordered reports a cycle"

# correct signalling / recursive programs: no cycle of any kind
for p in prodcons recursive; do
    analyze "$p"
    [ "$RC" -eq 0 ] || fail "$p: exit $RC, want 0"
    grep -qE "REAL" <<<"$OUT" && fail "$p reports a REAL cycle"
done

# exactly one cycle per real program (catches duplicate or rotated cycles)
for p in bank sem_sq philo_naive; do
    analyze "$p"
    grep -q '^  \[2\]' <<<"$OUT" && fail "$p: more than one cycle reported"
done

# run <timeout s> <prog> <args...>: exit code of a native run
run() { local t=$1; shift; timeout "$t" "$@" >/dev/null 2>&1; return $?; }

# the hang is real: programs with a real cycle hang in slow mode
for p in bank sem_sq philo_naive; do
    run 2 tests/bin/$p slow; [ $? -eq 124 ] || fail "$p slow should hang (124)"
done
# gate and philo_ordered must NEVER hang, in any mode
for p in gate philo_ordered prodcons recursive; do
    run 5 tests/bin/$p slow; [ $? -eq 0 ] || fail "$p slow should exit 0"
    run 5 tests/bin/$p race 200; [ $? -eq 0 ] || fail "$p race should exit 0"
done
# real-cycle programs in race mode: finished (0) or hung (124) are both fine
for p in bank sem_sq philo_naive; do
    run 2 tests/bin/$p race 200; rc=$?
    [ $rc -eq 0 ] || [ $rc -eq 124 ] || fail "$p race: exit $rc"
done
echo PASS
