#!/usr/bin/env python3
"""plot.py -- turn results/manifest.csv (+ overhead.csv) into charts and a table.

Hang rate is a proportion k/n, so error bars are 95% Wilson score intervals
rather than the normal approximation: Wilson stays inside [0,1] and behaves
sensibly at k=0 or k=n, which is exactly where our data sits (a bug that
*always* or *never* manifests).
Reads both manifest formats: old (no rc/block/ts columns) and new (rc column;
outcome may be hung/ok/error/killed).  error and killed runs are shown as their
own columns; hang rate = hung / n where n INCLUDES them (stated in the header).
overhead.csv: old (elapsed_ms, no outcome) or new (elapsed_ns, outcome, ns_per_lock_op);
rows with outcome=error are excluded from timing statistics and counted.
Usage: python3 scripts/plot.py [results_dir]
"""
import csv
import math
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")  # headless: no display in WSL/containers
import matplotlib.pyplot as plt

RES = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "results")
Z = 1.959964  # 97.5th percentile of the standard normal -> 95% two-sided


def wilson(k, n, z=Z):
    """95% Wilson score interval (lo, hi) for k successes in n trials."""
    if n == 0:
        return (0.0, 1.0)  # no information
    p = k / n
    d = 1 + z * z / n
    centre = (p + z * z / (2 * n)) / d
    half = z * math.sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d
    return (max(0.0, centre - half), min(1.0, centre + half))


def read(name):
    path = os.path.join(RES, name)
    if not os.path.exists(path):
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def plot_manifest(rows):
    # counts[binary][(sched, cores)][load] = [hung, total]
    counts = defaultdict(lambda: defaultdict(lambda: defaultdict(lambda: [0, 0])))
    extra = defaultdict(lambda: [0, 0, 0.0])   # key -> [error, killed, max ok elapsed_ms]
    for r in rows:
        c = counts[r["binary"]][(r["sched"], r["cores"])][r["load"]]
        c[1] += 1
        c[0] += r["outcome"] == "hung"
        e = extra[(r["binary"], r["sched"], r["cores"], r["load"])]
        e[0] += r["outcome"] == "error"
        e[1] += r["outcome"] == "killed"
        if r["outcome"] == "ok":
            e[2] = max(e[2], float(r["elapsed_ms"]))
    print("n includes error/killed runs; max_ok_ms = slowest successful run (should be << TIMEOUT_S*1000)")
    print(f"{'binary':14}{'sched':9}{'cores':6}{'load':8}{'hung/n':>9}{'rate':>7}{'err':>5}{'kill':>5}{'max_ok_ms':>10}   95% Wilson CI")
    for b, cfgs in counts.items():
        keys = sorted(cfgs, key=lambda k: (k[0], k[1]))
        fig, ax = plt.subplots(figsize=(max(6, 1.4 * len(keys)), 4))
        w = 0.38
        for i, load in enumerate(("idle", "loaded")):
            xs, ys, lo, hi = [], [], [], []
            for j, k in enumerate(keys):
                h, n = cfgs[k][load]
                ex = extra[(b, k[0], k[1], load)]
                if n == 0:
                    continue
                l, u = wilson(h, n)
                p = h / n
                xs.append(j + (i - 0.5) * w); ys.append(p)
                lo.append(max(0.0, p - l)); hi.append(max(0.0, u - p))  # clamp float rounding at p=0/1
                print(f"{b:14}{k[0]:9}{k[1]:6}{load:8}{f'{h}/{n}':>9}{p:7.2f}{ex[0]:5d}{ex[1]:5d}{ex[2]:10.0f}   [{l:.3f}, {u:.3f}]")
            ax.bar(xs, ys, w, yerr=[lo, hi], capsize=3, label=load)
        ax.set_xticks(range(len(keys)))
        ax.set_xticklabels([f"{s}\ncores {c}" for s, c in keys], fontsize=8)
        ax.set_ylim(0, 1.05)
        ax.set_ylabel("hang rate (deadlock manifested)")
        ax.set_title(f"{b}: hang rate by scheduler x cores (95% Wilson CI)")
        ax.legend()
        fig.tight_layout()
        out = os.path.join(RES, f"hangrate_{b}.png")
        fig.savefig(out, dpi=150)
        plt.close(fig)
        print("wrote", out)


def plot_overhead(rows):
    """Median/IQR-free simple summary: mean +- 95% CI per (binary, iter, mode)."""
    # t[binary][iter][mode] = list of ms; nerr counts excluded failed runs.
    t = defaultdict(lambda: defaultdict(lambda: defaultdict(list)))
    nerr = 0
    per_op = defaultdict(list)
    for r in rows:
        if r.get("outcome", "ok") == "error":
            nerr += 1
            continue
        ms = float(r["elapsed_ns"]) / 1e6 if "elapsed_ns" in r else float(r["elapsed_ms"])
        t[r["binary"]][r.get("iter", "")][r["mode"]].append(ms)
        if r.get("ns_per_lock_op"):
            per_op[(r["binary"], r["iter"])].append(float(r["ns_per_lock_op"]))
    if not t:
        return
    print(f"\noverhead: {nerr} failed runs excluded from timing stats")
    print(f"{'binary':16}{'iter':>8}{'baseline ms':>12}{'pdp ms':>10}{'slowdown':>10}{'ns/lock op':>12}")
    fig, ax = plt.subplots(figsize=(6, 4))
    for b in sorted(t):
        xs, ys, es = [], [], []
        for it in sorted(t[b], key=lambda x: float(x or 0)):
            mean = lambda v: sum(v) / len(v) if v else float("nan")
            bm, pm = mean(t[b][it]["baseline"]), mean(t[b][it]["pdp"])
            po = per_op.get((b, it), [])
            pom = sorted(po)[len(po) // 2] if po else float("nan")  # median
            print(f"{b:16}{it:>8}{bm:12.2f}{pm:10.2f}{pm / bm if bm else float('nan'):9.2f}x{pom:12.0f}")
            v = t[b][it]["pdp"]
            sd = (sum((x - pm) ** 2 for x in v) / (len(v) - 1)) ** 0.5 if len(v) > 1 else 0
            xs.append(float(it or 0)); ys.append(pm); es.append(1.96 * sd / math.sqrt(len(v)) if v else 0)
        ax.errorbar(xs, ys, yerr=es, marker="o", capsize=3, label=f"{b} pdp")
    ax.set_xlabel("ITER"); ax.set_ylabel("pdp wall time (ms)")
    if len(t) and any(len(t[b]) > 1 for b in t):
        ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_title("LD_PRELOAD cost vs ITER (mean, 95% CI)")
    ax.legend(); fig.tight_layout()
    out = os.path.join(RES, "overhead.png")
    fig.savefig(out, dpi=150); plt.close(fig)
    print("wrote", out)


if __name__ == "__main__":
    plot_manifest(read("manifest.csv"))
    plot_overhead(read("overhead.csv"))
