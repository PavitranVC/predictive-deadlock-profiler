# Stage 4: Confirmer (delay injection, watchdog, `pdp confirm`)

Status: built, `make && make test` passes (per the builder's status line). Commits: `73b455e` (WIP build), `d2b9e15`
(reviewer report `docs/reviews/stage-4.md`), `c8712fb` (review fixes, negative tests, DEADLOCK-line check). Source:
`src/libpdp.c` (delay hook and watchdog, the `# Stage 4` sections), `src/pdp` (the `confirm` subcommand, `run_batch`,
`cmd_confirm`), `src/report.c` (`print_cycle_sites`), `src/pdp.h` (ENVIRONMENT block), `src/analyze.c` (`--sites`),
tests `tests/prodcons.c`, `tests/recursive.c` (new negatives), `tests/check_confirm.sh` (the assertions).

## What was built

Stages 1–3 can only *predict* a deadlock: the analyzer says "these two orders could interleave into a hang". Stage 4
asks whether the hang really happens. It does this in three parts:

1. **Directed delay injection** (`PDP_DELAY_SITE`, `PDP_DELAY_US`). The analyzer's cycle names the call sites where the
   second lock is requested. The hook sleeps at those sites, so the thread that already holds a lock waits longer
   before asking for the next one. This widens the window in which the other threads can each take their first lock.
2. **A watchdog thread** (`PDP_WATCHDOG=1`). It looks at the live state of the program every 50 ms and reports a
   deadlock only if a real wait-for cycle exists in that state. When it finds one, it writes a log line and ends the
   process.
3. **`pdp confirm`** in `src/pdp`. It runs the program N times, with and without the delay, and counts how many runs
   the watchdog ended. The verdict is CONFIRMED if at least one run deadlocked.

The pipeline is:

```
pdp run -o bank.log ./bank seq          predict: the sequential run never hangs
pdp analyze --sites bank.log            REAL cycle + the REQ call sites of its edges
pdp confirm -n N -t T --log bank.log ./bank race 1
   |- N runs with PDP_DELAY_SITE=<sites> PDP_WATCHDOG=1  (the confirmer)
   |- verdict: CONFIRMED if >= 1 run deadlocked, else NOT CONFIRMED
```

`--baseline` runs the same N times without the delay. It is the control: if the baseline also deadlocks, the delay
was not the cause.

## Why each piece exists

### The delay (directed, not random)

A deadlock needs two threads to be between their first and second lock at the same time. A natural run rarely puts
them there. Stage 3's `slow` mode does it for every thread, with a 100 ms sleep. The confirmer does it only at the
sites the analyzer named. This is the idea of DeadlockFuzzer (Joshi et al., PLDI 2009): take the cycle from the
prediction and steer the scheduler only at the statements that matter, so the other code runs normally.

Where the delay sits in `libpdp.c`: `maybe_delay` runs in the lock hook **before** `waiting_on` is set and before the
real lock is called, and only when the thread holds at least one lock (`nheld > 0`). That is the same condition as a
graph edge (a REQ while holding `from`), and `--sites` prints exactly those site strings, so the delay hits the edges
the analyzer picked. The default sleep is `PDP_DELAY_US=100000` (100 ms). The site list is read once, in `pdp_init`,
while the process is still single-threaded.

### The watchdog (live detection)

- **Lazy start.** The watchdog thread is created by the first hooked call that finds `PDP_WATCHDOG=1`, not in the
  constructor. The atomic exchange on `watchdog_started` makes sure only one thread creates it. The `pthread_create`
  happens with `in_hook` set and without the table spin lock, so nothing inside it can call back into the hooks.
- **Snapshot.** Every 50 ms the watchdog copies the thread table (64 slots) under the spin lock. Each slot holds the
  thread's `held` list and `waiting_on`. `end_wait` clears `waiting_on` and adds the lock to `held` in one critical
  section, and unlock removes it from `held` before the real unlock. A thread caught between its real acquire and
  `end_wait` points at a lock that nobody lists as held, so its chain ends and gives no false cycle. The watchdog never calls a hooked function and never calls
  `self()`, so it has no slot of its own.
- **Wait-for graph.** Each waiting thread points to the owner of the lock it waits on. `owner_of` returns the first
  other slot whose `held` list contains that lock. Each thread waits on at most one lock, so every node has at most one
  outgoing edge. Following the edges from any thread either ends (no cycle) or reaches a slot twice (a cycle).
- **Self-edges are ignored.** A thread waiting for a lock it already lists as held is not a deadlock in the
  watchdog's view. It happens with a recursive mutex relock, and with a semaphore that the same thread signals. See
  the limits below.
- **Persistence check.** The same set of thread ids must form the cycle in **3 consecutive scans** (about 150 ms).
  One snapshot can catch a thread in the middle of a lock call, so one scan is not enough.
- **Report and stop.** The watchdog writes `# pdp DEADLOCK tids=.. locks=..` to the log and to stderr, then calls
  `_exit(42)`.

### Recovery by termination

The process cannot recover itself: its threads are all waiting and nothing will wake them. So the watchdog stops
the process with exit 42. That is recovery by termination. A real program would restart, or roll back its work. The
confirmer only needs proof that the hang happens, so exit 42 is the signal it collects. `timeout -k 1` is the outer
backstop for the case where the watchdog itself never fires (the program is hung for another reason, or the watchdog
is not running).

### `pdp confirm` and the verdict

`run_batch` runs the program `n` times under `timeout -k 1 t`, with `</dev/null` on stdin, and classifies each run:

| run ends with | counted as |
|---|---|
| exit 42 **and** its own log has `# pdp DEADLOCK` | deadlocked |
| exit 42 with no DEADLOCK line in its own log | completed (the program exited 42 by itself) |
| exit 124 (killed by `timeout`) | hung, no cycle seen |
| exit 125, 126, 127 (`timeout` or exec failed) | error, status 1 |
| any other status | completed (signal deaths are still counted here; see open items) |

Each run writes its own log to a `mktemp` directory, and a `trap ... EXIT` deletes it. The check "exit 42 **and** the
DEADLOCK line" is what stops a program that exits 42 by itself from being counted as a deadlock.

`-n` and `-t` must be positive integers, and a missing program is an error (exit 1), not a silent NOT CONFIRMED.

## Results

- **Bank, with the delay, 10 of 10 runs deadlocked. Baseline (no delay), 0 of 10.** The delay is what makes the
  deadlock happen. The 0/10 baseline is the control.
- The ground-truth test `tests/check_confirm.sh` predicts from a clean `seq` run and confirms with `race 1`. It asserts
  `deadlocked 5/5, hung 0, completed 0 -> CONFIRMED` for bank, sem_sq and philo_naive, and the negatives must exit 0
  under `PDP_WATCHDOG=1`.

## Review fixes (`docs/reviews/stage-4.md`)

The reviewer found four MUST-FIX items. They are fixed:

| Item | Fix |
|---|---|
| False self-cycle on a producer/consumer semaphore (a consumer's second `sem_wait` looked like waiting for itself) | Self-edges are ignored. Reproduced with `tests/prodcons.c`: plain run 0, old watchdog 42, fixed 0 |
| False self-cycle on a recursive mutex relock | Self-edges ignored. `tests/recursive.c`: old library exit 42 in 10 of 10 runs, now 0 |
| No test that the watchdog stays quiet on correct programs | Negative tests: gate, philo_ordered, prodcons and recursive must exit 0 under `PDP_WATCHDOG=1` |
| Exit 42 alone trusted as "deadlocked" | Deadlock needs exit 42 **and** the run's own DEADLOCK line |

Also fixed: a missing program is an error; `-n`/`-t` are validated; the prediction run has a timeout; `timeout -k 1`
is used and stdin is `/dev/null`; `owner_of` replaces two copies of the same loop.

### Still open from the review

These are in `docs/reviews/stage-4.md` and are **not** done in this stage (checked in `src/pdp` and `src/libpdp.c`):

- Signal deaths (status 128+n) are counted as "completed", not as errors.
- No check that the program named on the command line is the `exe=` in the log. A path mismatch gives a silent NOT
  CONFIRMED.
- No `# pdp WARN` line when `pthread_create` for the watchdog fails.
- The delay still matches sites by `dladdr` and `strcmp` at every nested lock. The cheaper raw-address match is not
  done.
- The site list is silently cut at 1023 bytes or 16 sites (no WARN line).

## Known limits (watchdog and confirmer)

- **Semaphores have no true owner.** The watchdog's owner of a semaphore is the thread that last waited on it. Any
  thread may post it, so a signalling pattern can look like a cycle while a later post would free it. This is the
  limit written in `src/pdp.h`. The binary-semaphore model is exact for `sem_sq`, where each thread posts only what it
  waited for.
- **A real one-thread deadlock is missed.** A NORMAL mutex relocked by its own owner really is a deadlock, but the
  watchdog ignores self-edges because it cannot tell a NORMAL relock from an ERRORCHECK relock (which returns
  EDEADLK) or a recursive relock. The safe choice is a missed case, not a false alarm.
- **Invisible waits.** `pthread_cond_wait` (the mutex looks held while waiting), `pthread_mutex_timedlock`,
  `sem_timedwait`, and locks in threads that did not get a table slot (`held=?`) are not in the graph. A deadlock
  through them is missed.
- **`fork()`.** The child inherits `watchdog_started=1` but not the watchdog thread, so a forked child runs with no
  watchdog.
- **A deadlock is confirmed, not proved.** One run that deadlocks shows the cycle can happen under the delay. It does
  not show that the program always deadlocks, and 0/N with no delay does not show that it never does.

## Explain it in the viva

The analyzer says "these orders could deadlock". The confirmer tries to make that happen and checks the real state of
the program. It does two things. It nudges the scheduler: at the sites on the predicted cycle, the thread sleeps
100 ms while it holds a lock and before it asks for the next one, so the other thread has time to take its first
lock. Then it watches: a thread every 50 ms writes down who waits for whom, and when the same loop shows up in three
checks in a row, the program is really stuck. The watchdog then writes the proof into the log and ends the process
with exit code 42. A run only counts if that exit code and that log line are both there, so a program that happens
to exit 42 on its own does not fake a deadlock.

Two questions come up. First, does the delay create a deadlock that could never happen? No. It only changes timing.
The program still has to take the two locks in the two orders the cycle names, and a gate lock still serialises
them, so a gated cycle stays safe with any delay. The watchdog only reports loops that are present in the live state.
Second, why `_exit` and not `exit`? `exit` runs `atexit` handlers and destructors, which may lock the very mutexes
that are deadlocked, and it flushes stdio, which may also call hooks. `_exit` ends the process at once, with no
hooked call. Our log lines are already on disk, because each one is a single `write` to an `O_APPEND` file.
