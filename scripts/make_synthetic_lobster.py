#!/usr/bin/env python3
"""Generates SYNTHETIC LOBSTER-format files (message + order book) for tests and
throughput runs. The data is random flow around a drifting mid price. It is not
market data, and agreement between lobkit and these files proves only that lobkit
matches this script, which was written from the same understanding of the format.
It is NOT an independent check (see docs/TESTING.md).

The book model here is deliberately separate from the C++ one: plain dicts and
sorted price lists, no shared code.

    python3 scripts/make_synthetic_lobster.py OUTPUT_PREFIX [--messages N] [--seed S] [--levels L]

writes OUTPUT_PREFIX_message_L.csv and OUTPUT_PREFIX_orderbook_L.csv.
Layout follows docs/MARKET_DATA.md. The book starts empty, so no message refers to
an order from before the first line.
"""
import argparse
import bisect
import collections
import random

DUMMY_ASK = 9999999999
DUMMY_BID = -9999999999
TICK = 100          # one cent in units of 1e-4 dollars
MID0 = 1000000      # $100.00


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prefix")
    ap.add_argument("--messages", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--levels", type=int, default=10)
    a = ap.parse_args()
    rng = random.Random(a.seed)

    orders = {}                                   # id -> [side, price, size]
    queues = {1: collections.defaultdict(collections.deque),
              -1: collections.defaultdict(collections.deque)}   # side -> price -> deque of ids
    agg = {1: collections.defaultdict(int), -1: collections.defaultdict(int)}
    prices = {1: [], -1: []}                      # sorted lists of occupied prices
    next_id = 10_000_000
    t_ns = 34_200_000_000_000
    mid = MID0

    def best(side):
        if side == 1:
            return prices[1][-1] if prices[1] else None
        return prices[-1][0] if prices[-1] else None

    def add(side, price, size, oid):
        orders[oid] = [side, price, size]
        if not queues[side][price]:
            bisect.insort(prices[side], price)
        queues[side][price].append(oid)
        agg[side][price] += size

    def reduce(oid, qty):
        side, price, size = orders[oid]
        agg[side][price] -= qty
        if qty >= size:
            del orders[oid]
            queues[side][price].remove(oid)
            if not queues[side][price]:
                del queues[side][price]
                del agg[side][price]
                prices[side].remove(price)
        else:
            orders[oid][2] -= qty

    def row():
        out = []
        for i in range(a.levels):
            if i < len(prices[-1]):
                p = prices[-1][i]
                out += [p, agg[-1][p]]
            else:
                out += [DUMMY_ASK, 0]
            if i < len(prices[1]):
                p = prices[1][-1 - i]
                out += [p, agg[1][p]]
            else:
                out += [DUMMY_BID, 0]
        return ",".join(map(str, out))

    msg_lines, row_lines = [], []
    for _ in range(a.messages):
        t_ns += rng.randint(1_000, 2_000_000)
        ts = f"{t_ns // 10**9}.{t_ns % 10**9:09d}"
        if rng.random() < 0.1:
            mid += TICK * rng.choice((-1, 1))
        r = rng.random()
        live = list(orders) if r >= 0.4 and len(orders) < 400 else None
        line = None
        if r < 0.40 or not orders:
            side = rng.choice((1, -1))
            off = TICK * (1 + int(rng.expovariate(0.35)))
            price = mid - off if side == 1 else mid + off
            # Keep the book uncrossed: LOBSTER data never shows a crossed visible book.
            if side == 1 and best(-1) is not None and price >= best(-1):
                price = best(-1) - TICK
            if side == -1 and best(1) is not None and price <= best(1):
                price = best(1) + TICK
            size = rng.randint(1, 500)
            oid = next_id
            next_id += rng.randint(1, 5)
            add(side, price, size, oid)
            line = f"{ts},1,{oid},{size},{price},{side}"
        elif r < 0.55:
            oid = rng.choice(list(orders))
            side, price, size = orders[oid]
            if size > 1:
                q = rng.randint(1, size - 1)
                reduce(oid, q)
                line = f"{ts},2,{oid},{q},{price},{side}"
            else:
                reduce(oid, size)
                line = f"{ts},3,{oid},{size},{price},{side}"
        elif r < 0.70:
            oid = rng.choice(list(orders))
            side, price, size = orders[oid]
            reduce(oid, size)
            line = f"{ts},3,{oid},{size},{price},{side}"
        elif r < 0.97:
            side = rng.choice((1, -1))
            if prices[side]:
                price = best(side)
                oid = queues[side][price][0]       # time priority: head of the best level
                size = orders[oid][2]
                q = size if rng.random() < 0.6 else rng.randint(1, size)
                reduce(oid, q)
                line = f"{ts},4,{oid},{q},{price},{side}"
        elif r < 0.995:
            side = rng.choice((1, -1))
            ref = best(side) if prices[side] else mid
            line = f"{ts},5,0,{rng.randint(1, 200)},{ref},{side}"
        else:
            line = f"{ts},7,0,0,{rng.choice((-1, 0, 1))},-1"
        if line is None:                            # nothing to execute on that side: add instead
            side = rng.choice((1, -1))
            price = mid - 2 * TICK if side == 1 else mid + 2 * TICK
            if side == 1 and best(-1) is not None and price >= best(-1):
                price = best(-1) - TICK
            if side == -1 and best(1) is not None and price <= best(1):
                price = best(1) + TICK
            oid = next_id
            next_id += 1
            add(side, price, 10, oid)
            line = f"{ts},1,{oid},10,{price},{side}"
        msg_lines.append(line)
        row_lines.append(row())

    with open(f"{a.prefix}_message_{a.levels}.csv", "w", newline="") as f:
        f.write("\n".join(msg_lines) + "\n")
    with open(f"{a.prefix}_orderbook_{a.levels}.csv", "w", newline="") as f:
        f.write("\n".join(row_lines) + "\n")


if __name__ == "__main__":
    main()
