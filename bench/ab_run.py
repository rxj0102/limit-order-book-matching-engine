#!/usr/bin/env python3
"""Run several bench_book binaries in alternating rounds so that slow drift on a shared VM hits
all of them alike, and write one JSON file per label (repetitions pooled over rounds).

    bench/ab_run.py --out DIR --rounds 3 --reps 5 [--filter engine/] base=/tmp/bin/base opt=/tmp/bin/opt

Each round runs every binary once with --benchmark_repetitions=REPS and random interleaving
inside the binary. Summarize with: bench/summarize.py DIR/base.json DIR/opt.json
"""
import argparse
import json
import os
import subprocess
import sys

ap = argparse.ArgumentParser()
ap.add_argument("--out", required=True)
ap.add_argument("--rounds", type=int, default=3)
ap.add_argument("--reps", type=int, default=5)
ap.add_argument("--filter", default="engine/")
ap.add_argument("variants", nargs="+", help="label=path")
a = ap.parse_args()
os.makedirs(a.out, exist_ok=True)
pooled = {}
for r in range(a.rounds):
    order = a.variants if r % 2 == 0 else a.variants[::-1]  # alternate who goes first
    for v in order:
        label, path = v.split("=", 1)
        tmp = os.path.join(a.out, f".{label}.{r}.json")
        subprocess.run([path, "--no-equivalence-check", f"--benchmark_filter={a.filter}",
                        f"--benchmark_repetitions={a.reps}", "--benchmark_enable_random_interleaving=true",
                        f"--benchmark_out={tmp}", "--benchmark_out_format=json"],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        d = json.load(open(tmp))
        os.remove(tmp)
        if label not in pooled:
            pooled[label] = d
        else:
            pooled[label]["benchmarks"].extend(d["benchmarks"])
for label, d in pooled.items():
    json.dump(d, open(os.path.join(a.out, f"{label}.json"), "w"))
print("wrote", ", ".join(pooled), "to", a.out, f"({a.rounds} rounds x {a.reps} reps)")
