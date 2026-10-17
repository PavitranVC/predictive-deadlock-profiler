# PDP Documentation Index

Recommended reading order, from the big picture to the details. Every file is plain markdown.

## 1. Overview

- [../README.md](../README.md): what PDP is, quick start, headline results, limitations.

## 2. Architecture (one diagram per stage, never overwritten)

- [arch/arch-v1.png](arch/arch-v1.png): the interceptor (Stage 1).
- [arch/arch-v2.png](arch/arch-v2.png): adds the analyzer (Stage 2).
- [arch/arch-v3.png](arch/arch-v3.png): adds the ground-truth programs and test suite (Stage 3).
- [arch/arch-v4.png](arch/arch-v4.png): adds the confirmer (Stage 4).
- [arch/arch-v5.png](arch/arch-v5.png): the final architecture, adding experiments and the demo (Stages 5 and 6). The matching `.dot` sources sit next to each image.

## 3. Stages

- [stages/stage-1.md](stages/stage-1.md): the `LD_PRELOAD` interceptor and its log format.
- [stages/stage-2.md](stages/stage-2.md): the analyzer, lock-order graph, GoodLock filters and the fix suggestion.
- [stages/stage-3.md](stages/stage-3.md): the ground-truth test programs and the suite that checks the tool.
- [stages/stage-4.md](stages/stage-4.md): the confirmer, delay injection and the wait-for watchdog.
- [stages/stage-5.md](stages/stage-5.md): the experiment harness, full results (WSL2 primary, cloud replication), overhead and Helgrind.
- [stages/stage-6.md](stages/stage-6.md): the live demo script and the two-minute viva script.

## 4. Reviews

- [reviews/stage-1.md](reviews/stage-1.md), [reviews/stage-2.md](reviews/stage-2.md), [reviews/stage-3.md](reviews/stage-3.md), [reviews/stage-4.md](reviews/stage-4.md): reviewer findings per stage.
- [reviews/stage-5-6.md](reviews/stage-5-6.md): reviewer findings on the experiment scripts and the demo.

## 5. Results

- [../results/wsl2/](../results/wsl2/): primary run (course platform): manifest, overhead, Helgrind, environment, charts.
- [../results/cloud/](../results/cloud/): replication on a cloud VM, same files.
- [../scripts/README.md](../scripts/README.md): the experiment scripts, CSV columns and threats to validity.

## 6. Viva Preparation

- [VIVA.md](VIVA.md): plain-English glossary and likely examiner questions with answers.

## 7. Process and History

- [PROCESS.md](PROCESS.md): how the project was built with AI agents, and what the reviews changed.
- [TIMELINE.md](TIMELINE.md): every commit in order, and which diagram goes with which stage.
- [CHANGELOG.md](CHANGELOG.md): one entry per stage plus later addenda.
- [STATUS.md](STATUS.md): the status board.
