#!/usr/bin/env python3
"""Summarize Google Benchmark JSON from bench_book: median and range over repetitions.

    bench/summarize.py run.json                       # table per scenario, engine and baseline
    bench/summarize.py before.json after.json         # before and after for the engine rows

Metrics: throughput in million operations per second (batch timing) and per operation latency
percentiles in nanoseconds (timer read around each operation, includes the timer overhead, see
docs/PERFORMANCE.md). Every cell is `median [min, max]` over the repetitions. There is no single
run figure. A p99.9 or max column whose max is more than 3x its median is flagged with `!` as VM
tail noise (shared host, no isolated cores).
"""
import json
import statistics
import sys
from collections import defaultdict

METRICS = [("items_per_second", "Mops/s", 1e-6), ("p50_ns", "p50", 1), ("p90_ns", "p90", 1),
           ("p99_ns", "p99", 1), ("p999_ns", "p99.9", 1), ("max_ns", "max", 1)]
ORDER = ["add", "cancel", "modify_reduce", "modify_reprice", "match_1", "match_4", "match_16", "match_64", "mixed",
         "feed_replay"]


def load(path):
    data = json.load(open(path))
    runs = defaultdict(lambda: defaultdict(list))
    for b in data["benchmarks"]:
        if b.get("run_type") != "iteration":
            continue
        name = b["name"].split("/iterations")[0]
        for key, _, _ in METRICS:
            runs[name][key].append(b[key])
        runs[name]["faults"].append(b["faults_thr"] + b["faults_lat"])
        runs[name]["rejects"].append(b["rejects"])
    return runs, data.get("context", {})


def cell(vals, scale, flag=False):
    v = [x * scale for x in vals]
    med = statistics.median(v)
    s = f"{med:.1f} [{min(v):.1f}, {max(v):.1f}]" if med < 1000 else f"{med:.0f} [{min(v):.0f}, {max(v):.0f}]"
    if flag and med > 0 and max(v) > 3 * med:
        s += " !"
    return s


def med(vals, scale=1):
    return statistics.median(vals) * scale


def single(path):
    runs, ctx = load(path)
    reps = len(next(iter(runs.values()))["items_per_second"])
    print(f"Repetitions per row: {reps}. Cells are median [min, max]. `!` marks max > 3x median (VM tail noise).\n")
    print("| scenario | implementation | " + " | ".join(m[1] for m in METRICS) + " | page faults |")
    print("|---|---|" + "---|" * (len(METRICS) + 1))
    for sc in ORDER:
        for impl in ("engine", "stdmap"):
            r = runs.get(f"{impl}/{sc}")
            if not r:
                continue
            cells = [cell(r[k], s, flag=k in ("p999_ns", "max_ns")) for k, _, s in METRICS]
            print(f"| {sc} | {impl} | " + " | ".join(cells) + f" | {int(max(r['faults']))} |")
    print("\nThroughput ratio (engine median / stdmap median) and p50, p99 ratios (stdmap / engine):\n")
    print("| scenario | throughput x | p50 x | p99 x |")
    print("|---|---|---|---|")
    for sc in ORDER:
        e, s = runs.get(f"engine/{sc}"), runs.get(f"stdmap/{sc}")
        if e and s:
            print(f"| {sc} | {med(e['items_per_second'])/med(s['items_per_second']):.2f} | "
                  f"{med(s['p50_ns'])/med(e['p50_ns']):.2f} | {med(s['p99_ns'])/med(e['p99_ns']):.2f} |")


def compare(a, b):
    ra, _ = load(a)
    rb, _ = load(b)
    print("| scenario | metric | before | after | change |")
    print("|---|---|---|---|---|")
    for sc in ORDER:
        x, y = ra.get(f"engine/{sc}"), rb.get(f"engine/{sc}")
        if not (x and y):
            continue
        for k, label, s in METRICS[:4]:
            mx, my = med(x[k], s), med(y[k], s)
            ch = (my / mx - 1) * 100
            print(f"| {sc} | {label} | {cell(x[k], s)} | {cell(y[k], s)} | {ch:+.1f}% |")
            if k == "items_per_second":
                bx, by = max(x[k]) * s, max(y[k]) * s
                print(f"| {sc} | Mops/s best of N | {bx:.1f} | {by:.1f} | {(by / bx - 1) * 100:+.1f}% |")


if __name__ == "__main__":
    if len(sys.argv) == 2:
        single(sys.argv[1])
    elif len(sys.argv) == 3:
        compare(*sys.argv[1:])
    else:
        sys.exit(__doc__)
