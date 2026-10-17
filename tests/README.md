# Test programs

All programs in `tests/` share one interface (`argv[1]` selects the mode):

| mode | meaning |
|------|---------|
| `seq` (default) | threads run one after another (create+join each). Never hangs, but every lock-order edge still occurs, so PDP can predict from this clean run. |
| `race [N]` | threads run concurrently, no delay, N iterations each (default 1000). May or may not hang. Used by the Stage 5 experiment. |
| `slow` | concurrent, `usleep(100000)` between first and second lock. Hangs almost always. |

Exit status is 0 on completion.

| program | ground truth |
|---------|--------------|
| `bank.c` | A->B vs B->A: real 2-cycle |
| `sem_sq.c` | semaphores S,Q taken in opposite order: real 2-cycle |
| `gate.c` | same opposite order but under global mutex `gate_G`: FILTERED, never hangs |
| `philo_naive.c` | 5 philosophers, left then right fork: real 5-cycle |
| `philo_ordered.c` | lower-numbered fork first: no cycle |

Checks: `check_basic.sh`, `check_analyze.sh` (hand-written logs + bank), `check_suite.sh` (all programs above). Run with `make test`.
