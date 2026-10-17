# PROCESS: How the Project Was Built

PDP was built in one day (2026-10-17) by an orchestrator and four role agents, with one parallel session for the experiments. This file explains who did what, the loop each stage went through, and which design decisions the review changed. The commit list is in `docs/TIMELINE.md`.

## Who did what

The orchestrator is the main session. It plans each stage, starts the agents, decides the order, and commits. It has no agent file in `.claude/agents/`.

| role | model | owns (may write) | never touches |
|---|---|---|---|
| orchestrator | main session | git, `docs/STATUS.md` (orchestrator line) | agents' files, except to commit |
| builder | sonnet | `src/`, `tests/`, `Makefile`, its STATUS line | `docs/`, `scripts/`, `results/` |
| experimenter | sonnet | `scripts/`, `results/`, its STATUS line | `src/`, `tests/`, `Makefile`, `docs/` |
| docs | haiku | `docs/` only, its STATUS line | `src/`, `tests/`, `scripts/` |
| reviewer | opus | `docs/reviews/stage-N.md`, its STATUS line | all code (read-only; builds and runs tests only) |

Ownership rules are written in each agent file (`.claude/agents/builder.md`, `experimenter.md`, `docs.md`, `reviewer.md`). The rule that matters most: **the reviewer never edits code**. A reviewer that fixes code grades its own work. Findings go to a report, the builder fixes them, and the reviewer's report is kept in the repo.

Every agent reads `docs/STATUS.md` first and updates only its own line. This is how agents hand over state without talking to each other.

## The per-stage loop

1. **Builder** writes the stage's code and tests in `src/`, `tests/` and `Makefile`.
2. **`make` and `make test`** run. A stage does not go on until both pass. If something fails twice, it goes under Blockers in STATUS and the work stops.
3. **Reviewer** reads the code and writes `docs/reviews/stage-N.md`, with each finding marked MUST-FIX, SHOULD-FIX or NIT, with file and line.
4. **Fixes**: the builder fixes the MUST-FIX items and some SHOULD-FIX items. Where possible a test is added that would fail without the fix.
5. **Docs** (haiku): `docs/stages/stage-N.md`, a CHANGELOG entry, a new `arch-vN` diagram (never overwritten), and VIVA terms and questions.
6. **Commit**. The commit is made only after the tests pass.

**Tested-before-commit rule.** A commit that does not build or fails `make test` is not made, except a WIP commit (below), which is clearly labelled.

**WIP commits pushed for safety.** When a stage was not finished but work was worth keeping, it was committed as `WIP` and pushed. Stage 1 (`891e236`), Stage 2 (`a8a3cfb`), Stage 3 (`7a5b444`), Stage 4 (`73b455e`) and Stage 6 (`4a930ec`) each have a WIP commit before the final one. Nothing was lost if a session stopped.

## The STATUS board

`docs/STATUS.md` has a Current stage line, Done and Next lines, a Blockers section, and one line per agent. It is the only shared state between agents. Each agent reads it first and updates only its own line, which avoids merge conflicts and tells the next agent exactly where things stand.

## The parallel experiment session

The Stage 5 experiment scripts were written by the experimenter in a separate Claude session, on the experiment branch (`ccr-b8583290-l43ezk-experiments`, auto-named by the cloud tool; merged into main and deleted), while the main line built Stages 2 and 3. That session wrote `scripts/run_experiment.sh`, `overhead.sh`, `helgrind.sh` and `plot.py` (`f0b7c28`, `19d3249`, `e969aba`), then ran the full experiment: **10,800 runs** on a cloud VM with 4 vCPUs (3 binaries x 3 schedulers x 3 core sets x idle or loaded x 200 runs), with 0 errors.

The two branches were merged twice:
- `7e357a9`: the experiment branch merged into the main line, so the scripts were in the repo before Stage 3 finished.
- `b182568`: a merge of the fixed scripts (from the Stage 5/6 review fixes, `dfe6a7b`) from the main line, so the later results (`1228caa`) came from the fixed script.

Work moved to `main` at `2c9ade3`. The experiment branch is fully merged and deleted; `main` is the only branch.

## Design decisions changed by review

Each stage was reviewed before its docs were written. The table records what the first version did, what the reviewer found, and what changed. Full details are in `docs/reviews/`.

