// Runs the Avellaneda-Stoikov simulation on a LOBSTER message file and writes the
// fill, inventory and PnL CSV files.
//
//   lobkit_sim_run --lobster messages.csv --out PREFIX [options]
//
// The data label printed with the results says what the input is. Run it on
// synthetic data and the output says SYNTHETIC; it never says anything else
// unless you tell it with --data-label.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "lobkit/md/lobster.hpp"
#include "lobkit/md/mapped_file.hpp"
#include "lobkit/md/replay.hpp"
#include "lobkit/sim/avellaneda_stoikov.hpp"
#include "lobkit/sim/report.hpp"
#include "lobkit/sim/simulator.hpp"

using namespace lobkit;
using namespace lobkit::sim;

int main(int argc, char** argv) {
  const char* msgs = nullptr;
  std::string out = "sim";
  std::string label = "UNSPECIFIED (use --data-label to say what this is)";
  SimConfig sc;
  ASParams ap;
  Price tick = 100;
  std::uint32_t levels = 65536, pool = 1u << 20;
  double submit_us = 0, ack_us = 0, md_us = 0;

  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* name) -> const char* {
      if (std::strcmp(argv[i], name) == 0 && i + 1 < argc) return argv[++i];
      return nullptr;
    };
    const char* v;
    if ((v = arg("--lobster")))
      msgs = v;
    else if ((v = arg("--out")))
      out = v;
    else if ((v = arg("--data-label")))
      label = v;
    else if ((v = arg("--submit-latency-us")))
      submit_us = std::atof(v);
    else if ((v = arg("--ack-latency-us")))
      ack_us = std::atof(v);
    else if ((v = arg("--md-latency-us")))
      md_us = std::atof(v);
    else if ((v = arg("--maker-fee")))
      sc.maker_fee = std::atoll(v);
    else if ((v = arg("--taker-fee")))
      sc.taker_fee = std::atoll(v);
    else if ((v = arg("--gamma")))
      ap.gamma = std::atof(v);
    else if ((v = arg("--k")))
      ap.k = std::atof(v);
    else if ((v = arg("--sigma0")))
      ap.sigma0 = std::atof(v);
    else if ((v = arg("--horizon")))
      ap.horizon_s = std::atof(v);
    else if ((v = arg("--size")))
      ap.order_size = static_cast<Qty>(std::atol(v));
    else if ((v = arg("--max-inventory")))
      ap.max_inventory = std::atoll(v);
    else if ((v = arg("--size-skew")))
      ap.size_skew = std::atof(v);
    else if ((v = arg("--tick-size")))
      tick = std::atoll(v);
    else if ((v = arg("--requote-ms")))
      ap.requote_interval_ns = static_cast<Timestamp>(std::atof(v) * 1e6);
    else {
      std::fprintf(
          stderr,
          "usage: lobkit_sim_run --lobster FILE [--out PREFIX] [--data-label TEXT]\n"
          "  --submit-latency-us N --ack-latency-us N --md-latency-us N\n"
          "  --maker-fee N --taker-fee N   (native units per share, 10000 = $1; negative = "
          "rebate)\n"
          "  --gamma G --k K --sigma0 S --horizon SEC --size N --max-inventory N --size-skew F\n"
          "  --tick-size N --requote-ms MS\n");
      return 2;
    }
  }
  if (!msgs) {
    std::fprintf(stderr, "missing --lobster FILE\n");
    return 2;
  }
  sc.submit_latency_ns = static_cast<Timestamp>(submit_us * 1e3);
  sc.ack_latency_ns = static_cast<Timestamp>(ack_us * 1e3);
  sc.md_latency_ns = static_cast<Timestamp>(md_us * 1e3);
  ap.tick = tick;

  const char* err = nullptr;
  md::MappedFile file;
  if (!file.open(msgs, &err)) {
    std::fprintf(stderr, "%s: %s\n", msgs, err);
    return 2;
  }
  BookConfig bc;
  bc.tick_size = tick;
  bc.num_levels = levels;
  bc.max_orders = pool;
  Price lo = 0, hi = 0;
  if (md::scan_price_range(md::lobster::MessageReader(file.text()), lo, hi))
    bc.base_price = md::suggest_base_price(lo, hi, tick, levels);
  try {
    bc.validate();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "bad configuration: %s\n", e.what());
    return 2;
  }
  ap.min_price = bc.base_price + tick;
  ap.max_price = bc.base_price + static_cast<Price>(levels - 2) * tick;

  md::lobster::MessageReader reader(file.text());
  AvellanedaStoikov strat(ap);
  Simulator<AvellanedaStoikov, md::lobster::MessageReader> sim(bc, sc, strat, reader);
  const RunResult r = sim.run();
  std::fputs(summarize(r, label.c_str()).c_str(), stdout);
  std::printf(
      "strategy: quotes sent %llu, cancels %llu, rejects %llu, final sigma %.5f $/sqrt(s)\n",
      static_cast<unsigned long long>(strat.quotes_sent()),
      static_cast<unsigned long long>(strat.cancels_sent()),
      static_cast<unsigned long long>(strat.rejects()), strat.sigma());
  if (!write_csv(r, out, &err)) {
    std::fprintf(stderr, "writing CSV: %s\n", err);
    return 2;
  }
  std::printf("wrote %s_fills.csv, %s_inventory.csv, %s_pnl.csv\n", out.c_str(), out.c_str(),
              out.c_str());
  return r.feed_status == md::ParseStatus::End ? 0 : 2;
}
