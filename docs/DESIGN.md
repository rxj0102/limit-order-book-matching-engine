# lobkit design

Status: APPROVED with revisions (Phase 0), revision 2. Section 15 records the decisions.
All performance statements below are hypotheses to be tested, not results.

## 1. Goals and non-goals

### Goals

1. A correct, deterministic, single-instrument price-time priority matching engine in C++20 with no allocation, no virtual dispatch, and no floating point on the hot path.
2. Correctness evidence that is stronger than example tests: a slow reference book, millions of differentially tested random operations, an invariant checker, fuzzing, sanitizers.
3. Honest, reproducible performance numbers, including failed optimizations, measured against a std::map baseline in the same harness.
4. A market data replay path (LOBSTER, and a documented ITCH 5.0 subset) that rebuilds the book and checks itself against recorded snapshots.
5. A backtester with an explicit latency and queue-position fill model, running an Avellaneda-Stoikov market maker, with a clear statement of what biases the results.
6. Python bindings and a small microstructure feature library for analysis.

### Non-goals

- Not a live trading system. No order gateway, no risk system, no persistence, no recovery protocol.
- Not multi-instrument in v0.x. One book per engine instance; multiple books are run as independent instances. Sharding is a roadmap item.
- Not a nanosecond-record claim. Numbers will be measured on whatever hardware the CI or I have, and labeled as such.
- No hidden or iceberg orders, no auctions, no stop orders, no pegged orders, no market-by-price-only feeds. Listed in LIMITATIONS.md.
- No market impact model. Replayed history does not react to the strategy (see section 9).
- No GPU, no kernel bypass, no network code.

## 2. Architecture

```
                 offline / analysis                         hot path (single thread per book)
   +------------------------------------+      +-------------------------------------------------+
   | python/ (pybind11, numpy)          |      |                                                 |
   |  sweeps, plots, features           |      |   +-----------+  Order msgs   +--------------+  |
   +----------------+-------------------+      |   | md/       |-------------->|              |  |
                    |                          |   | LOBSTER   |               |  book/       |  |
                    v                          |   | ITCH sub  |               |  Engine<Sink>|  |
   +----------------+-------------------+      |   | mmap      |               |  match,      |  |
   | sim/                               |      |   +-----------+               |  cancel,     |  |
   |  backtester, latency queues,       |<-----+         ^                     |  modify      |  |
   |  fill model, A-S strategy, PnL     |  events        | replay modes        |              |  |
   |  strategy orders --------------------------------+  | (afap, paced)       +------+-------+  |
   +------------------------------------+      |       |  |                            |          |
                                               |       +--+                     Event stream       |
                                               |                                      v          |
   +------------------------------------+      |                          +-----------------+    |
   | infra/ SPSC ring (optional glue)   |<-----+--------------------------| Sink (template) |    |
   +------------------------------------+      |                          +-----------------+    |
                                               +-------------------------------------------------+

   book/ depends on nothing but the C++ standard library.
   md/ and sim/ depend on book/. python/ depends on all of them. infra/ is standalone.
```

### Repository layout

```
include/lobkit/{book,infra,md,sim}/   public headers
src/                                  non-template implementation
tests/  fuzz/  bench/  python/  docs/  scripts/  data/synthetic/
cmake/  CMakePresets.json  .clang-format  .clang-tidy  .github/workflows/
```

Headers live under include/lobkit and non-template implementation under the matching src subdirectory. The engine is a header-heavy template library so the sink inlines.

## 3. Core types and data structures

### 3.1 Fixed point and instrument tick grid

- `Price` is an int64 in the instrument native fixed-point unit (for LOBSTER, dollars times 10000). `Qty` is a uint32 count of shares or lots, `OrderId` is uint64, `Seq` is uint64.
- Each book is configured with an instrument grid: `tick_size` (native units per tick) and `base_price` (native price of ladder index 0). The ladder is indexed in real ticks: `index = (price - base_price) / tick_size`. A one cent stock in LOBSTER units has `tick_size = 100`, so 65536 levels cover $655.36 of intraday range rather than $6.55, which is what indexing in native units would give.
- Orders store the ladder index (uint32), not the native price. Events carry native prices, reconstructed as `base_price + index * tick_size`, so consumers never see the grid.
- The engine rejects a price that is below base, at or above `base_price + num_levels * tick_size`, or not a multiple of `tick_size` from base. The reject reasons are distinct: `PriceOutOfBand` and `PriceOffTick`. The engine counts every such reject in `BookStats`.
- The replay layer picks `base_price` from the first valid price in the file (or from a command line value) minus half the band, aligned to a tick, so the band is centered on the opening price. It reports out-of-band and off-tick counts. In snapshot-verification mode any out-of-band or off-tick message is a hard failure, because the reconstructed book would silently diverge from the reference. In plain replay and in simulation they are counted, reported at the end, and the run is marked as degraded if the count is non-zero.
- Notional and PnL accumulate in int64 or __int128 where overflow is plausible. Overflow checks are asserted in debug builds.

