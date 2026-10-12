#!/usr/bin/env python3
"""Generates a large SYNTHETIC ITCH 5.0 BinaryFILE stream for throughput runs.
One stock (locate 1, AAPL) with random add/execute/cancel/delete/replace flow near
$150.00, plus a stock directory entry and some messages of another stock and of
an unknown type so the filter and skip paths are exercised. Not market data. Executions always hit the oldest order at their price level, as
on a price-time priority venue; cancels, deletes and replaces hit random orders.

    python3 scripts/make_synthetic_itch.py OUT.bin --messages 2000000 --seed 1
"""
import argparse
import collections
import random
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--messages", type=int, default=100000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--random-exec", action="store_true",
                    help="execute random orders instead of the oldest at a level: a stress case that deep queues turn into long walks")
    a = ap.parse_args()
    rng = random.Random(a.seed)
    out = bytearray()
    pack = struct.pack

    def hdr(t, loc, n):
        return t + pack(">HH", loc, 0) + n.to_bytes(6, "big")

    def fr(p):
        out.extend(pack(">H", len(p)))
        out.extend(p)

    stock = b"AAPL    "
    fr(hdr(b"S", 0, 1) + b"O")
    fr(hdr(b"R", 1, 2) + stock + b"Q" + b"N" + pack(">I", 100) + b"N" + b"C" + b"  " + b"P" + b" " + b"N" + b"1" + b"N" + pack(">I", 0) + b"N")
    live = {}            # ref -> [side, price, shares]
    keys = []            # refs, for random choice; lazily cleaned
    queues = {}          # (side, price) -> deque of refs, oldest first
    ref = 1000
    mid = 1500000
    t = 10
    for _ in range(a.messages):
        t += rng.randint(1, 5000)
        r = rng.random()
        if rng.random() < 0.02:
            fr(hdr(b"A", 2, t) + pack(">Q", rng.randint(1, 10**9)) + b"B" + pack(">I", 10) + b"MSFT    " + pack(">I", 3000000))
            continue
        if rng.random() < 0.01:
            fr(b"H" + pack(">HH", 1, 0) + t.to_bytes(6, "big") + stock + b"T" + b" " + b"    ")
            continue
        if r < 0.45 or not keys:
            ref += rng.randint(1, 3)
            side = rng.choice((b"B", b"S"))
            off = 100 * (1 + int(rng.expovariate(0.3)))
            price = mid - off if side == b"B" else mid + off
            n = rng.randint(1, 500)
            live[ref] = [side, price, n]
            keys.append(ref)
            queues.setdefault((side, price), collections.deque()).append(ref)
            fr(hdr(b"A", 1, t) + pack(">Q", ref) + side + pack(">I", n) + stock + pack(">I", price))
            continue
        while keys:
            i = rng.randrange(len(keys))
            k = keys[i]
            if k in live:
                break
            keys[i] = keys[-1]
            keys.pop()
        else:
            continue
        side, price, n = live[k]
        if r < 0.65:                    # execute the oldest order at some price level (time priority)
            dq = queues.get((side, price))
            if not a.random_exec:
                while dq and dq[0] not in live:
                    dq.popleft()
                k = dq[0]
                side, price, n = live[k]
            q = n if rng.random() < 0.5 else rng.randint(1, n)
            fr(hdr(b"E", 1, t) + pack(">QIQ", k, q, t))
            if q >= n:
                del live[k]
                if not a.random_exec:
                    dq.popleft()
            else:
                live[k][2] -= q
        elif r < 0.75 and n > 1:        # partial cancel
            q = rng.randint(1, n - 1)
            fr(hdr(b"X", 1, t) + pack(">QI", k, q))
            live[k][2] -= q
        elif r < 0.90:                  # delete
            fr(hdr(b"D", 1, t) + pack(">Q", k))
            del live[k]
        else:                           # replace
            ref += 1
            n2 = rng.randint(1, 500)
            p2 = price + 100 * rng.choice((-1, 0) if side == b"B" else (0, 1))  # never toward the mid: keeps the book uncrossed
            fr(hdr(b"U", 1, t) + pack(">QQII", k, ref, n2, p2))
            del live[k]
            live[ref] = [side, p2, n2]
            keys.append(ref)
            queues.setdefault((side, p2), collections.deque()).append(ref)
    with open(a.out, "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
