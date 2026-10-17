# Stage 5 and 6 Review: Experiment Scripts and Demo

Reviewer pass on the code only. The full results are still being generated in another session.
Files: `scripts/{README.md,run_experiment.sh,overhead.sh,helgrind.sh,plot.py,demo.sh}`, `tests/README.md`.

## What I ran (all read-only, small)
- `RUNS=2 TIMEOUT_S=1 OUT_DIR=<scratch> scripts/run_experiment.sh` took 54 s on 4 vCPUs as root. All 3 schedulers and 3 core sets ran, and no spinners or test processes were left behind afterwards. The slowest `ok` run took 20 ms and every `hung` run took 1003-1011 ms, so with this ITER the timeout separates the two outcomes cleanly.
- `N=3 scripts/overhead.sh`: baseline 4-6 ms, pdp 35-86 ms. plot.py reports "16.87x".
- `BINS="bank sem_sq gate" scripts/helgrind.sh`: bank 1, gate 1 (a false positive, which is a useful comparison), sem_sq 0 (Helgrind does not track lock order on semaphores).
- `python3 scripts/plot.py <scratch>`: works. I checked `wilson()` against reference values (0/10 -> [0, 0.2775], 5/10 -> [0.2366, 0.7634], 1/200 -> [0.0009, 0.0278]) and **it is correct**.
- `DEMO_NOPAUSE=1 scripts/demo.sh`: rc 0 in 4 s with no leftover `bank`. gdb attached only because this box runs as root with no Yama module (see the demo findings).
- Checked by hand: `timeout -k 1 1 <cmd that ignores TERM>` gives **137**, a plain timeout gives **124**, and an external `kill -9` of the child also gives **137**. A script with `trap f INT TERM` **keeps looping** after the signal.

---

## MUST-FIX

1. **run_experiment.sh:98: the INT/TERM trap does not exit, so data gets mislabelled.** `trap stop_load EXIT INT TERM` runs `stop_load` and then bash *resumes the loop*. I confirmed this with a small script. `timeout` puts the test in its own process group, so Ctrl-C reaches only the script. The effect: if you press Ctrl-C during a `loaded` cell, the spinners are killed, but every remaining run of that cell is still written to the manifest as `load=loaded` when it actually ran idle. The experiment also never stops. Fix: `trap stop_load EXIT` plus `trap 'exit 130' INT TERM` (the exit then fires the EXIT trap).

2. **run_experiment.sh:113 (and overhead.sh:22, helgrind.sh:59): results are silently overwritten.** The header is written with `>` to the default `results/manifest.csv`. One `RUNS=5` smoke test, which the README tells you to run (README:14), destroys the 10,800-run dataset that is being generated now. Fix: refuse to start if the file exists (unless `FORCE=1`), or write to `results/<timestamp>/`.

3. **overhead.sh:30,32: exit status is ignored.** A pdp run that crashes, aborts early or hits the 30 s timeout is still logged as a valid timing. A crash shows up as *lower* overhead. Record `rc` as a CSV column and have plot.py drop or flag rows where rc != 0.

4. **overhead.sh:27-35: the headline slowdown cannot be defended as an interceptor cost.** What gets timed is the whole process lifetime, including fork/exec of `timeout`, dynamic loading of libpdp, the constructor, writing the log file and the destructor. It is measured with `date +%s%N` and **integer-ms** truncation, against a 4-6 ms baseline, which is about 20% quantisation error. There is no warm-up run: the first pdp run pays for the cold page cache of libpdp.so. The pair order is always baseline then pdp. As written, "17x" mixes a fixed startup/teardown cost with the per-lock cost, and an examiner will ask which one it is. Fix: (a) keep µs (`/1000`) or use `perf stat -r` / `hyperfine`; (b) discard 1-2 warm-up runs per mode; (c) sweep ITER (for example 0, 1000, 10000, 100000) and report the slope, i.e. ns per lock op, separately from the intercept, i.e. fixed cost; (d) randomise or ABBA the order within each pair; (e) report the median and IQR, or a per-pair ratio, alongside the mean.

