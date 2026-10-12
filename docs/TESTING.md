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

Result of the last run (200,000 operations per scenario, GCC 13.3, release): all 12 killable mutants were killed (one of them by a crash, which the tool counts as killed), and the 2 equivalent mutants survived as expected.

| Mutant | Result |
|---|---|
| Resting orders appended at the head instead of the tail | killed |
| Modify keeps priority on a size increase | killed |
| Post-only allowed to trade at an equal price | killed |
| FOK availability off by one | killed |
| Feed execute never fills own orders | killed |
| Feed execute ignores feed orders ahead of an own order | killed |
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

The message parser fuzz target arrives with the parsers in Phase 4.
