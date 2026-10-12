# lobkit

[![CI](https://github.com/rxj0102/limit-order-book-matching-engine/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/rxj0102/limit-order-book-matching-engine/actions/workflows/ci.yml)
![book line coverage 99.5 percent](https://img.shields.io/badge/book%20line%20coverage-99.5%25-brightgreen)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)

lobkit is a C++20 limit order book matching engine with price-time priority, a market data replay harness for LOBSTER and a subset of Nasdaq ITCH 5.0, an event-driven backtester that runs an Avellaneda-Stoikov market maker against replayed flow with explicit latency and queue-position modeling, and Python bindings with a small microstructure feature library. The engine allocates nothing after construction, uses no virtual dispatch and no floating point, and is deterministic. It is a research and engineering project, not a trading system.

The coverage badge is a static value (line coverage of include/lobkit/book and src/book, GCC 13.3, measured with the command in docs/TESTING.md) updated by hand. The CI gate is 90 percent.

## Status: what has and has not been verified

| Claim | State |
|---|---|
| Engine behavior matches the documented rules | Verified against a reference book written by the same author from the same specification (differential test, millions of random operations per run, byte-for-byte event comparison), with a hand written mutation check (28 mutants, 26 killed, 2 equivalent), libFuzzer targets (minutes per target, no crash found), invariant checks, and ASan, UBSan and TSan runs. This shows consistency with the documented rules. It does not show that the rules match any real venue. |
| Reconstruction of real market data | NOT VERIFIED. Real-data LOBSTER verification has not been run: the data host is blocked in the environment this was built in. The parsers were written from public documentation, not from observed files. This line will be updated only after a real file has passed snapshot verification (docs/MARKET_DATA.md section 6). |
| Strategy simulator results | Synthetic data only. Nothing is evidence that any strategy is profitable. The fill model is an approximation (docs/SIMULATOR.md). |
| Performance numbers | VM measurements on synthetic workloads. See the benchmark section and docs/PERFORMANCE.md. |

Everything above is limited further by docs/LIMITATIONS.md, which lists every known limitation.

## Architecture

```
  feed files                        replay harness                    your code / simulator
  LOBSTER csv  --+                  +--------------------+            +----------------------+
  ITCH 5.0 bin --+--> md/ parsers ->| ReplayAdapter      |            | sim/                 |
  (mmap, no copy)                   |  add reduce delete |            |  Avellaneda-Stoikov  |
                                    |  execute           |            |  latency queues      |
                                    +---------+----------+            |  PnL decomposition   |
                                              |                       +----------+-----------+
                                              v                                  |
                                    +--------------------+  submit, cancel,      |
                                    | book/  Book<Sink>  |<----- modify ---------+
                                    |  price ladder      |
                                    |  + bitmaps         |  events (48 byte POD)
                                    |  intrusive FIFOs   |-------------------> Sink: Null, Counting,
                                    |  order pool        |                     Hash, Vector, yours
                                    |  id map            |
                                    +--------------------+

  python/  pybind11 module lobkit._core, features.py, analysis/sweep.py   (wraps all of the above)
  infra/   SPSC ring buffer (standalone, not wired into the engine)
  bench/   benchmarks and the std::map baseline     tests/ fuzz/ tools/ scripts/ data/synthetic/
```

## Quickstart

Needs CMake 3.25 or newer, Ninja, a C++20 compiler (GCC 13 and Clang 18 are tested), and GoogleTest (found on the system or fetched at a pinned commit).

```
cmake --preset release && cmake --build --preset release
ctest --preset release
build/release/lobkit_sim_run --lobster data/synthetic/SYNTH_lobster_1000_message_10.csv --out /tmp/demo --data-label SYNTHETIC
cmake --preset bench && cmake --build --preset bench
build/bench/bench/bench_book --benchmark_filter=/mixed/ --benchmark_repetitions=5
```

The third command replays a committed 1,000 message synthetic file through the simulator and prints a PnL summary with its caveats. Python bindings, sanitizer, coverage and fuzz presets are described in docs/PYTHON.md, docs/TESTING.md and CONTRIBUTING.md.

## Benchmarks

All numbers are VM measurements (a KVM guest, 4 vCPUs, Intel Xeon at 2.10 GHz, GCC 13.3 at -O3, no isolated cores, no frequency control) on SYNTHETIC workloads. No real market data was available. Cells are medians over 20 repetitions taken in interleaved rounds, with the min to max range for throughput. The noise floor measured by running one binary against itself is about 20 percent in the median of a single row, so differences smaller than that mean nothing, and absolute values describe this VM only. Both books produced byte-identical event streams on every scenario before any number was taken.

The baseline is a separate minimal std::map plus std::list book (one allocation per order, tree descent per level), not the engine with a part swapped. Throughput comes from batch timing, in which operations overlap in the processor. Per-operation latency comes from timing every operation with serialization, includes about 25 ns of timer cost, and is therefore not comparable with the throughput figure. Both are shown because they say different things, and the engine is not ahead on both in every row.

| Scenario | Engine throughput, M ops/s | Engine p50, ns | Engine p99, ns | Baseline throughput, M ops/s | Baseline p50, ns | Baseline p99, ns |
|---|---|---|---|---|---|---|
| add | 17.4 [14.8, 20.4] | 148 | 404 | 12.4 [7.5, 13.0] | 109 | 332 |
| cancel | 14.3 [11.1, 19.5] | 209 | 536 | 3.7 [2.4, 5.4] | 430 | 884 |
| modify, reduce in place | 19.2 [8.6, 22.6] | 240 | 550 | 13.4 [6.5, 19.1] | 404 | 811 |
| modify, reprice | 7.4 [4.5, 10.6] | 223 | 547 | 2.3 [2.0, 2.6] | 475 | 949 |
| match 1 level | 8.1 [3.9, 11.3] | 144 | 327 | 7.2 [5.1, 7.8] | 158 | 321 |
| match 4 levels | 2.0 [1.2, 3.3] | 498 | 890 | 2.0 [1.4, 2.0] | 504 | 861 |
| match 16 levels | 0.8 [0.6, 0.9] | 1218 | 2185 | 0.5 [0.3, 0.5] | 1988 | 4724 |
| match 64 levels | 0.2 [0.1, 0.2] | 4708 | 13080 | 0.1 [0.1, 0.1] | 8057 | 27871 |
| mixed | 15.7 [9.7, 17.0] | 156 | 440 | 11.8 [10.2, 12.5] | 105 | 440 |
| mixed, strided ids | 17.2 [10.1, 19.4] | 149 | 429 | 10.8 [8.0, 11.7] | 114 | 471 |
| feed replay | 16.6 [11.1, 19.2] | 156 | 448 | 9.9 [6.2, 10.5] | 121 | 488 |

A match row is one IOC order sweeping that many price levels of 4 orders each. The p99.9 and maximum columns are omitted here because in this VM they are dominated by hypervisor and scheduler events; the full table with them, the method, the profile, the optimization attempts (none was adopted) and the open items are in docs/PERFORMANCE.md. Raw data: docs/results/phase7/final/final.json.

Replay throughput on the largest replay that exists here, a SYNTHETIC 1,000,000 message LOBSTER file, as fast as possible, 9 runs: 4.7 million messages per second median, 4.2 to 4.9 range (VM). An in-memory apply of similar messages runs at more than three times that rate, and the gap is an open item in docs/PERFORMANCE.md. The SPSC ring buffer in infra/ ranged from 30 to 184 million items per second on this VM for single items, and is only ever quoted as a range (docs/INFRA.md).

## Design decisions

The reasoning for each is in docs/DESIGN.md. The ones that shape everything else:

- Dense price ladder with two-level bitmaps. Prices map to ladder indices in real ticks, so a level is one array access and the best price is a few bit operations. The cost is a bounded price band, with out-of-band prices rejected and counted.
- Fixed memory. Orders live in a preallocated pool linked by 32 bit indices, and the id map is open addressing with backward-shift deletion, sized for the configured maximum. Nothing allocates after construction, and a test replaces operator new to prove it.
- The engine is a class template over its event sink, so the sink inlines. Events are 48 byte PODs with a sequence number. Time is an input on every message and the engine reads no clock, so the same input gives byte-identical output, which golden hashes check.
- One book per thread. Matching is sequential per instrument, so there are no locks. Concurrency exists only at the edges, through an SPSC ring.
- Replay is a separate adapter over the same book. Feed executes name the resting order and do not re-run matching. Strategy orders rest at the back of the queue and fill only when replayed flow reaches them, with explicit rules for the cases where the replica diverges, which are counted rather than hidden.
- Replay starts from an empty book (strict, no seeding). This is a deliberate, documented choice made without being able to inspect real files, and it is the first thing real-data verification must confirm.
- Integer money. The simulator accounts in half price units in 128 bit integers, so the PnL decomposition identity is checked exactly against an independent cash ledger. Prices and quantities become floating point in exactly one place, the Avellaneda-Stoikov quote math, and its output is rounded to integers immediately.
- Correctness evidence is layered because each layer has a blind spot: a slow reference book, mutation of the engine to see whether the tests notice, fuzzing, sanitizers and invariant checks.
- Benchmarks must be able to disprove an optimization. Failed attempts are recorded with their tables, and a change has to beat the measured noise floor to count.

## Limitations

The short version: no real data has been used anywhere, the matching semantics are validated only against their own specification, the fill model is an approximation with no market impact, latency is constant, k is uncalibrated, sigma is crude, hidden executions are not applied, and every timing is from a VM. The complete list, with where each is documented, is in docs/LIMITATIONS.md.

## Roadmap

Nothing below is promised or scheduled.

1. Run snapshot verification on real LOBSTER files, then decide the seeding policy with that evidence, and update the status block above.
2. Explain the replay gap in docs/PERFORMANCE.md section 8.
3. Revisit the id hash experiments with real order ids (docs/PERFORMANCE.md section 8).
4. Rerun the benchmarks on bare metal with isolated cores.
5. Ladder window with recentering, for feeds with a wide intraday range.
6. Self-trade policy CancelNewest. Calibration of k from data, and a latency model with jitter.
7. Wire the SPSC ring into a parser thread and engine thread pipeline.
8. Multiple instruments as independent engines. A pip package for the Python module.
9. CI on macOS, and clang-tidy in CI.

## Documentation

| File | Contents |
|---|---|
| docs/DESIGN.md | Plan, data structures, decisions, where the build differs from the plan |
| docs/MATCHING_RULES.md | Exact matching semantics, own order rules, anomalies |
| docs/MARKET_DATA.md | LOBSTER and ITCH handling, seeding policy, what was verified |
| docs/SIMULATOR.md | Latency model, accounting, Avellaneda-Stoikov, biases |
| docs/PYTHON.md | Bindings, ownership of arrays, features, sweep |
| docs/TESTING.md | Every verification layer and what it cannot show |
| docs/PERFORMANCE.md | Methodology, results, profiles, optimization attempts, open items |
| docs/INFRA.md | SPSC ring and its measurements |
| docs/LIMITATIONS.md | All limitations in one place |
| docs/RELEASE_NOTES_v0.1.0.md | Prepared release notes (the tag does not exist yet) |
| CONTRIBUTING.md, CHANGELOG.md | How to contribute, history |

## License

The intended license is MIT, but there is no LICENSE file on this branch yet; it is being added on main by the repository owner. Until that file exists, treat the license as unspecified. This paragraph is to be reduced to one line pointing at the LICENSE file once it does.
