# Testing

How lobkit is checked, and what each check can and cannot tell you.

## Layers

| Layer | Where | What it catches |
|---|---|---|
| Unit tests | tests/book/test_orders.cpp, test_modify.cpp, test_replay_*.cpp | Specified behavior of each order type, edge case and replay rule |
| Primitive tests | tests/book/test_primitives.cpp | Bitmap, id map and pool against standard containers under random operations |
| Invariant checker | include/lobkit/book/invariants.hpp, tests/book/test_invariants.cpp | Structural damage: crossed book, wrong aggregates, broken links, pool leaks. Every failure path has a corruption test that breaks one structure and expects a specific complaint. Every traversal is bounded by the pool capacity or the table size, so corruption (a cycle, a wild link, a full id table) is reported and never hangs; tests cover each of these |
| Differential test | tests/book/test_differential.cpp | Any behavioral disagreement with the reference book |
| Determinism test | tests/book/test_determinism.cpp | Non-reproducible output, accidental semantic change (golden hashes) |
| Allocation test | tests/book/test_no_alloc.cpp | Any heap call during matching, cancel, modify or replay |
| SPSC ring | tests/infra/ | Edge cases (empty, full, wraparound, counter overflow, batches across the end) and a timed two-thread stress test checking order, loss, duplication and torn reads, also run under TSan. See INFRA.md |
| Market data parsers | tests/md/ | Per message type and per error path for LOBSTER and ITCH: truncated, oversized, zero length and unknown frames, overflow, sticky errors, every prefix of the sample files, random bytes. Replay, pacing, verification divergence reporting (each divergence reason has a test) |
| Simulator and strategy | tests/sim/ | Strategy math against hand values and a randomized no-cross property; latency timeline to the nanosecond; queue position and the own-order rules under latency; market view; cancel and post-only races; PnL decomposition on a hand worked case and as an exact integer identity across latencies; inventory limit at six latency settings; determinism. All on SYNTHETIC feeds. See SIMULATOR.md |
| Fuzzing | fuzz/ | Inputs nobody thought of: engine versus reference with invariants, under ASan and UBSan |

## The reference book

tests/support/reference_book.hpp is a std::map of price to std::deque implementation written from docs/MATCHING_RULES.md without sharing code with the engine. It uses native prices, not ladder indices, linear searches, and an unordered_map for liveness. It is slow on purpose. The differential test compares the full event stream after every operation (byte for byte, including sequence numbers), compares the visible state of both books (touch, depth and queue order at every price) every 1009 operations, runs the invariant checker every 997 operations, and compares the statistics counters at the end.

### Independence, and what this test cannot show

The reference book and the engine share an author and a specification. Both were written by the same person from the same document, so a misreading of the rules would appear in both implementations and the differential test would agree with itself. What it does establish is that the optimized data structures (ladder, bitmaps, intrusive lists, pool, id map) implement the documented rules and that nothing diverges over millions of operations. What it does not establish is that the rules are right.

The planned independent check is Phase 4: replaying real LOBSTER messages and comparing the reconstructed book against the vendor orderbook snapshots, which were produced by a different system. Until that passes, the semantics in MATCHING_RULES.md should be read as the documented intent, not as a validated model of any venue. The unit tests that assert exact expected values, written separately from the reference, are a partial defense in the meantime.

## Does the differential test actually detect bugs

A test that never fails proves nothing, so a fixed list of hand written bugs is applied to the engine one at a time and the differential test is run against each. This is a manual sanity tool, not a CI job and not a full mutation analysis.

    python3 -I tools/mutation/run.py --build-dir build/release          # runs every mutant (a few minutes)
    python3 -I tools/mutation/run.py --check                            # only verifies the fragments still match

The mutants live in tools/mutation/mutants.py, each with the exact text it replaces and whether it must be killed or is equivalent. The script exits non-zero if any mutant behaves differently from what the list says. It edits include/lobkit/book/book.hpp in place and restores it afterwards, so run it on a clean working tree and not while building anything else.

Result of the last run (200,000 operations per scenario, GCC 13.3, release): of the 15 engine mutants, all 13 killable ones were killed (one of them by a crash, which the tool counts as killed) and the 2 equivalent mutants survived as expected. The list also holds 13 simulator and strategy mutants (suite sim, run against the AS and Sim tests): ignoring each of the three latencies, flipping the sign of spread capture or fees, removing the inventory cap or not counting working shares against it, leaving own orders in the market view, sending limit instead of post-only orders, flipping the maker flag or the aggressor side, and letting the feed win timestamp ties. All 13 were killed. Three of them survived the first version of the simulator tests (fill notice latency, the maker flag, the tie rule) and tests were added.

