# Performance

Every number in this file is a VM measurement: a KVM guest on a shared host, 4 vCPUs, no isolated cores, no control over the frequency governor. Every workload is SYNTHETIC. No real market data was available (the data hosts are blocked in the environment this was built in), so nothing here describes behavior on a real feed. No number is a single run: each is a median with its range over many repetitions, taken in interleaved rounds. Treat absolute values as indicative of this VM only, and trust ratios and orderings more than levels. A bare metal rerun is listed as a TODO at the end, with exact instructions.

## 1. Environment

| Item | Value |
|---|---|
| CPU as seen by the guest | Intel Xeon Processor @ 2.10 GHz, 4 vCPUs, 1 thread per core, hypervisor KVM |
| Caches as reported | L1d 48 KiB and L2 2 MiB per vCPU, L3 reported as 260 MiB shared (what the hypervisor tells the guest; not a statement about the host) |
| Kernel | Linux 6.18.44 (Firecracker style guest) |
| Governor | not exposed by the VM, so unknown and uncontrollable |
| Transparent huge pages | madvise (the benchmarks do not request them; section 6 has an experiment) |
| Compiler and flags | GCC 13.3.0, CMake Release: `-O3 -DNDEBUG`, no `-march=native`, no LTO. Warnings as errors, as everywhere in the repository |
| Benchmark library | Google Benchmark 1.9.1 (pinned) with manual timing |
| Process | one benchmark process at a time, pinned to vCPU 2 (`LOBKIT_BENCH_CPU`, default 2), page cache warm |
| TSC | invariant: cpuid leaf 0x80000007 bit 8 set, cpuinfo flags constant_tsc and nonstop_tsc, clocksource is tsc. Calibrated against CLOCK_MONOTONIC_RAW over three 150 ms windows: 2.099995 to 2.099999 ticks per ns |
| Cost of reading the timer | `lfence; rdtsc; lfence` back to back: 22.9 ns minimum, 25.7 ns median |
| perf | hardware counters are not supported in this guest (perf stat prints not supported for cycles, instructions, cache-misses and branch-misses). Software sampling with `-e cpu-clock` works |

`bench/env.sh` prints this description, and the benchmark prints the timer facts into its context block, so they travel with every JSON file.

## 2. Methodology

### Workloads

All are generated deterministically (bench/book_scenarios.hpp, with the shared generator in tests/support/workload.hpp). Prices sit around a fixed mid with a geometric-like distance and a fat tail. The ladder has 16,384 levels and the order pool 524,288 slots (so the id table is 1,048,576 slots of 16 bytes, 16 MiB).

| Scenario | What is timed |
|---|---|
| add | 200k non-crossing limit adds into a book holding 100k orders |
| cancel | 200k cancels in random order from a book of 300k orders |
| modify_reduce | 200k in-place quantity reductions (priority kept), random order |
| modify_reprice | 200k price changes (cancel and replace, priority lost), random order |
| match_N, N = 1, 4, 16, 64 | one IOC order that consumes exactly N price levels of 4 orders each. Setup is repeated so that every run has about 4000 samples |
| mixed | 500k messages after 100k that build the book: limit 45 percent, market 3, IOC 5, FOK 3, post-only 6, cancel 18, modify 20, owners 0 to 3. Many cancels and modifies target ids that are already gone, as in the shared test generator |
| mixed_strided_ids | the mixed scenario with every id multiplied by 1024, to probe the id hash |
| feed_replay | 500k messages from the synthetic feed generator (add, reduce, delete, execute) through the replay adapter after 100k to build the book |

The mixed workload is not derived from real data because none was supplied. When real LOBSTER or ITCH data exists, re-derive it (TODO below) and do not reuse these numbers.

### Baseline

bench/std_map_book.hpp is a separate, minimal book: one `std::map` per side holding one `std::list` of orders per price, plus an `unordered_map` from id to list iterator. It is not the ReferenceBook in tests/support, which is deliberately O(n) and so could not be compared fairly. The baseline is the straightforward design with O(1) cancels through stored iterators and a reserved id table. Differences from the engine are listed at the top of the file: one heap allocation per order, tree descent per level, no ladder and no bitmaps.