### 3.2 Price ladder

Choice: a dense array of price levels over a configured band of ticks, plus a two-level bitmap for finding the best price.

- The band is `num_levels` ticks starting at `base_price`, with `num_levels` a power of two, default 65536 (configurable). One shared level array serves both sides. This is sound because a price level can hold only one side at a time when the book is uncrossed.
- `Level` is 32 bytes: head index, tail index, total quantity (uint64), order count, plus padding. Two levels per 64 byte cache line, 2 MiB at N = 65536, so the whole ladder fits in a typical L2.
- Two bitmaps (bids and asks), each 64-ary in two levels: 65536 bits is 1024 words, summarized by 16 words. Best price lookup is a count-leading-zeros or count-trailing-zeros over at most 2 words after the summary. Bid best uses highest set bit, ask best uses lowest.
- Best bid and best ask are cached as plain members and updated incrementally, with the bitmap used only when a level empties.
- Prices outside the band are rejected (`PriceOutOfBand`) and counted. The caller chooses the band, see 3.1. A recentering mechanism is not in v0.1 (documented limitation, roadmap).

Alternatives considered:

| Option | Pro | Con | Verdict |
|---|---|---|---|
| std::map<Price, Level> | trivial, unbounded range | node allocation, pointer chasing, poor locality | used only in the reference book and baseline benchmark |
| Dense array + bitmap (chosen) | O(1) level access, no allocation, best price in a few instructions | bounded band, memory proportional to band | chosen |
| Ladder window around touch with recentering | small footprint, cache friendly | recentering complexity, correctness risk | deferred, roadmap |
| Sorted vector of levels | compact | O(n) insert in the middle | rejected |
| Hash map of levels + heap for best | unbounded range | heap ops on every level create and delete, cache misses | rejected |

### 3.3 Orders and per-level FIFO

- `Order` is 32 bytes, alignas(32), so exactly two per cache line and never straddling one: id (8), remaining qty (4), ladder index (4), prev index (4), next index (4), owner id (2), side (1), flags (1), 4 bytes reserved. static_assert on size and alignment.
- Orders live in a fixed pool array, addressed by uint32 index rather than pointer. Indices halve the link size, survive relocation of the pool, and make the layout trivially serializable for the determinism hash.
- Per-level queue is an intrusive doubly linked list through `prev` and `next`. Doubly linked is needed for O(1) cancel from the middle. Append at tail, match from head.
- The pool is a free list threaded through the same array (the `next` field of free slots). Allocation and free are a pop and a push. Capacity is a constructor parameter, exhaustion produces a `Reject(PoolExhausted)` rather than a growth. The pool counts allocated and free slots for the invariant checker.
- Cache line reasoning to be written up in PERFORMANCE.md: the match loop touches the level (one line), the head order (one line), the hash entry on a fill-then-erase, and the next order when advancing. Orders at one level are usually allocated close in time, which gives some adjacency in the pool, but this is not guaranteed and will be measured, not assumed.

### 3.4 Order id lookup

- Open addressing, linear probing, power-of-two capacity, Fibonacci multiplicative hash on the 64 bit id, backward-shift deletion so there are no tombstones and lookup cost does not degrade over a long day.
- Slot is 16 bytes (id, pool index, state) so four slots per line. Capacity is fixed at construction, sized to at most 50 percent load for the configured max live orders. No rehash on the hot path, ever.
- Duplicate live ids are rejected (`DuplicateOrderId`). Ids of fully filled or cancelled orders may be reused by the caller; the engine does not remember dead ids (documented).

### 3.5 Events

