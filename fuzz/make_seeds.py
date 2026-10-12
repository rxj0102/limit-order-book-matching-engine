#!/usr/bin/env python3
"""Writes a small deterministic seed corpus for fuzz_engine.

The encoding mirrors fuzz/engine_input.hpp. Seeds are hand shaped so that the
fuzzer starts from streams that already cross the book, trade, cancel, modify and
exercise feed executions against own orders, instead of rediscovering that from
random bytes. Run: python3 fuzz/make_seeds.py fuzz/corpus/engine
"""
import os
import random
import sys

KIND = dict(limit=0, market=1, ioc=2, fok=3, post=4, cancel=5, modify=6,
            fadd=7, fred=8, fdel=9, fexec=10)


def rec(kind, side=0, owner=0, tick=0, qty=0, oid=0):
    return bytes([KIND[kind], side, owner, tick & 255, (tick >> 8) & 255, qty,
                  oid & 255, (oid >> 8) & 3])


def stream(grid, recs):
    return bytes([grid]) + b"".join(recs)


def main(out):
    os.makedirs(out, exist_ok=True)
    seeds = {}
    # Own order behind a feed order, then feed executes reaching it.
    seeds["own_queue"] = stream(1, [
        rec("fadd", 1, 0, 40, 10, 1), rec("limit", 1, 1, 40, 5, 2), rec("fadd", 1, 0, 40, 8, 3),
        rec("fexec", 0, 0, 0, 4, 1), rec("fexec", 0, 0, 0, 6, 1), rec("fexec", 0, 0, 0, 3, 3),
        rec("cancel", 0, 0, 0, 0, 2)])
    seeds["cross_sweep"] = stream(0, [
        rec("limit", 1, 1, 10, 5, 1), rec("limit", 1, 2, 11, 5, 2), rec("limit", 1, 3, 12, 5, 3),
        rec("limit", 0, 1, 12, 12, 4), rec("ioc", 0, 2, 5, 9, 5), rec("fok", 1, 3, 3, 20, 6),
        rec("market", 0, 1, 0, 50, 7), rec("post", 1, 1, 30, 9, 8), rec("modify", 0, 0, 31, 4, 8)])
    seeds["pool_pressure"] = stream(0, [rec("limit", i & 1, i & 3, 20 + (i % 9), 3, i) for i in range(60)])
    rng = random.Random(12345)
    for n in range(4):
        grid = rng.randrange(4)
        recs = []
        for _ in range(300):
            recs.append(bytes(rng.randrange(256) for _ in range(8)))
        seeds[f"random_{n}"] = bytes([grid]) + b"".join(recs)
    for name, data in seeds.items():
        with open(os.path.join(out, name), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "fuzz/corpus/engine")
