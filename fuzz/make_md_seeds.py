#!/usr/bin/env python3
"""Seed corpora for the LOBSTER and ITCH fuzz targets, built from the committed
SYNTHETIC data (data/synthetic). Run from the repository root:

    python3 -I fuzz/make_md_seeds.py fuzz/corpus
"""
import os
import random
import struct
import sys

root = sys.argv[1] if len(sys.argv) > 1 else "fuzz/corpus"
D = "data/synthetic"
msgs = open(f"{D}/SYNTH_lobster_1000_message_10.csv", "rb").read().split(b"\n")
rows = open(f"{D}/SYNTH_lobster_1000_orderbook_10.csv", "rb").read().split(b"\n")
itch = open(f"{D}/itch_fixture.bin", "rb").read()

lob = os.path.join(root, "lobster")
itc = os.path.join(root, "itch")
os.makedirs(lob, exist_ok=True)
os.makedirs(itc, exist_ok=True)


def lobster_seed(m: bytes, r: bytes) -> bytes:
    return struct.pack("<H", min(len(m), 65535)) + m + r


rng = random.Random(7)
for name, n in (("synth_40", 40), ("synth_200", 200)):
    m = b"\n".join(msgs[:n]) + b"\n"
    r = b"\n".join(rows[:n]) + b"\n"
    open(os.path.join(lob, name), "wb").write(lobster_seed(m, r))
# every message type, hand written
hand = (b"34200.0,1,1,10,1000000,1\n34200.1,1,2,5,1000100,-1\n34200.2,2,1,3,1000000,1\n"
        b"34200.3,4,2,5,1000100,-1\n34200.4,5,0,9,1000000,1\n34200.5,6,0,1,1000000,1\n"
        b"34200.6,7,0,0,-1,-1\n34200.7,3,1,7,1000000,1\n")
open(os.path.join(lob, "hand_all_types"), "wb").write(lobster_seed(hand, b"9999999999,0,1000000,10\n" * 8))
# damaged variants of a real-looking input
base = lobster_seed(b"\n".join(msgs[:30]) + b"\n", b"\n".join(rows[:30]) + b"\n")
open(os.path.join(lob, "truncated"), "wb").write(base[: len(base) // 2])
bad = bytearray(base)
for _ in range(12):
    bad[rng.randrange(2, len(bad))] = rng.randrange(256)
open(os.path.join(lob, "bitflipped"), "wb").write(bytes(bad))
open(os.path.join(lob, "crlf"), "wb").write(lobster_seed(hand.replace(b"\n", b"\r\n"), b""))

# ITCH: byte 0 selects the filter. 0 = no filter, 129 = locate 1.
open(os.path.join(itc, "fixture_all"), "wb").write(b"\x00" + itch)
open(os.path.join(itc, "fixture_locate1"), "wb").write(b"\x81" + itch)
open(os.path.join(itc, "fixture_truncated"), "wb").write(b"\x81" + itch[: len(itch) - 17])
bad = bytearray(b"\x81" + itch)
for _ in range(8):
    bad[rng.randrange(1, len(bad))] = rng.randrange(256)
open(os.path.join(itc, "fixture_bitflipped"), "wb").write(bytes(bad))
open(os.path.join(itc, "zero_length"), "wb").write(b"\x00\x00\x00")