- Fixed-size POD event, 48 bytes or less, with a type tag: `Ack`, `Reject`, `Trade`, `Cancel`, `Modify`. Every event carries a monotonically increasing engine sequence number. `Trade` carries taker id, maker id, price, qty, and aggressor side. `Reject` carries a reason code.
- Delivery: the engine is a class template over a `Sink` type that is any callable object with `operator()(const Event&)`, checked with a C++20 concept. The compiler inlines the sink, so there is no virtual call and no std::function. A ring-buffer sink and a vector sink (tests) and a hashing sink (determinism test) are provided.
- Determinism: the engine reads no clock and no global state. Time is an input field on messages and is copied through to events. Same input stream implies byte-identical event bytes; the test hashes the raw event bytes (FNV-1a or similar, not security relevant) and compares across runs and across the reference implementation. Padding bytes in events are zeroed explicitly so the byte hash is meaningful.

## 4. Matching rules (summary; full text goes in MATCHING_RULES.md)

Order types: limit (GTC), market, IOC, FOK, post-only.

- Priority: price, then arrival order within a price. Maker price is the execution price.
- Market: sweeps the opposite side until filled or the book is empty; the remainder is cancelled, never rested.
- IOC: matches what it can at or better than its limit, cancels the rest.
- FOK: checks available liquidity at or better than the limit first (walks levels using the level aggregates, no mutation), and either fills completely or rejects with no state change. The check must be O(levels walked), and the cost is documented.
- Post-only: if it would cross, it is rejected (not slid). Sliding to the touch is a venue-specific variant and is out of scope, noted in the doc.
- Cancel: by id. Cancel of an unknown, filled, or already cancelled id yields `Reject(UnknownOrderId)`.
- Zero quantity: `Reject(InvalidQty)`.
- Modify: new quantity strictly less than current and same price keeps queue position (in-place reduction). Any price change or quantity increase is cancel-and-replace, keeps the order id, and moves the order to the back of the queue at the new price. A modify that would cross follows normal limit-order matching after the replace. This is the conservative convention and is the one most exchanges use for size increases; it is documented as a choice, not a universal.
- Self-trade: a config enum `SelfTradePolicy`. v0.1 implements `Allow` only: self-trades execute and the Trade event carries a self-trade flag (both owners equal and non-zero; owner 0 means the feed and is never a self-trade). `CancelNewest` is reserved for a later release. The market maker avoids crossing its own quotes at the strategy level, not in the engine.

## 5. Memory layout summary

| Structure | Size | Alignment | Reason |
|---|---|---|---|
| Order | 32 B | 32 | two per line, no straddling |
| Level | 32 B | 32 | two per line, one load for head, tail, qty |
| Hash slot | 16 B | 16 | four per line |
| Bitmap L0 / L1 | 8 KiB / 128 B per side | 64 | summary words stay hot in L1 |
| Event | at most 48 B | 8 | fits one line with the sequence number |
| Engine hot members (best bid and ask, counters, seq) | one line | 64 | avoid false sharing with cold config |

Each size is static_assert-ed. If measurement shows that a different layout wins (for example struct-of-arrays for the quantity field), it goes in the optimization log whether it helps or not.

## 6. Threading model

- One engine instance is single threaded and owns all its state. This is a deliberate choice: matching is inherently sequential per instrument, locks would add latency and nondeterminism, and the replay and simulator need exact reproducibility.
- Concurrency exists only at the edges, through the SPSC ring: for example a feed-parsing thread producing messages and an engine thread consuming them, or the engine thread producing events for a logging thread. The core library does not require it; the replay harness runs fine inline.
- SPSC ring (infra/): power-of-two capacity, head and tail each on their own cache line, each side keeps a cached copy of the other side index to avoid reading the shared line on every operation, producer publishes with release, consumer observes with acquire, own-index reads relaxed. Each memory_order gets a justifying comment next to it. Benchmarked against a mutex queue and a naive seq_cst atomic queue. TSan clean and a fixed-duration stress test with sequence checks.
- Thread pinning and isolcpus notes go in PERFORMANCE.md as optional; pinning is used by the benchmarks when the host permits it, and the doc says when it did not.

## 7. Market data

