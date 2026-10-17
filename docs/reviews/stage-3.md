# Stage 3 Review: Ground-Truth Test Suite

Scope: tests/README.md, tests/bank.c, tests/sem_sq.c, tests/gate.c,
tests/philo_naive.c, tests/philo_ordered.c, tests/check_suite.sh, Makefile.
`make && make test` passes (3x PASS).

## What I checked and what holds

- **Ground truth is right.** By inspection and by running:
  - bank, sem_sq and philo_naive in `slow` mode all hang (exit 124 under
    `timeout 2`).
  - gate and philo_ordered finish with exit 0 in `slow` and in `race 100000`.
  - philo_ordered's edges are 0->1, 1->2, 2->3, 3->4 and 0->4. That graph has
    no cycle, which is the ordering argument a student can explain.
- **seq mode never hangs and still produces every edge.** Each thread runs
  once (iters=1), one after another with create+join. The interceptor records
  the kernel TID (libpdp.c:76), so the threads stay distinct even though
  glibc reuses `pthread_t` after a join. The analyzer output matches the
  expected graph: bank 2 locks / 2 edges, gate 3 / 6, philo 5 / 5.
- **No API misuse.** Every unlock happens on the thread that locked. Nothing
  held is ever destroyed. Static initializers are used. Binary semaphores are
  posted by the thread that waited on them.

## Measured race-mode behaviour (4 CPUs, 20 runs each, timeout 2 s)

| program | race 10 | race 100 | race 1000 | race 100000 | race 1000 under PDP |
|---|---|---|---|---|---|
| bank | 1/20 | 4/20 | 13/20 | 20/20 | 10/10 |
| sem_sq | - | - | 11/20 | 20/20 | - |
| philo_naive | 0/20 | 0/20 | **0/20** | 20/20 | **10/10** |

## MUST-FIX

1. **check_suite.sh:41-44 accepts a hang from programs that must never hang.**
   The loop accepts exit 124 for gate and philo_ordered, and philo_ordered
   `slow` is never run. So the suite never checks the "never hangs in any
   mode" claim (gate.c:12, README). A regression that deadlocks gate would
   still PASS.
   - Fix: for gate and philo_ordered, require rc == 0 for both `slow` and
     `race N`. Keep "0 or 124" only for the three real-cycle programs.

2. **check_suite.sh:22-28 is too weak to catch a broken cycle search.**
   - Nothing checks that there is exactly one cycle. A DFS that reports each
     of the 5 rotations of the philosopher cycle, or a duplicate S->Q->S,
     still passes. That is exactly the class of bug the Stage 2 review found
     in the DFS.
   - The philo check counts arrows but does not check that the 5 names are
     distinct. `fork_ -> fork_ -> ...` with 5 arrows would pass.
   - Fix:
     - Assert there is no `^  \[2\]` line for bank, sem_sq and philo_naive.
     - For philo_naive, split the `[1]` line on ` -> `, drop the last
       element, and require `sort -u | wc -l` == 5.

3. **The race-mode workload measures thread-start stagger, not scheduling.**
   Code: bank.c:75-78, sem_sq.c:57-60, gate.c:61-64, philo_naive.c:46-51,
   philo_ordered.c:47-52.
   - Thread i starts running before thread i+1 is even created. With small N,
     the first threads finish before the last one starts.
   - philo_naive `race 1000` never hangs natively (0/20) but always hangs
     under PDP (10/10). The slowdown from logging stretches each thread's
     runtime until they overlap.
   - So Stage 5 would mostly be measuring creation latency plus observer
     effect. The README's "may or may not hang" (tests/README.md:8) hides
     this. A decision is needed before Stage 5 starts.
   - Recommendation: add a start barrier in race and slow mode only.
     - Use `pthread_barrier_t`, initialised to the thread count, with one
       `pthread_barrier_wait` at the top of each thread function, before the
       loop.
     - Add a WHY comment: "so every thread starts contending at the same
       moment".
   - Pros of a barrier:
     - Hang probability then depends on interleaving and N, not on how fast
       `pthread_create` is.
     - Comparisons between native and PDP runs become fair.
     - philo becomes comparable to bank.
     - It is easy to defend in the viva.
     - glibc's barrier does not go through the hooked `pthread_mutex_lock`
       or `sem_wait`, so it adds no edges to the log. Confirm with one
       analyze run.
   - Cons of a barrier:
     - It is an artificial alignment, so it is less "natural" than real
       programs, where threads start at different times.
     - It raises hang rates. Say so in the experiment write-up.
     - Do not use it in seq mode (a barrier count of 1 would be a no-op
       anyway).
   - If you keep the current design instead, the README must say explicitly
     that race-mode overlap depends on N and on thread-creation latency.
     Stage 5 must then sweep N and report the native-vs-PDP observer effect.

