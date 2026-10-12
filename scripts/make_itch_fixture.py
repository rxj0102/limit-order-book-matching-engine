#!/usr/bin/env python3
"""Builds the small hand-made ITCH 5.0 fixture used by the unit tests.

SYNTHETIC. Written from the public Nasdaq TotalView-ITCH 5.0 specification, with
the BinaryFILE framing (2 byte big endian length before each message). Because the
same person wrote the decoder and this builder from the same document, decoding it
correctly shows self-consistency, not conformance to real Nasdaq files.

    python3 scripts/make_itch_fixture.py data/synthetic/itch_fixture.bin

Content, stock AAPL = locate 1, MSFT = locate 2 (the expected result is asserted in
tests/md/test_itch.cpp, derived by hand):
   S, R(AAPL), R(MSFT), H(unknown type, skipped),
   A 1001 B 100 @150.0000, A 1002 S 200 @150.0100, A(MSFT) 2001,
   F 1003 B 300 @149.9900, E 1001 x40, C 1002 x50, X 1003 -100,
   U 1001 -> 1004 80 @150.0000, P (trade) 25 @150.0050, D 1002
Final AAPL book: bid 1500000 x80 (1004), bid 1499900 x200 (1003), no asks.
"""
import struct
import sys


def frame(payload: bytes) -> bytes:
    return struct.pack(">H", len(payload)) + payload


def ts(n: int) -> bytes:
    return n.to_bytes(6, "big")


def head(t: bytes, locate: int, n: int) -> bytes:
    return t + struct.pack(">HH", locate, 0) + ts(n)


def stock(s: str) -> bytes:
    return s.encode().ljust(8, b" ")


def add(t, locate, n, ref, bs, shares, sym, price, mpid=None):
    p = head(t, locate, n) + struct.pack(">Q", ref) + bs + struct.pack(">I", shares) + stock(sym) + struct.pack(">I", price)
    if mpid is not None:
        p += mpid
    return frame(p)


def main(path):
    m = []
    m.append(frame(head(b"S", 0, 1) + b"O"))
    for loc, sym in ((1, "AAPL"), (2, "MSFT")):
        m.append(frame(head(b"R", loc, 2) + stock(sym) + b"Q" + b"N" + struct.pack(">I", 100) + b"N" + b"C" + b"  " + b"P" + b" " + b"N" + b"1" + b"N" + struct.pack(">I", 0) + b"N"))
    m.append(frame(head(b"H", 1, 3) + stock("AAPL") + b"T" + b" " + b"    "))
    m.append(add(b"A", 1, 10, 1001, b"B", 100, "AAPL", 1500000))
    m.append(add(b"A", 1, 11, 1002, b"S", 200, "AAPL", 1500100))
    m.append(add(b"A", 2, 12, 2001, b"B", 50, "MSFT", 3000000))
    m.append(add(b"F", 1, 13, 1003, b"B", 300, "AAPL", 1499900, mpid=b"GSCO"))
    m.append(frame(head(b"E", 1, 14) + struct.pack(">QIQ", 1001, 40, 1)))
    m.append(frame(head(b"C", 1, 15) + struct.pack(">QIQ", 1002, 50, 2) + b"Y" + struct.pack(">I", 1500100)))
    m.append(frame(head(b"X", 1, 16) + struct.pack(">QI", 1003, 100)))
    m.append(frame(head(b"U", 1, 17) + struct.pack(">QQII", 1001, 1004, 80, 1500000)))
    m.append(frame(head(b"P", 1, 18) + struct.pack(">Q", 0) + b"B" + struct.pack(">I", 25) + stock("AAPL") + struct.pack(">IQ", 1500050, 3)))
    m.append(frame(head(b"D", 1, 19) + struct.pack(">Q", 1002)))
    with open(path, "wb") as f:
        f.write(b"".join(m))


if __name__ == "__main__":
    main(sys.argv[1])