- LOBSTER: message file (time, type 1-5, order id, size, price, direction) and orderbook file (N levels of ask/bid price and size). Replay applies message types: 1 new, 2 partial cancel, 3 delete, 4 execute visible, 5 execute hidden (counted, not applied), 6 cross trade (ignored), 7 halt (noted).
- Book reconstruction goes through `ReplayAdapter` (section 16), which shares the Book structures and the invariant checker with `Engine`. Execute messages name the resting order and do not re-run matching.
- Verification mode: after each message (or every k messages) compare the engine top N levels with the LOBSTER orderbook row. Divergences are counted and the first few are printed with context. Known sources of expected divergence (hidden executions, trading halts, orders from before the file begins) are documented rather than hidden.
- ITCH 5.0 subset: System Event (S), Stock Directory (R, parse and skip), Add Order (A), Add with MPID (F), Order Executed (E), Order Executed with Price (C), Order Cancel (X), Order Delete (D), Order Replace (U), Trade non-cross (P). Others are length-skipped. Big endian fields, bounds checked on every read, no unaligned pointer casts (memcpy plus byte swap).
- Zero copy: file is mmap-ed (read-only, MAP_PRIVATE, sequential advice) and parsed as string_view or span without copying. LOBSTER is text CSV, so a hand-written integer scanner is used, not strtod or iostreams. Where an mmap is unavailable (non-POSIX), a buffered fallback is used. Parsers return a status code; they never throw and never read out of bounds, which is what the fuzz target checks.
- Replay modes: as-fast-as-possible, and timestamp-paced using a monotonic clock with a speed multiplier. Pacing busy-waits on the final microseconds and sleeps for long gaps. Paced mode is for demonstration and latency-sensitive sim tests; it is not used for throughput numbers.
- Data: a script downloads the free LOBSTER sample from the vendor site. Network access from this environment is not guaranteed; if the download is blocked I will say so and leave exact instructions. A tiny synthetic dataset (a few thousand messages with a matching orderbook file, generated by a committed generator seeded deterministically) is committed for CI. Real-data benchmarks are labeled with the file used and are skipped, not faked, if the file is absent.

## 8. Simulator

- Event-driven loop over a time-ordered queue with four event sources: market data messages, strategy timers, order acknowledgements, and market data delivery to the strategy. Two configurable latencies: order latency (strategy to exchange, and ack back) and market data latency (exchange to strategy). The strategy only sees the book state as of (now minus md_delay), and its orders reach the book at (decision time plus order_delay). Deterministic given the seed and config.
- Fill model: strategy orders enter the same engine, joining the back of the queue at their price. They fill only when replayed flow trades through the queue ahead of them. A strategy order never changes what the historical feed does next (no impact). Historical cancels ahead of us advance our position because the engine processes them as real removals. This is the standard optimistic-to-neutral queue model, and its bias (no impact, no adverse reaction of other participants, cancels in front are assumed independent of our presence) is stated in LIMITATIONS.md.
- Because hidden executions and the feed start state are imperfect, the simulator reports how many historical messages could not be applied (referencing unknown ids) so the user can judge fidelity.
- Strategy: Avellaneda-Stoikov with
  - reservation price r = s - q * gamma * sigma^2 * (T - t)
  - total optimal spread delta = gamma * sigma^2 * (T - t) + (2 / gamma) * ln(1 + gamma / k)
  - quotes at r -/+ delta / 2, rounded to ticks, clipped to stay passive, skewed with inventory, with a hard max inventory that suppresses the side that would add exposure.
  - sigma estimated online (EWMA of mid returns), k either configured or fitted offline from trade arrival intensity vs distance, documented as a rough fit.
  - The infinite horizon variant (constant T - t) is the default for intraday backtests; finite horizon is selectable.
- The strategy and PnL accounting use double precision. This is a deliberate, documented exception to the no-floating-point rule, which applies to the engine. Fills crossing the boundary are integer.
- PnL decomposition: spread capture (fill price vs mid at fill time, signed), inventory PnL (position times mid change), fees and rebates (configurable per side and per liquidity role). The three components must sum to total mark-to-market PnL to within integer rounding, and a test asserts that.
- Output: per-run CSV for fills, inventory path, PnL path, plus a JSON file with the exact config and seed for reproducibility.

## 9. Python bindings and analysis

