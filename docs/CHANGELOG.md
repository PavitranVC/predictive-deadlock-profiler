# Changelog

All notable changes to PDP, one entry per stage. Newest stage last.

## Stage 0 — scaffolding (2026-10-17, commit `59aac05`)

- Project README: goal, prior art, and the planned pipeline (interceptor, analyzer, confirmer, experiment harness).
- `.gitignore`, status board `docs/STATUS.md`.
- Agent definitions under `.claude/agents/` (builder, reviewer, docs, experimenter, orchestrator) with role rules.

## Stage 1 — LD_PRELOAD interceptor (2026-10-17, commit `891e236`, WIP)

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

## Stage 2 — analyzer `pdp-analyze` (2026-10-17, not committed, WIP)

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
