# Stage 6 — Live demo (`scripts/demo.sh`) and the viva script

Status: built and smoke-tested (`DEMO_NOPAUSE=1 scripts/demo.sh` exits 0 in about 4 s, no leftover `bank` process).
Review: `docs/reviews/stage-5-6.md`, items 13–18. Fixes applied: ptrace reason and fix printed, check that `bank slow`
actually hung, INT/TERM trap, colours only on a terminal. Source: `scripts/demo.sh`, `scripts/README.md` (Demo section).

## What it is for

The demo is the live part of the viva. It shows, in about five minutes, the whole argument of the project: a clean
test run passes; the same program hangs under a different schedule; PDP predicts the hang from the clean run alone;
PDP confirms the hang really happens; the fix removes the prediction; a gate lock is correctly filtered; and the
measured hang rates are on a chart. Each step is one command, so the examiner can see the tool's real output rather
than a slide.

## Walkthrough, step by step

Run it with `scripts/demo.sh`. It pauses before each step (press Enter). `DEMO_NOPAUSE=1 scripts/demo.sh` runs straight
through, which is useful for checking the demo before the viva. It works in a temp directory that is removed on exit.
Ctrl-C ends it at any point.

**Step 0 — Build.** Command: `make -s`.
Say: "Everything is built from source with plain `make`. Nothing is pre-built or hidden."

**Step 1 — Testing says it is fine.** Command: `timeout 10 tests/bin/bank seq; echo "exit status: $?"`.
Say: "This is the sequential run. The two bank tellers take their locks one after the other, so nothing overlaps and
the program exits 0. A normal test suite would pass this program. That is the problem the project is about."

**Step 2 — But a slower schedule hangs.** Command: `tests/bin/bank slow &`, then `top -b -n1 -H -p` for its threads, then
`gdb -batch -p ... -ex 'thread apply all bt'` (filtered to the lock calls).
Say: "Same program, different timing. Look at the threads: they are asleep at 0% CPU. They are not spinning; they are
blocked in `pthread_mutex_lock`, each waiting for the lock the other one holds. The stack traces show it." If the
program happened to finish, the demo says "did not hang this time; re-run the demo". Hang rate is not 100%, so this
can happen; say so if it does.

**Step 3 — PDP predicts from the clean run.** Commands: `build/pdp run -o bank.log tests/bin/bank seq`, then
`build/pdp analyze bank.log`.
Say: "PDP ran the *clean* run with its interceptor, `LD_PRELOAD`, and recorded every lock and unlock. The analyzer
builds a lock-order graph: an arrow from A to B whenever a thread takes B while holding A. Here it finds A to B and B to
A. Both orders happened in the clean run, which never hung. The report also gives the fix: a lock order that removes the
cycle, which is the F(R) suggestion." Point at the cycle and the call sites.

**Step 4 — Confirm the prediction.** Command: `build/pdp confirm --log bank.log --baseline -n 10 ./tests/bin/bank race 1`.
Say: "The analyzer is only a prediction. The confirmer runs the program 10 times with a small delay at exactly the call
sites on the cycle, and a watchdog thread looks at the live state every 50 ms. It reports a deadlock only when a real
wait-for cycle is present in three checks in a row, and then it ends the process with exit code 42. The `--baseline`
runs are the control: the same 10 runs with no delay." Expected: CONFIRMED, with a deadlocked count (Stage 4 measured
10 of 10 with the delay and 0 of 10 at baseline).

**Step 5 — The fixed version is clean.** Commands: `pdp run` and `pdp analyze` on `tests/bin/bank_fixed`, then
`bank_fixed slow` with `timeout 10`, which should exit 0.
Say: "bank_fixed takes the locks in the same global order, so there is no cycle to predict. The same slow schedule
that hung before now finishes. Exit 0, not 124."

**Step 6 — The gate lock is not a false alarm.** Commands: `pdp run` and `pdp analyze` on `tests/bin/gate`.
Say: "This program has the same two-lock orders, but both threads take them while holding a third lock, the gate.
Two threads can never be inside the pair at the same time, so there is no deadlock. PDP sees the cycle in the graph,
then its filter looks at the held sets, sees the common gate, and marks the cycle FILTERED. Helgrind reports this same
case as a violation; that is the false positive in the Helgrind comparison."

