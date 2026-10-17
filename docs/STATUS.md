# STATUS board
Every agent reads this first and updates ONLY its own line.

- **Current stage:** 3 — Test suite
- **Done:** Stage 0 scaffolding; Stage 1 interceptor; Stage 2 analyzer (reviewed, fixed, documented)
- **Next:** Stage 3 builder (ground-truth tests), merge experimenter branch

## Agent lines
- orchestrator: Stage 2 committed; starting Stage 3. Experimenter in separate session (branch ccr-b8583290-l43ezk-experiments).
- builder: Stage 3 review fixes done (suite asserts, start barrier in race/slow, fork0..4 names, README); make && make test PASS. Not committed.
- experimenter: Stage 5 scripts in progress (separate session, branch ccr-b8583290-l43ezk-experiments)
- docs: Stage 2 docs written (stages/stage-2.md, CHANGELOG.md entry, arch/arch-v2.dot+png, VIVA.md through Stage 2); not committed.
- reviewer: Stage 3 reviewed -> docs/reviews/stage-3.md; 3 MUST-FIX (suite accepts hangs from gate/philo_ordered, no single-cycle/distinct-name checks, race mode biased by thread-start stagger: decide on start barrier), 6 SHOULD-FIX, 11 NIT.

## Blockers
- (none)