## SHOULD-FIX

5. **run_experiment.sh:133 and helgrind.sh:65: 124 and 137 are merged into `hung` without recording the raw rc.** With `-k`, GNU timeout returns 124 when the child dies from SIGTERM. It returns 137 only when the child ignored TERM and was then KILLed, and that KILL takes `timeout` itself down too. But 137 is *also* what you get from an external SIGKILL such as the OOM killer, `kill -9`, or the WSL VM reclaiming memory, and then the run is not a hang. The test binaries don't catch TERM, so a genuine hang should always give 124. Fix: add an `rc` column, classify 124 as `hung`, classify 137 as `hung` only if `elapsed_ms >= TIMEOUT_S*1000`, and otherwise `error`. Explain this in a comment.

6. **run_experiment.sh:114-141: order effects.** Every run of a cell is back-to-back, cells run in a fixed nested order (bin, sched, cores, load), and `loaded` always comes straight after `idle`. Over a run of several hours, any drift (WSL host activity, thermal or turbo state, Windows background work, page cache) becomes **confounded with the factor that is changing**, for example "fifo50 looks different" when the real cause is "fifo50 ran at hour 2". Recommendation: a *randomised block design*. Split the RUNS into B blocks (for example 20 blocks of 10). In each block, `shuf` the full list of (bin, sched, cores, load) cells using a recorded seed (`shuf --random-source=<(yes $SEED)`), and run 10 runs per cell, starting and stopping load per cell as now. Add `block` and a per-run UTC timestamp to the manifest. At the very least, interleave idle and loaded.

7. **run_experiment.sh:57-70: RT safety and the probe.** The current test programs block on futexes and don't spin (grep found no trylock/spin/yield loops). `timeout` is neither pinned nor RT. So a hard lockup is unlikely today. But the safety rests entirely on `sched_rt_runtime_us`, and nothing checks it. If it is `-1` (no RT throttling) and someone later adds a spinning test, cores=`0-3` on a 4-vCPU WSL2 box could freeze the VM. Fix: read `/proc/sys/kernel/sched_rt_runtime_us`, write it to env.txt, and refuse RT configs if it is `-1`, or cap with `ulimit -t`. Also `rt_ok` probes only `-f` and never `-r`, so probe both. The silent-mislabel concern is fine as it stands: if `chrt` fails at runtime the run becomes `error`, not `ok`. Add a comment saying so.

8. **run_experiment.sh:37-51: env.txt is incomplete and written before the probes.** It doesn't record which SCHEDS and CORES actually ran (after skips), whether sudo was used, `sched_rt_runtime_us`, CPU model (`lscpu`), cpufreq governor, git commit (`git rev-parse HEAD` plus a dirty flag), `gcc --version`, or the load generator kind (stress-ng vs. bash loop). Move the block after the probes, or append to it. Also, the `container` check (l.44) misses cgroup v2, where `/proc/1/cgroup` is `0::/`. Already recorded and fine: kernel, WSL, ITER, TIMEOUT, RUNS.

9. **plot.py:47-50: `error` runs count toward n but are never shown.** Crashes silently lower the hang rate. Print `err` as its own column and either exclude those runs from n or state that they are included.

10. **run_experiment.sh / plot.py: no timeout-validity check.** The README (l.34) admits that slow-but-live runs could be counted as hangs, but nothing measures it. Make plot.py print the maximum `elapsed_ms` of `ok` runs per cell next to TIMEOUT_S. The 20 ms vs. 1000 ms gap I measured is strong evidence, so show it.

11. **Interpretation the students must be ready to explain (README "Threats" should state it).** Under `fifo50`/`rr50` with `cores=0`, a hang is close to impossible *by construction*. FIFO never preempts a thread for another thread of equal priority, and the RR quantum (100 ms) is longer than a `race 1000` run, so teller 1 finishes before teller 2 runs. The 0/N result there is expected, not a measurement artefact. Also, for RT cells, `loaded` means CFS spinners that the RT threads always preempt, so the idle/loaded contrast doesn't mean the same thing as it does under `default`. README l.33 half-says this already.