- pybind11 module `lobkit` exposing Engine (with a Python-side event collection into numpy structured arrays), replay, and simulator run with config dataclasses. Outputs are numpy arrays that share memory with C++ buffers (capsule-owned) rather than lists of Python objects. Per-event Python callbacks are not offered on the hot path.
- Features module (pure numpy, tested with pytest against hand computed cases): order flow imbalance (Cont, Kukanov, Stoikov), queue imbalance, microprice, realized spread, and a simple volume-bucketed VPIN with documented bucketing and the usual caveats.
- Analysis script or notebook: parameter sweeps over gamma and latency, PnL curve, inventory distribution, spread capture vs adverse selection, sensitivity plots. Runs on the synthetic data in CI; real-data results only if I can obtain real data here.

## 10. Testing and verification strategy

1. Unit tests per order type and edge case.
2. Reference book: std::map<Price, std::deque<Order>> with linear and obviously correct logic, sharing no code with the engine. A randomized generator (seeded, with biased distributions to hit crossing, ties, cancels of dead ids, boundary prices, modify races) feeds both; event streams must match exactly. Millions of operations per CI run, more in a nightly job.
3. Allocation test: global operator new and delete replaced in a dedicated test executable; any call between a start and stop marker fails the test. The allocation-free claim covers the engine hot path with a sink that does not allocate.
4. Determinism test: hash of event bytes for a fixed seeded stream is compared against a committed golden hash, and across two runs in one process.
5. Invariant checker, compiled in with an option (on in Debug and sanitizer presets, compiled out of Release): uncrossed book, level aggregate equals the sum of its orders, FIFO links consistent in both directions, bitmap bit set iff level non-empty, best prices correct, pool allocated plus free equals capacity, hash map entries equal live orders.
6. libFuzzer targets (clang only): parser targets, and an engine target that decodes arbitrary bytes into operations and runs them with the invariant checker on. A short corpus-seeded smoke run in CI, longer runs manual.
7. Sanitizer CMake presets: asan, ubsan, tsan. TSan applies to the infra tests and any threaded harness.
8. Coverage via gcov or llvm-cov in CI; goal above 90 percent line coverage on the book code. I will report the actual number, and if it is lower I will say so.

## 11. Build, tooling, CI

- CMake 3.25+, presets: `debug`, `release`, `asan`, `ubsan`, `tsan`, `coverage`, `fuzz`, `bench`. Warnings as errors on project targets only (-Wall -Wextra -Wpedantic -Wconversion -Wshadow and so on). Both GCC and Clang. Local environment here has GCC 13.3, Clang 18.1, CMake 3.28, Python 3.13, 4 vCPUs on a virtualized Xeon.
- clang-format and clang-tidy configs committed. clang-tidy runs in CI as a separate job, initially on the book and infra directories.
- GitHub Actions: matrix of GCC and Clang by Debug and Release on Ubuntu, a macOS job marked best-effort, sanitizer jobs, coverage job, benchmark smoke run (tiny iteration count, verifies it runs, does not publish numbers because shared runners are noisy).
- Build flags for benchmarks: -O2 vs -O3, -march=native only in a dedicated benchmark preset and recorded with the results. Release artifacts and defaults do not use -march=native, so builds are portable.

## 12. Benchmark methodology (summary; detail goes in PERFORMANCE.md)

- Google Benchmark for throughput style microbenchmarks. A custom latency harness for per-operation distributions: rdtsc (with lfence or rdtscp ordering) around each operation, warmup pass, results into a preallocated array, percentiles p50, p90, p99, p99.9, max computed offline. TSC to nanosecond conversion calibrated against a monotonic clock, invariant TSC checked.
- Measurement overhead (cost of an empty timed region) is measured and reported alongside, not silently subtracted.
- Environment recording: CPU model, microcode if available, compiler and version, flags, governor, whether the core was pinned, and whether this was a VM. This sandbox is a VM, so frequency scaling control, isolcpus, and clean pinning may be unavailable. If so, the numbers are labeled as VM numbers and the doc says that tail latencies here are dominated by hypervisor noise and should not be compared with bare metal figures.
- Workloads: add only, cancel only, modify, match through N levels (N = 1, 10, 100), mixed workload whose operation mix and price distribution are derived from the real or synthetic data and described with the statistics used.
- Baseline: the std::map plus std::list reference book in the same harness, same inputs, same sink.
- Optimization log: every attempt gets a before and after row with the measured delta, including regressions and no-ops, with the commit hash. perf stat (cache and branch misses) and one flamegraph are included only if perf is usable in this environment; otherwise a TODO with exact commands is left.
- Throughput in million messages per second on a full-day replay requires a full-day file. If only the free sample (one stock, one day, ten levels) is reachable, that is what is reported, and it is labeled as such.

