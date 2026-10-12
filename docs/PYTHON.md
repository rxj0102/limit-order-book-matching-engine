# Python bindings and analysis

All data used by the Python examples, tests and the sweep is SYNTHETIC. Nothing here is market data and nothing here is evidence that a strategy is profitable.

## Build

The bindings are an optional CMake target. The C++ core builds and tests without Python.

    pip install numpy matplotlib pytest pybind11
    cmake --preset python
    cmake --build --preset python
    ctest --preset python                       # C++ tests plus pytest (test name python_pytest)
    PYTHONPATH=build/python/python python3 -m pytest python/tests

The option is LOBKIT_BUILD_PYTHON (default OFF). pybind11 is taken from the active Python (pip install pybind11) if present. Otherwise it is fetched at a pinned commit when LOBKIT_FETCH_DEPS is ON. The fetch path pins release 3.1.0 (commit 97bf890, checked with git ls-remote), the version the bindings are tested with. The fetch path was built and imported once with the pip copy disabled; CI installs pybind11==3.1.0 from pip.

## What is exposed (module lobkit, compiled part lobkit._core)

| Name | Purpose |
|---|---|
| BookConfig, SimConfig, ASParams | Keyword constructed configuration. Unknown names raise TypeError, bad values ValueError. Fields match the C++ structs. |
| Engine, ReplayAdapter | Submit, cancel and modify; apply feed messages. Events are collected and returned by drain_events(). |
| read_lobster_messages, read_lobster_book, replay_lobster | LOBSTER reader and replay through an Engine. |
| synthetic_feed | Seeded random order flow generator (SYNTHETIC). |
| run_simulation, run_simulation_file | Avellaneda-Stoikov simulation over an array or a LOBSTER file. Returns a SimResult. |
| book_config_for, strategy_params | Mirror what the lobkit_sim_run command line does (band centred on the data, quoting limits inside the band). |
| lobkit.features | order_flow_imbalance, queue_imbalance, microprice, realized_spread, vpin. Pure numpy. |

## Arrays and ownership

Result arrays are numpy structured arrays whose dtypes are built from the C++ struct offsets (EVENT_DTYPE, FEED_DTYPE, FILL_DTYPE, PATH_DTYPE).

- SimResult.fills and SimResult.path are read-only views of memory owned by the SimResult. They have the result as their base object, so they stay valid after every Python name for the result is dropped. Each access returns a new view of the same buffer. Copy them if you want to modify them.
- Engine.drain_events, read_lobster_messages, read_lobster_book and synthetic_feed move a freshly built C++ buffer into a read-only array. The array owns the buffer through a capsule and frees it when the last view is collected. Nothing is copied.
- run_simulation reads its input array in place and does not copy it. It must have dtype FEED_DTYPE and be one dimensional and C contiguous (a strided slice is rejected with ValueError). The GIL is released during the run; the array is kept alive by the call.
- PATH_DTYPE is a conversion, not a view of the C++ sample type, because the C++ side uses 128 bit integers that numpy cannot hold. It is built once when the result is created. A value that does not fit in 64 bits raises OverflowError. The exact totals are in SimResult.pnl as Python integers.
- Money in arrays is in half native units (native unit = 1e-4 dollars), so dollars = value / 20000. The columns spread2, inventory2, fees2 and total2 are cumulative. total2 = spread2 + inventory2 - fees2.
- ReplayAdapter keeps its Engine alive.
- An Engine and its adapters are not thread safe. Different engines can be used from different threads.

## Errors

The C++ hot path never throws. Exceptions are created at the binding boundary only.

| Situation | Python exception |
|---|---|
| Missing file | FileNotFoundError |
| Other I/O failure, unwritable output | OSError |
| Malformed input file | lobkit.ParseError (a ValueError) with attributes status, line, text |
| Invalid configuration or array shape | ValueError |
| Unknown configuration field | TypeError |
| Engine rejects (bad price, unknown id, and so on) | Not exceptions. They are Reject events. |

## Features

Each function documents its formula and source in its docstring and is tested against values worked out by hand in python/tests/test_features.py. Where a value is undefined the result is NaN.

- order_flow_imbalance: Cont, Kukanov and Stoikov 2014.
- queue_imbalance and microprice: Stoikov 2018.
- realized_spread: Huang and Stoll 1996. The horizon is a required argument.
- vpin: Easley, Lopez de Prado and O'Hara 2012. Simplified: the caller supplies trade signs instead of using bulk volume classification, and there is no default bucket volume.

## Sweep

python/analysis/sweep.py runs a grid of risk aversion (gamma) by constant one-way latency (the same value on the submit, acknowledgement and market data legs), many seeds per cell, and writes tables, plots and a report to docs/results/phase6. Every table cell is a mean and standard deviation over seeds with the minimum and maximum, and every table is followed by the fill-model artifact rate. The report ends with the limitations section. See docs/results/phase6/sweep_report.md, whose Environment section records the machine and the wall time of the run that produced it.

The sweep draws no conclusions. The patterns visible in the synthetic tables are consequences of a random feed with no structure, an uncalibrated k and a crude sigma, and are not findings about the strategy.
