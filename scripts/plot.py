#!/usr/bin/env python3
"""plot.py -- turn results/manifest.csv (+ overhead.csv) into charts and a table.

Hang rate is a proportion k/n, so error bars are 95% Wilson score intervals
rather than the normal approximation: Wilson stays inside [0,1] and behaves
sensibly at k=0 or k=n, which is exactly where our data sits (a bug that
*always* or *never* manifests).
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
        return (0.0, 0.0)
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
    for r in rows:
        c = counts[r["binary"]][(r["sched"], r["cores"])][r["load"]]
        c[1] += 1
        c[0] += r["outcome"] == "hung"
    print(f"{'binary':14}{'sched':9}{'cores':6}{'load':8}{'hung/n':>9}{'rate':>7}   95% Wilson CI")
    for b, cfgs in counts.items():
        keys = sorted(cfgs)
        fig, ax = plt.subplots(figsize=(max(6, 1.4 * len(keys)), 4))
        w = 0.38
        for i, load in enumerate(("idle", "loaded")):
            xs, ys, lo, hi = [], [], [], []
            for j, k in enumerate(keys):
                h, n = cfgs[k][load]
                if n == 0:
                    continue
                l, u = wilson(h, n)
                p = h / n
                xs.append(j + (i - 0.5) * w); ys.append(p)
                lo.append(max(0.0, p - l)); hi.append(max(0.0, u - p))  # clamp float rounding at p=0/1
                print(f"{b:14}{k[0]:9}{k[1]:6}{load:8}{f'{h}/{n}':>9}{p:7.2f}   [{l:.3f}, {u:.3f}]")
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
    t = defaultdict(lambda: defaultdict(list))
    for r in rows:
        t[r["binary"]][r["mode"]].append(float(r["elapsed_ms"]))
    if not t:
        return
    bins = sorted(t)
    fig, ax = plt.subplots(figsize=(5, 4))
    w = 0.38
    print(f"\n{'binary':16}{'baseline ms':>12}{'pdp ms':>10}{'slowdown':>10}")
    for i, mode in enumerate(("baseline", "pdp")):
        means, errs = [], []
        for b in bins:
            v = t[b][mode]
            m = sum(v) / len(v) if v else 0
            sd = (sum((x - m) ** 2 for x in v) / (len(v) - 1)) ** 0.5 if len(v) > 1 else 0
            means.append(m); errs.append(1.96 * sd / math.sqrt(len(v)) if v else 0)  # 95% CI of the mean
        ax.bar([j + (i - 0.5) * w for j in range(len(bins))], means, w, yerr=errs, capsize=3, label=mode)
    for b in bins:
        bm = sum(t[b]["baseline"]) / max(1, len(t[b]["baseline"]))
        pm = sum(t[b]["pdp"]) / max(1, len(t[b]["pdp"]))
        print(f"{b:16}{bm:12.1f}{pm:10.1f}{(pm / bm if bm else float('nan')):9.2f}x")
    ax.set_xticks(range(len(bins))); ax.set_xticklabels(bins)
    ax.set_ylabel("wall time (ms)"); ax.set_title("LD_PRELOAD overhead (mean, 95% CI)")
    ax.legend(); fig.tight_layout()
    out = os.path.join(RES, "overhead.png")
    fig.savefig(out, dpi=150); plt.close(fig)
    print("wrote", out)


if __name__ == "__main__":
    plot_manifest(read("manifest.csv"))
    plot_overhead(read("overhead.csv"))
