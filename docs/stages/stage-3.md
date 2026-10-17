# Stage 3 — Ground-truth test suite (`tests/`)

Status: built, `make && make test` passes (per the builder's status line). Committed as `09cda59`; the reviewer
report is `docs/reviews/stage-3.md` (commit `fd35b2b`). Source: `tests/bank.c`, `tests/sem_sq.c`, `tests/gate.c`,
`tests/philo_naive.c`, `tests/philo_ordered.c` (the five programs), `tests/check_suite.sh` (the assertions),
`tests/README.md` (the interface), `Makefile` (pattern rule `tests/bin/%`).

## What was built

Five small C programs, each with a **known ground truth**: we know in advance whether the program can deadlock.
A detector is only believable if it says "yes" for the programs that can deadlock and "no" for the ones that
cannot. These programs are that answer key.

| program | what it does | ground truth | expected PDP verdict |
|---|---|---|---|
| `bank.c` | teller 1 locks A then B, teller 2 locks B then A | real 2-cycle (two locks) | REAL `acct_a -> acct_b -> acct_a`, exit 2 |
| `sem_sq.c` | P0: wait S, wait Q; P1: wait Q, wait S (binary semaphores) | real 2-cycle (two semaphores) | REAL `S -> Q -> S`, exit 2 |
| `gate.c` | same opposite order as bank, but both first take a global mutex `gate_G` | cannot deadlock (gate serialises the two) | FILTERED, gate lock `gate_G`, exit 0 |
| `philo_naive.c` | 5 philosophers, each takes left fork then right fork | real 5-cycle (five forks) | REAL 5-cycle through `fork0`..`fork4`, exit 2 |
| `philo_ordered.c` | 5 philosophers, each takes the lower-numbered fork first | cannot deadlock (no cycle in the order) | no cycle, exit 0 |

The two "clean" programs are as important as the two "real" ones. A detector that reports every lock pair
as a deadlock would pass the real tests and fail the clean ones.

Each program has the same command line (`argv[1]` picks the mode):

| mode | threads | purpose |
|---|---|---|
| `seq` (default) | one after another (create, join, next) | predict from a run that never hangs |
| `race [N]` | all at once, `N` iterations each (default 1000), no delay | the natural hang rate, for the scheduling experiment |
| `slow` | all at once, `usleep(100 ms)` between the two locks | forced hang, so the confirmer and the tests have a sure deadlock |

### Why each mode exists

- **`seq`** is the core idea of the whole project. A sequential run cannot block itself (the second thread
  only starts after the first has finished and released everything), so the program always exits 0. But the
  interceptor still logs every REQ and ACQ, so the lock-order edges (`A -> B`, `B -> A`) are in the log. The
  analyzer then finds the cycle. `check_suite.sh` checks this for every real program: `pdp run` in `seq`, then
  `pdp analyze`, expecting exit 2 for bank, sem_sq, philo_naive and exit 0 for gate, philo_ordered.
- **`race [N]`** is the natural manifestation. No artificial delay: the only thing that decides whether the
  program hangs is the scheduler. This is what the Stage 5 experiment varies (scheduler policy, cores, load).
  The suite runs it only as a smoke test (`race 200`, both 0 and 124 are accepted for the real-cycle programs).
- **`slow`** forces the hang. The 100 ms sleep between the first and second lock gives the other thread time to
  take its first lock, so the circular wait is certain. The suite expects exit 124 (killed by `timeout`) for the
  three real programs. Stage 4 (confirmer) will use the same idea: add delays at chosen sites.

## The start barrier (added after review)

The first version of `race` mode started threads with `pthread_create` and did no other synchronisation. The
reviewer measured what that really measured: **thread-creation stagger**. Thread 1 started running before
thread 2 existed, often finished its loop, and only then did thread 2 begin. With small N the threads did
not overlap at all.

Concrete numbers from the review (4 CPUs, 20 runs each, before the barrier):

| program | race 1000, native | race 1000, under PDP |
|---|---|---|
| bank | 13/20 hang at `race 1000` (20/20 at `race 100000`) | 10/10 |
| philo_naive | **0/20** | **10/10** |

philo_naive never hung natively at N=1000, yet always hung under PDP. The logging slowed each thread enough that
the threads overlapped. So the experiment would have measured the **observer effect** and the creation latency,
not the scheduler.

The fix is a `pthread_barrier_t` with count = number of worker threads. Each worker calls
`pthread_barrier_wait` once, at the top of its thread function, before the loop. Only `race` and `slow` use it;
`seq` has no barrier (a count of 1 would be a no-op).

Why this is safe for the analyzer: PDP intercepts `pthread_mutex_*` and `sem_*` only. `pthread_barrier_wait` is
not hooked, so it writes no log event and creates no edge. The threads hold no lock while they wait at it.

After the barrier, native `race 1000` on 4 cores (20 runs each): **bank 17/20, sem_sq 15/20, philo_naive 12/20**
hang. The rates are now set by interleaving and N, not by thread-creation speed. The barrier is artificial
alignment and raises hang rates compared with a natural program. The experiment write-up must say so.

## What `check_suite.sh` asserts

The suite is strict: each assertion has a reason, and a broken analyzer should fail it.

- **Seq prediction (every program).** `pdp run -o <log> ./prog seq` must finish within `timeout 10` (a hang in
  the interceptor would otherwise hang `make test`). Then `pdp analyze` must return the expected exit code.
- **Report text.** bank: `acct_a -> acct_b -> acct_a`. sem_sq: `S -> Q -> S`. philo_naive: 5 arrows, and the first
  cycle line must name 5 **distinct** locks and `fork0`. gate: `FILTERED`, `gate lock gate_G`, and the header
  `3 locks, 6 edges`. philo_ordered: `No deadlock potential` and the header `5 locks, 5 edges`.
- **Exactly one cycle** for bank, sem_sq and philo_naive (no `[2]` line). A search that reports each rotation
  of the same cycle, or a duplicate, would fail here. This is the bug class found in the Stage 2 review.
- **The header counts.** An empty or dead log also prints "No deadlock potential" with exit 0. The `N locks,
  M edges` line proves the analyzer saw the edges and rejected the cycle, rather than seeing nothing.
- **Real hangs.** `slow` for bank, sem_sq and philo_naive must exit 124 under `timeout 2`.
- **Clean programs never hang.** `gate` and philo_ordered must exit 0 in `slow` mode and in `race 200`. A
  regression that deadlocks gate would fail.
- **Race smoke test.** bank, sem_sq, philo_naive in `race 200`: exit 0 or 124 are both fine, anything else fails.

## Review fixes applied

Reviewer report: `docs/reviews/stage-3.md` (3 MUST-FIX, 6 SHOULD-FIX, 11 NIT).

| Item | Status |
|---|---|
| M-1 gate and philo_ordered may hang without failing; philo_ordered `slow` never run | Fixed: both must exit 0 in `slow` and `race 200` |
| M-2 no single-cycle check; philo names not checked distinct | Fixed: no `[2]` line; 5 arrows and 5 distinct names |
| M-3 race measures thread stagger | Fixed: start barrier in race and slow (see above) |
| S-1 seq run has no timeout | Fixed: `timeout 10`, 124 reported as failure |
| S-2 unknown mode or N <= 0 accepted silently | Open: `./bank bogus` runs seq, `race abc` runs 0 iterations |
| S-3 empty log passes for gate and philo_ordered | Fixed: header counts asserted |
| S-4 call sites for sem_sq and philo not checked | Open: markers exist (`DEADLOCK-RELEVANT`), no assertion yet |
| S-5 only bank `slow` tested for hang | Fixed: sem_sq and philo_naive `slow` must exit 124 |
| S-6 `pthread_create` return values unchecked | Open in all five programs |
| N-1 `fork_` needs a comment | Fixed (comment says why it is not `fork`) |
| N-2 report shows `fork_+0x28` | Fixed: five named mutexes `fork0`..`fork4` plus a pointer array |
| N-3 `#define N` clashes with the `race [N]` text | Fixed: `NPHIL` |
| N-4 `(int)(long)arg` | Fixed: `intptr_t` round trip |
| N-6 `timeout 5` on race 200 costs time | Fixed: `timeout 2` |
| N-8 duplicate explicit Makefile rules | Fixed: only the `tests/bin/%` pattern rule |
| N-11 `sem_sq` never calls `sem_destroy` | Open (harmless: the process exits) |

Not done: a shared `tests/mode.h` for the five `main()` functions (NIT 10). The copies are kept on purpose,
so each program reads on its own.

## Known limits

- **Race-mode hang rates are machine-specific.** They depend on core count, scheduler, and load (Stage 5
  measures this). The suite does not check a rate; it only checks that the programs do not crash.
- **The barrier raises hang rates.** Good for comparing native and PDP runs fairly, but not a natural
  workload.
- **Clean programs are clean by ordering or by a gate, not by timing.** A test that passes today can only
  fail if the code changes, not because of load. The race tests accept both outcomes for the real programs,
  so they cannot flake.
- **Sites are not checked** (S-4 open). The report could cite the wrong line and the suite would still pass.
- **Mode validation is missing** (S-2 open). A typo runs seq and exits 0.
- **Semaphores as locks.** `sem_sq` models binary semaphores as locks with an owner (the thread that waited).
  Our post is always done by the waiter here, which keeps the model exact for this program.

## Explain it in the viva

Each test program is an answer key. We know whether it can deadlock, so we can check the detector. Three programs
have a real circular wait (bank, the two-semaphore program, and the naive philosophers). Two programs cannot
deadlock (the gate one because a global lock serialises the two orders, and the ordered philosophers because
every thread takes forks in one global order). The test checks that the detector says REAL for the first kind
and FILTERED or nothing for the second.

The three modes answer three questions. `seq` asks: can we predict a deadlock from a run that did not hang?
`race` asks: how often does the real program hang with the scheduler alone? `slow` asks: can we make the hang
happen on purpose? The barrier makes `race` honest: without it, the threads start one after another, and we
would measure how fast `pthread_create` is, not how the locks interleave. The barrier does not add any edges,
because we do not hook it.

The suite is strict on purpose. It checks the exact number of cycles (one), that the five philosophers are
five different locks, and that the clean programs never hang, even in race mode. A test that accepted any
output would not tell us anything.