Fairness is checked, not assumed: before any measurement, `bench_book` replays every scenario plus a 400k operation workload that includes invalid input and all five order types through both books with a hashing sink and aborts if the event streams differ by a single byte. They are identical. The baseline does not implement the own order queue rules for feed executes (MATCHING_RULES.md section 9) because no benchmark mixes own orders with feed operations.

### Measurement

Each repetition of a benchmark does three things on freshly built books: a warm-up run that is discarded, a throughput run with one timer read around the whole batch (reported as million operations per second), and a latency run with a timer read around every operation (percentiles in nanoseconds). Preparing the book is never timed. The page fault count inside the timed loops is recorded: it is 0 in every row, because glibc is told to keep freed memory in the heap (mallopt), so the warm-up leaves pages mapped. A first touch page fault would otherwise dwarf any book operation.

Read the two kinds of figure differently:

- Throughput comes from batch timing. Consecutive operations overlap in the out-of-order core, so cache misses of different operations overlap.
- Latency percentiles come from per-operation timing with lfence on both sides of the timer read. That serializes the pipeline, so nothing overlaps, and every value includes about 25 ns of timer cost (not subtracted). That is why the p50 is typically 2 to 3 times the reciprocal of the throughput. Both are real, and neither is the latency of a message arriving at an idle engine on a real system.

Repetitions are taken in alternating rounds with random interleaving inside the binary (`--benchmark_enable_random_interleaving`), so drift on the shared host hits the engine and the baseline alike. Final numbers use 20 repetitions per row (4 rounds of 5). Cells are median [min, max].

The tail columns (p99.9, max) are flagged with an exclamation mark when the maximum exceeds 3 times the median. In this VM they usually are: the maximum values of tens of microseconds to several milliseconds are scheduler and hypervisor events, not book behavior, the same tail noise seen in Phase 3 (docs/INFRA.md). The p99.9 is mostly trustworthy only where it is not flagged.

### A methodology error found and fixed

The first run of the modify scenarios visited orders in id order. That let the baseline unordered_map, whose hash is the identity, walk memory sequentially, and it showed the baseline beating the engine on in-place modifies (54 against 28 million operations per second). Visiting the orders in random order, as the cancel scenario already did, removed the artifact: baseline 16, engine 24 in that run. The superseded run is kept in docs/results/phase7/superseded_modify_sequential_ids.json. Section 6 shows that id order matters this much, which is why the lesson is recorded.

### Noise floor

The same binary was run against itself in the same interleaved way (4 rounds of 5 repetitions; docs/results/phase7/attempts/aa_noise). The medians of throughput differed by up to 19 percent between the two copies (match_4), 5 to 14 percent in most rows, and the p50 by up to 20 percent. So any change of less than about 20 percent in one scenario is inconclusive on this VM, and an optimization needs a consistent direction across scenarios as well. Section 6 applies that rule.

## 3. Results: engine versus std::map baseline

Data: SYNTHETIC. Environment: section 1 (VM). 20 repetitions per row. Raw JSON: docs/results/phase7/final/final.json. Regenerate with `bench/summarize.py`.

Repetitions per row: 20. Cells are median [min, max]. `!` marks max > 3x median (VM tail noise).

