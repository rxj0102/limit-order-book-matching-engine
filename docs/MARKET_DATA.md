# Market data

Parsers for LOBSTER and a subset of Nasdaq ITCH 5.0, the replay harness, and snapshot verification. Read section 6 first: it states what has and has not been verified against real data.

## 1. Components

| Piece | Where | Notes |
|---|---|---|
| LOBSTER message reader | include/lobkit/md/lobster.hpp | text, one event per line |
| LOBSTER order book reader | same | one row of N levels per message |
| ITCH 5.0 decoder | include/lobkit/md/itch.hpp | big endian, length prefixed frames |
| File mapping | include/lobkit/md/mapped_file.hpp, src/md | mmap on POSIX, read into memory elsewhere |
| Replay and verification | include/lobkit/md/replay.hpp | drives the ReplayAdapter |
| Command line | tools/lobkit_replay.cpp | build/release/lobkit_replay |

All readers work on a std::string_view or std::span over the mapped file. They do not copy, allocate or throw. Every failure is a ParseStatus code; after a failure the reader stays failed and reports the same code, so a caller cannot read past damage. The tests include a zero-allocation test that covers parsing, replay, pacing and verification, and error paths.

## 2. LOBSTER

Message file, no header, `time,type,order_id,size,price,direction`:

| Field | Meaning |
|---|---|
| time | seconds after midnight, up to 9 decimals; converted to integer nanoseconds without floating point |
| type | 1 new limit order, 2 partial cancel (size is the quantity removed), 3 delete, 4 execution of a displayed order, 5 execution of a hidden order, 6 cross trade, 7 trading halt |
| size | shares, must fit in 32 bits |
| price | dollars times 10000, signed |
| direction | 1 buy order, -1 sell order: the side of the order the message refers to |

How each type is applied through the ReplayAdapter:

| Type | Action | Counted as |
|---|---|---|
| 1 | apply_add(id, side, price, size) | adds |
| 2 | apply_reduce(id, size) | reduces |
| 3 | apply_delete(id) | deletes |
| 4 | apply_execute(id, size) | executes (and a book trade event) |
| 5 | none: a hidden order is not in the displayed book | hidden_executions and hidden_shares, reported separately |
| 6 | none | cross_trades |
| 7 | none | halts. The raw price field is kept in the parsed message, because I do not know which codes real files use |

Order book file, no header: for level i, `ask_price, ask_size, bid_price, bid_size`. A missing level is ask price 9999999999 or bid price -9999999999 with size 0. The reader takes the level count from the first line (or the caller) and rejects rows with a different number of fields.

Prices are used as native values: the engine grid is `price = base_price + k * tick_size`, so for a stock quoted in cents the tick size is 100 and prices need no conversion. `lobkit_replay` picks base_price by scanning the file for the price range and centering the band on it (`--base-price` overrides). A price that falls outside the band, or off the tick grid, is rejected by the engine, counted, and reported. In verification mode either one is a hard failure.

## 3. ITCH 5.0

Framing is the BinaryFILE layout: a 2 byte big-endian length, then that many bytes of message, repeated. Fields are big endian, timestamps are 6 bytes (nanoseconds since midnight), prices are 4 byte integers with 4 implied decimals, which is the same scale as LOBSTER.

| Type | Size | Handling |
|---|---|---|
| S system event | 12 | counted |
| R stock directory | 39 | counted; used by `--symbol` to find the stock locate |
| A add order | 36 | Add |
| F add order with MPID | 40 | Add |
| E order executed | 31 | Execute |
| C order executed with price | 36 | Execute (the execution price is not needed to reduce the order) |
| X order cancel | 23 | Reduce |
| D order delete | 19 | Delete |
| U order replace | 35 | delete the old reference, add the new one on the same side, losing priority |
| P trade, non-cross | 44 | counted as non-book trade; the book is not touched |
| any other type | | skipped using its length and counted |

