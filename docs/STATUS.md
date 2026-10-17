# Project Status Board
Every agent reads this first and updates ONLY its own line.

- **Current stage:** complete
- **Done:** stages 0-6 built, reviewed, fixed and documented; WSL2 replication (primary results in results/wsl2/, cloud replication in results/cloud/); repository tidy-up; `main` is the only branch
- **Next:** demo rehearsal and viva

## Agent lines
- orchestrator: all stages committed and pushed.
- builder: Stage 4 + bank_fixed done
- experimenter: done (remote session finished; results merged)
- docs: repository tidy-up done (README rewrite, docs/README.md index, readable branch labels, results paths, STATUS complete). Not committed.
- reviewer: Stage 5/6 reviewed -> docs/reviews/stage-5-6.md; 4 MUST-FIX (INT/TERM trap resumes loop and mislabels loaded runs, results CSVs overwritten, overhead ignores exit status, overhead timing ms-quantised/conflates startup), 11 SHOULD-FIX, 10 NIT.

## Blockers
- (none)