| Mutant | Result |
|---|---|
| Resting orders appended at the head instead of the tail | killed |
| Modify keeps priority on a size increase | killed |
| Post-only allowed to trade at an equal price | killed |
| FOK availability off by one | killed |
| Feed execute never fills own orders | killed |
| Feed execute does not flag a feed order ahead of the target | killed |
| Feed execute keeps walking past the first feed order (the early exit removed) | killed |
| Self-trade flag set for the feed owner | killed |
| Market Ack carries the price field | killed |
| IOC remainder reported with the wrong reason | killed |
| Pool exhaustion check skipped for post-only | killed (crash) |
| Feed reduce reports the wrong quantity | killed |
| Aggregate quantity not updated on feed execute | killed |
| Modify event emitted before the old order is removed | survives: equivalent. remove_order emits no events and the Modify event is built from values copied before the removal, so swapping the two statements changes neither the event stream nor the state. |
| Next ask searched from level + 1 after clearing level | survives: equivalent. The bit for the level was cleared on the line above, so a search starting at level or at level + 1 returns the same index. |

## Coverage

Measured with gcov through gcovr, with line merging across template instantiations, on the code under include/lobkit/book and src/book:

    cmake --preset coverage && cmake --build --preset coverage && ctest --preset coverage
    gcovr -r . build/coverage --filter include/lobkit/book/ --filter src/book/ --exclude-throw-branches --merge-lines --print-summary

Without merge-lines, gcovr counts a line once per template instantiation, which makes header-only code look worse or better than it is, depending on which instantiations a test happens to use. The merged number is the one to read. CI fails below 90 percent line coverage. The lines that remain uncovered are the unreachable FOK case in the post-match switch of Book::submit_impl, which exists to keep the switch exhaustive.

Market data code (include/lobkit/md and src/md), same method: 91.8 percent of lines before the name tables were tested; the uncovered lines were the to_string tables (now tested) and a handful of error-handling lines such as a failed mmap and some defensive returns. The CI coverage job prints this figure without gating on it.

## Running longer

    LOBKIT_DIFF_OPS=20000000 ctest --preset release -R Differential

runs 20 million operations per scenario (five scenarios). The default is one million per scenario.

## Fuzzing

