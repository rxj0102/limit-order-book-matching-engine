#pragma once
// Output of a simulation run: three CSV files and a text summary. Prices are
// printed from integers (no floating point formatting), so output is
// byte-for-byte reproducible.
//
//   <prefix>_fills.csv      one row per own fill
//   <prefix>_inventory.csv  inventory path (sampled at a fixed interval and at every fill)
//   <prefix>_pnl.csv        PnL path: spread capture, inventory PnL, fees, total

#include <string>

#include "lobkit/sim/types.hpp"

namespace lobkit::sim {

// Fixed point text of an integer with `decimals` implied decimals, e.g.
// format_fixed(1234500, 4) is 123.4500. Handles negatives and 128 bit values.
std::string format_fixed(Money2 value, int decimals);

// Writes the three files. Returns false and a static message on I/O failure.
bool write_csv(const RunResult& r, const std::string& prefix, const char** error);

// Human readable summary, with the standard caveats.
std::string summarize(const RunResult& r, const char* data_label);

// FNV-1a over fills and path as written to CSV: a cheap fingerprint for
// determinism tests.
std::uint64_t fingerprint(const RunResult& r);

}  // namespace lobkit::sim
