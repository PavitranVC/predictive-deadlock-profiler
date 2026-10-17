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
# Usage:  RUNS=5 scripts/run_experiment.sh        (smoke test)
#         scripts/run_experiment.sh               (full: 200 runs per cell)
# Env overrides: RUNS, BIN_DIR, BINS, OUT_DIR, TIMEOUT_S
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN_DIR="${BIN_DIR:-$ROOT/tests/bin}"
OUT_DIR="${OUT_DIR:-$ROOT/results}"
# Binaries whose bug is a real deadlock (edit this list when tests change).
BINS="${BINS:-bank sem_sq philo_naive}"
RUNS="${RUNS:-200}"
TIMEOUT_S="${TIMEOUT_S:-5}"

mkdir -p "$OUT_DIR"
NPROC="$(nproc)"
HOST="$(hostname)"
KERNEL="$(uname -r)"
DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# ---- environment record: needed so results can be interpreted later --------
{
  echo "date: $DATE"
  echo "uname: $(uname -a)"
  echo "nproc: $NPROC"
  # WSL2 is a Hyper-V VM: its vCPUs are scheduled by Windows underneath Linux,
  # which adds a second, invisible scheduler. Record it as a threat to validity.
  if grep -qi microsoft /proc/version 2>/dev/null; then echo "wsl: yes"; else echo "wsl: no"; fi
  if [ -f /.dockerenv ] || grep -qaE 'docker|containerd|kubepods' /proc/1/cgroup 2>/dev/null; then
    echo "container: likely"; else echo "container: no"; fi
  echo "runs_per_cell: $RUNS"
  echo "timeout_s: $TIMEOUT_S"
  echo "bins: $BINS"
  echo "stress_ng: $(command -v stress-ng || echo none)"
} > "$OUT_DIR/env.txt"

# ---- scheduler configs ------------------------------------------------------
# Real-time policies (SCHED_FIFO/SCHED_RR) need CAP_SYS_NICE (root) or an
# rtprio rlimit.  Probe once; fall back to non-interactive sudo; else skip.
SUDO=""
rt_ok() { chrt -f 50 true 2>/dev/null; }
if ! rt_ok; then
  if command -v sudo >/dev/null && sudo -n true 2>/dev/null && sudo -n chrt -f 50 true 2>/dev/null; then
    SUDO="sudo -n"
    echo "NOTE: using sudo for RT scheduling (binaries run as root)"
  fi
fi
SCHEDS=("default")
if [ -n "$SUDO" ] || rt_ok; then
  SCHEDS+=("fifo50" "rr50")
else
  echo "WARNING: real-time scheduling (chrt -f/-r 50) not permitted; skipping fifo50 and rr50." >&2
  echo "         Re-run as root or with passwordless sudo to include them." >&2
fi
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

# ---- background load --------------------------------------------------------
# One busy process per core in the cpuset, PINNED to that same cpuset: load on
# other cores would not compete with the test threads.  Cleaned up by trap so a
# Ctrl-C never leaves spinners behind.
LOAD_PIDS=()
stop_load() {
  for p in "${LOAD_PIDS[@]:-}"; do [ -n "$p" ] && kill "$p" 2>/dev/null; done
  for p in "${LOAD_PIDS[@]:-}"; do [ -n "$p" ] && wait "$p" 2>/dev/null; done
  LOAD_PIDS=()
}
trap stop_load EXIT INT TERM
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

# ---- main loop ---------------------------------------------------------------
echo "date,host,kernel,binary,sched,cores,load,run,outcome,elapsed_ms" > "$OUT_DIR/manifest.csv"
for bin in $BINS; do
  exe="$BIN_DIR/$bin"
  [ -x "$exe" ] || { echo "WARNING: $exe missing, skipping" >&2; continue; }
  for sched in "${SCHEDS[@]}"; do
    pre="$(sched_prefix "$sched")"
    for cores in "${CORES[@]}"; do
      for load in idle loaded; do
        [ "$load" = loaded ] && start_load "$cores" "$(ncores_of "$cores")"
        sleep 0.2   # let spinners reach steady state
        echo "== $bin sched=$sched cores=$cores load=$load ($RUNS runs)" >&2
        for ((r = 1; r <= RUNS; r++)); do
          t0=$(date +%s%N)
          # -k 1: escalate to SIGKILL if SIGTERM is ignored. timeout is the
          # outermost command so it still works if sudo/chrt/taskset are chained.
          timeout -k 1 "$TIMEOUT_S" $SUDO taskset -c "$cores" $pre "$exe" >/dev/null 2>&1
          rc=$?
          t1=$(date +%s%N)
          case $rc in
            0)       outcome=ok ;;
            124|137) outcome=hung ;;   # timed out => deadlock manifested
            *)       outcome=error ;;
          esac
          echo "$DATE,$HOST,$KERNEL,$bin,$sched,$cores,$load,$r,$outcome,$(( (t1 - t0) / 1000000 ))" >> "$OUT_DIR/manifest.csv"
        done
        [ "$load" = loaded ] && stop_load
      done
    done
  done
done
echo "wrote $OUT_DIR/manifest.csv and env.txt" >&2
