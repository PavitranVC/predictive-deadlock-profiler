#!/usr/bin/env bash
# run_experiment.sh -- Stage 5 core experiment.
#
# QUESTION: how often does a *real* deadlock bug actually manifest (hang) as a
# function of Linux scheduler policy, number of cores and background load?
#
# WHY no LD_PRELOAD / no delay injection: we want the *natural* manifestation
# rate of the bug.  PDP's confirmer deliberately forces the bad interleaving;
# here we want the scheduler alone to decide.
#
# WHY `timeout`: a deadlocked process never exits.  If it is still alive after
# TIMEOUT_S seconds we call it "hung" (exit code 124 from timeout(1)).
#
# DESIGN (randomised blocks): RUNS is split into blocks of BLOCK_RUNS (10).  In
# every block ALL cells (binary x sched x cores x load) are visited once, in a
# random order (shuf, seeded with SEED), BLOCK_RUNS runs per visit.  So slow
# drift over hours (host noise, thermal) is spread over all cells instead of
# being confounded with whichever factor happens to run at that hour.
#
# Usage:  RUNS=5 OUT_DIR=$(mktemp -d) scripts/run_experiment.sh   (smoke test)
#         scripts/run_experiment.sh               (full: 200 runs per cell)
# Env overrides: RUNS, ITER, BIN_DIR, BINS, OUT_DIR, TIMEOUT_S, SEED, BLOCK_RUNS,
#                FORCE=1 (overwrite an existing manifest.csv)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="${BIN_DIR:-$ROOT/tests/bin}"
OUT_DIR="${OUT_DIR:-$ROOT/results}"
# Binaries whose bug is a real deadlock (edit this list when tests change).
BINS="${BINS:-bank sem_sq philo_naive}"
RUNS="${RUNS:-200}"
TIMEOUT_S="${TIMEOUT_S:-5}"
BLOCK_RUNS="${BLOCK_RUNS:-10}"
SEED="${SEED:-$RANDOM}"
# Test programs take a mode argument: `race ITER` = concurrent, no artificial delay,
# ITER iterations (natural manifestation). `slow` is for the confirmer, never used here.
ITER="${ITER:-1000}"

mkdir -p "$OUT_DIR"
MANIFEST="$OUT_DIR/manifest.csv"
# Never silently destroy earlier data (a smoke test must not clobber a full run).
if [ -e "$MANIFEST" ] && [ "${FORCE:-0}" != 1 ]; then
  echo "ERROR: $MANIFEST exists; refusing to overwrite (set FORCE=1 or use another OUT_DIR)" >&2
  exit 1
fi
NPROC="$(nproc)"
HOST="$(hostname)"
KERNEL="$(uname -r)"
DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# ---- background load: cleanup first so any later exit path is safe ----------
# One busy process per core in the cpuset, PINNED to that same cpuset: load on
# other cores would not compete with the test threads.
LOAD_PIDS=()
stop_load() {
  for p in "${LOAD_PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
  for p in "${LOAD_PIDS[@]:-}"; do [ -n "$p" ] && wait "$p" 2>/dev/null; done
  LOAD_PIDS=()
}
# EXIT always stops the load.  INT/TERM must also END the script: a plain
# `trap stop_load INT TERM` would return to the loop, which would then keep
# writing rows labelled load=loaded although the spinners are dead.
# `exit 130` fires the EXIT trap, and no further rows are written.
trap stop_load EXIT
trap 'echo "interrupted; stopping" >&2; exit 130' INT TERM

# ---- scheduler configs ------------------------------------------------------
# Real-time policies (SCHED_FIFO/SCHED_RR) need CAP_SYS_NICE (root) or an
# rtprio rlimit.  Probe both -f and -r; fall back to non-interactive sudo.
# SAFETY: a SCHED_FIFO/RR busy thread is never preempted by CFS tasks.  The only
# thing keeping the machine responsive is RT throttling (sched_rt_runtime_us,
# default 950000 of 1000000 us).  If it is -1 (unlimited) a spinning RT thread
# on every core could freeze the whole VM, so we then refuse to run RT configs.
# (If chrt fails at run time the run exits non-zero and is labelled `error`,
# never `ok`, so RT mislabelling is not possible.)
RT_RUNTIME="$(cat /proc/sys/kernel/sched_rt_runtime_us 2>/dev/null || echo NA)"
SUDO=""
rt_ok() { chrt -f 50 true 2>/dev/null && chrt -r 50 true 2>/dev/null; }
RT_USABLE=no
if [ "$RT_RUNTIME" = "-1" ]; then
  echo "WARNING: sched_rt_runtime_us=-1 (no RT throttling); skipping fifo50/rr50 for safety." >&2
elif rt_ok; then
  RT_USABLE=yes
elif command -v sudo >/dev/null && sudo -n true 2>/dev/null && sudo -n chrt -f 50 true 2>/dev/null \
     && sudo -n chrt -r 50 true 2>/dev/null; then
  SUDO="sudo -n"; RT_USABLE=yes
  echo "NOTE: using sudo for RT scheduling (binaries run as root)"
else
  echo "WARNING: real-time scheduling (chrt -f/-r 50) not permitted; skipping fifo50 and rr50." >&2
  echo "         Re-run as root or with passwordless sudo to include them." >&2
fi
SCHEDS=("default")
[ "$RT_USABLE" = yes ] && SCHEDS+=("fifo50" "rr50")
sched_prefix() {   # echo the command prefix implementing a named policy
  case "$1" in
    default) echo "" ;;
    fifo50)  echo "chrt -f 50" ;;
    rr50)    echo "chrt -r 50" ;;
  esac
}