| scenario | implementation | Mops/s | p50 | p90 | p99 | p99.9 | max | page faults |
|---|---|---|---|---|---|---|---|---|
| add | engine | 17.4 [14.8, 20.4] | 148.1 [100.0, 158.1] | 198.1 [178.1, 244.8] | 403.8 [388.6, 439.0] | 540.5 [481.9, 2951.4] ! | 50036 [31373, 2384125] ! | 0 |
| add | stdmap | 12.4 [7.5, 13.0] | 108.6 [107.6, 159.0] | 139.5 [126.7, 195.2] | 332.4 [305.7, 476.2] | 603.3 [530.5, 2733.3] ! | 46271 [26260, 1152237] ! | 0 |
| cancel | engine | 14.3 [11.1, 19.5] | 209.0 [161.0, 263.8] | 340.0 [258.1, 424.8] | 535.7 [461.9, 658.1] | 765.2 [631.4, 3214.3] ! | 130659 [34702, 3110126] ! | 0 |
| cancel | stdmap | 3.7 [2.4, 5.4] | 430.5 [318.1, 538.1] | 646.2 [512.4, 841.0] | 884.3 [738.1, 1201.0] | 4979 [1326, 17927] ! | 159248 [34650, 831155] ! | 0 |
| modify_reduce | engine | 19.2 [8.6, 22.6] | 240.5 [191.4, 290.5] | 375.2 [299.0, 457.1] | 550.5 [496.2, 647.6] | 791.0 [689.5, 18967.6] ! | 101891 [33307, 1248084] ! | 0 |
| modify_reduce | stdmap | 13.4 [6.5, 19.1] | 404.3 [324.8, 461.9] | 611.4 [528.6, 679.0] | 811.0 [732.4, 898.1] | 3873 [1557, 4875] | 105482 [39387, 1960832] ! | 0 |
| modify_reprice | engine | 7.4 [4.5, 10.6] | 223.3 [162.9, 331.4] | 357.1 [249.5, 509.5] | 547.1 [445.7, 2555.2] | 1259 [575, 4240] ! | 96097 [35332, 3063274] ! | 0 |
| modify_reprice | stdmap | 2.3 [2.0, 2.6] | 474.8 [395.2, 732.4] | 696.7 [603.8, 1008.6] | 949.0 [840.0, 1437.1] | 5178 [3957, 19527] ! | 157774 [37325, 1632949] ! | 0 |
| match_1 | engine | 8.1 [3.9, 11.3] | 144.3 [136.2, 267.6] | 173.3 [153.3, 392.4] | 327.1 [202.9, 581.0] | 534.3 [421.9, 6750.5] ! | 21445 [759, 37995] | 0 |
| match_1 | stdmap | 7.2 [5.1, 7.8] | 158.1 [149.5, 221.0] | 206.7 [197.1, 298.1] | 321.4 [278.1, 462.9] | 681.9 [447.6, 5393.3] ! | 19868 [2389, 214556] ! | 0 |
| match_4 | engine | 2.0 [1.2, 3.3] | 497.6 [357.1, 814.3] | 694.8 [398.1, 1033.3] | 890.5 [569.5, 1319.0] | 6708 [856, 31830] ! | 28991 [20228, 523934] ! | 0 |
| match_4 | stdmap | 2.0 [1.4, 2.0] | 503.8 [481.9, 700.0] | 611.0 [555.2, 925.7] | 861.4 [721.9, 1332.4] | 7614 [4947, 29924] ! | 28530 [17444, 127873] ! | 0 |
| match_16 | engine | 0.8 [0.6, 0.9] | 1218 [1165, 1273] | 1304 [1223, 2078] | 2185 [1439, 4339] | 25028 [22336, 34580] | 35096 [29309, 291489] ! | 0 |
| match_16 | stdmap | 0.5 [0.3, 0.5] | 1988 [1889, 3185] | 2460 [2167, 4797] | 4724 [3173, 6579] | 28520 [19908, 135564] ! | 44710 [26450, 16133724] ! | 0 |
| match_64 | engine | 0.2 [0.1, 0.2] | 4708 [4444, 8050] | 5680 [4719, 8760] | 13080 [8442, 29904] | 44198 [31170, 171168] ! | 116112 [41267, 727452] ! | 0 |
| match_64 | stdmap | 0.1 [0.1, 0.1] | 8057 [7790, 12312] | 11095 [9070, 13440] | 27871 [23561, 38143] | 48067 [34032, 104097] | 78607 [44981, 2509620] ! | 0 |
| mixed | engine | 15.7 [9.7, 17.0] | 156.2 [150.5, 170.5] | 250.5 [237.1, 301.9] | 439.5 [427.6, 497.1] | 700.0 [590.5, 2831.4] ! | 180670 [40032, 7462747] ! | 0 |
| mixed | stdmap | 11.8 [10.2, 12.5] | 104.8 [102.9, 106.7] | 213.8 [203.8, 221.9] | 440.5 [408.6, 472.4] | 1568 [1287, 3195] | 150873 [35408, 2088650] ! | 0 |
| mixed_strided_ids | engine | 17.2 [10.1, 19.4] | 148.6 [129.5, 169.5] | 231.4 [208.6, 300.0] | 429.0 [410.5, 2206.7] | 611.0 [534.3, 3370.5] ! | 156571 [38229, 2893660] ! | 0 |
| mixed_strided_ids | stdmap | 10.8 [8.0, 11.7] | 113.8 [109.5, 148.6] | 235.7 [221.9, 318.1] | 471.0 [422.9, 665.7] | 1803 [1501, 3810] | 163135 [33116, 1409700] ! | 0 |
| feed_replay | engine | 16.6 [11.1, 19.2] | 156.2 [146.7, 181.0] | 269.0 [241.0, 319.0] | 448.1 [425.7, 515.2] | 645.2 [561.9, 3561.9] ! | 112517 [34726, 2440569] ! | 0 |
| feed_replay | stdmap | 9.9 [6.2, 10.5] | 121.0 [117.1, 145.7] | 274.8 [256.2, 327.6] | 488.1 [431.4, 569.5] | 750.5 [659.0, 3547.6] ! | 88867 [38345, 2200355] ! | 0 |

