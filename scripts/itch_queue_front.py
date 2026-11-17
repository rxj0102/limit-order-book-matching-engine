"""Count executions that are not at the front of their price level's queue.

    python scripts/itch_queue_front.py FILE SYMBOL [SYMBOL ...]

Independent of the C++ engine: rebuilds each price level as an insertion
ordered dict of order references (a replace goes to the back, a partial cancel
keeps its place) and, for every E and C execution, records whether the order
executed was first in its queue. Results are split by message type, by whether
a C execution was printable, and by session phase (opening cross, closing
cross, continuous, outside regular hours). The total of NOT front rows should
equal lobkit's queue_order anomaly count for the same symbol.
"""
import collections
import struct
import sys


def phase(hours):
    if abs(hours - 9.5) < 0.0003:
        return "open cross"
    if abs(hours - 16.0) < 0.0003:
        return "close cross"
    return "outside regular hours" if hours < 9.5 or hours > 16 else "continuous"


def main(path, symbols):
    data = open(path, "rb").read()
    for sym in symbols:
        pos, loc = 0, None
        orders, queues = {}, collections.defaultdict(dict)
        stat = collections.Counter()
        while pos < len(data):
            length = int.from_bytes(data[pos:pos + 2], "big")
            m = data[pos + 2:pos + 2 + length]
            pos += 2 + length
            t = m[0:1]
            if t == b"R":
                if m[11:19].decode().rstrip() == sym:
                    loc = struct.unpack(">H", m[1:3])[0]
                continue
            if loc is None or struct.unpack(">H", m[1:3])[0] != loc:
                continue
            hours = int.from_bytes(m[5:11], "big") / 3.6e12
            if t in (b"A", b"F"):
                ref, side, shares, _, price = struct.unpack(">QcI8sI", m[11:36])
                orders[ref] = [side, price, shares]
                queues[(side, price)][ref] = None
            elif t in (b"E", b"C", b"X", b"D"):
                ref = struct.unpack(">Q", m[11:19])[0]
                o = orders[ref]
                q = queues[(o[0], o[1])]
                if t in (b"E", b"C"):
                    kind = "E" if t == b"E" else ("C printable" if m[35:36] == b"Y" else "C nonprintable")
                    front = next(iter(q)) == ref
                    stat[(kind, phase(hours), "front" if front else "NOT front")] += 1
                shares = o[2] if t == b"D" else struct.unpack(">I", m[19:23])[0]
                o[2] -= shares
                if o[2] == 0:
                    del q[ref]
                    del orders[ref]
            elif t == b"U":
                old, new, shares, price = struct.unpack(">QQII", m[11:35])
                o = orders.pop(old)
                del queues[(o[0], o[1])][old]
                orders[new] = [o[0], price, shares]
                queues[(o[0], price)][new] = None
        print(f"== {sym}")
        for k, v in sorted(stat.items()):
            print(f"  {k[0]:15s} {k[1]:22s} {k[2]:10s} {v}")
        print(f"  NOT front total {sum(v for k, v in stat.items() if k[2] == 'NOT front')}")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2:])
