# Test programs

All programs in `tests/` share one interface (`argv[1]` selects the mode):

| mode | meaning |
|------|---------|
| `seq` (default) | threads run one after another (create+join each). Never hangs, but every lock-order edge still occurs, so PDP can predict from this clean run. |
| `race [N]` | threads run concurrently, no delay, N iterations each (default 1000). May or may not hang. Used by the Stage 5 experiment. |
| `slow` | concurrent, `usleep(100000)` between first and second lock. Hangs almost always. |

In `race` and `slow` modes every worker first waits on a `pthread_barrier_t`
(count = number of worker threads), so all workers start contending together.
Without it, thread 1 could finish before thread 2 is created, and the
experiment would measure thread-creation stagger instead of lock interleaving.
The barrier is artificial alignment and raises hang rates compared with
natural programs. `pthread_barrier_wait` is not intercepted by PDP, so it adds
no log events or edges. `seq` mode has no barrier.

Exit status is 0 on completion.

| program | ground truth |
|---------|--------------|
| `bank.c` | A->B vs B->A: real 2-cycle |
| `sem_sq.c` | semaphores S,Q taken in opposite order: real 2-cycle |
| `gate.c` | same opposite order but under global mutex `gate_G`: FILTERED, never hangs |
| `philo_naive.c` | 5 philosophers, left then right fork: real 5-cycle |
| `philo_ordered.c` | lower-numbered fork first: no cycle |

Checks: `check_basic.sh`, `check_analyze.sh` (hand-written logs + bank), `check_suite.sh` (all programs above). Run with `make test`.
