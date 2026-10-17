# STATUS board
Every agent reads this first and updates ONLY its own line.

- **Current stage:** 3 — Test suite
- **Done:** Stage 0 scaffolding; Stage 1 interceptor; Stage 2 analyzer (reviewed, fixed, documented)
- **Next:** Stage 3 builder (ground-truth tests), merge experimenter branch

## Agent lines
- orchestrator: Stage 2 committed; starting Stage 3. Experimenter in separate session (branch ccr-b8583290-l43ezk-experiments).
- builder: Stage 2 review MUST-FIX 1-4 + pairwise gate + all-sites advice fixed (per-start bounded cycle enumeration replaces 3-colour DFS; UNDECIDED exit 2); 4 new test logs; make && make test PASS. Not committed.
- experimenter: Stage 5 scripts in progress (separate session, branch ccr-b8583290-l43ezk-experiments)
- docs: Stage 2 docs written (stages/stage-2.md, CHANGELOG.md entry, arch/arch-v2.dot+png, VIVA.md through Stage 2); not committed.
- reviewer: Stage 2 reviewed -> docs/reviews/stage-2.md; 4 MUST-FIX (DFS misses cycles sharing nodes, MAX_COMBOS counts leaves only = unbounded search, capped search reported FILTERED/exit 0, recursive re-lock makes false edges), 8 SHOULD-FIX, 14 NIT.

## Blockers
- (none)
