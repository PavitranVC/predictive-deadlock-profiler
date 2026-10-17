# Stage 2 Review: Analyzer

Reviewer, 2026-10-17. Reviewed at commit a8a3cfb (Stage 2 WIP). Includes the `lk=` change from `git diff HEAD~1 -- src/libpdp.c src/pdp.h`.
`make && make test`: both suites PASS. bank demo: REAL acct_a -> acct_b -> acct_a, bank.c:39/51, exit 2. That is correct.
Size: analyze.c 321 + report.c 259 = 580 lines. The code is compact and every function has a header comment.

I built every repro below as a short hand-written log in the scratchpad and ran it through `build/pdp-analyze`.
Each repro is described in enough detail to rebuild it as a test log.

## What is correct (you can defend these in the viva)
- **Edges come from REQ, not ACQ** (analyze.c:144-158). A run that hangs is still analysed (three_cycle.log). The WHY comment is good.
- **Trylock rule**: TRY_* adds no incoming edge, but a lock taken by TRY_OK still shows up in later held sets. This is the right model. libpdp logs no REQ for trylock, so the two sides agree.
- **held=?** is skipped and counted, not treated as "holds nothing" (analyze.c:151). It is tested.
- **DESTROY generations** (analyze.c:96-113, 163-165): the WHY comment is clear and the test fails without the feature. A held address that was destroyed earlier correctly creates a new generation.
- **Distinct-thread pruning** in `choose` (analyze.c:267) is pairwise and correct.
- **`find_gate`** matches the spec: the gate is a lock held in every chosen edge and is not a cycle node.
- **popen quoting** (report.c:20-32, 49, 74): the module must start with `/`, is limited to `[A-Za-z0-9/._+-]`, and is wrapped in single quotes. The offset must be `0x`+hex. No quote, `$`, backtick, space or `;` can get through, and a leading `/` rules out option injection. I found no shell injection. Paths with unusual characters fall back to raw text, which is acceptable.
- Parsing bounds: `%15s` and `%255s` fit their buffers, and the held list is capped at MAX_HELDSET. A 5000-char line, an empty line, a truncated line and `held=zz` are all handled without a crash.
- **The F(R) drops break every *reported* real cycle.** Each real cycle either gets one pair removed or already contains a removed pair, and the Kahn order is consistent with the dropped pair: the target comes first.

## MUST-FIX

1. **analyze.c:195-215: the three-colour DFS misses real cycles that share nodes with a cycle found earlier.** The result is a false negative with exit 0.
   The DFS reports only back edges to GREY nodes. A cycle that closes through a node already BLACK is never seen.
   Repro:
   - T1: A, then B while holding A. Later B, then A while holding B.
   - T2: holds A, requests C.
   - T3: holds C, requests B.

   A<->B is found and filtered (same thread). A->C->B->A (T2, T3, T1, three distinct threads, no gate) is a real deadlock. The analyzer never enumerates it and prints "No deadlock potential found", exit 0.
   This is not "acceptable if documented". The filters make it serious: the cycle that was found gets filtered, and the real cycle hidden behind it is lost.
   Fix: enumerate the elementary cycles. The simplest fix you can explain in a viva: for each start node s, run a DFS that only visits nodes > s and un-marks nodes on backtrack (an "on path" flag, no BLACK). Record a cycle whenever an edge returns to s. Johnson's algorithm, simplified. Cap the work and set `g.dropped` when the cap is hit.
   If you keep the current DFS, you must at least document in the code and in the report that only one cycle per back edge is found. You must then also stop printing "No deadlock potential found" as a definite claim.

2. **analyze.h:9 + analyze.c:253-272: MAX_COMBOS does not cap the search.** `tried` counts only complete combinations (leaves). When the distinct-thread rule prunes every branch before the last hop, `tried` stays 0 and the search is exponential.
   Repro: a ring of 13 locks where each of 12 threads does every hop. That is 156 edges, and the cycle needs 13 distinct threads.
   - 11 threads, 12 locks: 15.5 s.
   - 12 threads, 13 locks: still running after 100 s (killed).

   Every inner node also scans all `g.nedges`.
   Fix: count every call to `choose` (or every candidate edge examined), not leaves. Also pre-collect the candidate edges for each hop once, in `classify`.

3. **analyze.c:279-280 + report.c:138: a capped search is reported as FILTERED and exits 0.** When the cap hits, combinations that could be real have not been tried.
   Repro: 2-cycle A<->B.
   - Threads 1..450 hold {G, A} and request B.
   - Threads 1001..1450 hold {G, B} and request A.
   - Thread 999 holds {B} and requests A. It is the last edge.

   Thread 999 paired with any of threads 1..450 is a real deadlock. The output is "common gate lock ... (search capped, may be incomplete)" and then "No deadlock potential found", exit 0.
   A capped cycle must count as "possible" (UNDECIDED). It should be printed in its own group and should make the exit code non-zero (2, or a distinct code documented in the usage text).
   It would also help to merge edges that differ only by tid and site for the gate test, because the gate test depends only on held sets.

4. **analyze.c:152-157: re-locking a recursive mutex creates false edges.** If `to` is already in the held set, the request cannot block: it is a recursive re-acquire, or for a normal mutex a self-deadlock, which is a different bug. The code drops only the h->to edge and still adds edges from every other held lock.
   Repro:
   - T1 holds R, takes A, then re-locks R.
   - T2 holds R, requests A.

   The analyzer reports REAL R -> A -> R, exit 2. It also prints T1 as "holding {A}", which hides that T1 holds R.
   Fix: if `to` is in the held set, add no edges at all (count it if you like). Update the comment at line 155.

