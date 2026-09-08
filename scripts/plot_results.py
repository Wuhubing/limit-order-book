#!/usr/bin/env python3
"""Regenerate report tables + figures from raw benchmark JSONs.

Usage:
  python3 scripts/plot_results.py <matrix_dir> <out_dir>
    matrix_dir defaults to results/stageD/matrix; out_dir to results/figures.

Reads results/stageD/matrix/<variant>/<load>.batch.json (reqs_per_sec_*) and
.latency.json (percentiles.all.p50/p95/p99), writes:
  <out_dir>/throughput.csv, <out_dir>/latency.csv   (tables used in PERFORMANCE.md)
  <out_dir>/throughput.png, <out_dir>/latency_p50.png
Data source is the committed ablation matrix; no benchmark is re-run here.
"""
import csv
import json
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

VARIANT_ORDER = ["base", "a", "b", "ab"]
VARIANT_LABEL = {"base": "baseline", "a": "+ pools (A)", "b": "+ cached heights (B)", "ab": "A+B"}
LOADS = [
    "add_cancel_dense", "fill_dense", "level_churn",
    "small_low_density", "small_high_density",
    "large_low_density", "large_high_density",
]


def load_json(path):
    with open(path) as f:
        return json.load(f)


def find_rps(d):
    for k, v in d.items():
        if "reqs_per_sec" in k:
            return v
    raise KeyError("no reqs_per_sec field in %s" % d)


def main():
    matrix_dir = sys.argv[1] if len(sys.argv) > 1 else "results/stageD/matrix"
    out_dir = sys.argv[2] if len(sys.argv) > 2 else "results/figures"
    os.makedirs(out_dir, exist_ok=True)

    rps = {v: {} for v in VARIANT_ORDER}
    lat = {v: {} for v in VARIANT_ORDER}
    for v in VARIANT_ORDER:
        for w in LOADS:
            bf = os.path.join(matrix_dir, v, w + ".batch.json")
            lf = os.path.join(matrix_dir, v, w + ".latency.json")
            if not os.path.exists(bf):
                continue
            rps[v][w] = find_rps(load_json(bf))
            if os.path.exists(lf):
                d = load_json(lf)
                p = d.get("percentiles", {}).get("all", {})
                lat[v][w] = p

    # CSV: throughput
    with open(os.path.join(out_dir, "throughput.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["load"] + VARIANT_ORDER + ["ab_over_base"])
        for w in LOADS:
            if w not in rps["base"]:
                continue
            wr.writerow([w] + [round(rps[v][w]) for v in VARIANT_ORDER] +
                        [round(rps["ab"][w] / rps["base"][w], 2)])
    # CSV: latency p50
    with open(os.path.join(out_dir, "latency.csv"), "w", newline="") as f:
        wr = csv.writer(f)
        wr.writerow(["load"] + VARIANT_ORDER)
        for w in LOADS:
            if w in lat["base"] and lat["base"][w]:
                wr.writerow([w] + [lat[v][w].get("p50") for v in VARIANT_ORDER])

    # Figure 1: throughput, log scale, grouped bars
    fig, ax = plt.subplots(figsize=(11, 5.2))
    x = range(len(LOADS))
    width = 0.2
    for i, v in enumerate(VARIANT_ORDER):
        vals = [rps[v].get(w, 0) for w in LOADS]
        ax.bar([xi + (i - 1.5) * width for xi in x], vals, width,
               label=VARIANT_LABEL[v])
    ax.set_yscale("log")
    ax.set_ylabel("requests/sec (all ops, log scale)")
    ax.set_xticks(list(x))
    ax.set_xticklabels(LOADS, rotation=20, ha="right", fontsize=8)
    ax.legend(title="variant")
    ax.grid(axis="y", which="both", alpha=0.3)
    ax.set_title("Throughput by workload and variant (M4 Pro, Release -O2, 7 rounds)")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "throughput.png"), dpi=150)

    # Figure 2: p50 latency, selected loads
    sel = [w for w in LOADS if w in lat["base"] and lat["base"][w]]
    fig, ax = plt.subplots(figsize=(10, 4.6))
    width = 0.2
    for i, v in enumerate(VARIANT_ORDER):
        vals = [lat[v].get(w, {}).get("p50", 0) for w in sel]
        ax.bar([xi + (i - 1.5) * width for xi in range(len(sel))], vals, width,
               label=VARIANT_LABEL[v])
    ax.set_yscale("log")
    ax.set_ylabel("p50 service latency (ns, log scale)")
    ax.set_xticks(range(len(sel)))
    ax.set_xticklabels(sel, rotation=20, ha="right", fontsize=8)
    ax.legend(title="variant")
    ax.grid(axis="y", which="both", alpha=0.3)
    ax.set_title("Per-request p50 latency by workload and variant")
    fig.tight_layout()
    fig.savefig(os.path.join(out_dir, "latency_p50.png"), dpi=150)

    print("wrote tables+figures to", out_dir)
    for f in sorted(os.listdir(out_dir)):
        print(" ", f)


if __name__ == "__main__":
    main()