A known type whose declared length differs from its fixed size is an error (BadLength). ITCH sizes are fixed, so a different length means the stream is damaged and trusting it could desynchronize everything after. The decoder filters to one stock locate when asked; messages of other stocks are counted, not decoded further.

## 4. Replay harness

- Modes: as fast as possible, and paced. Paced mode releases each message at its recorded time offset divided by a speed multiplier, sleeping through long gaps and spinning for the last stretch. The wall clock exists only in the Pacer in the harness; the engine and adapter receive the message timestamp as data. The report gives the maximum lateness so a run that could not keep up is visible.
- Snapshot verification (LOBSTER): after every message the top N levels of the reconstructed book are compared with the matching row of the order book file. The run stops at the first divergence and reports the message index, the file line, the message text, and expected versus actual levels. These are also hard failures: any reject, any of the four anomaly counters growing, a message whose side or price disagrees with the order it names, a missing or malformed row.
- Plain replay never stops on book problems. It counts and reports them and marks the run degraded if any occurred.
- Exit status of `lobkit_replay`: 0 success (including a degraded plain replay), 1 verification failed, 2 usage or input error.

## 5. Seeding: orders that exist before the first message

A LOBSTER file might refer to order ids that were placed before its first line (for example orders resting from before the start of the file). A faithful replay then needs those orders, and an order book snapshot alone cannot supply them: the snapshot gives aggregate size per price, not the individual orders that later cancel or execute by id.

The decision I made, and why:

- The default policy is strict, no seeding. The replay starts from an empty book. A message that names an unknown id is rejected by the engine, counted, and in verification mode stops the run at that message.
- I did not implement a seeding mode that fabricates orders from the first book row. Without knowing whether the real sample needs one, it would be guessing at the book state, and a seeded aggregate order cannot be cancelled by the real ids that follow.
- I could not determine empirically how the real sample behaves, because the data host is blocked in the environment where this was written (section 6). The synthetic data starts from an empty book by construction, so it says nothing about real files.

What to do when real data is available, in order:

1. Run `lobkit_replay --lobster MSG --book ROWS --verify`.
2. If it passes, the file starts from an empty book (or from a state the first messages reproduce) and strict is correct. Record that here.
3. If it fails with an UnknownOrderId reject at a small message index, the file refers to earlier orders. Run it without `--verify` and look at the reject counts and at the first row of the book file: the number of unknown-id rejects, and whether they stop after a while, tell how large the pre-existing population is. Then decide on a seeding policy with that evidence, record the evidence here, and implement it. Possibilities include skipping verification until all pre-existing orders have left the book, which needs the evidence to be sure of.
4. If it fails with RowMismatch with no rejects, the semantics of some message type differ from what MATCHING_RULES.md assumes, and that is a finding about the rules, not about seeding.

Open questions for the real files that I could not settle: whether any halt message carries a code other than the three I expect, whether type 4 executions always carry the resting order price, whether there is a trailing comma or other whitespace in lines, and whether the order book file has a row for every message including types 5 and 7.

## 6. What has and has not been verified

| Check | Status |
|---|---|
| Parsers and replay on synthetic LOBSTER data (SYNTH_lobster_1000), verified against its generated order book rows | Run and passing. SELF-CONSISTENCY ONLY: the generator and the code share an author and an understanding of the format |
| Decoder on the hand-built ITCH fixture | Run and passing. Same caveat: the fixture builder and the decoder were written from one reading of the specification |
| Verification against real LOBSTER order book files | NOT RUN. lobsterdata.com is blocked by the egress policy of this environment (HTTP 403 on CONNECT) |
| Decoding of a real Nasdaq ITCH file | NOT RUN. emi.nasdaq.com is blocked in the same way |
| Fuzzing of the parsers | Run, see docs/TESTING.md for the counts |

So the independent check required by the project has not been done. The parsers and the replay semantics are tested for internal consistency and robustness, and the file layouts are implemented from my knowledge of the public documentation, not from observed files. Any claim that lobkit reproduces LOBSTER or ITCH data is unproven until a real file passes verification.

