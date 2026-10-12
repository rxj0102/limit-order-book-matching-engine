# Contributing

Thank you for looking. This project values correct, simple, honestly measured code over clever code. Read docs/LIMITATIONS.md first so that you know what the project does not claim.

## Build and test

```
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
```

Presets: debug, release, asan, ubsan, tsan, coverage, fuzz (Clang only), bench, python. Warnings are errors on project targets (-Wall -Wextra -Wpedantic -Wshadow -Wconversion and more). A change must build clean with both GCC and Clang. Before opening a pull request run at least debug, release, asan, ubsan and tsan with GCC, and debug with Clang. CI runs more, see .github/workflows/ci.yml.

The core builds without Python. The Python extension is behind LOBKIT_BUILD_PYTHON (the python preset); install numpy, matplotlib, pytest and pybind11==3.1.0 first. docs/PYTHON.md has details.

## Rules that are not negotiable

1. Correctness before speed. The engine hot path has no allocation, no virtual dispatch, no exceptions and no floating point. The engine (include/lobkit/book) and the simulator accounting use integers only. Prices and quantities become doubles in exactly one place, the Avellaneda-Stoikov quote math in include/lobkit/sim/avellaneda_stoikov.hpp, and are rounded back to integers immediately. Other floating point is limited to wall-clock timing and pacing, report display and the synthetic generator. Exceptions are created only at the Python binding boundary.
2. Changing matching behavior means changing docs/MATCHING_RULES.md, the reference book in tests/support/reference_book.hpp, and the engine together, with tests that fail without the change. The reference book must stay independent of the engine code. Run the mutation tool afterwards (below).
3. Never fabricate or round up a number. Every benchmark or simulation figure carries its environment and its data source. Anything from a VM says so. Anything from synthetic data says SYNTHETIC. Report a median with its range over repeated, interleaved runs, never one run, and measure the noise floor of your machine by running one binary against itself (bench/ab_run.py does it).
4. A performance change must be justified by a measurement that beats the noise floor in a consistent direction, and failed attempts are recorded, not deleted (docs/PERFORMANCE.md section 6).
5. Changes to the public API (Engine, ReplayAdapter, the sinks, the Python module) need a discussion in an issue first.
6. State limitations plainly. If your change adds a bias or an assumption, add it to docs/LIMITATIONS.md.

## Style

- clang-format with the committed .clang-format. CI checks it: `find include src tests fuzz bench tools python/src -name '*.hpp' -o -name '*.cpp' | xargs clang-format --dry-run --Werror`.
- Comments explain why, not what. Match the surrounding code in density and naming.
- In README and docs prose: no em dashes and no quotation marks. Code, commands and tables are exempt.
- Conventional commit messages (feat, fix, docs, test, bench, style, refactor, chore), small commits, one logical change each.

## Tests you should run for the area you touched

| Area | Run |
|---|---|
| Book, adapter | ctest in debug and the sanitizer presets. The differential test length can be raised with the environment variable described in docs/TESTING.md |
| Parsers, replay | ctest, then the fuzz targets: `cmake --preset fuzz && cmake --build --preset fuzz`, then `build/fuzz/fuzz/fuzz_lobster CORPUS_DIR -max_total_time=300` (and fuzz_itch, fuzz_engine). A crash goes into fuzz/regressions with a regression test |
| Engine semantics | `python3 -I tools/mutation/run.py --build-dir build/release` on a clean working tree, and nothing else building at the same time. It must report 26 killed and 2 equivalent survivors, or you must explain the difference |
| Simulator | ctest. Any new strategy behavior needs a test in tests/sim, and the PnL identity must still hold |
| Python | `ctest --preset python`, and the sanitizer run from docs/TESTING.md |
| Benchmarks | `cmake --preset bench`, build, run bench_book with at least 20 interleaved repetitions. The start-up equivalence check must pass |

## Coverage

`cmake --preset coverage && cmake --build --preset coverage && ctest --preset coverage`, then the gcovr command in docs/TESTING.md. CI fails below 90 percent line coverage of the book code. The coverage badge in README.md is a static number: update it when you change the figure.

## Real data

If you can run `lobkit_replay --lobster MSG --book ROWS --verify` on a real LOBSTER sample, please do, and report the result in an issue whether it passes or fails. That result is the most valuable contribution to this project right now. Do not commit real data files (data/lobster and data/itch are ignored; the vendors restrict redistribution).

## Pull requests

Describe what changed and why, list which tests and presets you ran, and attach measured results if you touched performance. Keep unrelated cleanups out of the diff.
