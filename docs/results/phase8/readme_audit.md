# README audit (Phase 8)

Every factual claim in README.md, mapped to the file or test that supports it, with how it was checked on the date of the audit (2026-10-12, KVM VM, GCC 13.3). Claims that could not be supported were changed or removed; they are listed at the end. Claims about the future (roadmap) are labeled as plans in the README and are not audited here.

## Pitch paragraph

| Claim | Evidence | Checked |
|---|---|---|
| C++20 limit order book matching engine with price-time priority | include/lobkit/book/book.hpp; docs/MATCHING_RULES.md; tests/book/test_orders.cpp | ctest release: 221 of 221 passed |
| Market data replay for LOBSTER and a subset of ITCH 5.0 | include/lobkit/md/lobster.hpp, itch.hpp; docs/MARKET_DATA.md sections 2 and 3; tests/md | ctest |
| Event-driven backtester with Avellaneda-Stoikov, explicit latency and queue-position modeling | include/lobkit/sim; docs/SIMULATOR.md; tests/sim | ctest; lobkit_sim_run demo run |
| Python bindings and a small feature library | python/src/bindings.cpp, python/lobkit/features.py; python/tests | ctest --preset python: 222 of 222 (221 plus pytest, 32 tests) |
| Allocates nothing after construction | tests/book/test_no_alloc.cpp (NoAlloc.MatchingCancelModifyAndReplayDoNotAllocate, NoAllocMarketData.*) which replaces operator new | passed in release |
| No virtual dispatch | grep for virtual in include and src finds only two comments saying there is none | grep run |
| No floating point in the engine | grep for double and float in include/lobkit/book and src/book finds none. (double does appear in md/replay.hpp for wall-clock timing and the Pacer, in the strategy math, report display and the synthetic generator; the README wording was narrowed accordingly) | grep run |
| Deterministic | tests/book/test_determinism.cpp (same stream gives identical event bytes, golden hashes) | passed |
| Not a trading system | docs/LIMITATIONS.md section 1; docs/SIMULATOR.md first paragraph | read |
| Coverage badge value 99.5 percent, CI gate 90 percent | gcovr on include/lobkit/book and src/book, merged lines: 608 of 611 lines. .github/workflows/ci.yml coverage job has --fail-under-line 90 | coverage preset built and run today, all 221 tests passed |
| CI badge | URL pattern of GitHub Actions for .github/workflows/ci.yml on branch main | NOT checkable until main exists and CI has run there; the badge will show no status until then |

## Status table

| Claim | Evidence | Checked |
|---|---|---|
| Reference book, differential test, millions of random operations per run, byte for byte event comparison | tests/support/reference_book.hpp, tests/book/test_differential.cpp (5 scenarios, 1,000,000 operations each by default, LOBKIT_DIFF_OPS raises it); docs/TESTING.md | 5 differential tests passed |
| Mutation check: 28 mutants, 26 killed, 2 equivalent | tools/mutation/mutants.py, run.py; docs/TESTING.md pre-release table | full run done on commit 25d38f2 (output in the session, summarized in TESTING.md); `run.py --check` re-run today, exit code 0 |
| Fuzz targets, minutes per target, no crash found | fuzz/; docs/TESTING.md (301 s each: 175,989, 969,403 and 1,903,326 executions) | run on commit 25d38f2 |
| Invariant checks | include/lobkit/book/invariants.hpp; tests/book/test_invariants.cpp | passed |
| ASan, UBSan and TSan runs | CMakePresets.json; ci.yml; ctest on GCC debug, release, asan, ubsan, tsan and Clang debug, release, asan, tsan | GCC debug, release, asan, ubsan, tsan and coverage were re-run today on the Phase 8 tree: 221 of 221 each. The Clang builds were last run in Phase 6 (the engine headers are unchanged since) |
| Real-data LOBSTER verification not run, data host blocked, parsers from public documentation | docs/MARKET_DATA.md section 6; docs/TESTING.md last table | consistent in all documents |
| Simulator results synthetic only | docs/SIMULATOR.md; docs/results/phase5, phase6 | read |

## Architecture diagram and quickstart

| Claim | Evidence | Checked |
|---|---|---|
| Component boxes and arrows | include/lobkit/{md,sim,book,infra}; DESIGN.md section 2 | matches the directory layout and the dependencies stated in DESIGN.md |
| Sinks Null, Counting, Hash, Vector | include/lobkit/book/sinks.hpp | read |
| Events are 48 byte PODs | static_assert(sizeof(Event) == 48) in include/lobkit/book/types.hpp | compiles |
| infra is standalone and not wired into the engine | grep: spsc_ring.hpp is included only by tests/infra and bench/ (verified today) | read |
| mmap, no copy | src/md/mapped_file.cpp; include/lobkit/md/lobster.hpp parse from a string_view | read |
| Quickstart commands | the five commands in README.md | all five were run on this machine: release build and ctest (221 passed), lobkit_sim_run on the committed synthetic file printed a summary with a matching ledger check, bench build and `bench_book --benchmark_filter=/mixed/ --benchmark_repetitions=3` ran and printed the equivalence line |
| Needs CMake 3.25, Ninja, GoogleTest found or fetched at a pinned commit | CMakeLists.txt, cmake/Dependencies.cmake, CMakePresets.json (generator Ninja) | read |
| GCC 13 and Clang 18 tested | docs/DESIGN.md section 11; CI matrix | GCC re-run today; Clang last run in Phase 6, unchanged headers; the CI result on GitHub is not checked here |

## Benchmarks