# ---- core configs: skip any that need cores we do not have -----------------
CORES=()
for c in 0 0-1 0-3; do
  hi="${c##*-}"
  if [ "$hi" -lt "$NPROC" ]; then CORES+=("$c"); else
    echo "WARNING: skipping cores=$c (machine has only $NPROC CPUs)" >&2; fi
done
ncores_of() { local c="$1"; [ "$c" = 0 ] && echo 1 || echo $(( ${c##*-} - ${c%%-*} + 1 )); }

start_load() {   # $1 = cpuset, $2 = number of spinners
  if command -v stress-ng >/dev/null; then
    taskset -c "$1" stress-ng --cpu "$2" --quiet >/dev/null 2>&1 &
    LOAD_PIDS+=($!)
  else
    local i
    for ((i = 0; i < $2; i++)); do
      taskset -c "$1" bash -c 'while :; do :; done' &
      LOAD_PIDS+=($!)
    done
  fi
}

# ---- environment record (after the probes so it shows what really ran) -----
GOV="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unavailable)"
CPUMODEL="$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')"
GITC="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
[ -n "$(git -C "$ROOT" status --porcelain 2>/dev/null)" ] && GITC="$GITC-dirty"
{
  echo "date: $DATE"
  echo "uname: $(uname -a)"
  echo "nproc: $NPROC"
  echo "cpu_model: ${CPUMODEL:-unknown}"
  echo "cpufreq_governor: $GOV"
  # WSL2 is a Hyper-V VM: its vCPUs are scheduled by Windows underneath Linux,
  # which adds a second, invisible scheduler. Record it as a threat to validity.
  if grep -qi microsoft /proc/version 2>/dev/null; then echo "wsl: yes"; else echo "wsl: no"; fi
  if [ -f /.dockerenv ] || grep -qaE 'docker|containerd|kubepods' /proc/1/cgroup 2>/dev/null; then
    echo "container: likely"; else echo "container: no"; fi
  echo "git_commit: $GITC"
  echo "runs_per_cell: $RUNS"
  echo "block_runs: $BLOCK_RUNS"
  echo "seed: $SEED"
  echo "timeout_s: $TIMEOUT_S"
  echo "iter: $ITER"
  echo "bins: $BINS"
  echo "scheds: ${SCHEDS[*]}"
  echo "cores: ${CORES[*]}"
  echo "sched_rt_runtime_us: $RT_RUNTIME"
  echo "sudo_for_rt: ${SUDO:-no}"
  echo "stress_ng: $(command -v stress-ng || echo none)"
} > "$OUT_DIR/env.txt"

# ---- build the cell list ------------------------------------------------------
CELLS=()
for bin in $BINS; do
  [ -x "$BIN_DIR/$bin" ] || { echo "WARNING: $BIN_DIR/$bin missing, skipping" >&2; continue; }
  for sched in "${SCHEDS[@]}"; do for cores in "${CORES[@]}"; do for load in idle loaded; do
    CELLS+=("$bin|$sched|$cores|$load")
  done; done; done
done

# ---- main loop ---------------------------------------------------------------
# New columns (appended, so old readers still work): rc, block, ts (per-run UTC).
echo "date,host,kernel,binary,sched,cores,load,iter,run,outcome,elapsed_ms,rc,block,ts" > "$MANIFEST"
NBLOCKS=$(( (RUNS + BLOCK_RUNS - 1) / BLOCK_RUNS ))
for ((blk = 1; blk <= NBLOCKS; blk++)); do
  first=$(( (blk - 1) * BLOCK_RUNS + 1 )); last=$(( blk * BLOCK_RUNS )); [ $last -gt $RUNS ] && last=$RUNS
  # Deterministic shuffle: seed (SEED, block) -> repeatable cell order.
  mapfile -t ORDER < <(printf '%s\n' "${CELLS[@]}" | shuf --random-source=<(yes "$SEED-$blk"))
  for cell in "${ORDER[@]}"; do
    IFS='|' read -r bin sched cores load <<< "$cell"
    exe="$BIN_DIR/$bin"; pre="$(sched_prefix "$sched")"
    echo "== block $blk/$NBLOCKS $bin sched=$sched cores=$cores load=$load (runs $first-$last)" >&2
    if [ "$load" = loaded ]; then start_load "$cores" "$(ncores_of "$cores")"; sleep 0.2; fi
    for ((r = first; r <= last; r++)); do
      t0=$(date +%s%N)
      # -k 1: escalate to SIGKILL if SIGTERM is ignored. timeout is the
      # outermost command so it still works if sudo/chrt/taskset are chained.
      timeout -k 1 "$TIMEOUT_S" $SUDO taskset -c "$cores" $pre "$exe" race "$ITER" >/dev/null 2>&1
      rc=$?
      t1=$(date +%s%N)
      ms=$(( (t1 - t0) / 1000000 ))
      # 124 = timeout expired (child died of TERM) => deadlock manifested.
      # 137 = SIGKILL: either our `-k 1` escalation (child ignored TERM; then
      # elapsed >= TIMEOUT_S) or an EXTERNAL kill (OOM killer, kill -9) which is
      # not a hang.  Label the first "hung", the second "killed".
      case $rc in
        0)   outcome=ok ;;
        124) outcome=hung ;;
        137) if [ $ms -ge $(( TIMEOUT_S * 1000 )) ]; then outcome=hung; else outcome=killed; fi ;;
        *)   outcome=error ;;
      esac
      echo "$DATE,$HOST,$KERNEL,$bin,$sched,$cores,$load,$ITER,$r,$outcome,$ms,$rc,$blk,$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$MANIFEST"
    done
    [ "$load" = loaded ] && stop_load
  done
done
echo "wrote $MANIFEST and env.txt" >&2