Throughput ratio (engine median / stdmap median) and p50, p99 ratios (stdmap / engine):

| scenario | throughput x | p50 x | p99 x |
|---|---|---|---|
| add | 1.40 | 0.73 | 0.82 |
| cancel | 3.81 | 2.06 | 1.65 |
| modify_reduce | 1.43 | 1.68 | 1.47 |
| modify_reprice | 3.27 | 2.13 | 1.73 |
| match_1 | 1.13 | 1.10 | 0.98 |
| match_4 | 1.03 | 1.01 | 0.97 |
| match_16 | 1.77 | 1.63 | 2.16 |
| match_64 | 1.80 | 1.71 | 2.13 |
| mixed | 1.32 | 0.67 | 1.00 |
| mixed_strided_ids | 1.59 | 0.77 | 1.10 |
| feed_replay | 1.67 | 0.77 | 1.09 |

How to read this honestly:

- Throughput: the engine is ahead in every scenario except match_4, which is a tie (1.03), and ranges from 1.13 times (match_1) to 3.8 times (cancel), 3.3 times (modify_reprice). Cancels and cancel and replace are where the baseline pays for tree and allocator work.
- Per operation median latency is mixed. On add, mixed and feed_replay the baseline has the lower p50 (the ratio column is below 1: 0.73, 0.67 and 0.77, and 0.77 on mixed_strided_ids), even though the engine has the higher throughput. Section 6 shows an experiment that points at the id hash as the cause: with synthetic ids that are dense and sequential, the baseline hash (identity) keeps recently used ids on the same cache lines, and the engine hash (Fibonacci) scatters them. Real order ids may not be dense, so this result may not transfer in either direction.
- p99 is within about 20 percent in the add, mixed, feed_replay and match_1/4 rows and 1.5 to 2.2 times better for the engine where the baseline walks trees or frees nodes (cancel, modify, match_16 and match_64).
- The match rows scale with the number of levels as expected: about 4.7 microseconds p50 for 64 levels (256 fills), roughly 18 ns per fill in the serialized measurement.

## 4. Throughput on the largest replay available

Source: SYNTHETIC files generated with scripts/make_synthetic_lobster.py (1,000,000 messages, seed 3, 40 MB) and scripts/make_synthetic_itch.py (3,000,000 frames, seed 3, 99 MB, of which 2,910,592 are AAPL messages), the same recipe as Phase 4. These are the largest replays that exist here. No real full-day file has been replayed. Tool: `build/release/lobkit_replay`, as fast as possible, engine pool 1,048,576, 65,536 levels, NullSink, pinned with taskset to vCPU 2, page cache warm, 9 interleaved rounds. VM.