| Claim | Evidence | Checked |
|---|---|---|
| Every table cell | docs/results/phase7/final/final.json, summarized by bench/summarize.py; the README table was generated from that file by a script, not typed | regenerated from the JSON when the README was written |
| 20 repetitions, interleaved rounds | bench/ab_run.py (4 rounds of 5 with random interleaving inside the binary); the JSON holds 20 iteration rows per benchmark | counted |
| VM, 4 vCPUs, Xeon 2.10 GHz, GCC 13.3 -O3, no isolated cores, no frequency control | docs/results/phase7/environment.txt, docs/PERFORMANCE.md section 1 | read |
| Noise floor about 20 percent | docs/results/phase7/attempts/aa_noise (same binary twice: up to 19 percent in a median, up to 20 percent in a p50) | tables in attempts_tables.md |
| Both books produce byte-identical event streams before any number is taken | bench/bench_book.cpp check_equivalence, run at start-up | printed at every run, including today's |
| Baseline design (std::map plus std::list, allocation per order) | bench/std_map_book.hpp header comment and code | read |
| Per-operation latency includes about 25 ns of timer cost and serializes the pipeline | bench/book_harness.hpp; docs/PERFORMANCE.md section 1 (22.9 ns minimum, 25.7 median) | read |
| The engine is not ahead on both throughput and latency in every row | table: baseline has the lower p50 on add, mixed, mixed strided and feed replay, and the lower p99 on add and match 1 | read from the table |
| Match row definition | bench/book_scenarios.hpp make_match | read |
| p99.9 and max omitted because of VM noise, full table in PERFORMANCE.md | docs/PERFORMANCE.md section 3 contains them | read |
| Optimization attempts, none adopted | docs/PERFORMANCE.md section 6, docs/results/phase7/attempts_tables.md | read |
| Replay 4.7 million messages per second median, 4.2 to 4.9 range, 9 runs, synthetic 1,000,000 message file, as fast as possible | docs/results/phase7/replay_throughput.txt (min 4.18, median 4.72, max 4.93) | computed from the file |
| In-memory apply more than three times that rate, gap open | docs/PERFORMANCE.md sections 4 and 8: 16.6 against 4.7 is 3.5 times | read |
| SPSC ring 30 to 184 million items per second for single items on this VM | docs/INFRA.md sections 4 and 7 | read |

## Design decisions

| Claim | Evidence |
|---|---|
| Dense ladder, two-level bitmaps, index in real ticks, out-of-band rejected and counted | include/lobkit/book/level_bitmap.hpp, config.hpp; MATCHING_RULES.md section 2; DESIGN.md 3.1, 3.2 |
| Pool with 32 bit index links, open addressing id map with backward-shift deletion, sized for the maximum | include/lobkit/book/order_pool.hpp, order.hpp, id_map.hpp; tests/book/test_primitives.cpp |
| Nothing allocates after construction, proven by a test that replaces operator new | tests/book/test_no_alloc.cpp |
| Class template over the sink, 48 byte events with sequence number | include/lobkit/book/engine.hpp, sinks.hpp, types.hpp |
| Time is an input, no clock in the engine | comment and code in types.hpp; grep for chrono and clock in include/lobkit/book finds none; golden hash test |
| One book per thread, concurrency only at the edges through an SPSC ring | DESIGN.md section 6; docs/INFRA.md (the ring is not connected to the engine, which the README states) |
| Replay is a separate adapter over the same book; executes name the resting order and do not re-run matching | include/lobkit/book/replay_adapter.hpp, book.hpp feed_execute; MATCHING_RULES.md section 7 |
| Strategy orders rest at the back and fill only when flow reaches them; divergences counted | tests/book/test_replay_own_orders.cpp; MATCHING_RULES.md sections 9 and 10 |
| Strict no seeding | docs/MARKET_DATA.md section 5; include/lobkit/md/replay.hpp (starts with an empty engine) |
| Integer money, 128 bit, PnL identity checked against an independent cash ledger | include/lobkit/sim/types.hpp (Money2); tests/sim/test_as_runs.cpp ASRun.PnlComponentsSumToTheTotalExactly (EXPECT_EQ of total2 and total2_from_ledger); tests/sim/test_simulator.cpp SimPnl.* |
| Prices and quantities become floating point only in the quote math | grep of include/lobkit/sim and src/sim: double appears in avellaneda_stoikov.hpp (the math), types.hpp (a constant), synthetic.hpp (a mix parameter of the data generator) and report.cpp (display of the simulated span) |
| Layered correctness evidence | docs/TESTING.md |
| Failed attempts recorded; a change must beat the noise floor | docs/PERFORMANCE.md section 6 |

## Limitations, roadmap, documentation table, license

| Claim | Evidence |
|---|---|
| Short limitation list | each item appears in docs/LIMITATIONS.md with its source document |
| Documentation table | all files listed exist (checked by script) |
| License | NOT in this branch. The README now says the license is unspecified until main supplies LICENSE (flagged, per the owner's decision) |

## Claims changed or removed during the audit

1. README originally said floating point appears only in the Avellaneda-Stoikov quote math. Narrowed: prices and quantities become floating point only there; timing, pacing, display and the data generator also use double. CONTRIBUTING.md and LIMITATIONS.md were corrected the same way.
2. DESIGN.md said PnL accounting uses double precision; it is integer. Corrected (DESIGN.md section 8 and 17).
3. DESIGN.md said clang-tidy runs in CI and that a macOS job exists. Neither does. Corrected.
4. DESIGN.md said a ring-buffer sink exists. It does not. Corrected.
5. The coverage badge cannot be generated by a service in this repository. It is a static value, stated as such in the README, with the measurement command.
6. The CI badge, the license line and the sanitizer re-run claim depend on things that exist only after the merge to main; the README and this audit say so instead of asserting them.

## Not supported and therefore not in the README

- Any headline speedup figure. The README gives the table and no ratio.
- Any statement that real data has been verified.
- Any statement that the replay gap is understood.
