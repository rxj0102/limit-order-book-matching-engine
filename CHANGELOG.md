# Changelog

Format follows Keep a Changelog. Versions follow Semantic Versioning once a release exists. No release has been tagged yet: v0.1.0 is prepared (docs/RELEASE_NOTES_v0.1.0.md) and will be tagged after the first pull request is merged to main. Everything below is under Unreleased until then, grouped by development phase.

## [Unreleased]

### Real data: one Nasdaq ITCH day
- Replayed 2019-12-30 from Nasdaq's public server: every frame decodes, and the top of book of AAPL, MSFT, AMZN, TSLA and QQQ matches an independent Python reconstruction at all 2.6 million changes (docs/results/itch_2019-12-30.md).
- Fixed: the automatic price band was centred on the midpoint of all add prices, which real far away orders (0.0001 dollars to nearly 200,000 dollars) moved to about 99,700 dollars, so every real order was rejected. When the range does not fit, the band is now centred on the median add price (`scan_price_median`, `base_price_around`, also in the Python helper).
- Added `itch_extract` (keep chosen stocks from a streamed day and cut it into pieces), `lobkit_replay --top-csv`, `scripts/itch_reference_top.py`, `scripts/compare_top.py`, `scripts/itch_queue_front.py` and `scripts/verify_itch_day.sh`.
- Documented that 1 to 3 percent of real displayed executions are not at the front of their queue, so the queue_order counter measures departures from strict priority on real data rather than divergence of the book.

### Phase 8: repository polish
- Added README.md, docs/LIMITATIONS.md, CONTRIBUTING.md, this changelog and the v0.1.0 release notes.
- Documentation audit: DESIGN.md brought in line with the code (sinks, modify rule, band selection, simulator latencies and output, CI jobs, release plan, benchmark workloads), see DESIGN.md section 17.
- TESTING.md: measured coverage for all libraries, note on disabled leak checking in the Python sanitizer job.
- PERFORMANCE.md: replay gap recorded as an open item with the first hypothesis tested; id hash experiments recorded as future work.

### Phase 7: benchmarks and profiling
- bench/bench_book: engine and std::map baseline under one harness (add, cancel, modify, match through N levels, mixed, strided ids, feed replay), rdtsc timing with calibration and invariant TSC check, interleaved repetitions, equivalence check of the two books before measuring.
- bench/summarize.py, bench/ab_run.py for medians with ranges and interleaved A/B runs.
- docs/PERFORMANCE.md with method, results, noise floor, profile, simulated cache counters, flame graph and every optimization attempt including failures. No optimization was adopted; the engine is unchanged.
- Pre-release run of the mutation tool and fuzz targets recorded in docs/TESTING.md.
- Pinned pybind11 to v3.1.0, built static libraries position independent for the extension, added the python and python-sanitizers CI jobs.

### Phase 6: Python bindings and analysis
- pybind11 module lobkit._core behind LOBKIT_BUILD_PYTHON: Engine, ReplayAdapter, LOBSTER reader, simulator and strategy configuration, numpy structured arrays with documented ownership, exceptions only at the binding boundary.
- python/lobkit/features.py: order flow imbalance, queue imbalance, microprice, realized spread, VPIN, tested against hand computed values.
- python/analysis/sweep.py: risk aversion by latency sweep with 30 seeds per cell on synthetic data, artifact rates, limitations, plots.
- include/lobkit/sim/synthetic.hpp: the seeded synthetic feed generator moved into the library.

### Phase 5: simulator and Avellaneda-Stoikov strategy
- Event-driven backtester with three separate one-way latencies, own orders at the back of the queue, post-only quotes, integer accounting in 128 bit integers with an exact PnL identity against an independent cash ledger, fees and rebates, CSV output.
- Avellaneda-Stoikov strategy with inventory limit, size skew, self-cross guard and throttled requoting.
- Simulator mutants added to the mutation tool.

### Phase 4: market data
- LOBSTER and ITCH 5.0 subset parsers (zero copy, no exceptions, status codes), memory mapped files, replay as fast as possible and paced, snapshot verification, strict no-seeding policy, synthetic datasets and generators, libFuzzer targets for both parsers.
- Fixed a Phase 1 performance bug found by the replay measurements: feed_execute walked the whole queue to the target even without own orders; it now stops at the first feed order.
- Real-data verification NOT run (data hosts blocked).

### Phase 3: lock-free infrastructure
- SPSC ring buffer with cached indices and justified memory orders, comparison queues, false sharing and ablation measurements, TSan stress test. Measurements are VM ranges.

### Phase 2: differential testing
- Reference book, differential test against the engine, mutation tool (hand written mutants), libFuzzer engine target, bounded invariant checker with negative tests, coverage gate.

### Phase 1: engine core
- Dense ladder with bitmaps, order pool, id map, intrusive FIFOs, order types (limit, market, IOC, FOK, post-only), cancel and modify, own order rules and anomaly counters in the replay adapter, event sinks, invariant checker, allocation and determinism tests.

### Phase 0
- Design document, repository skeleton, CMake presets, formatting configuration, CI.