## 13. Milestones and releases

| Milestone | Content | Release |
|---|---|---|
| M0 | This document, repo skeleton, CMake presets, formatting and tidy configs, empty CI | none |
| M1 | Phase 1 engine core, unit tests, invariant checker, allocation and determinism tests | v0.1.0 |
| M2 | Phase 2 reference book, differential tests, fuzz targets, coverage in CI | v0.2.0 |
| M3 | Phase 3 SPSC ring, comparison queues, TSan stress | v0.3.0 |
| M4 | Phase 4 parsers, replay, verification, synthetic data, download script | v0.4.0 |
| M5 | Phase 5 simulator and A-S strategy | v0.5.0 |
| M6 | Phase 6 Python bindings, features, analysis | v0.6.0 |
| M7 | Phase 7 benchmark suite and results, optimization log | v0.7.0 |
| M8 | Phase 8 README, docs, CONTRIBUTING, LICENSE, badges | v1.0.0 candidate |

Each phase ends with: clean build with warnings as errors on GCC and Clang, all tests, sanitizer runs, a conventional-commit history of small commits, and a summary with the measured numbers. I will tag locally and ask before pushing tags, since tags are outward facing.

## 14. Risks

- Real data access: the vendor sample may not be reachable from this sandbox. Mitigation: synthetic data in CI and honest labeling.
- Benchmark noise in a VM: addressed by labeling and by reporting distributions, not single numbers.
- Bounded price band: simple and fast, but a real feed with a wide intraday range or erroneous prints will hit rejects. Mitigation: the band is configurable, the replay reports out-of-band counts, and recentering is on the roadmap.
- Queue position fill model is an approximation and likely optimistic in some regimes; documented, not hidden.

## 15. Decisions (from the Phase 0 review)

1. Dependencies: GoogleTest, Google Benchmark, pybind11 approved. System packages are preferred when found, otherwise CMake FetchContent at pinned commit SHAs. Python related dependencies (pybind11, numpy, matplotlib, pytest) sit behind a CMake option, and the core builds and tests without them.
2. Name stays lobkit.
3. Self-trade is a config enum, Allow by default, CancelNewest later. The engine allows and flags. The market maker guards against crossing its own quotes at the strategy level.
4. Public API is `Engine<Sink>` plus a separate `ReplayAdapter<Sink>`. Both operate on the same Book structures and use the same invariant checker.
5. Layout is include/lobkit/{book,infra,md,sim} with matching src/{book,infra,md,sim}.
6. Price band 65536 levels and pool 1M orders are defaults, both configurable.
7. Commits are pushed to the working branch, tags stay local, and one pull request is opened per phase.

## 16. Replay adapter and own orders

`ReplayAdapter` applies exogenous feed messages to the book without running the matching loop for them: `apply_add`, `apply_reduce`, `apply_delete`, `apply_execute`. Feed orders carry owner id 0. Orders submitted through `Engine::submit` by the simulator carry owner ids of 1 or more and are called own orders.

How own orders interact with `apply_execute` (full text in MATCHING_RULES.md, section 9):

- Own orders rest at the back of the queue at their price, behind every feed order already there. Feed orders that arrive later queue behind them.
- A feed execute names a feed order Y and a quantity q. Own orders ahead of Y at that level are filled first, in queue order, up to q, but only while no feed order sits ahead of them. An own order therefore fills only after every feed order that was ahead of it has drained, which is the statement that real flow traded through its queue position.
- Feed orders are always reduced by exactly the feed quantity, so the feed side of the book stays an exact replica of the feed. Own fills in this step are counterfactual and are not subtracted from Y. The consequence (traded volume can exceed the feed volume) is documented as a bias.
- If a feed order is ahead of an own order when a feed order behind it executes, the replica has diverged from the feed. The own order is not filled, and `queue_order_anomalies` is incremented.
- Feed cancels and reductions ahead of an own order advance its queue position and never fill it.
- A feed add that would cross an own resting order trades with it as an incoming limit order and the feed order is reduced by the traded amount. This is rare, it keeps the book uncrossed, and it is counted in `feed_add_displaced_qty`.
- Adapter calls that name an own order id are rejected with `WrongOwner`.

A Phase 1 test (tests/book/test_replay_own_orders.cpp) pins each of these rules.