**Step 7 — Experiment results.** Command: opens the first hang-rate chart found in `results/` (`hangrate_*.png`), with
`wslview`, `xdg-open` or `explorer.exe` if available, otherwise prints the file path.
Say: "This is the 10,800-run experiment. The hang rate depends on the scheduler, the cores and the load, and each bar
has a 95% Wilson interval. The headline: one core never hangs; more cores hang more; load hides the bug under the
default scheduler and exposes it under real-time scheduling. The numbers are from a cloud VM, and the WSL2 rerun is
next."

## Fallbacks (what to do when something goes wrong)

| symptom | cause | what to say or do |
|---|---|---|
| Step 2 prints "(ptrace_scope=1 blocks gdb attach. Fix: sudo sysctl kernel.yama.ptrace_scope=0 ...)" | Yama `ptrace_scope` is 1 by default on a fresh Ubuntu / WSL2, so a process cannot attach to a non-child. The demo shell is not gdb's ancestor. | This is a good moment: say what Yama is and why the default exists. Then run `sudo sysctl kernel.yama.ptrace_scope=0` (reverts on reboot) and re-run. If passwordless sudo is set up, the demo uses `sudo -n gdb` on its own. |
| "(gdb not installed: sudo apt install gdb)" | gdb is not in a default install. | Install it before the viva. Without it, `top` alone still shows the threads at 0% CPU. |
| "(bank slow did not hang this time; re-run the demo)" | The hang is not certain on every run (Stage 5: 64% to 94% on 2 to 4 cores). | Re-run. Or say the number: "it hangs about 9 times in 10 on this machine". |
| Step 5 "(tests/bin/bank_fixed not built yet; skipping)" | Only if the build was broken. | Run `make -s` again and check the build log. |
| No colours, `^[[1;36m` codes | Output piped or recorded. | Colours are printed only on a terminal (`[ -t 1 ]`). Not a bug. |
| Step 7 says "No charts" or prints a path | No `results/hangrate_*.png` (the experiment was not run here), or no GUI opener on this machine. | Open the PNG from the file browser. The charts are also in `docs/stages/stage-5.md`. |
| Ctrl-C in the middle of step 4 | `timeout` runs its child in its own process group, so Ctrl-C does not reach it directly. | The demo traps INT/TERM and exits 130, and `timeout --foreground` is used for the confirm step. The temp directory is removed on exit. |
| Step 4 takes time | Ten runs, each up to 1 s + watchdog, and the delay adds 100 ms per site. | Under 120 s. Use `DEMO_NOPAUSE=1` in advance to check timing. |

## Two-minute viva script

Use this if the examiner asks "show me" or "explain it briefly". Each sentence is one breath.

"A deadlock needs two threads to take two locks in opposite orders. A normal test passes, because it takes the locks one
after the other. PDP interposes on `pthread_mutex_lock` and `sem_wait` with `LD_PRELOAD`, so the unmodified program
logs every lock it takes, and it does this on a run that never hangs. The analyzer builds a lock-order graph from that
log. A cycle in the graph is a possible deadlock, even though the run was fine. That is a prediction. To check it, the
confirmer adds a small delay at the call sites on the cycle, and a watchdog reads the live wait-for graph; a cycle
present in three checks in a row is a real deadlock, so the program is ended with exit 42. We counted only runs with
both the exit code and the log line. A gate lock on both orders is filtered, so we do not raise a false alarm, and
Helgrind does raise one on that case. For the scheduling question we ran the real programs 10,800 times without PDP:
no hangs on one core, many on several, and the load does opposite things under the default scheduler and under
real-time scheduling. We have a hypothesis for why, and we say it is not proven. The cost is 7 to 11 microseconds per
lock, which is large for a microbenchmark and is future work to reduce with per-thread buffers. The limits are that
static and setuid binaries ignore `LD_PRELOAD`, and that the experiment ran on a cloud VM, not WSL2."

## Explain it in the viva

The demo is the same argument as the report, in a fixed order: the bug is invisible to tests, PDP predicts it from a
run that did not hang, a separate check confirms it, the fix removes it, a known false positive is handled, and the
measured data is on the chart. If something fails live, the fallback table says which failure is expected and what to
say about it. A failed gdb attach is itself a good answer to "what is ptrace?", and it is a lesson about the default
security settings of WSL2.
