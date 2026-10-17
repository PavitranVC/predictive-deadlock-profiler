# Changelog

All notable changes to PDP, one entry per stage. Newest stage last.

## Stage 0: Scaffolding (2026-10-17, commit `59aac05`)

- Project README: goal, prior art, and the planned pipeline (interceptor, analyzer, confirmer, experiment harness).
- `.gitignore`, status board `docs/STATUS.md`.
- Agent definitions under `.claude/agents/` (builder, reviewer, docs, experimenter) with role rules. The orchestrator is the
  main session, not a file; it has no definition under `.claude/agents/`.

## Stage 1: LD_PRELOAD Interceptor (2026-10-17, commits `891e236` WIP, `3b02523` final)

- `src/libpdp.so` (`src/libpdp.c`): hooks for `pthread_mutex_lock`, `pthread_mutex_unlock`,
  `pthread_mutex_trylock`, `pthread_mutex_destroy`, `sem_wait`, `sem_post`, resolved with `dlsym(RTLD_NEXT)`.
- Per-thread table (64 slots, up to 16 held locks each) guarded by a test-and-set spin lock; slots are freed
  when a thread exits (`pthread_key` destructor).
- Log format v1 (`src/pdp.h`): `seq tid OP kind lockaddr site held=...`, with `REQ`, `ACQ`, `REL`, `FAIL`,
  `TRY_OK`, `TRY_FAIL`, `DESTROY`, `POST`. Warnings as `# pdp WARN overflow ...` lines.
- Call sites as `module+0xoffset` from `dladdr(ret - 1)`, so `addr2line` names the call's line under ASLR.
- `held=?` marks lost data (table full, more than 16 held). Errno preserved in every hook.
- Review fixes applied: call-site off-by-one, lost-data marker, re-entry guard in `pdp_init`, `waiting_on`
  under the table lock, `FAIL` events, absolute program path, PIE requirement (details in
  `docs/stages/stage-1.md`).
- Build: `make`, `make test` (`tests/basic.c`, `tests/overflow.c`, `tests/check_basic.sh`). Passes on gcc 13.3, glibc 2.39.
- Open: `fork()`, `pthread_atfork`, condvar and timed-lock hooks, semaphore edge rules, spin-loop backoff.

## Stage 2: Analyzer `pdp-analyze` (2026-10-17, commits `a8a3cfb` WIP, `3526c8d` final)

- `src/analyze.c`: parses `pdp.log` (sorted by `seq`, `held=?` skipped and counted), builds the lock-order graph
  from REQ events (edge = tid, call site, held set, count), lock identity = address + generation (DESTROY).
- Cycle search: per-start-node elementary cycle enumeration (replaces a three-colour DFS that missed cycles
  sharing nodes). Caps: length 8, 256 cycles, 200k DFS steps, 200k candidate edges per cycle.
- Filters: GoodLock rule (pairwise distinct threads, pairwise disjoint held sets). Verdicts REAL, UNDECIDED
  (capped search), FILTERED.
- `src/report.c`: names from `lk=` + `nm -S` or `L1, L2`; call sites via `addr2line`; ANSI report; F(R) fix
  suggestion (drop least frequent edge per real cycle, Kahn topological order, all call sites listed); `--dot`.
- `src/analyze.h`: limits and shared structs. `src/pdp`: `run`, `analyze` (renders PNG when `dot` exists), `confirm` stub.
- Exit codes: 0 nothing, 2 REAL or UNDECIDED, 1 error.
- Tests: `tests/check_analyze.sh` with logs in `tests/logs/` (ab_ba, gate, partial_gate, capped, recursive,
  shared_nodes, destroy_reuse, same_thread, three_cycle, unknown_held). Demo `tests/bank.c`.
- Review fixes (`docs/reviews/stage-2.md`): MUST-FIX 1-4 fixed; SHOULD-FIX 5, 6, 7, 11 fixed; 8, 9 partly; 10 open.
  Details in `docs/stages/stage-2.md`.

## Stage 3: Ground-Truth Test Suite (2026-10-17, commits `7a5b444` WIP, `fd35b2b` review, `09cda59` final, `f0028d3` and `6d99610` docs)

- Five test programs in `tests/` with known ground truth: `bank.c` (real 2-cycle), `sem_sq.c` (real 2-cycle on
  binary semaphores S, Q), `gate.c` (opposite order under gate lock `gate_G`, FILTERED), `philo_naive.c` (real
  5-cycle), `philo_ordered.c` (lower fork first, no cycle).
- Common interface: `seq` (default, predict from a run that never hangs), `race [N]` (natural concurrency),
  `slow` (100 ms between locks, forced hang). Documented in `tests/README.md`.
- Start barrier (`pthread_barrier_t`, count = thread count) in race and slow modes only. Added after review: without
  it, race mode measured thread-creation stagger (philo_naive native 0/20 vs 10/10 under PDP). `pthread_barrier_wait`
  is not intercepted, so it adds no edges. Native race-1000 hang rates after the barrier on 4 cores: bank 17/20,
  sem_sq 15/20, philo_naive 12/20.
