# Synthetic test data

Everything in this directory is SYNTHETIC. It is not market data.

| File | Made by | Contents |
|---|---|---|
| SYNTH_lobster_1000_message_10.csv | scripts/make_synthetic_lobster.py --messages 1000 --seed 1 | 1000 LOBSTER-format messages (types 1 to 5 and 7) of random flow around $100.00 |
| SYNTH_lobster_1000_orderbook_10.csv | same run | the 10-level order book row after each message |
| itch_fixture.bin | scripts/make_itch_fixture.py | 14 hand-built ITCH 5.0 frames (two stocks, all decoded message types, one unknown type) |

What agreement with these files means: the C++ replay agrees with a separate
Python model of the book that was written from the same understanding of the
LOBSTER format. It shows the code is self-consistent. It does not show that the
format is understood correctly, and it is not independent verification. The
independent check is a run against real LOBSTER files, see docs/TESTING.md for
whether that has been done.

The book in the LOBSTER files starts empty, so no message refers to an order that
existed before the first line. Real files may differ; see docs/MARKET_DATA.md.

Regenerate:

    python3 -I scripts/make_synthetic_lobster.py data/synthetic/SYNTH_lobster_1000 --messages 1000 --seed 1
    python3 -I scripts/make_itch_fixture.py data/synthetic/itch_fixture.bin