## 7. Getting real data

LOBSTER (free samples, registration not needed for the sample page): run `scripts/get_lobster_sample.sh`, or download a sample from lobsterdata.com (the Data Samples page) by hand and unzip it into data/lobster/. Both csv files are needed, with the same number of levels.

Nasdaq ITCH 5.0 sample: Nasdaq publishes daily sample files on its public EMI server (emi.nasdaq.com/ITCH/Nasdaq ITCH/), gzip compressed with names like 01302019.NASDAQ_ITCH50.gz and several gigabytes uncompressed. They are not committed to this repository. Decompress one and run `lobkit_replay --itch FILE --symbol AAPL`. The path and file naming are from memory and unchecked.

## 8. Throughput

All figures are VM measurements (KVM guest, 4 vCPUs, Intel Xeon @ 2.10 GHz, GCC 13.3 -O3, no -march=native, one process at a time, page cache warm) on SYNTHETIC files, because no real file was available. Environment and raw output: docs/results/phase4/throughput_final.txt. The synthetic book is small (hundreds of live orders), so these numbers say little about a real full-day book with deep queues and cold cache lines. No real full-day file has been replayed; there is no full-day number in this document.

| Measurement | Runs | Range (million messages per second) | Median |
|---|---|---|---|
| LOBSTER parse only, 1,000,000 messages | 9 | 12.7 to 15.2 | 15.0 |
| LOBSTER replay, parse and apply, as fast as possible | 9 | 4.7 to 5.5 | 5.3 |
| LOBSTER replay with snapshot verification, 10 levels (also parses 268 MB of rows) | 5 | 1.2 to 1.3 | 1.25 |
| ITCH parse only, 2,910,592 stock messages out of 3,000,000 frames | 9 | 38 to 51 | 50 |
| ITCH replay, parse and apply | 9 | 5.8 to 8.1 | 6.5 |

Reading the numbers: parsing is cheap next to applying (about 65 ns per LOBSTER line against about 190 ns for parse plus apply), so the book operations dominate. They were profiled in Phase 7 (docs/PERFORMANCE.md sections 4 and 5); the same synthetic file was rerun there and gave 4.7 million messages per second median (range 4.2 to 4.9), consistent with the 4.7 to 5.5 range above. The gap between this end to end rate and the in-memory apply rate is an open item in docs/PERFORMANCE.md section 8. The verification mode is dominated by parsing the order book file, which has about 270 bytes per row.

### A performance bug found by this measurement

The first ITCH replay did not finish after more than five minutes. The cause was in the Phase 1 engine, not in the parser: `feed_execute` walked the queue from the front to the target to look for own orders, so executing a deep feed order cost time proportional to its queue position, even when no own order existed. My synthetic ITCH generator executed random orders, which made queues of tens of thousands of orders at a few prices and exposed it. Real feeds mostly execute the front of a queue, which is why this would have been easy to miss.

Fix: the walk stops at the first feed order ahead of the target, since nothing behind a feed order can fill and only the existence of the feed order matters. Behavior is unchanged (the differential test, the mutation tool and a new unit test cover it). Measured on the stress stream (executes at random depth; one run per row before, as the run was only a diagnosis; VM):

| Messages replayed | Before | After |
|---|---|---|
| 100,000 | 0.069 s | 0.010 s |
| 200,000 | 0.275 s | 0.021 s |
| 400,000 | 1.349 s | 0.055 s |
| 2,911,134 (all) | not finished after more than 5 minutes (stopped) | 0.415 s |

The before column grew quadratically. The generator was then changed to execute the oldest order at a level (price-time priority), which is the realistic case and is used for the throughput table above; the old behavior is kept as a stress case behind `scripts/make_synthetic_itch.py --random-exec`.

A queue_order anomaly is counted for every execute of a feed order that is not at the front of its queue, even with no own orders present, because on a price-time priority venue that means the replica no longer matches the feed. The stress stream therefore reports hundreds of thousands of anomalies, and the realistic one reports none.