- `tests/check_suite.sh` (run by `make test`): strict assertions. Exactly one cycle for real programs, 5 distinct
  locks in the philosopher cycle, header counts (`N locks, M edges`), `timeout` on the seq run, clean programs must
  exit 0 in `slow` and `race`, real programs must exit 124 in `slow`.
- Named forks `fork0`..`fork4` so reports show exact names; `NPHIL` constant; `intptr_t` for thread arguments.
- Review (`docs/reviews/stage-3.md`, commit `fd35b2b`): 3 MUST-FIX fixed; open items are mode validation, site
  checks in the suite, `pthread_create` return checks, `sem_destroy` in sem_sq (listed in `docs/stages/stage-3.md`).
- Parallel session: the Stage 5 experiment scripts (`scripts/run_experiment.sh`, `overhead.sh`, `helgrind.sh`,
  `plot.py`) were written early by the experimenter in a separate session (on the
  experiment branch, auto-named `ccr-b8583290-l43ezk-experiments`, merged into main and deleted; commit `e969aba`) and merged into the main branch (merge `7e357a9`). They are
  not part of Stage 3 and are not described here beyond this note.

## Stage 4: Confirmer (2026-10-17, commits `73b455e` WIP, `d2b9e15` review, `c8712fb` fixes, `6781836` docs)

- Directed delay injection: `PDP_DELAY_SITE` (comma-separated `module+0xoff` sites) and `PDP_DELAY_US` (default
  100000). A thread that already holds a lock sleeps before requesting a lock from a listed site. Sites come from
  `pdp analyze --sites`.
- Watchdog thread (`PDP_WATCHDOG=1`, started lazily on the first hooked call). Every 50 ms it snapshots the thread
  table, builds the wait-for graph (thread to owner of the lock it waits on), and reports a cycle only after the same
  thread set is seen in 3 consecutive scans. Self-edges (recursive relock, semaphore signalling) are ignored. It writes
  `# pdp DEADLOCK tids=.. locks=..` and calls `_exit(42)`.
- `pdp confirm [-n N] [-t T] [--log f] [--baseline] ./prog args...`: runs the program N times, counts a deadlock only
  if exit 42 and the run's own log has the DEADLOCK line, and prints CONFIRMED if at least one run deadlocked.
  Errors (status 125-127, bad `-n`/`-t`, missing program) exit 1.
- Measured: bank 10/10 deadlocked with the delay, 0/10 at baseline.
- New test programs `tests/prodcons.c` and `tests/recursive.c` as negatives (must exit 0 under `PDP_WATCHDOG=1`), and
  `tests/check_confirm.sh` (true positives for bank, sem_sq, philo_naive; negatives for gate, philo_ordered, prodcons,
  recursive).
- Review (`docs/reviews/stage-4.md`): 4 MUST-FIX fixed (self-cycle false positives on semaphores and recursive
  mutexes, negative tests, exit 42 alone trusted). Several SHOULD-FIX and NIT items remain open (signal deaths counted
  as completed, no `exe=` check, no `pthread_create` WARN, string-based site matching); listed in
  `docs/stages/stage-4.md`.
- Known limits: semaphores have no true owner (`src/pdp.h`); a real one-thread relock deadlock is missed; cond_wait and
  timedlock are invisible to the watchdog; a forked child has no watchdog.

## Stage 5: Experiment Harness and Results (2026-10-17, commits `f0b7c28`, `19d3249`, `e969aba` scripts from the parallel session, `7e357a9` and `b182568` merges, `fe5def7` review, `dfe6a7b` fixes, `9742098` and `1228caa` results, `2c9ade3` docs)

- Harness: `scripts/run_experiment.sh` (native race runs, no LD_PRELOAD, `ITER=1000`, `TIMEOUT_S=1`, start barrier
  in race mode), `scripts/overhead.sh` (`philo_ordered` and `gate`, baseline vs `LD_PRELOAD`), `scripts/helgrind.sh`,
  `scripts/plot.py` (Wilson 95% intervals), `scripts/runtime_ns.py`.
- **Full run, 10,800 runs, 0 errors** (3 binaries x 3 schedulers x 3 core sets x idle/loaded x 200 runs), cloud VM
  (not WSL2), 4 vCPUs, bash busy-loop load. Data: `results/cloud/manifest.csv`, `results/cloud/env.txt`, charts
  `results/cloud/hangrate_{bank,sem_sq,philo_naive}.png`.
- Findings (hypotheses marked in `docs/stages/stage-5.md`): 0% on one core for every config; more cores raise the
  rate; load hides the bug under the default scheduler (bank on 2 cores 64.0% to 0.5%) and exposes it under
  SCHED_FIFO and SCHED_RR (bank on 2 cores to 99 to 100%).
- Overhead: median 6.7 to 11.3 us per lock operation; wall-time ratio 18x to 141x on lock-only microbenchmarks.
  Results in `results/cloud/overhead.csv`, chart `results/cloud/overhead.png`.
- Helgrind baseline (`results/cloud/helgrind.csv`): flags bank and philo_naive, misses sem_sq (no semaphore lock-order
  support), flags gate (false positive). PDP catches all three real cycles and filters gate.