| Measurement | Million messages per second, median [min, max] |
|---|---|
| LOBSTER parse only, 1,000,000 messages | 14.6 [10.8, 15.1] |
| LOBSTER replay, parse and apply | 4.7 [4.2, 4.9] |
| ITCH parse only, 2,910,592 messages | 46.1 [39.4, 49.9] |
| ITCH replay, parse and apply, symbol AAPL | 6.5 [6.0, 6.9] |

Raw output: docs/results/phase7/replay_throughput.txt. These agree with the Phase 4 figures (replay 5.3 for LOBSTER and 6.5 for ITCH in that run).

The in-memory benchmark applies the same kind of messages at 16.6 million per second (feed_replay, engine), more than three times the end to end LOBSTER replay rate. I have not fully explained the gap. A perf profile of the replay tool (software sampling) puts about 47 percent of the samples in applying messages and at least 16 percent in the named parsing functions (more is inlined into the loop), with the rest in start-up and page faults, and parse and apply do not overlap because the next message cannot be parsed until the previous one has been read. The replay tool also uses a larger book (pool and id table 32 MiB each, against 16 MiB in the benchmark). Reducing the pool to 262,144 orders raised the replay rate to 5.6 [4.6, 6.4] (9 rounds, interleaved with the default 4.9 [4.5, 5.2]; ranges overlap, so this is suggestive only). Pools of 65,536 and 32,768 cannot hold this synthetic session (PoolExhausted rejects), so they are not comparable. Raw: docs/results/phase7/replay_max_orders.txt.

## 5. Where the time goes

Hardware counters are unavailable, so these are the available substitutes. Both are labeled for what they are.

Sampling profile (perf record, `-e cpu-clock`, 5 kHz, software timer, so skid is possible): flame graph docs/results/phase7/flame_engine_mixed.svg for engine/mixed. About 55 percent of samples are in `submit_impl` (everything the engine does for a new order is inlined into it), about 15 percent in `apply` (cancel and modify paths, inlined), about 15 percent building the book, which is part of setup and not of the timed operations. Line level attribution inside submit_impl puts 70 to 84 percent of its samples on the first load of an id map slot (include/lobkit/book/id_map.hpp, the duplicate check), and in `remove_order` 69 percent on the id map probe loop of `erase`. The profile includes the untimed setup that applies 100k to 300k adds, which also hit the id map, so these shares overstate the timed part. Still, the direction is clear: the first touch of a random slot in a 16 MiB table is the most expensive step of most operations.

Simulated cache and branch counters (valgrind callgrind with `--cache-sim=yes --branch-sim=yes`, collecting only inside run_once, so setup and timed operations together, 600,000 operations; the simulator models a 48 KiB 12-way L1 data cache and a 276 MB last level cache taken from this VM, so it says nothing about real hardware). Per operation:

| Counter per operation | engine | std::map baseline |
|---|---|---|
| instructions | 189 | 394 |
| data reads / writes | 50 / 38 | 111 / 71 |
| L1 data read misses | 2.9 | 3.7 |
| L1 data write misses | 1.7 | 0.7 |
| conditional branch mispredicts | 1.4 | 2.9 |
| indirect branch mispredicts | 0.54 | 1.35 |

The engine executes about half the instructions and mispredicts about half the branches of the baseline, but takes a similar number of L1 misses, which is why its advantage is a factor of 1.3 to 1.4 on mixed rather than 2. The mispredicts are largely the random operation mix itself (the type switch and data dependent branches in a random workload) and would be lower on a feed with more regular structure; I did not test that.

Exact commands to collect real hardware counters and a flame graph on a machine that has them are in the TODO.

## 6. Optimization attempts, before and after

Rule applied: an attempt is adopted only if it improves the target scenarios by more than the noise floor (about 20 percent) in a consistent direction without making others worse. Method for each: build the variant to its own binary, run it against the unmodified binary in alternating rounds (3 to 4 rounds of 5 repetitions), compare medians. Full per-scenario tables are in docs/results/phase7/attempts_tables.md and the raw JSON is in docs/results/phase7/attempts/.

Result: NO attempt met the rule. The engine source is unchanged by Phase 7. The summary below gives the range of the median change over all scenarios measured (throughput, then p50 latency; negative throughput and positive latency are worse).

