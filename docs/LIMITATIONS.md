# Limitations

Every limitation stated anywhere in this repository, in one place, grouped by what it affects. Each item points at the document that holds the detail. If another document and this one disagree, that is a bug in one of them; please open an issue.

Two sentences summarize the project state. Everything has been checked against a reference that shares an author with the engine, and nothing has been checked against real exchange data. Every simulator, Python analysis and benchmark result so far comes from SYNTHETIC data, and every timing is a VM measurement.

## 1. What the project is not

- Not a live trading system. No order gateway, no risk checks, no persistence, no recovery. (DESIGN.md section 1, SIMULATOR.md)
- One instrument per engine instance. Several instruments need several independent engines. Sharding is not built. (DESIGN.md)
- Not a record claim. No number in this repository is a latency or throughput claim for any other machine.

## 2. Matching engine

- The matching semantics are the documented intent, not a validated model of any venue. The reference book used for differential testing and the engine were written by the same author from the same document, so a misreading of the rules would be in both. (TESTING.md, MATCHING_RULES.md)
- Not supported: hidden or iceberg orders, auctions and crosses, stop orders, pegged orders, odd lots, minimum quantities, pro-rata allocation, self-match prevention groups. (DESIGN.md section 1, SIMULATOR.md section 8)
- Self-trade policy: only Allow is implemented. A self-trade executes and is flagged. CancelNewest is reserved and not built. (MATCHING_RULES.md section 6)
- The price band is fixed at construction (default 65,536 ticks). A price outside it is rejected and counted. There is no recentering, so a feed with a wide intraday range or erroneous prints needs a larger band. (DESIGN.md section 3.2)
- The order pool is fixed (default 1,048,576 orders). When it is full a resting order is rejected with PoolExhausted, and this is checked before matching, so an order that would have matched completely is also rejected. (MATCHING_RULES.md section 2)
- The engine does not remember dead ids: a caller may reuse the id of a filled or cancelled order. A feed that reuses ids while the order is still live is rejected as a duplicate. (MATCHING_RULES.md section 1)
- Order ids with either of the two top bits set are reserved for strategy orders (ids in 2^62 to 2^63). A feed message carrying such an id is skipped and counted by the simulator. (MATCHING_RULES.md section 11)
- Modify semantics follow one convention (price change or size increase loses priority). Venues differ. (MATCHING_RULES.md section 5)
- Quantities are 32 bit. Prices are 64 bit native fixed point, with the tick grid supplied by the caller.
- Single threaded by design. The engine and adapters are not thread safe; different engines can run on different threads. (DESIGN.md section 6, PYTHON.md)

## 3. Market data and replay

- No real LOBSTER or ITCH file has ever been replayed. The data hosts are blocked in the environment where the work was done. The parsers and the file layouts were implemented from knowledge of the public documentation, not from observed files. Until a real file passes snapshot verification, nothing here should be read as showing that lobkit reproduces real exchange data. (MARKET_DATA.md sections 6 and 7, TESTING.md)
- The synthetic LOBSTER and ITCH data and the code that reads it were written from one understanding of the formats. Agreement between them proves self-consistency, not correctness. (data/synthetic/README.md)
- Strict no-seeding policy. Replay starts from an empty book. A real file that refers to orders placed before its first line will produce unknown-id rejects, and in verification mode the run stops at the first one. Whether real files need seeding is undetermined, because the real files could not be inspected. Seeding was deliberately not implemented, since fabricating orders from a book snapshot would guess at state and could not match the real ids that follow. (MARKET_DATA.md section 5)
- Open format questions that only real files can answer: halt message codes, whether type 4 executions always carry the resting order price, whitespace and trailing commas, and whether the order book file has a row for every message including types 5 and 7. (MARKET_DATA.md section 5)
- Hidden executions are not applied. LOBSTER type 5 and ITCH P are counted (with their shares) and do not touch the displayed book. Cross trades (LOBSTER 6) and halts (7) are counted and not applied. (MARKET_DATA.md, MATCHING_RULES.md section 11)
- ITCH support is a subset (S, R, A, F, E, C, X, D, U, P). Other types are skipped by length. A replace of an unknown reference produces an unknown-id reject. (MARKET_DATA.md section 3)
- Anomaly semantics. A replica that no longer matches the feed is counted, never hidden. In plain replay and in simulation the run continues and is marked degraded. In snapshot verification any anomaly or rejected feed operation is a hard failure. The four anomaly counters are queue order anomalies, feed adds that crossed an own order, feed adds that crossed a feed order, and feed executes larger than the remaining quantity. After an anomaly the replica is no longer exact. (MATCHING_RULES.md sections 9 and 10, MARKET_DATA.md section 4)
- The replay tools choose the price band by reading the whole file once to find its price range. That is look-ahead, acceptable for a backtest and unusable live. (MARKET_DATA.md section 2)
- Paced replay uses a monotonic clock, sleeps through long gaps and spins at the end. It is for demonstration and is not used for any throughput number. (MARKET_DATA.md section 4)
- Feed timestamps are venue timestamps. Real receive times are not in these files. (SIMULATOR.md section 8)

