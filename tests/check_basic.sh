#!/bin/bash
# check_basic.sh - run the test programs under libpdp.so and assert on the logs.
cd "$(dirname "$0")/.." || exit 1
LOG=tests/bin/basic.log
OLOG=tests/bin/overflow.log
fail() { echo "FAIL: $1"; exit 1; }

LD_PRELOAD=./build/libpdp.so PDP_LOG=$LOG ./tests/bin/basic >/dev/null || fail "exit code"
head -1 $LOG | grep -q '^# pdp v1 pid=[0-9]* exe=/' || fail "header (exe must be absolute)"
req=$(grep -c ' REQ ' $LOG); acq=$(grep -c ' ACQ ' $LOG); bad=$(grep -c ' FAIL ' $LOG)
[ "$req" -gt 0 ] && [ "$req" -eq $((acq + bad)) ] || fail "REQ != ACQ+FAIL ($req/$acq/$bad)"
grep ' REQ ' $LOG | grep -vq 'held=-' || fail "no REQ with non-empty held set"
grep -q ' DESTROY ' $LOG || fail "no DESTROY"
grep -q ' S 0x' $LOG || fail "no semaphore events"
grep -q ' TRY_OK ' $LOG || fail "no TRY_OK"
grep -q 'WARN' $LOG && fail "unexpected WARN in basic run"

# The first REQ with a non-empty held set is the lock of B; its site must be
# the exact line marked SITE_B in basic.c (checks the return-address-1 fix).
site=$(grep ' REQ M ' $LOG | grep -v 'held=-' | head -1 | awk '{print $6}')
mod=${site%%+*}; off=${site##*+}
want=$(grep -n 'SITE_B' tests/basic.c | head -1 | cut -d: -f1)
got=$(addr2line -e "$mod" "$off")
case "$got" in *basic.c:$want) ;; *) fail "site $site resolved to '$got', want basic.c:$want";; esac

# Overflow run: 70 live threads (63 tracked + main), 17 nested locks, then slot reuse.
LD_PRELOAD=./build/libpdp.so PDP_LOG=$OLOG ./tests/bin/overflow || fail "overflow exit code"
[ "$(grep -c 'WARN overflow.*what=threads' $OLOG)" -eq 7 ] || fail "want 7 threads warnings"
[ "$(grep -c 'WARN overflow.*what=held' $OLOG)" -eq 1 ] || fail "want 1 held warning"
# held=- is only legitimate for a first lock: 1 (main) + 63 tracked workers + 1 reused slot
[ "$(grep ' REQ ' $OLOG | grep -c 'held=-')" -eq 65 ] || fail "unexpected number of held=- (data lost silently?)"
grep ' REQ ' $OLOG | tail -1 | grep -q 'held=0x' || fail "freed slot not reused (last REQ B)"
echo PASS
