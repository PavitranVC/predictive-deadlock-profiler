#!/usr/bin/env python3
"""runtime_ns.py -- run a command once and print "<rc> <elapsed_ns>".

Used by overhead.sh because `date +%s%N` in bash costs a fork per call and is
only as good as the surrounding shell.  Here time.perf_counter_ns() brackets
just the child's lifetime (spawn -> exit).  Usage: runtime_ns.py TIMEOUT_S cmd args...
rc is 124 on timeout (child killed), otherwise the child's exit status
(negative = killed by signal, mapped to 128+sig as a shell would).
"""
import subprocess
import sys
import time

tmo = float(sys.argv[1])
cmd = sys.argv[2:]
t0 = time.perf_counter_ns()
try:
    p = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=tmo)
    rc = p.returncode if p.returncode >= 0 else 128 - p.returncode
except subprocess.TimeoutExpired:
    rc = 124
t1 = time.perf_counter_ns()
print(rc, t1 - t0)