## 4. Simulator and strategy

- The fill model is an approximation. An own order fills when a historical execution reaches the queue position the order would have had. Real queue dynamics depend on information the feed does not contain. (SIMULATOR.md section 8, MATCHING_RULES.md section 9)
- No market impact. The feed does not react to the strategy, and other participants do not react to its quotes. Own fills are counterfactual extra volume, so traded volume can exceed feed volume. (SIMULATOR.md, MATCHING_RULES.md section 9 rule 5)
- Cancels ahead of the strategy are assumed independent of its presence. In real markets participants cancel when they see a large order arrive. This tends to flatter the fill model.
- Hidden executions are not applied to strategy orders even when they happened at its price. This under-fills, so fill rates are biased low. Whether this and the cancel assumption offset each other is unknown.
- Latency is constant. Each of the three one-way delays (submit, acknowledgement, market data) is one fixed number. A constant latency understates the effect of jitter, which matters most when it reorders a cancel and a fill. There is no model of contention or queueing inside a gateway. (SIMULATOR.md section 2)
- A feed add that crosses a resting strategy order trades with it and displaces the feed order, which is counted as an anomaly. In a 1,000,000 message synthetic run this affected tens of messages. (SIMULATOR.md section 8)
- The strategy sends post-only orders only. It never takes liquidity. One strategy, one instrument.
- Fees are a flat amount per share by liquidity role. Fee tiers, exchange and regulatory fees, borrow costs and financing are not modeled.
- The mark is the mid of the other participants' book. A real position could not be closed at the mid.
- Avellaneda-Stoikov assumes a Brownian mid and Poisson arrivals with intensity decaying exponentially with distance. Real markets do not behave that way.
- k is uncalibrated. It is a configured parameter with a placeholder default (50 per dollar). Fitting it from data is not built.
- sigma is crude. It is an exponentially weighted average of squared mid changes sampled every second, started from a configured value.
- The strategy math is the one place where prices and quantities become floating point. Runs are reproducible on one binary and one machine; across compilers or libm versions a quote within rounding error of a tick boundary could move by one tick. (SIMULATOR.md section 5)
- PnL decomposition does not separate adverse selection from ordinary price drift. Inventory PnL contains both. (SIMULATOR.md section 4)
- Every simulator result so far is on synthetic data. Synthetic results are not evidence of profitability, and results on a real feed would still be an optimistic or uncertain estimate for the reasons above. (SIMULATOR.md)

## 5. Python bindings and analysis

