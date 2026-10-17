#!/bin/bash
# check_confirm.sh - Stage 4: delay injection + watchdog + `pdp confirm`.
cd "$(dirname "$0")/.." || exit 1
fail() { echo "FAIL: $1"; exit 1; }

# Predict from a clean seq run, confirm with concurrent "race 1" (one iteration each).
for p in bank sem_sq philo_naive; do
    log=tests/bin/$p.confirm.log
    timeout 10 ./build/pdp run -o "$log" "./tests/bin/$p" seq >/dev/null || fail "$p: seq run failed"
    out=$(./build/pdp confirm -n 5 -t 5 --log "$log" "./tests/bin/$p" race 1); rc=$?
    [ "$rc" -eq 2 ] || fail "$p: confirm exit $rc, want 2"
    grep -qF "deadlocked 5/5, hung 0, completed 0 -> CONFIRMED" <<<"$out" || fail "$p: not 5/5 confirmed: $out"
done

# Nothing to confirm when no cycle is predicted (or it is gated).
for p in gate philo_ordered bank_fixed; do
    log=tests/bin/$p.confirm.log
    timeout 10 ./build/pdp run -o "$log" "./tests/bin/$p" seq >/dev/null || fail "$p: seq run failed"
    out=$(./build/pdp confirm --log "$log" "./tests/bin/$p" race 1); rc=$?
    [ "$rc" -eq 0 ] || fail "$p: confirm exit $rc, want 0"
    grep -qF "nothing to confirm" <<<"$out" || fail "$p: expected 'nothing to confirm'"
done

# Watchdog alone (slow mode hangs by itself): exit 42 quickly, DEADLOCK line in the log.
log=tests/bin/bank.wd.log
start=$(date +%s)
PDP_WATCHDOG=1 PDP_LOG=$log LD_PRELOAD=./build/libpdp.so timeout 10 tests/bin/bank slow >/dev/null 2>&1; rc=$?
[ "$rc" -eq 42 ] || fail "watchdog: exit $rc, want 42"
[ $(( $(date +%s) - start )) -le 3 ] || fail "watchdog: took more than 3 s"
grep -q '^# pdp DEADLOCK tids=[0-9,]* locks=0x' "$log" || fail "watchdog: no DEADLOCK line in log"

# Negative tests: correct programs must NEVER be killed by the watchdog (exit 42).
# No delay injection here; the watchdog is the only thing under test.
neg() {
    local desc=$1; shift
    PDP_WATCHDOG=1 PDP_LOG=/dev/null LD_PRELOAD=./build/libpdp.so timeout 20 "$@" >/dev/null 2>&1
    local rc=$?
    [ "$rc" -eq 0 ] || fail "watchdog false alarm or hang on $desc: exit $rc, want 0"
}
neg "gate race"          tests/bin/gate race 200
neg "philo_ordered race" tests/bin/philo_ordered race 200
neg "bank_fixed race"    tests/bin/bank_fixed race 200
neg "prodcons race"      tests/bin/prodcons race 200
neg "prodcons slow"      tests/bin/prodcons slow
neg "recursive race"     tests/bin/recursive race 300000
echo PASS