The libFuzzer target needs Clang with the compiler runtime installed (on Ubuntu, libclang-rt-dev).

    cmake --preset fuzz && cmake --build --preset fuzz
    mkdir -p /tmp/corpus && cp fuzz/corpus/engine/* /tmp/corpus/
    build/fuzz/fuzz/fuzz_engine /tmp/corpus -max_total_time=300

Without libFuzzer (for example with GCC), the ctest entry fuzz_engine_corpus replays fuzz/corpus/engine and fuzz/regressions through the same entry point. Any crash found by the fuzzer should be minimized and saved in fuzz/regressions together with a regression test.

The seed corpus is generated by fuzz/make_seeds.py. The input format is documented in fuzz/engine_input.hpp.

### Market data parser fuzzing

fuzz_lobster (messages and order book rows, then plain and verified replay with an invariant check) and fuzz_itch (decoder, replay, symbol scan, invariant check) run under ASan and UBSan with committed seed corpora (fuzz/corpus/lobster, fuzz/corpus/itch, made by fuzz/make_md_seeds.py from the synthetic data) and are replayed by ctest like the engine target.

    mkdir -p /tmp/c && cp fuzz/corpus/itch/* /tmp/c && build/fuzz/fuzz/fuzz_itch /tmp/c -max_total_time=420 -max_len=4096

Runs made for Phase 4 (Clang 18, ASan and UBSan, one process each, run at the same time on a 4 vCPU VM, 421 s each):

| Target | Executions | Rate | Findings |
|---|---|---|---|
| fuzz_lobster | 1,297,854 | about 3,100 per second | none |
| fuzz_itch | 2,647,242 | about 6,300 per second | none |

Sanity check that the targets can find a bug: the frame length check in the ITCH decoder and the end-of-input check in the LOBSTER integer scanner were removed one at a time (a one-off manual mutation, as for the engine), and each fuzzer reported a heap-buffer-overflow within the 40 second limit I gave it. The sources were restored afterwards.

These are coverage guided runs of a few minutes. They show no crash on the explored inputs; they do not prove the parsers are free of defects.

## Python tests

pytest runs through ctest (test python_pytest) when the python preset is used. Files in python/tests:

- test_features.py: each feature against hand computed values on tiny inputs, plus input validation.
- test_bindings.py: a synthetic LOBSTER file is simulated through the bindings and through the lobkit_sim_run command line, and the three CSV files are compared byte for byte (both from a file and from an array). The CSV values are also compared with the numpy arrays (timestamps, ids, quantities, inventory, cumulative PnL). Also: dtypes and shapes, read-only zero-copy views that outlive the result name, adopted buffers, input checks, engine and adapter behaviour, and the error paths (missing file, malformed line with line number, bad configuration, unwritable output).
- test_sweep.py: a tiny sweep produces the report, plots and raw CSV, results do not depend on the thread count, and an artifact rate sits beside every table.

Limits: the CLI comparison uses the default strategy parameters because the committed 1000 message file gives no fills with some other parameter sets, which would make a comparison vacuous (the test asserts that fills exist).

### Python under ASan and UBSan

The extension is built with `-fsanitize=address,undefined` (Debug, GCC 13.3, `cmake -S . -B build/pysan -DLOBKIT_BUILD_PYTHON=ON -DLOBKIT_BUILD_TESTS=OFF -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"`) and the pytest suite is run with the runtimes preloaded because the Python interpreter itself is not instrumented:

    ASAN_OPTIONS=detect_leaks=0 LD_PRELOAD="$(g++ -print-file-name=libasan.so) $(g++ -print-file-name=libubsan.so)" PYTHONPATH=build/pysan/python python3 -m pytest python/tests

Result on the Phase 6 code: 32 passed, no sanitizer report. The check that ASan was active: the module imports 26 sanitizer symbols and the runtime printed its init line. This exercises the zero-copy views (they outlive the result name in a test) and the capsule owned buffers. Leak checking is off because CPython does not free everything at exit, so leaks in the bindings are not covered by this run. CI runs the same steps in the python-sanitizers job.

## Pre-release run of the mutation tool and the fuzz targets

Run on the code of commit 25d38f2 (Phase 6 plus the pybind11 pin), before any v0.1.0 tag, on the KVM VM described in docs/PERFORMANCE.md. The fuzz targets and the mutation tool ran at the same time on 4 vCPUs, so the exec/s figures are lower than the earlier ones in this document.

| Check | Result |
|---|---|
| tools/mutation/run.py --build-dir build/release | 28 mutants: 26 killed as expected, 2 survived as expected (the two documented equivalent mutants: modify-emits-event-before-removal and next-ask-searched-from-level-plus-one). No mutant behaved differently from the list |
| fuzz_engine, 301 s, corpus from fuzz/corpus/engine, max_len 4096 | 175,989 executions (about 585 per second), no crash, no sanitizer report |
| fuzz_lobster, 301 s | 969,403 executions (about 3,200 per second), no crash, no sanitizer report |
| fuzz_itch, 301 s | 1,903,326 executions (about 6,300 per second), no crash, no sanitizer report |

The working tree was clean after the mutation run (the tool restores the source). As before, these are coverage guided runs of a few minutes: no crash on the explored inputs, not a proof.

## Which verification has actually been run

| Verification | Run? | What it shows |
|---|---|---|
| Engine versus reference book (differential test) | yes, every build | same author and specification: consistency of the optimized structures with the documented rules, not correctness of the rules |
| Replay of the committed SYNTHETIC LOBSTER sample against its generated order book rows | yes, every build | SELF-CONSISTENCY ONLY. The generator (scripts/make_synthetic_lobster.py) and the C++ replay were written from the same understanding of the format. Not independent |
| Decoding of the hand-built SYNTHETIC ITCH fixture | yes, every build | same caveat |
| Replay of a real LOBSTER file against the vendor order book file | NO, not run | this is the planned independent check. The data host (lobsterdata.com) is blocked by the network policy of the environment the work was done in, so no real file has been available. docs/MARKET_DATA.md section 6 and 7 say what to run |
| Decoding of a real Nasdaq ITCH 5.0 file | NO, not run | the host (emi.nasdaq.com) is blocked in the same way |

Until the LOBSTER row is run and passes, no document in this repository should be read as claiming that lobkit reproduces real exchange data.