- The sweep draws no conclusions. It runs on synthetic feeds, with constant latency, uncalibrated k and crude sigma, and its tables show how much results move between seeds, not how the strategy behaves. Patterns in them are not findings. (PYTHON.md, docs/results/phase6/sweep_report.md)
- Inventory PnL in the sweep is a proxy for adverse selection and includes plain noise.
- The sweep reports mean and spread over 30 seeds of 100,000 messages (about 100 seconds of simulated time each). That is short, and the seeds are draws from one generator.
- VPIN here is the simple version: the caller supplies trade signs instead of bulk volume classification, there is no default bucket volume, and the trailing incomplete bucket is dropped. Realized spread requires a horizon argument. (python/lobkit/features.py)
- The SimResult path array is a conversion of 128 bit sums to 64 bit and raises OverflowError when a value does not fit. Exact totals are in the pnl dictionary. (PYTHON.md)
- Arrays returned by the bindings are read-only. An Engine and its adapters are not thread safe.
- The Python sanitizer job runs without leak checking, so leaks in the bindings are not covered. (TESTING.md)
- Python is built through CMake. There is no pip package.

## 6. Performance numbers

- All timings are VM measurements. The machine is a KVM guest with 4 vCPUs, no isolated cores and no control over frequency scaling. Absolute levels will differ on bare metal. (PERFORMANCE.md section 1)
- All benchmark workloads are synthetic. Nothing describes behavior on a real feed. (PERFORMANCE.md section 2)
- Noise floor: the same binary run against itself differs by up to about 20 percent in the median of one scenario. Changes smaller than that are inconclusive. (PERFORMANCE.md section 2)
- Per-operation latency percentiles include about 25 ns of timer cost and are measured with the pipeline serialized, so they are not the latency of a message arriving at an idle engine. Throughput comes from batch timing, in which operations overlap. The p99.9 and the maximum are dominated by hypervisor and scheduler events in this VM. (PERFORMANCE.md section 2)
- The baseline is one reasonable std::map design, not the best possible one. (PERFORMANCE.md section 7)
- Hardware performance counters are not available in the VM. The cache and branch figures are simulated by valgrind, and the flame graph is from software sampling, which can skid. (PERFORMANCE.md section 5)
- Layout effects of 20 percent or more between independently built binaries were observed, which means small source changes cannot be resolved on this VM. (PERFORMANCE.md section 6)
- Ring buffer throughput is only ever quoted as a range with the VM label: it varied by a factor of four on the same code in this VM. The comparison queues are the author's own. (INFRA.md sections 4 and 7)
- Open item: the end-to-end LOBSTER replay (4.7 million messages per second) is less than a third of the in-memory apply benchmark (16.6 million per second), and the gap is not explained. (PERFORMANCE.md section 8)
- No full-day real replay throughput exists. The largest replay is a synthetic 1,000,000 message LOBSTER file and a 3,000,000 frame ITCH file. (PERFORMANCE.md section 4, MARKET_DATA.md section 8)
- Nothing was optimized in Phase 7. Every attempt failed to beat the noise floor. The id hash experiments are unadopted future work pending real ids. (PERFORMANCE.md sections 6 and 8)
- Bare metal reruns with isolated cores are not done and are listed as a TODO with exact commands.

## 7. Verification

- The differential test, the mutation tool and the fuzz targets are by the same author as the engine. The mutation tool is a fixed list of 28 hand written mutants (26 killed, 2 equivalent), not a full mutation analysis, and it is run by hand. (TESTING.md)
- Fuzzing was run for minutes per target, not hours. No crash was found on the explored inputs, which is not a proof of absence. (TESTING.md)
- Coverage is line and branch coverage, which shows that code ran, not that its behavior was checked. Python code, benchmarks, tools and fuzz targets are not measured. (TESTING.md)
- Tested on Linux x86-64 with GCC 13.3 and Clang 18.1 only. No macOS, no Windows, no ARM. The benchmark timer uses rdtsc and falls back to a plain clock off x86-64, untested.
- The allocation test is skipped under Clang ThreadSanitizer, whose runtime replaces operator new. (tests/CMakeLists.txt)
- clang-tidy is configured and not run in CI. (DESIGN.md section 11)
- The libFuzzer targets need Clang. The GCC build replays the committed corpus through the same entry points instead of fuzzing.