| Attempt | Why it was tried (measurement that justified it) | Throughput change, range over scenarios | p50 change, range | Outcome |
|---|---|---|---|---|
| 32 bit division in price to index | -9% to +19% | -13% to +15% |
| lookahead prefetch of id slot, 4 ahead | -28% to +3% | -1% to +48% |
| lookahead prefetch, 8 ahead | -19% to +12% | -1% to +38% |
| lookahead prefetch, 16 ahead | -21% to +4% | -2% to +41% |
| transparent huge pages for id map and order pool | -12% to +14% | -29% to +15% |
| prefetch next maker id slot during matching | -13% to +11% | -15% to +10% |
| -march=native | -25% to +6% | +1% to +27% |
| -flto | -20% to +6% | -4% to +22% |
| -O2 instead of -O3 | -22% to +12% | -11% to +31% |
| Clang 18 instead of GCC 13 | -23% to +7% | -8% to +18% |
| id hash keeps nearby ids nearby (full locality) | -56% to +135% | -64% to -20% |
| id hash with groups of 2 ids | -19% to +7% | -27% to +18% |
| groups of 4 ids | -25% to +9% | -56% to +35% |
| groups of 8 ids | -41% to +28% | -56% to +42% |
| groups of 16 ids | -47% to +52% | -56% to +69% |

Why each was tried and what the result means:

1. 32 bit division in `to_index`. The profile showed a 64 bit `divq` in submit_impl. Changes are mixed in sign and inside the noise floor (A/A shows up to 19 percent): no effect. The division is hidden behind cache misses. Not adopted.
2. Lookahead prefetch of the id map slot (distance 4, 8, 16). The profile points at the id map miss. This needs a new method on the public Engine (prefetch_id), so it was only run as an experiment (docs/results/phase7/prefetch_experiment.patch) and not adopted: it made add p50 about 40 percent worse and cancel, modify_reprice and match_4 throughput 15 to 28 percent worse, and nothing improved. I do not know why a prefetch hurts; one possibility is that it competes for the same line fill buffers, which I did not verify.
3. Transparent huge pages (madvise) for the id table and the order pool, to test whether TLB misses were behind the id map cost. Confirmed active (32 MiB reported as AnonHugePages), no consistent gain: the TLB hypothesis is rejected.
4. Prefetching the id slot of the next maker while matching (docs/results/phase7/match_prefetch_experiment.patch). The profile put 69 percent of remove_order on the id map erase. No gain; match_4 and match_16 slightly worse.
5. Compiler options: -march=native, -flto, -O2 and Clang 18. None helps. All four variants, which were built separately, show match_4 20 to 25 percent slower than the reference binary, which suggests that the reference binary has a favorable code layout. That is itself a finding: layout effects of this size mean that small source changes cannot be resolved on this VM.
6. Id hash that keeps nearby ids on nearby slots. Justified because the baseline gets exactly that for free from std::hash and its p50 is lower on add, mixed and feed_replay (section 3). Full locality (low bits kept): add throughput 2.4 times and p50 64 percent lower, mixed 1.4 times, but cancel 56 percent slower, feed_replay 26 percent slower, and modify_reprice did not finish one repetition in 60 seconds (dense sequential ids form one long contiguous cluster of the open addressing table and every erase shifts it), so that row is missing from its table. Not adopted: pathological.
7. Grouped variant of 6: 2, 4, 8 or 16 consecutive ids share a randomly placed group of slots (docs/results/phase7/idmap_group_experiment.patch). Groups of 4 and larger cut p50 on add, mixed and feed_replay by 34 to 57 percent and raise mixed throughput 9 to 23 percent, but cancel loses 25 to 47 percent, match_16 and match_64 20 to 40 percent and modify_reprice 15 to 38 percent. The erase cost grows because consecutive ids now sit in a contiguous run. Not adopted. It is a trade between add heavy and cancel or match heavy flows.

