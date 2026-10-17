# STATUS board
Every agent reads this first and updates ONLY its own line.

- **Current stage:** 2 — Analyzer
- **Done:** Stage 0 scaffolding; Stage 1 interceptor (reviewed, fixed, documented)
- **Next:** Stage 2 builder (analyzer)

## Agent lines
- orchestrator: Stage 1 committed; starting Stage 2.
- builder: Stage 1 review fixes done (3 MUST + errno, FAIL event, locked waiting_on, abs exe path, PIE); make && make test PASS (libpdp.c ~320 lines). Not committed.
- experimenter: idle
- docs: Stage 1 docs written (stages/stage-1.md, CHANGELOG.md, arch/arch-v1.dot+png, VIVA.md); not committed.
- reviewer: Stage 1 reviewed -> docs/reviews/stage-1.md; 3 MUST-FIX (site ret-1, silent table/held overflow, pdp_init re-entry guard), 10 SHOULD-FIX, 9 NIT.

## Blockers
- (none)
