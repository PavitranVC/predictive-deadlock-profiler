# Stage 2 — Analyzer (`pdp-analyze`, `pdp analyze`)

Status: built, `make && make test` passes (per the builder's status line). Not committed yet. Reviewed
against commit `a8a3cfb` (`docs/reviews/stage-2.md`); the review fixes are in the table below. Source:
`src/analyze.c` (parse, graph, cycles, filters), `src/report.c` (names, report, fix, dot), `src/analyze.h`
(limits and shared structs), `src/pdp` (the front-end script). Demo: `tests/bank.c`.

## What was built

The analyzer reads a `pdp.log` written by Stage 1 and predicts deadlocks that could happen, even if the
run that produced the log finished. It runs in five steps:

```
pdp.log -> parser -> lock-order graph -> cycle enumeration -> filters -> report / F(R) / dot
```

1. **Parser** (`read_log`, `parse_event`, `parse_held`). Reads every line, skips `#` comments (counting
   `# pdp WARN` lines), parses `seq tid OP kind addr site held=... lk=...`, and sorts the events by `seq`.
   `held=?` is kept as "unknown" and is never turned into an empty set.
2. **Lock-order graph** (`node_for`, `add_edge`, `handle_request`, `build_graph`). One node per lock
   (address plus generation). One edge `A -> B` per situation "a thread asked for B while holding A".
3. **Cycle enumeration** (`find_cycles`, `extend`, `record_cycle`). Lists every elementary cycle (no node
   repeated) up to length 8, with caps.
4. **Filters** (`classify`, `choose`, `shared_lock`, `in_held`). For each cycle, decides whether some choice
   of one concrete edge per hop is a real deadlock (GoodLock rule). Each cycle is REAL, UNDECIDED or FILTERED.
5. **Report** (`print_report`, `print_fix`, `drop_edges`, `print_order`, `write_dot`). Coloured text,
   a suggested lock order (F(R)), and optional Graphviz output. `name_locks` and `resolve_site` (in
   `report.c`) turn addresses and call sites into names and source lines.

### Edges

- An edge comes from a **REQ** event, not an ACQ. A run that hangs never logs the ACQ of the lock it is stuck
  on, but it does log the REQ. So a hung run is analysed too (`tests/logs/three_cycle.log`).
- Each edge keeps: the thread (`tid`), the **call site** (`module+0xoff`, resolved with `addr2line` when the
  binary is available), the **held set** (the locks the thread held when it asked, including the source lock
  of the edge), and a **count** (how many times this exact situation was logged).
- Identical observations (same threads, site and held set) only bump the count. The count is the frequency
  used later by F(R).
- **Trylock:** `TRY_OK` / `TRY_FAIL` add no edge, because a trylock never blocks and so cannot be a link in a
  wait cycle. A lock obtained with `TRY_OK` still appears in later held sets, because the thread really holds it.
- **Recursive re-lock:** if the requested lock is already in the thread's held set, the request cannot block
  (it is a recursive re-acquire). No edges are added for it (reviewer MUST-FIX 4). Before the fix, a re-lock
  created false edges from the other held locks.
- **DESTROY and generations:** after `pthread_mutex_destroy`, the allocator may give the same address to a new
  lock. `node_for` marks the old node dead, and the next use of that address creates a new node with `gen+1`.
  Without this, two unrelated locks would be merged and could invent a cycle. The printed name gets `#2`,
  `#3` for the later generations.
- **Lock names:** libpdp writes `lk=module+0xoff` (the lock object's own location) on every event. `name_locks`
  asks `nm -S` for a data symbol at that offset (a `static` mutex has one) and uses its name (`acct_a`).
  If there is no symbol (heap lock, or a hand-written log), the lock gets `L1`, `L2`, ... in first-seen order.

### Cycle enumeration (bounded, per start node)

The first version was a **three-colour DFS** (WHITE = not visited, GREY = on the current DFS path,
BLACK = finished). A cycle is found when an edge points to a GREY node (a "back edge"). That finds one
cycle per back edge, and it skips BLACK nodes forever. The reviewer's repro showed the cost:

- T1 holds A then takes B; later holds B then takes A. That is the cycle A<->B.
- T2 holds A and asks for C. T3 holds C and asks for B. That gives the real cycle A -> C -> B -> A
  (three distinct threads, no common lock).
- The DFS found A<->B first. Both threads of A<->B were the same thread (T1), so it was FILTERED. The
  DFS never came back to explore A -> C -> B -> A, and the report said "No deadlock potential found", exit 0.

So the DFS was replaced with **per-start-node elementary cycle enumeration**:

- For each start node `s`, search for every path that returns to `s`, using only nodes with index greater
  than `s`. Each elementary cycle has exactly one smallest node, so it is found exactly once and no
  rotation check is needed.
- It is still a depth-first search. The `onpath` flag plays the role of GREY, but it is cleared on
  backtrack (`onpath[v] = 0`), so a node can be used again on another path. There is no BLACK colour.
  This is the simple form of Johnson's algorithm without its blocked-set optimisation.

Caps (in `src/analyze.h`), each with a warning:

| Cap | Value | What happens when hit |
|---|---|---|
| `MAX_CYC_LEN` | 8 locks per cycle | longer paths are not followed; `cyc_capped` is set |
| `MAX_CYCLES` | 256 cycles | enumeration stops; `cyc_capped` is set |
| `MAX_CYC_STEPS` | 200 000 DFS visits | enumeration stops for good |
| `MAX_STEPS` | 200 000 candidate edges per cycle | that cycle is marked **UNDECIDED** |
| `MAX_NODES` / `MAX_EDGES` | 512 / 4096 | data dropped, `g.dropped` counts it |

If a cap cuts the enumeration, the report says "some cycles may be missing", and exit is 2 (see below).
A capped edge search gives UNDECIDED: the analyzer cannot say the cycle is safe, so it is reported as
a possible deadlock (reviewer MUST-FIX 3: a capped search used to print "no deadlock", exit 0).

### Filters: is the cycle a real deadlock? (GoodLock rule)

A cycle `L1 -> L2 -> ... -> Lk -> L1` can have several edges per hop (different threads or sites). `choose`
tries combinations of one edge per hop, keeping the first complete combination that is valid:

- The threads of the chosen edges must be **pairwise distinct**. One thread cannot deadlock with itself
  (that is a "same-thread cycle").
- The held sets of the chosen edges must be **pairwise disjoint**. If two threads both hold lock G, they
  cannot both be stuck in the cycle at the same time, because G serialises them. This is the iGoodlock
  condition.

The pairwise check subsumes the old "one gate lock common to all edges" rule. It also catches a gate held
by only some of the threads. Reviewer SHOULD-FIX 5: a 3-cycle where T1 and T2 share G but T3 does not was
wrongly REAL under the old rule.

Verdicts (`verdict()` in `report.c`): **REAL** (a valid combination exists), **UNDECIDED** (the edge search
hit `MAX_STEPS`), **FILTERED** (no valid combination; the first blocker is recorded as the gate lock).

### F(R): the fix suggestion

For each REAL cycle, `drop_edges` removes one pair: the **least frequent** edge of the cycle (lowest count),
because a rarely executed ordering is the cheapest to change. If a pair removed for an earlier cycle is
already on this cycle, nothing more is removed. `print_order` then runs **Kahn's topological sort** on the
remaining pairs. It repeatedly outputs a lock that no remaining lock has to come before. The output is a
suggested acquisition order, and the removed pair's target comes first (the example below: `acct_b` before
`acct_a`). Nodes left over are still inside a cycle that remains (filtered or unanalysed). They are listed as
"still cyclic" and are not given an order (reviewer item 7).

The "change the call site" advice lists every distinct site that produced the dropped pair, not just one
(reviewer item 6).

### Output and exit codes

- Groups, in this order: REAL, UNDECIDED, FILTERED. Colour is ANSI red/yellow; `--no-color` turns it off.
- Exit codes: **0** nothing found (only FILTERED cycles or none); **2** at least one REAL or UNDECIDED
  cycle; **1** usage or I/O error. If `--dot` cannot write the file, the exit is 1 even when cycles were
  printed. That is "1 wins", written in the code (`if (dot && write_dot(dot) != 0) return 1;`).
- `--dot file.dot` writes Graphviz. `pdp analyze` renders a PNG next to it when `dot` is installed.
  Dot node ids are display names.
- The front end: `pdp run [-o log] ./prog args` (runs with `LD_PRELOAD`), `pdp analyze log [--dot f] [--no-color]`,
  `pdp confirm` (Stage 4, not built).

### Example output

`pdp analyze tests/logs/ab_ba.log --no-color` (hand-written log, no binary, so sites stay raw):

```
REAL potential deadlocks:
  [1] L1 -> L2 -> L1
      L1 -> L2  tid 101  at /nonexistent/prog+0x20  holding {L1}
      L2 -> L1  tid 102  at /nonexistent/prog+0x50  holding {L2}
Suggested fix:
  drop L1 -> L2 (seen 1x): change the call site(s): ...
  acquire locks in this order: L2, L1
```
exit 2.

`tests/logs/gate.log`: `L2 -> L3 -> L2` FILTERED, gate lock `L1` held by tid 101 and tid 102, exit 0.

`tests/logs/partial_gate.log`: `L1 -> L4 -> L3 -> L1` FILTERED, gate lock `L2` held by tid 2 and tid 1, while
tid 3 holds only `L4`. The old rule would have called this REAL. exit 0.

`pdp run ./tests/bin/bank` (the two tellers run one after the other, so it never hangs), then `pdp analyze`:
REAL `acct_a -> acct_b -> acct_a`, sites `bank.c:39 (teller1)` and `bank.c:51 (teller2)`, exit 2.

`tests/logs/capped.log` (224 edges, long cycle): an UNDECIDED cycle is shown, exit 2.

## Why

A deadlock needs a cycle of threads each waiting for a lock the next holds. A single run usually does not
hang, so the analyzer predicts the cycle from the order in which each thread took its locks. The graph has
to be built from REQ events, because the blocked lock is never acquired. The filters are needed because
many cycles in a graph are not real: the same thread, or a common lock that serialises the threads.

## Design decisions

- **Elementary cycles, not one DFS.** A single search can hide a real cycle behind a filtered one. Searching
  from each start node costs more, but the caps bound it, and the report says so when a cap is hit.
- **UNDECIDED is not "no deadlock".** A capped search cannot rule a cycle out, so it is reported and exits 2.
  A false alarm is better than a missed deadlock for this tool.
- **Pairwise disjoint held sets** (GoodLock) instead of one common gate lock. It is the standard rule and
  it is exact for the situations we model.
- **Trylock adds no edge, but its lock counts as held.** This is the correct model: a trylock cannot wait,
  and a lock obtained by trylock really is held afterwards.
- **Generations for reused addresses.** Address alone is not a lock identity once memory is freed and reused.
- **`held=?` is skipped and counted.** Treating unknown as empty would invent or hide edges silently; the count
  is printed as a warning.
- **Fixed-size tables** (`analyze.h`). Simple, bounded memory, easy to explain. Overflow is counted and printed.
- **Shell safety for `addr2line` and `nm`.** Both are run through `popen`. The module name must start with `/`,
  use only `[A-Za-z0-9/._+-]`, and is single-quoted; the offset must be `0x` plus hex. Anything else falls back
  to raw text. The reviewer found no injection path.
- **Single process assumed.** After `fork` the child shares the log and the addresses, which gives false
  edges. A reused tid after a thread exits can merge two threads (so a real cycle can be filtered as
  same-thread). The header comment of `analyze.c` says so.

## Review fixes applied

Reviewer report: `docs/reviews/stage-2.md` (4 MUST-FIX, 8 SHOULD-FIX, 14 NIT in the list).

| Item | Status |
|---|---|
| M-1 DFS misses cycles that share nodes | Fixed: per-start elementary cycle enumeration (`find_cycles`, `extend`) |
| M-2 caps counted leaves only, so search could be exponential | Fixed: `steps` counts every candidate examined; `MAX_STEPS` per cycle, `MAX_CYC_STEPS` total |
| M-3 capped search reported FILTERED, exit 0 | Fixed: UNDECIDED group, exit 2, warning line |
| M-4 recursive re-lock makes false edges | Fixed: no edges added when the lock is already held |
| S-5 gate test weaker than GoodLock | Fixed: pairwise distinct threads and disjoint held sets |
| S-6 "change the call site at X" names one site | Fixed: every distinct site for the dropped pair is listed |
| S-7 leftover Kahn nodes printed as ordered | Fixed: printed as "still cyclic" |
| S-8 semaphores modelled as locks, `kind` unused | Partly: `kind` is stored, but cycles through `S` nodes are not labelled approximate |
| S-9 test gaps | Partly: logs added for trylock, FAIL, recursion, shared nodes, capped, partial gate |
| S-10 `nm` runs once per lock | Open: `lookup_symbol` runs `nm` each time (no cache by module) |
| S-11 single-process assumption undocumented | Fixed: in the `analyze.c` header |
| S-12 DESTROY logged before the real call | Open in libpdp (not re-checked here) |
| NIT: `MAX_CYCLES` check before duplicate check | Not re-checked |
| NIT: "same-thread" wording with too few threads | Fixed: "same-thread cycle or too few distinct threads" |
| NIT: `addr2line` discriminator suffix | Open: not stripped |
| NIT: `held=` junk treated as empty | Not re-checked |

## Known limits

- **Predicted, not proven.** A REAL cycle means a valid combination of edges exists in the log. The threads
  may never be scheduled that way. Stage 4 (confirmer) is meant to try it.
- **Only what the log shows.** Locks that were never taken in the run, and code paths that were not executed,
  are invisible. This is a dynamic analysis.
- **Semaphores are binary locks** (from Stage 1). A semaphore used as a signal leaves the waiter "holding" it,
  so later locks get edges from it. Those cycles may be false.
- **Not hooked** (from Stage 1): condition variables (`pthread_cond_wait` drops and retakes the mutex
  without a hook), timed locks, `fork`.
- **Caps can miss cycles.** Length above 8, more than 256 cycles, or step limits. The report warns.
- **Scale.** `add_edge` scans all edges for each REQ, so the cost is roughly events times edges times held size. Fine
  for the demo logs; not tuned for large traces.
- **Names.** `nm` finds only data symbols, so a heap lock shows as `L1`. C++ names are not demangled.
- **Line numbers.** Resolution needs debug info (`-g`) and a PIE binary. Without them the report shows the raw
  `module+0xoff`.

## Explain it in the viva

The analyzer reads the log and builds a graph. Each lock is a point. When a thread asks for lock B while it
holds lock A, we draw an arrow A to B, and we note which thread, which line of code, and which other locks
it held. We take the request, not the acquisition, because a thread that is stuck never gets the lock it
asked for, but the request is still in the log.

If the graph has a loop (A to B and then B to A, possibly through more locks), that loop is a possible
deadlock. But a loop is not automatically a real deadlock. Two checks decide that. First, the arrows must
come from different threads, because one thread cannot wait for itself. Second, the threads must not both
hold the same lock while they wait, because that lock would make them take turns and so they could never
block each other in this loop. Loops that pass both checks are REAL. Loops that fail one are FILTERED and
reported only so the examiner can see why. If we run out of our search budget before we know, the loop is
UNDECIDED and we treat it as a warning.

We search for loops from each lock in turn, and only through locks with a larger number, so each loop is
found once. We use a depth-first search. The first version used three colours: white (not seen), grey (on
the current path) and black (finished). A loop is an arrow back to a grey node. The problem is that a black
node is never visited again, so a second loop through it is missed. Our version keeps only "on the current
path" and removes the mark when it backs up, so every loop is found.

For the fix, each real loop needs one ordering broken. We drop the rarest arrow, because that is the cheapest
code to change, and then sort the remaining arrows topologically (Kahn's method: repeatedly output a lock
that nothing still has to come before). The result is a lock order that the code could follow.

Exit codes: 0 means nothing real, 2 means a real or undecided deadlock was found, 1 means the tool failed.
Colours are only for the terminal, and `--dot` writes a picture of the graph.

Limits to say out loud: the tool predicts from one run, so it can be wrong in both directions. It can report
a deadlock that never happens (a false positive) and miss one whose locks were never taken in the log (a
false negative). Stage 4 is where we try to confirm the prediction by running the program again with
delays.
