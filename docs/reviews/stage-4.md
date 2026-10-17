# Stage 4 Review: Confirmer

Scope: `git diff HEAD~1 -- src tests Makefile` (commit 73b455e), plus src/pdp.h,
src/libpdp.c (hooks and table), src/pdp, src/report.c `print_cycle_sites`.
`make && make test` passes (4x PASS, about 10 s).

## What I checked and what holds

- **Delay injection is in the right place.** `maybe_delay` runs before
  `waiting_on` is set and before the real lock, and only when `nheld > 0`. That
  matches the edge definition (REQ site of `to` while holding `from`).
  `--sites` emits exactly those `e->site` strings, so the delay matches the edges
  the analyzer picked. The env vars are parsed once, in `pdp_init`, while the
  process is still single-threaded, so `strtok` is safe there. When
  PDP_DELAY_SITE is unset the hot-path cost is one int compare.
- **Lazy start is race-free.** The atomic exchange on `watchdog_started` means
  only one thread calls `pthread_create`. That call happens with `in_hook=1`
  and without the table spin lock held, so nothing inside pthread_create can
  recurse into us or deadlock on `tbl_lock`.
- **The watchdog never calls a hooked function.** It only uses usleep, the spin
  lock (held just for a 64-slot memcpy), snprintf into stack buffers, write and
  _exit. It never calls `self()`, so it has no table slot.
- **The snapshot is consistent.** `end_wait` clears `waiting_on` and pushes to
  `held` in one critical section. Unlock drops from `held` *before*
  `real_unlock`. So a thread that has just acquired but has not yet cleared
  `waiting_on` points at a lock nobody lists as held: the chain ends, and you
  get no false positive from that window.
- **Cycle detection is correct on chains.** The walk follows each waiter to the
  owner of the lock it waits for (functional graph). The first slot seen twice
  is on the loop, and `report_and_die` walks that loop back to `at`. The buffer
  bounds in `report_and_die` hold: each tid is at most 12 bytes and the guard
  is 24, each `%p` is at most 19 bytes and the guard is 24, and `buf` is 900 >
  30+256+512.
- **The log is complete at _exit.** Every line goes out through an unbuffered
  write() with O_APPEND, so nothing of ours is lost. The program's own stdio
  buffers are lost, which is fine for a killed deadlocked process.
- **Flakiness.** I ran `taskset -c 0` with `-n 40 --baseline` on bank, sem_sq and
  philo_naive: 40/40 CONFIRMED each, baseline 0/40. Under 8 CPU-burner
  processes, philo_naive and bank still gave 30/30. The 5/5 test assertion
  looks safe for CI. gate, philo_ordered and basic in `race 2000` under
  PDP_WATCHDOG=1 exit 0.

## MUST-FIX

1. **Watchdog false positive on semaphores used for signalling. It kills
   correct programs with exit 42.** libpdp.c:253-266 together with the
   binary-semaphore model (libpdp.c:413-437). A consumer that does
   `sem_wait(&full)` keeps `full` in its `held[]` forever, because the producer's
   `sem_post` is a no-op `held_drop`. The consumer's next `sem_wait(&full)` then
   finds its owner is *itself*, which looks like a 1-cycle. Reproduced with a
   10-line producer/consumer (5 posts, 200 ms apart): under PDP_WATCHDOG=1 it
   exits 42 with `# pdp DEADLOCK tids=<one tid>`, but plain it exits 0. In the
   analyzer this model was only an approximation. In the watchdog it terminates
   the program, and `pdp confirm` reports CONFIRMED. Minimum fix: when looking
   up the owner, never treat the waiter as the owner of its own sem. Better
   fix: also record `kind` in the tcb and either leave `S` waits out of the
   watchdog graph or require the same cycle in two consecutive snapshots.
   Document the remaining limitation in pdp.h.
2. **Watchdog false positive on recursive mutexes.** Same self-edge, same lines.
   A `PTHREAD_MUTEX_RECURSIVE` relock sets `waiting_on=m` while `held[]` already
   contains `m`. If a snapshot lands in that window it reports a 1-cycle.
   Reproduced: a loop of 3M nested lock/lock/unlock/unlock under PDP_WATCHDOG=1
   exits 42. (A self-wait on a NORMAL mutex really is a deadlock, but on an
   ERRORCHECK mutex it returns EDEADLK. The watchdog cannot tell these apart, so
   the safe rule is to ignore self-edges or confirm them across two snapshots.)
3. **No test that the watchdog stays quiet on correct programs.**
   tests/check_confirm.sh only checks true positives. Add negatives that must
   exit 0 under PDP_WATCHDOG=1: gate and philo_ordered in `race N`, a
   producer/consumer, and a recursive-mutex program. Items 1 and 2 would have
   been caught.
4. **Exit code 42 is ambiguous, so the CONFIRMED verdict can be wrong.**
   src/pdp:50-54. Any program that exits 42 on its own counts as "deadlocked".
   Reproduced: `pdp confirm --log bank.log ./ex42` gives `deadlocked 2/2 ->
   CONFIRMED`, exit 2, even though no site matched and nothing locked. The
   verdict is the main output of this stage, so it must not rest on an exit
   code alone. Fix: log each run to a temp file (mktemp; delete it with a
   `trap ... EXIT`) and count a deadlock only if the exit code is 42 *and* the
   log contains a `# pdp DEADLOCK` line.

## SHOULD-FIX

1. src/pdp:50-54: timeout exit codes 125, 126 and 127 (timeout failed, not
   executable, not found) and deaths by signal (128+n) are all counted as
   "completed". `pdp confirm --log bank.log ./nonexistent` prints `completed 2
   -> NOT CONFIRMED`, exit 0. Classify them as errors, or abort the batch.
