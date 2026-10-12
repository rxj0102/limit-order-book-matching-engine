# lobkit v0.1.0 release notes (prepared, not tagged)

Status: DRAFT. The tag v0.1.0 does not exist. It is created locally only after the pull request that brings this work to main is merged, on the merge commit, and tags are pushed only with the repository owner's approval. If the work lands as several pull requests, split these notes to match.

## What this is

The first public version of lobkit: a C++20 limit order book matching engine, a LOBSTER and Nasdaq ITCH 5.0 (subset) replay harness, an event-driven backtester with an Avellaneda-Stoikov market maker, an SPSC ring buffer, Python bindings with a feature library, a test and benchmark suite, and documentation that says what has and has not been verified.

## Highlights

- Matching engine: price-time priority, limit, market, IOC, FOK and post-only orders, cancel, modify (in place size reduction, otherwise cancel and replace). Dense price ladder with bitmaps, fixed order pool, open addressing id map. No allocation after construction, no virtual dispatch, no floating point, deterministic.
- Replay: LOBSTER (message and order book files) and an ITCH 5.0 subset, memory mapped, as fast as possible or paced, with snapshot verification and counted, never hidden, anomalies.
- Simulator: three one-way latencies, queue-position fill model, exact integer PnL decomposition, Avellaneda-Stoikov strategy, CSV output.
- Python: pybind11 bindings with documented array ownership, features (order flow imbalance, queue imbalance, microprice, realized spread, VPIN) and a parameter sweep script.
- Evidence: reference book and differential tests, mutation tool, fuzz targets, sanitizers, coverage, and a benchmark suite with a std::map baseline.

## What was verified

- 221 C++ tests pass on GCC 13.3 (debug, release, ASan, UBSan, TSan) and Clang 18.1 (debug, release, ASan, TSan with 217 tests); 32 Python tests pass, also under ASan and UBSan. Line coverage (GCC, merged lines): book 99.5 percent, md 98.9, sim 97.8, infra 100.
- 28 hand written mutants: 26 killed, 2 equivalent as expected. Fuzz targets ran 301 seconds each without a crash.

## What was NOT verified

- Real market data. No real LOBSTER or ITCH file has been replayed, so the claim that lobkit reproduces real exchange order books is unproven. The strict no-seeding replay policy is unconfirmed.
- The matching rules against any venue. The reference book shares an author with the engine.
- Any strategy result. All simulator and sweep output is from synthetic data and is not evidence of profitability.
- Any performance number on bare metal. All timings are KVM VM measurements with a noise floor of about 20 percent.

## Known issues and open items

- The end to end LOBSTER replay (4.7 million messages per second, synthetic file, VM) is less than a third of the in-memory apply rate, and the gap is not explained (docs/PERFORMANCE.md section 8).
- No performance optimization was adopted in this release; every attempt failed to beat the noise floor (docs/PERFORMANCE.md section 6).
- clang-tidy is configured but not run in CI. No macOS CI. The Python sanitizer job runs without leak checking.
- The complete list is docs/LIMITATIONS.md.

## Build

```
cmake --preset release && cmake --build --preset release && ctest --preset release
```

Requirements: CMake 3.25, a C++20 compiler (GCC 13 and Clang 18 tested), GoogleTest. Python bindings: the python preset with numpy, matplotlib, pytest and pybind11 3.1.0.

## Tagging checklist (for after the merge)

1. Confirm CI is green on the merge commit and the status block in README.md is still accurate.
2. `git tag -a v0.1.0 MERGE_COMMIT -m 'lobkit v0.1.0'` locally.
3. Ask the repository owner before `git push origin v0.1.0`.