What this suggests, without proving it: on dense sequential ids the id hash decides much of the per-operation latency, and a different delete scheme (for example tombstones, or erasing without a backward shift) combined with a locality preserving hash might win both ways. That should be evaluated on real order ids first, because the benefit exists only if real ids are dense and recently used ids are looked up again soon, and a hash that exploits structure in ids has a worst case on ids with the wrong structure. I did not pursue it further without real data.

## 7. What these numbers cannot say

- Nothing about a real feed: synthetic order flow has no clustering, no bursts and no realistic depth profile.
- Nothing about bare metal: the VM adds noise to every tail, hides the governor, and may share caches with other guests. Absolute levels will differ.
- Not a latency to a client: no network, no kernel bypass, and the timer cost and pipeline serialization are inside the percentiles.
- The baseline is one reasonable standard library design, not the best possible one (a flat sorted vector of levels or a hand made pool allocator would be faster than the baseline and closer to the engine).
- The engine was not tuned by this phase; the table in section 3 describes the Phase 1 implementation.

## 8. TODO: rerun on bare metal with isolated cores

On a Linux machine you control (not a VM), as root:

    # 1. Isolate two cores (example: 2 and 3) at boot, then reboot
    #    kernel command line: isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0,1
    # 2. Fixed frequency, no turbo, SMT siblings of 2 and 3 offline
    cpupower frequency-set -g performance
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo     # Intel; AMD: echo 0 > /sys/devices/system/cpu/cpufreq/boost
    # take the hyperthread siblings of cores 2 and 3 offline (see /sys/devices/system/cpu/cpu2/topology/thread_siblings_list)
    # 3. Build
    cmake --preset bench && cmake --build --preset bench
    # 4. Record the machine
    bench/env.sh | tee env.txt; lscpu >> env.txt; uname -a >> env.txt
    # 5. Engine versus baseline, interleaved, pinned
    LOBKIT_BENCH_CPU=2 python3 bench/ab_run.py --out results --rounds 6 --reps 5 --filter . final=build/bench/bench/bench_book
    python3 bench/summarize.py results/final.json
    # 6. Hardware counters, one scenario at a time (any of engine/mixed, engine/cancel, stdmap/mixed ...)
    LOBKIT_BENCH_PROFILE=engine/mixed:50 perf stat -e cycles,instructions,branches,branch-misses,L1-dcache-load-misses,LLC-load-misses,dTLB-load-misses taskset -c 2 build/bench/bench/bench_book
    # 7. Flame graph (build with -g -fno-omit-frame-pointer first: cmake -S . -B build/fp -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS='-g -fno-omit-frame-pointer' -DLOBKIT_BUILD_BENCH=ON -DLOBKIT_BUILD_TESTS=OFF)
    LOBKIT_BENCH_PROFILE=engine/mixed:80 perf record -F 4999 -g -o fp.data taskset -c 2 build/fp/bench/bench_book
    perf script -i fp.data | stackcollapse-perf.pl | flamegraph.pl > flame.svg      # https://github.com/brendangregg/FlameGraph
    # 8. Replay throughput
    taskset -c 2 build/release/lobkit_replay --lobster FILE_message_10.csv

Then compare: with isolated cores the tail columns should lose most of the flagged noise, the A/A noise floor should shrink (rerun ab_run with the same binary twice to measure it), and the id hash experiments in section 6 are worth repeating because bare metal has real cache sizes and memory latency. Replace the synthetic mixed scenario with one derived from the real data: replay a real LOBSTER message file, record the operation stream as Op records, and use that as the setup and measured vectors in bench/book_scenarios.hpp, keeping the same harness.

## 9. Reproducing this document

    cmake --preset bench && cmake --build --preset bench
    python3 bench/ab_run.py --out docs/results/phase7/final --rounds 4 --reps 5 --filter . final=build/bench/bench/bench_book
    python3 bench/summarize.py docs/results/phase7/final/final.json
    python3 bench/summarize.py --compact before.json after.json        # for an optimization attempt
    # A/A noise floor: pass the same binary twice, for example A=build/bench/bench/bench_book B=build/bench/bench/bench_book

`bench_book` prints the equivalence check result at start-up. `LOBKIT_BENCH_CPU` selects the pinned vCPU. `--no-equivalence-check` skips the check for repeated runs.
