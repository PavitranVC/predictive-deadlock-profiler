# STATUS board
Every agent reads this first and updates ONLY its own line.

- **Current stage:** 4 — Confirmer
- **Done:** Stages 0–3 (committed); Stage 5 scripts merged (7e357a9)
- **Next:** Stage 4 review -> docs; then full Stage 5 run

## Agent lines
- orchestrator: Stage 3 committed (09cda59); Stage 4 builder running.
- builder: Stage 4 (confirmer + watchdog) in progress
- experimenter: Stage 5 scripts done, merged; full run pending after Stage 4
- docs: Stage 3 docs written (stages/stage-3.md, CHANGELOG.md entry, arch/arch-v3.dot+png, VIVA.md through Stage 3); committed.
- reviewer: Stage 4 reviewed -> docs/reviews/stage-4.md; 4 MUST-FIX (watchdog self-cycle false positive on signalling sems, same on recursive mutex, no watchdog negative tests, exit 42 alone trusted as deadlock), 9 SHOULD-FIX, 12 NIT.

## Blockers
- (none)
