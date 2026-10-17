#!/bin/bash
# check_analyze.sh - run pdp-analyze on hand-written logs, then on the real bank demo.
cd "$(dirname "$0")/.." || exit 1
A=./build/pdp-analyze
fail() { echo "FAIL: $1"; exit 1; }

# expect <log> <exit code> <text that must appear> [more texts...]
expect() {
    local log=$1 want=$2; shift 2
    local out; out=$($A --no-color tests/logs/$log); local rc=$?
    [ $rc -eq "$want" ] || fail "$log: exit $rc, want $want"
    for text in "$@"; do
        grep -qF -- "$text" <<<"$out" || fail "$log: report lacks '$text'"
    done
}

expect ab_ba.log         2 "REAL potential deadlocks" "L1 -> L2 -> L1" "tid 101" "tid 102" "acquire locks in this order"
expect same_thread.log   0 "FILTERED" "same-thread" "No deadlock potential"
expect gate.log          0 "FILTERED" "common gate lock L1" "No deadlock potential"
expect three_cycle.log   2 "REAL" "L1 -> L2 -> L3 -> L1"
expect destroy_reuse.log 0 "No deadlock potential"
expect unknown_held.log  0 "skipped 1 REQ" "No deadlock potential"

# the filtered logs must not be reported as REAL
$A --no-color tests/logs/gate.log | grep -q "REAL" && fail "gate.log reported REAL"

# error handling and colour switch
$A /nonexistent.log >/dev/null 2>&1; [ $? -eq 1 ] || fail "missing file should exit 1"
$A >/dev/null 2>&1; [ $? -eq 1 ] || fail "no args should exit 1"
$A tests/logs/ab_ba.log | grep -q $'\033' && fail "colour codes when stdout is not a tty"

# dot output: cycle edges red
D=tests/bin/ab_ba.dot
$A --no-color tests/logs/ab_ba.log --dot $D >/dev/null
grep -q 'color=red' $D || fail "dot file has no red edge"

# real program: bank in default (non-hanging) mode, through the pdp front end
LOG=tests/bin/bank.log
./build/pdp run -o $LOG ./tests/bin/bank >/dev/null || fail "bank run"
out=$(./build/pdp analyze $LOG --no-color); rc=$?
[ $rc -eq 2 ] || fail "bank: exit $rc, want 2"
t1=$(grep -n 'SITE_T1' tests/bank.c | head -1 | cut -d: -f1)
t2=$(grep -n 'SITE_T2' tests/bank.c | head -1 | cut -d: -f1)
for text in "REAL" "bank.c:$t1" "bank.c:$t2" "acct_a" "acct_b" "acct_a -> acct_b -> acct_a"; do
    grep -qF -- "$text" <<<"$out" || fail "bank report lacks '$text'"
done
[ "$(./build/pdp confirm)" = "not implemented yet (Stage 4)" ] || fail "pdp confirm message"
echo PASS
