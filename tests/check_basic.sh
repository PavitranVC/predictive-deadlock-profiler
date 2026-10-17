#!/bin/bash
# check_basic.sh - run basic under libpdp.so and assert on the log.
cd "$(dirname "$0")/.." || exit 1
LOG=tests/bin/basic.log
fail() { echo "FAIL: $1"; exit 1; }

LD_PRELOAD=./build/libpdp.so PDP_LOG=$LOG ./tests/bin/basic >/dev/null || fail "exit code"
head -1 $LOG | grep -q '^# pdp v1 pid=[0-9]* exe=' || fail "header"
req=$(grep -c ' REQ ' $LOG); acq=$(grep -c ' ACQ ' $LOG)
[ "$req" -gt 0 ] && [ "$req" -eq "$acq" ] || fail "REQ/ACQ mismatch ($req/$acq)"
grep ' REQ ' $LOG | grep -vq 'held=-' || fail "no REQ with non-empty held set"
grep -q ' DESTROY ' $LOG || fail "no DESTROY"
grep -q ' S 0x' $LOG || fail "no semaphore events"
grep -q ' TRY_OK ' $LOG || fail "no TRY_OK"

site=$(grep ' ACQ M ' $LOG | head -1 | awk '{print $6}')
mod=${site%%+*}; off=${site##*+}
addr2line -e "$mod" "$off" | grep -q 'tests/basic.c' || fail "addr2line did not resolve to basic.c ($site)"
echo PASS