## SHOULD-FIX

1. **check_suite.sh:9 has no timeout on the seq run under PDP.** If an
   interceptor bug makes a seq run hang, `make test` hangs forever. Wrap it
   in `timeout 10`, and treat 124 as a fail with a clear message.
2. **Bad modes and bad N are accepted silently.** Code: bank.c:70-73,
   sem_sq.c:50-53, gate.c:56-59, philo_naive.c:41-44, philo_ordered.c:42-45.
   - `./bank bogus` runs seq mode and exits 0.
   - `./bank race abc` runs 0 iterations and exits 0.
   - In Stage 5, a typo would show up as "0% hang rate".
   - Fix: reject an unknown mode or N <= 0 with a usage message and exit 1.
3. **check_suite.sh:33 lets an empty or dead log pass for philo_ordered.**
   An empty graph also prints "No deadlock potential" with rc 0. Assert the
   header too: `"5 locks, 5 edges"` for philo_ordered and `"3 locks, 6 edges"`
   for gate. Then a pass shows the analyzer saw the edges and rejected the
   cycle, rather than seeing nothing.
4. **check_suite.sh:23,25 do not check the reported call sites for sem_sq and
   philo.** The `DEADLOCK-RELEVANT` markers (sem_sq.c:30,42, philo_naive.c:33)
   exist but nothing uses them. check_analyze.sh:50-53 already does this for
   bank with `SITE_T1`/`SITE_T2`; reuse that pattern. Also pick one marker
   style for all files (bank uses `SITE_T1`, the others `DEADLOCK-RELEVANT`).
5. **check_suite.sh:36-38 tests only bank `slow` for a real hang.** The README
   claims sem_sq and philo_naive hang too, and both do (verified). Add them
   (rc must be 124). This is cheap and turns the README claim into a test.
6. **`pthread_create` return values are not checked** (all five mains). If
   create fails, `pthread_join` on an uninitialised `pthread_t` is undefined
   behaviour. Add one `if (... != 0) { perror; exit(1); }`. An examiner will
   ask about this.

## NIT

1. philo_naive.c:20, philo_ordered.c:20: the name `fork_` needs a WHY comment
   (it avoids clashing with `fork(2)` from unistd.h).
2. **Cosmetic: the report shows `fork_`, `fork_+0x28`, ... for the forks.**
   - Simplest fix, test side only: declare five named mutexes `fork0`..`fork4`
     plus `static pthread_mutex_t *fork_[N] = { &fork0, ... }`. The report then
     prints exact names with zero analyzer change and nothing new to defend.
   - Alternative, analyzer side (report.c:70-87): pass the node's `kind` to
     `lookup_symbol`.
     - Let elem = `kind=='S' ? sizeof(sem_t) : sizeof(pthread_mutex_t)`.
     - If `size > elem && size % elem == 0 && (off-val) % elem == 0`, print
       `name[(off-val)/elem]`.
     - This is about 4 lines. It relies on the analyzer and the target
       sharing an ABI, which is true here.
     - It gives a wrong index for arrays of structs that embed a lock (for
       example `struct account x[4]`, 48-byte elements: offset 240 would print
       `[6]` instead of `[5]`), so document that limit.
   - I prefer the test-side fix.
3. philo_*.c:19 `#define N 5` collides by name with the "race [N]" argument
   in the comments and README. Rename it `NPHIL`.
4. philo_*.c:29 `(int)(long)arg`: use `intptr_t` or add a one-line comment
   on why the round trip through `long` is safe.
5. check_suite.sh:26: the comment "4 arrows + return" reads as 4. Say "5
   arrows (a->b->c->d->e->a)".
6. check_suite.sh:42: `timeout 5` on `race 200` costs 5 s every time bank,
   sem_sq or philo_naive hang. `timeout 2` is enough.
7. check_suite.sh:37: `bank slow` relies on t2 being created within 100 ms.
   That is fine in practice, but a one-line comment would cover it.
8. Makefile:11-13,23-29: the explicit rules for basic, bank and overflow
   duplicate the `tests/bin/%` pattern rule. Delete them.
9. Makefile:3: the trailing `# PIE...` comment leaves trailing spaces in
   TESTCF (harmless). Also add a WHY note that tests build without `-O` so
   the line numbers at call sites stay exact.
10. The five `main()` functions are copy-pasted. That is acceptable for
    readability, but a shared `tests/mode.h` (parse mode, create/join
    pattern) would make SHOULD-FIX 2 and MUST-FIX 3 one-place changes.
11. sem_sq.c:54-55: no `sem_destroy`. It is harmless because the process
    exits, but an examiner may ask; mention it in a comment or add it.