- **Caveat:** the full manifest was produced with the pre-fix `run_experiment.sh` (fixed cell order, not
  block-randomised). The WSL2 rerun uses the fixed script.
- Review fixes (`docs/reviews/stage-5-6.md`) applied to `scripts/`: INT/TERM trap exits instead of resuming the loop
  (MUST 1); no silent overwrite of CSVs, `FORCE=1` to override (MUST 2); overhead rc column and error rows excluded
  (MUST 3); overhead timing in ns, pair order randomised, ITER swept (MUST 4); raw exit code recorded, 137 classified
  by elapsed time (SHOULD 5); randomised block order with `block` and `ts` columns (SHOULD 6); RT guard on
  `sched_rt_runtime_us` (SHOULD 7); fuller `env.txt` (SHOULD 8).
- Still open from the review: `error` column in plot.py, timeout-validity table in plot.py, NIT items.
- Docs: `docs/stages/stage-5.md`, `docs/arch/arch-v5.dot` and `.png`.

## Stage 6: Live Demo (2026-10-17, commits `4a930ec` WIP, `4339dbf` bank_fixed, `fe5def7` review, `dfe6a7b` fixes, `2c9ade3` docs and README)

- `scripts/demo.sh`: build, clean pass, hang with top and gdb, `pdp run` and `pdp analyze` prediction, `pdp confirm`
  with baseline, fixed version, gate-lock filtering, results chart. Pauses per step; `DEMO_NOPAUSE=1` runs straight
  through. Temp directory removed on exit.
- Review fixes: ptrace_scope reason and fix printed (and `sudo -n gdb` when available); check that `bank slow` hung;
  INT/TERM trap exits with 130; colours only on a terminal.
- Docs: `docs/stages/stage-6.md` (step-by-step walkthrough, fallbacks, two-minute viva script).

## Branch: Work Moved to `main` (2026-10-17)

- Work moved to `main`. `main` was created from the development branch (`ccr-b8583290-l43ezk`, auto-named by the cloud tool; merged into main and deleted) at `2c9ade3`.
- The experiment branch (`ccr-b8583290-l43ezk-experiments`, auto-named by the cloud tool) is fully merged (`7e357a9`, and the fixed scripts in `b182568`).
- Commit references above were checked against the commit list; see `docs/TIMELINE.md` for every commit in order.

## Stage 5 Addendum: WSL2 replication (2026-10-17, results commit `4e2e80b`, docs commit `7a6cd5e`)

- `results/wsl2/`: the full matrix on WSL2 (Intel Core Ultra 9 275HX, 22 vCPUs, kernel 6.6 WSL2). 10,800 runs,
  200 per cell, 0 errors. Randomised block order (block 10, seed 3464). Load: `stress-ng --cpu n` pinned to the core
  set (the cloud run used bash busy loops, a confound). Also `overhead.csv`, `helgrind.csv`, `env.txt`, and the charts.
  Code at commit `b96f9a5`.
- Replicated on both machines: 0 hangs on one core (36 cells); the default scheduler with load hides the bug on two
  cores (bank 67.0% to 0.5%, sem_sq 73.0% to 1.0% on WSL2); real-time scheduling with load exposes it at 99.0% to
  100.0% in all 16 loaded bank and sem_sq cells with two or four cores; Helgrind's result is identical.
- Observed differences, not explained: four-core idle rates for bank and sem_sq (57 to 68% on WSL2, about 94% on cloud);
  philo_naive on two real-time cores (1.0 to 2.0% idle on WSL2, 43.5 to 45.0% on cloud). Candidate causes are
  hypotheses only (`docs/stages/stage-5.md`, "WSL2 replication").
- Overhead on WSL2: 1.5 to 3.1 us per lock operation (cloud 6.7 to 11.3 us); wall ratio 12x to 33x (cloud 18x to 141x).
- Caveat: on WSL2 every finished run took 104 to 106 ms, a fixed floor, so the elapsed column does not measure the work
  there. The hang separation still holds (106 ms against 1005 ms).
- Docs: `docs/stages/stage-5.md` (WSL2 replication section), `README.md` (headline results), `docs/VIVA.md` (updated
  answers and questions 54 to 56). No new architecture version: this addendum changes results and docs, not the design,
  so `arch-v5` still applies.

## Repository Tidy-Up (2026-10-17, commit pending)

- Cloud results moved from `results/*.{csv,png,txt}` to `results/cloud/`; WSL2 results (primary) stay in `results/wsl2/`. All path references in the README and docs updated.
- The auto-generated branches `ccr-b8583290-l43ezk` and `ccr-b8583290-l43ezk-experiments` were merged into `main` and deleted. `main` is the only branch. Docs now call them "the development branch" and "the experiment branch".
- New `docs/README.md`: documentation index with a recommended reading order.
- `README.md` rewritten: overview, key features, quick start with real output, results, how it works, layout, limitations, future work.
- `docs/stages/stage-6.md` now cites the WSL2 numbers, with the cloud as replication. `docs/STATUS.md` marks the project complete. No code changed, so `arch-v5` still applies.