| stage | original | problem found | change |
|---|---|---|---|
| 1 | call site taken from the return address | off by one: names the line after the call (`stage-1.md` MUST-FIX 1) | use `dladdr(ret - 1)` |
| 1 | slots never reused; a full table lost events silently | more than 64 threads over the program's life lost their events; lost data looked like "holds nothing" | free a slot when its thread exits (pthread key destructor); log `held=?` and a WARN line |
| 1 | `pdp_init` not covered by the recursion guard | init could call a hook and recurse | re-entry guard in `pdp_init` |
| 2 | three-colour DFS for cycles | missed real cycles that share nodes, exit 0 (false negative) | per-start-node elementary cycle enumeration, with caps |
| 2 | capped search could be read as "nothing found" | a capped search was reported FILTERED and exit 0 | a capped search gives verdict UNDECIDED, exit 2 |
| 2 | gate test: one common gate lock in all edges | weaker than the standard Goodlock rule | pairwise GoodLock rule: distinct threads and pairwise disjoint held sets |
| 2 | re-locking a lock already held added an edge | false edges for recursive mutexes | no edge when the lock is already held |
| 3 | threads started in a loop, no common start | race mode measured thread-creation stagger (philo_naive 0/20 natively, 10/10 under PDP) | start barrier in race and slow modes only (barrier not hooked, so no edges) |
| 3 | `check_suite.sh` accepted hangs where none are allowed; no timeout on the seq run | the suite could pass a broken tool | strict asserts: exact cycle count, 5 distinct locks, timeouts, clean programs exit 0, real programs must hang in slow mode |
| 3 | forks in one array `fork_[]` | report showed `fork_+0x28`, not a name | named mutexes `fork0`..`fork4` plus a pointer array |
| 4 | watchdog reports any cycle in the wait-for graph | a semaphore used for signalling, or a recursive mutex, gives a self-edge: a correct program killed with exit 42 | skip self-edges; a cycle must persist in 3 consecutive scans |
| 4 | exit 42 alone counted as "deadlocked" | any program that exits 42 gave CONFIRMED | a run also needs its own `# pdp DEADLOCK` line in its log |
| 4 | no tests that the watchdog stays quiet | no proof of no false positives | negative tests: `prodcons`, `recursive` must exit 0 |
| 5 | overhead = whole process wall time | includes fork/exec, the loader and the log, so not an interceptor cost | time inside the process in ns; pair order randomised; ITER swept; raw rc column; failed runs excluded |
| 5 | results CSVs written with `>` and overwritten silently; INT/TERM trap resumed the loop | a smoke test could overwrite the real data; interrupted runs were mislabelled | no silent overwrite (`FORCE=1` to override); trap exits |
| 5 | fixed cell order (bin, sched, cores, load), `loaded` straight after `idle` | drift over hours is confounded with the factor | block-randomised order, with `block` and `ts` columns |
| 5 | exit 124 and 137 both counted as hung; no RT guard | an OOM kill looks like a hang; a hard lockup under RT is possible in principle | 137 classified by elapsed time, raw rc recorded; RT guard on `sched_rt_runtime_us` |
| 5 | `TIMEOUT_S` 5 s in the README; timeout validity not measured | slow but live runs could be counted as hangs | `TIMEOUT_S=1` for the cloud run; slowest ok run 76 ms, fastest timeout 1003 ms. The review asked for a printed validity check; that is still open |
| 6 | no check that `bank slow` really hung; INT/TERM trap resumed the demo | if the run finished early, the attach steps looked broken | check that `bank slow` hung; INT/TERM trap exits 130; ptrace_scope reason and fix printed |

Open from the reviews (also listed in the stage docs): `error` runs in `plot.py`, the timeout-validity table, and several NIT items.

## Stage 0: scaffolding

Stage 0 (`59aac05`) set up the repository: the README with the goal and plan, `.gitignore`, the STATUS board, and the agent definitions in `.claude/agents/`. It wrote no code.

Custom agent types (`builder`, `reviewer`, and the rest) became usable only after the Claude session was reloaded. Early stages therefore used general-purpose agents, each given its role file (`.claude/agents/<role>.md`) as its brief.

## Explain it in the viva

Four roles, one loop: the builder writes, `make test` must pass, the reviewer (who cannot edit code) reports, the builder fixes, the docs agent records, then we commit. The review changed real decisions, for example the cycle search, the GoodLock rule, the watchdog's 3-scan rule, and the measurement method, and the table above shows each change and why.