## SHOULD-FIX

5. **analyze.c:238-249: the gate test is weaker than the standard rule.** For a real deadlock, the chosen threads' held sets must be pairwise disjoint: no lock can be held by two threads at once. That is the usual Goodlock/iGoodlock condition.
   With a common lock across ALL edges, a 3-cycle where T1 and T2 share G but T3 does not is reported REAL. Repro: T1 holds {G,A} and wants B, T2 holds {G,B} and wants C, T3 holds {C} and wants A. That is a false positive.
   The pairwise check needs no "exclude cycle locks" rule and is easier to explain. Report the shared lock and the two threads that share it.

6. **report.c:170-189: "change the call site at X" names one site.** `pair_count` sums every edge for the pair, and those edges can come from many sites. In the capped repro the output was "seen 450x" with one site listed. Changing one site does not remove the pair. List every distinct site for the dropped pair, or say "N sites, e.g. X".

7. **report.c:213-214: leftover Kahn nodes are printed as if they were ordered.** Nodes left over sit in cycles that remain: filtered cycles, and also real cycles that were missed (MUST-FIX 1) or dropped by MAX_CYCLES. Appending them in id order looks like a valid order but contradicts observed edges. Print them separately: "still cyclic (filtered or unanalysed): ...".

8. **analyze.c / report.c ignore `kind`.** Semaphores (`S`) are modelled as locks. A `sem_wait` used as a signal (posted by another thread) stays "held" forever, so every later lock gets an edge from it. Cycles through an `S` node should be labelled approximate, or the limitation from libpdp.c:300 should be repeated in the report. Also, `node.kind` stays `'?'` when a lock is first seen in a held list (analyze.c:154). That field is currently dead, so remove it or fix it.

9. **Test gaps (tests/check_analyze.sh).** Add logs for:
   - the trylock rule (TRY_OK lock in held, TRY_FAIL edge-free)
   - FAIL
   - recursive re-lock (MUST-FIX 4)
   - the shared-node miss (MUST-FIX 1)
   - a capped cycle (MUST-FIX 3)
   - a 3-cycle with a partial gate (item 5)

   destroy_reuse.log should also assert that the `#2` name appears.

10. **report.c:70-88: `nm` runs once per lock node** and parses the whole symbol table each time. Cache by module. Also note in a comment that the nm match assumes PIE or a shared object (value = offset from load base). For a non-PIE exe, `dli_fbase` = 0x400000 and every lookup silently fails.

11. **Single-process assumption is undocumented.** After `fork`, the child shares the log, duplicates `seq` values and reuses the same addresses for different locks, so false cross-process edges appear. The same goes for tid reuse when a thread exits and a new one gets its tid: two threads merge and a real cycle can be filtered as "same-thread". Document both in analyze.c's header and in VIVA.md. You could also check for a second `# pdp v1` header.

12. **libpdp.c:294: DESTROY is logged before `real_destroy`** and whether or not it succeeds. An EBUSY destroy of a lock that is still in use bumps the generation and splits one lock into two nodes, which can hide a cycle. Log it after the call, only when `r == 0`.

## NIT

- analyze.c:182: the `MAX_CYCLES` check runs before the duplicate check. Once the table is full, a re-discovered duplicate counts as "dropped" and inflates the warning.
- analyze.c:76: `evs = realloc(evs, ...)` loses the old pointer on failure. The process exits anyway, but a viva examiner may ask about it.
- analyze.c:30-39: `held=` with junk or nothing after it is treated as "holds nothing" (reliable). Treat it as unknown.
- analyze.c:72-79: a line longer than 4095 chars is split by fgets, and its tail is parsed as a new line. Skip to the newline when no `\n` was read.
- analyze.c:73: any `#` line containing "WARN" is counted, including test comments. Match `# pdp WARN`.
- analyze.c:317: if `write_dot` fails, the exit is 1 even when real cycles were printed. Document that 1 wins, or keep 2.
- analyze.h:9 / report.c:40: MAX_COMBOS=200000 and cache[256] have no WHY comment.
- analyze.c:126-142: `add_edge` scans every edge for each REQ: O(events x edges x held^2). That is fine for the demo. Say so if the examiner asks about scale.
- report.c:137: "same-thread cycle" is also printed when there are several threads but too few distinct ones (the 12-thread ring). Use "not enough distinct threads".
- report.c:247: dot node ids are display names. Two locks with the same symbol name in different modules collapse into one dot node.
- report.c:56: addr2line may append " (discriminator N)" to the file:line. Strip it.
- report.c:74: nm without `-C` shows C++ names mangled.
- src/pdp:16: overwrites any existing `LD_PRELOAD`. src/pdp:7: if build/pdp is reached through a symlink, `here` is the symlink's directory.
- libpdp.c:166: `lk_str` is a second `dladdr` per event, and for heap locks it scans every link map. The cost of the `lk=` field is worth one sentence in the docs. If a line truncates (very long paths), the `lk` value can be cut mid-path. The analyzer then splits it at an unrelated '+'. That is harmless (the lookup fails) but should be noted.