2. src/pdp:62-63: `-n` and `-t` are not validated. With `-n abc` you get
   `deadlocked 0/abc`, and `-n 0` gives a meaningless 0/0. Check both against
   `^[1-9][0-9]*$`.
3. src/pdp:70-73: without `--log`, the prediction run uses the *confirm* args
   (usually `race`), has no timeout, and can hang forever on the very deadlock
   being confirmed. It also overwrites ./pdp.log silently. Either require
   `--log` or wrap the run in `timeout "$t"`.
4. src/pdp:42-49: the program inherits stdin from the `<<<"$lines"` here-string
   (src/pdp:87). A target that reads stdin would swallow the remaining CYCLE
   lines and silently skip cycles. Add `</dev/null` to the timeout command.
   Also, loop variable `i` is not `local`.
5. src/pdp:45/48: `timeout` sends SIGTERM only. A program that ignores or blocks
   SIGTERM hangs the confirm forever. Use `timeout -k 1 "$t"`.
6. libpdp.c:239-244: when a delay site is set, every nested lock calls `dladdr`
   and builds the string twice (here, then again in log_event:160). Matching
   on the raw return address would be cheaper: resolve each configured site to
   an address once, then compare pointers. Or at least pass the computed site
   string to log_event. With the current cost, the "after the first lock" delay
   also slows down unrelated nested locks.
7. libpdp.c:312: every lock call by every thread does an atomic exchange, even
   after the watchdog has started, which bounces the cache line. Check first
   with `if (watchdog_started) return;` (plain read), then do the exchange.
   Also, if `pthread_create` fails the flag stays 1 and the user is never told.
   Write a `# pdp WARN watchdog` line.
8. libpdp.c:256-262: the owner lookup takes the *first* slot that lists the
   lock. With stale sem entries or cond_wait (pdp.h:43: "mutex looks held while
   waiting"), more than one slot can list the same lock, so the walk may follow
   the wrong owner. Locks taken through unhooked paths (timedlock, cond_wait
   reacquire) are invisible, so the watchdog misses those deadlocks. Document
   both as watchdog limitations next to the PDP_WATCHDOG text in pdp.h.
9. src/pdp: no check that the program being confirmed is the `exe=` in the
   log. If the paths differ, no site ever matches and the result is a silent
   NOT CONFIRMED. At least warn when `exe=` differs from `realpath "$1"`.

## NIT

1. libpdp.c:227: `atoi` is called twice with no range check. A value of 1e6
   or more passed to `usleep` may fail with EINVAL on some systems, so use
   nanosleep or document a cap. `strtol` once would be cleaner.
2. libpdp.c:229-231: a site list over 1023 bytes is silently truncated (the
   last site gets cut mid-string), and sites beyond 16 are silently dropped.
   Write a WARN line in both cases. Use `strtok_r`, or say in a comment that
   strtok is safe only because the process is single-threaded here.
3. libpdp.c:221-222: `watchdog_on` is declared between the delay comment and
   `parse_stage4_env`. Move it into the watchdog section with its own comment.
   The forward declaration at :183 is fine, but it's cleaner to define both
   parsers before `pdp_init`.
4. libpdp.c:256-262 and 276-279: the owner-lookup loop is duplicated. Pull it
   into `static int owner_of(const struct tcb *snap, void *lock)`. That is one
   function to explain in the viva instead of two copies, and the inner
   `break` plus the outer `owner < 0` guard becomes obvious.
5. libpdp.c:240: `t->nheld` is read without the spin lock. That is safe
   because only the owner thread and `pthread_mutex_destroy` write it, but say
   so in a comment.
6. libpdp.c:296: the 50 ms period and the 100000 µs default delay (:219) need
   a WHY comment. For example: 50 ms is short enough that a deadlock is
   reported well inside a 1 s test, and cheap enough at a 64-slot scan; 100 ms
   is much longer than a scheduling quantum.
7. libpdp.c:301: a cycle is re-found from every slot on it. That's harmless
   because we exit on the first one, but a comment saying so would help.
8. libpdp.c:271-289: a truncated tids/locks list gets no `...` marker.
9. fork(): the child inherits `watchdog_started=1` but not the thread, so a
   forked child runs with no watchdog. This is also a small extra risk on top
   of the existing fork-while-spinlock-held hazard. Document it.
10. src/report.c:286-301: the sites are comma-separated, so a module path
    containing a comma breaks the split. Add commas to the "no spaces" caveat
    in pdp.h. Only `pick[h]` (one site per hop) is delayed, while the other
    sites for the same pair are not. Say so in the comment.
11. tests/check_confirm.sh:29: the timing check uses `date +%s`, which has
    1-second resolution, so "at most 3 s" really allows up to about 4 s. Fine,
    but `SECONDS` or `date +%s%N` would be more precise.
12. tests/check_confirm.sh:32-33: the "without PDP" check proves nothing about
    Stage 4. Either drop it, or replace it with the negative watchdog tests
    from MUST-FIX 3.

## Viva-defensibility

Each new function can be explained in one sentence:
- `parse_stage4_env`: read the env vars once.
- `maybe_delay`: sleep at a listed site while holding a lock.
- `chain_loop`: follow waiter, then owner, then waiter, until a slot repeats.
- `report_and_die`: print the loop and exit.
- `watchdog_main`: every 50 ms, copy the table and look for a loop.
- `start_watchdog_once`: start the watchdog thread exactly once.
- `run_batch` and `cmd_confirm`: run the program N times and count exit codes.

The WHY comments for "after the first lock", `_exit` and lazy start are good.
What a student could not defend today is the self-cycle behaviour (MUST-FIX 1
and 2) and why exit code 42 alone is trusted (MUST-FIX 4).
