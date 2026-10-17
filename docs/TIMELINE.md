# Timeline — every commit, in order

All commits are dated 2026-10-17 (times are local, from the commit list). Hashes are on `main`.

Types: **WIP** = work in progress, pushed for safety before the stage was finished. **code** = implementation commit (scripts or source, final). **review** = reviewer's report. **fix** = fixes after review. **docs** = stage notes, CHANGELOG, arch, VIVA, README. **merge** = join of the parallel experimenter branch. **results** = experiment data and charts.

| hash | time | stage | type | one-line summary |
|---|---|---|---|---|
| `59aac05` | 12:19 | 0 | docs | Scaffolding: README, `.gitignore`, STATUS board, agent definitions |
| `891e236` | 12:23 | 1 | WIP | LD_PRELOAD interceptor `libpdp.so` and basic test |
| `3b02523` | 12:28 | 1 | code | Interceptor final, with review fixes and stage 1 docs |
| `99d77cb` | 12:35 | 2 | docs | STATUS updated: Stage 2 and the experimenter session started |
| `a8a3cfb` | 12:37 | 2 | WIP | Analyzer, `pdp` CLI, bank demo, analyzer tests |
| `f0b7c28` | 12:39 | 5 | code | Parallel session: experiment harness, overhead, Helgrind and plotting scripts |
| `19d3249` | 12:43 | 5 | fix | Parallel session: `plot.py` clamps negative error bars at p=0 and p=1 |
| `3526c8d` | 12:48 | 2 | code | Analyzer final: GoodLock filters, F(R) fix suggestion, review fixes, docs |
| `e969aba` | 12:50 | 5 | code | Parallel session: binaries invoked as `<bin> race ITER`, iter column recorded |
| `7a5b444` | 12:51 | 3 | WIP | Ground-truth test suite (bank, sem_sq, gate, philosophers) |
| `7e357a9` | 12:51 | 5 | merge | First merge of the experimenter branch into `ccr-b8583290-l43ezk` |
| `fd35b2b` | 13:00 | 3 | review | Stage 3 review report (3 MUST-FIX) |
| `09cda59` | 13:01 | 3 | code | Suite final: start barrier, strict asserts, named forks |
| `f0028d3` | 13:03 | 3 | docs | Stage 3 notes, CHANGELOG, arch-v3, VIVA terms |
| `6d99610` | 13:03 | 3 | docs | Same stage 3 docs commit, repeated (same message as `f0028d3`) |
| `73b455e` | 13:04 | 4 | WIP | Delay injection, wait-for watchdog, `pdp confirm` |
| `d2b9e15` | 13:08 | 4 | review | Stage 4 review report (4 MUST-FIX) |
| `c8712fb` | 13:12 | 4 | fix | Confirmer fixes: no watchdog self-cycles, 3-scan persistence, DEADLOCK-line check, negative tests |
| `4a930ec` | 13:13 | 6 | WIP | `scripts/demo.sh` viva demo walkthrough |
| `4339dbf` | 13:14 | 6 | code | `bank_fixed`: bank demo with PDP's suggested lock order applied |
| `6781836` | 13:14 | 4 | docs | Stage 4 notes, CHANGELOG, arch-v4, VIVA terms |
| `fe5def7` | 13:20 | 5/6 | review | Stage 5 and 6 review report (4 MUST-FIX) |
| `dfe6a7b` | 13:24 | 5/6 | fix | Script fixes: no silent overwrite, trap exits, ns-timed randomised overhead, RT guard, raw exit codes, block-randomised order |
| `9742098` | 14:35 | 5 | results | Full experiment results: manifest, env, hang-rate charts (cloud VM, 10,800 runs) |
| `b182568` | 14:36 | 5 | merge | Merge of the fixed scripts from the main line back into the experimenter branch |
| `1228caa` | 14:52 | 5 | results | Overhead results, Helgrind baseline, updated charts |
| `2c9ade3` | 14:57 | 5+6 | docs | Experiment results docs, demo walkthrough, final README and VIVA (arch-v5) |
| `b96f9a5` | 16:15 | 6 | docs | Full commit timeline, process and review-driven design changes, fixed changelog refs |
| `4e2e80b` | 18:13 | 5 | results | WSL2 replication: 10,800 runs with randomised blocks, `results/wsl2/` (manifest, env, overhead, Helgrind, charts). Pushed from the local machine via Antigravity |
| pending | pending | 5 | docs | WSL2 replication docs: `stage-5.md` section, README headline, VIVA (answers 44 to 56), CHANGELOG and TIMELINE (not committed yet) |

Stage 1 final is `3b02523` (WIP `891e236`). Stage 2 is `a8a3cfb` (WIP) and `3526c8d` (final). Stage 6 is `4a930ec` (demo), `4339dbf` (bank_fixed), `fe5def7` (review), `dfe6a7b` (fixes) and `2c9ade3` (docs and README).

## Version map

The architecture diagrams are never overwritten. Each version is a new file, drawn at the end of the stage it describes.

| diagram | stage | commit(s) | what it shows |
|---|---|---|---|
| `docs/arch/arch-v1.dot` / `.png` | Stage 1 | `891e236` WIP, `3b02523` final (dot written 12:27) | Interceptor (`libpdp.so`): hooks, thread table, log. Later stages shown as "Planned" |
| `docs/arch/arch-v2.dot` / `.png` | Stage 2 | `3526c8d` final (`a8a3cfb` WIP) | Adds `pdp-analyze`: parser, lock-order graph, cycle search, GoodLock filter, report |
| `docs/arch/arch-v3.dot` / `.png` | Stage 3 | `f0028d3` and `6d99610` docs (13:03) | Adds the ground-truth programs (bank, sem_sq, gate, philosophers) and `check_suite.sh` |
| `docs/arch/arch-v4.dot` / `.png` | Stage 4 | `6781836` docs (`73b455e` WIP, `c8712fb` fixes) | Adds the confirmer: delay injection, watchdog thread, `pdp confirm` |
| `docs/arch/arch-v5.dot` / `.png` | Stage 5 and 6 | `2c9ade3` docs (dot written 14:55) | Adds experiments (harness, overhead, Helgrind), `bank_fixed` and the demo |

There is no arch-v6: this pass changed documentation only, not the architecture. The WSL2 replication (`4e2e80b` and its docs) adds results and docs only, so `arch-v5` still applies.
