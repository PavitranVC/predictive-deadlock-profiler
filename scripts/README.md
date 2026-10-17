# Experiment scripts (Stage 5)

All scripts are bash/python3, read binaries from `tests/bin/` (override with `BIN_DIR`) and write to `results/` (override with `OUT_DIR`). Build first with `make`.

| Script | Purpose | Output |
|---|---|---|
| `run_experiment.sh` | Run each real-deadlock binary (`bank sem_sq philo_naive`, list at top of script) with **no** LD_PRELOAD and no delay injection, over scheduler x cores x load. A run that outlives `timeout 5` (exit 124) = deadlock manifested. | `results/manifest.csv`, `results/env.txt` |
| `overhead.sh` | Wall time of clean binaries (`philo_ordered gate`) with vs without `LD_PRELOAD=build/libpdp.so` | `results/overhead.csv` |
| `helgrind.sh` | `valgrind --tool=helgrind` on the 5 binaries in `BINS` (bank sem_sq philo_naive gate philo_ordered); does it print "lock order"? | `results/helgrind.csv`, `results/helgrind/*.log` |
| `plot.py` | Hang-rate charts with 95% Wilson CIs, overhead chart, summary table on stdout | `results/hangrate_<binary>.png`, `results/overhead.png` |

## Running
```
RUNS=5 OUT_DIR=$(mktemp -d) scripts/run_experiment.sh   # smoke test, NEVER into results/ (full default: RUNS=200 per cell)
N=50 scripts/overhead.sh              # ITERS="1000 10000 100000", 3 warm-ups, random pair order
scripts/helgrind.sh
python3 scripts/plot.py               # needs matplotlib (pip install --user matplotlib)
```
Full matrix = 3 binaries x 3 schedulers x 3 core sets x 2 loads x 200 runs = 10,800 runs; every hang costs 5 s, so budget hours. Other env vars: `TIMEOUT_S`, `BINS`, `LIBPDP`, `SEED`, `BLOCK_RUNS`. **All scripts refuse to overwrite an existing CSV unless `FORCE=1`.** Run order: RUNS is split into blocks of 10; each block visits every cell once in a seeded random order (SEED in `env.txt`), so drift is not confounded with a factor. `env.txt` also records CPU model, governor, git commit, `sched_rt_runtime_us`. RT configs are skipped if `sched_rt_runtime_us` is -1 (a spinning RT thread could freeze the machine).

## manifest.csv columns
`date` UTC start of the batch; `host`, `kernel` machine identity; `binary` test program; `sched` `default` (CFS/EEVDF), `fifo50` (`chrt -f 50`), `rr50` (`chrt -r 50`); `cores` `taskset -c` set (`0`, `0-1`, `0-3`); `iter` ITER passed as `<bin> race ITER` (env `ITER`, default 1000; more iterations = more chances per process to hit the bad interleaving); `load` `idle` or `loaded` (pinned to the core set: one busy bash process per core, or one stress-ng with N workers if installed); `run` index; `outcome` `hung` (rc 124, or 137 after >= TIMEOUT_S), `killed` (rc 137 sooner: external SIGKILL, not a hang), `ok` (exit 0), `error` (other non-zero exit, e.g. crash); `elapsed_ms` wall time (≈5000 for hung). Newer runs append `rc` (raw exit code), `block`, `ts` (per-run UTC); older manifests lack these and plot.py handles both.
`overhead.csv`: `date,host,seed,binary,mode(baseline|pdp),iter,pair,order,rc,outcome(ok|error),elapsed_ns,lock_ops,ns_per_lock_op`. `order` says which mode ran first in the pair (random). `lock_ops` = ACQ lines in the PDP log (pdp rows). `ns_per_lock_op` = (t_pdp - t_baseline of the same pair)/lock_ops (pdp rows, both ok). Error rows are excluded from stats. Sweeping `ITER` separates fixed startup cost (intercept) from per-op cost (slope). Timing via `scripts/runtime_ns.py` (perf_counter_ns).
`helgrind.csv`: `binary,exit_code,timed_out,lock_order_reported,lock_order_violations,error_summary`. Helgrind reports each lock pair once, so a k-cycle is not counted k times.

## Skipped configs
Core sets above `nproc` are skipped with a warning. RT policies need root/CAP_SYS_NICE; the script tries plain `chrt`, then `sudo -n`, otherwise skips `fifo50`/`rr50` with a warning (so results may contain only `default`).

## Threats to validity
- **WSL2 is a VM.** vCPUs are scheduled by Hyper-V underneath Linux, so "core 0" is not a physical core and an extra, invisible scheduler adds noise. `env.txt` records WSL yes/no.
- **Cloud container vs WSL2.** Container CPU quotas/cgroups, shared noisy neighbours and a different kernel make absolute hang rates non-comparable across machines; compare configs only within one host.
- **RT policy needs privileges.** With sudo the process runs as root; SCHED_FIFO also may be throttled by `sched_rt_runtime_us` (95%). RT results may be missing entirely.
- **Load realism.** Busy loops are pinned to the same cores as the test; real workloads block/yield differently. Load processes are default-policy, so under FIFO/RR they barely compete.
- **FIFO/RR on one core cannot hang by construction.** FIFO never preempts an equal-priority thread and the RR quantum (100 ms) exceeds a `race 1000` run, so 0/N there is expected. Under RT, `loaded` means CFS spinners that RT threads always preempt.
- **Timeout = hang.** A 5 s limit may misclassify a very slow but live run; `error` outcomes are not counted as hangs.
- **Pinning changes thread count effects:** `taskset -c 0` forces threads to interleave only by preemption, which is the point but not typical.
- Stub-free: committed scripts were smoke-tested only; real numbers come from the real test binaries.

## Demo
`scripts/demo.sh` is the live viva walkthrough (build, clean pass, hang with top/gdb, `pdp analyze` prediction, `pdp confirm`, fixed version, gate-lock filtering, results chart). It pauses at each step; `DEMO_NOPAUSE=1 scripts/demo.sh` runs straight through. Uses a temp dir removed on exit. If `ptrace_scope>0` it uses `sudo -n gdb` when available, else prints `sudo sysctl kernel.yama.ptrace_scope=0`.
