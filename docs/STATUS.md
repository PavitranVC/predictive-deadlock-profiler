# STATUS board
Every agent reads this first and updates ONLY its own line.

- **Current stage:** DONE (Stages 1–6). Remaining: user reruns scripts/run_experiment.sh on WSL2
- **Done:** Stages 0–6 built, reviewed, fixed, documented; docs completeness pass done (CHANGELOG commit refs, TIMELINE.md, PROCESS.md, README and VIVA links); branch is `main`; cloud experiment results in results/
- **Next:** WSL2 rerun (sudo -E scripts/run_experiment.sh), viva prep with docs/VIVA.md

## Agent lines
- orchestrator: all stages committed and pushed.
- builder: Stage 4 + bank_fixed done
- experimenter: done (remote session finished; results merged)
- docs: completeness pass done (CHANGELOG commit refs and branch entry, TIMELINE.md, PROCESS.md, README and VIVA links). Not committed.
- reviewer: Stage 5/6 reviewed -> docs/reviews/stage-5-6.md; 4 MUST-FIX (INT/TERM trap resumes loop and mislabels loaded runs, results CSVs overwritten, overhead ignores exit status, overhead timing ms-quantised/conflates startup), 11 SHOULD-FIX, 10 NIT.

## Blockers
- (none)
