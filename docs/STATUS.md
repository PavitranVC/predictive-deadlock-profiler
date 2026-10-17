# STATUS board
Every agent reads this first and updates ONLY its own line.

- **Current stage:** 5/6 — Experiment results + demo
- **Done:** Stages 0–4 committed; Stage 5 scripts merged; Stage 6 demo.sh + bank_fixed written
- **Next:** merge full experiment results from session branch, Stage 5/6 review + docs

## Agent lines
- orchestrator: Stage 4 committed; waiting on full experiment run (remote session).
- builder: Stage 4 + bank_fixed done
- experimenter: full 200-run experiment in progress (remote session); demo.sh done
- docs: Stage 4 docs written (stages/stage-4.md, CHANGELOG.md entry, arch/arch-v4.dot+png, VIVA.md through Stage 4); not committed.
- reviewer: Stage 5/6 reviewed -> docs/reviews/stage-5-6.md; 4 MUST-FIX (INT/TERM trap resumes loop and mislabels loaded runs, results CSVs overwritten, overhead ignores exit status, overhead timing ms-quantised/conflates startup), 11 SHOULD-FIX, 10 NIT.

## Blockers
- (none)