12. **helgrind.sh:66: `grep -ci "lock order"` counts lines, not cycles.** Today exactly one line per report matches (`Thread #N: lock order "..." violated`). Match on `'lock order ".*" violated'` so that a future Helgrind message or a path name can't inflate the count, and also parse `ERROR SUMMARY`. Rename the column to `lock_order_violations`. Helgrind reports each lock pair once, so a 5-cycle in philo_naive is not "5". Say so in the README.

13. **demo.sh:37-42: gdb on a fresh WSL2 Ubuntu.** `kernel.yama.ptrace_scope=1` is the default there, and the demo shell is not gdb's ancestor, so the attach *will* fail and the demo prints only "skipping". That is the best visual moment of the viva. Print the actual reason and the one-line fix (`sudo sysctl kernel.yama.ptrace_scope=0`, reverts on reboot), or try `sudo -n gdb` as a fallback. Also mention gdb is not installed by default (`sudo apt install gdb`).

14. **demo.sh:32-35: there's no check that `bank slow` actually hung.** If it finishes in under 1 s (it hangs "almost always", not always), `top` and `gdb` show nothing and the step looks broken. Add `kill -0 "$BGPID" || echo "did not hang this time; re-run"`.

15. **demo.sh:28,47,52,59-61,68 (and the timeout in run_experiment.sh): Ctrl-C does not reach timed commands.** `timeout` puts its child in a separate process group, so Ctrl-C during step 4 leaves the demo waiting up to 120 s. Add `trap 'exit 130' INT` and/or use `timeout --foreground` in the demo, where you want interactive behaviour.

## NIT

16. demo.sh:57: the 5-iteration `sleep 1` loop waiting for `bank_fixed` to *appear* cannot be explained, because `make` has already finished at that point. Remove it and keep the `-x` check.
17. demo.sh:17-18: ANSI colours are printed unconditionally, so they show up as `^[[1;36m` when the output is piped or recorded. Guard with `[ -t 1 ]`.
18. demo.sh:81-82: `wslview`/`xdg-open` are left running in the background. That's harmless, but say so. On WSL, `explorer.exe "$(wslpath -w "$chart")"` is a fallback that works without the wslu package.
19. run_experiment.sh:106: each spinner is pinned to the whole set rather than one core each. CFS will spread them, but pinning spinner i to core i is easier to defend ("exactly one competitor per core").
20. run_experiment.sh:122: `sleep 0.2` also runs for idle cells, and the "steady state" comment over-claims for a bash loop. Run it only when loaded.
21. run_experiment.sh:136: `date` is the batch start, so every row has the same value. Add a per-run timestamp (see 6).
22. plot.py:27-28: `wilson(0 successes, n=0)` returns (0, 0). The callers skip n=0, but (0, 1) is the honest "no information" answer.
23. plot.py:56-66: the table prints all idle rows and then all loaded rows for each binary. Sorting by (sched, cores, load) reads better.
24. scripts/README.md:9 says Helgrind runs on "every test binary", but `BINS` lists 5 (bank_fixed, prodcons and recursive are missing). README:22 says "one pinned busy process per core", which is not true for the stress-ng path (one process, N workers).
25. overhead.sh:20: `mktemp` puts the PDP log in /tmp. Record where it lives, since tmpfs vs. ext4 vs. a 9p mount on WSL changes the cost.

## Viva-defensibility summary
The scripts are short and well commented, and the overall design is sound: no preload in the experiment, pinned load on the same cpuset, the same sudo wrapper for every policy, interleaved overhead runs, Wilson rather than Wald intervals, and an honest Threats section. The weak spots an examiner will probe are the overhead number (4), the run ordering (6), and the "why 0% under FIFO on one core" question (11).
