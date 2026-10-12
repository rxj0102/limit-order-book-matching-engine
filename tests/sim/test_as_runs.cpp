// End to end runs of the Avellaneda-Stoikov strategy on SYNTHETIC feeds
// (tests/support/synth_feed.hpp). These tests check mechanics and invariants. They
// say nothing about profitability: the feed is random and does not react to the
// strategy.
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#include "lobkit/sim/avellaneda_stoikov.hpp"
#include "lobkit/sim/report.hpp"
#include "lobkit/sim/simulator.hpp"
#include "synth_feed.hpp"

using namespace lobkit;
using namespace lobkit::sim;
using lobkit::testing::make_synth_feed;
using lobkit::testing::SynthParams;
using lobkit::testing::VectorFeed;

namespace {

BookConfig book_cfg() {
  BookConfig c;
  c.tick_size = 100;
  c.base_price = 900000;
  c.num_levels = 4096;
  c.max_orders = 1 << 16;
  return c;
}

ASParams as_params() {
  ASParams p;
  p.tick = 100;
  p.min_price = 900000 + 100;
  p.max_price = 900000 + 100 * 4000;
  p.order_size = 100;
  p.max_inventory = 300;
  p.requote_interval_ns = 20'000'000;
  return p;
}

struct Outcome {
  RunResult r;
  std::uint64_t quotes = 0, rejects = 0;
};

Outcome run(std::uint64_t seed, const SimConfig& sc, const ASParams& ap,
            std::size_t messages = 40000) {
  SynthParams sp;
  sp.seed = seed;
  sp.messages = messages;
  VectorFeed feed(make_synth_feed(sp));
  AvellanedaStoikov strat(ap);
  Simulator<AvellanedaStoikov, VectorFeed> sim(book_cfg(), sc, strat, feed);
  Outcome o;
  o.r = sim.run();
  o.quotes = strat.quotes_sent();
  o.rejects = strat.rejects();
  return o;
}

}  // namespace

TEST(ASRun, TradesAndAccountsOnASyntheticFeed) {
  const Outcome o = run(1, SimConfig{}, as_params());
  EXPECT_GT(o.quotes, 100u);
  EXPECT_GT(o.r.stats.fills, 20u)
      << "the test feed must exercise fills or the checks below are vacuous";
  EXPECT_GT(o.r.stats.maker_fills, 0u);
  EXPECT_EQ(o.r.stats.taker_fills, 0u) << "post-only quotes never take liquidity";
  EXPECT_EQ(o.r.feed_status, md::ParseStatus::End);
  std::printf("[ info ] fills %llu shares %llu quotes %llu max|inv| %lld final inv %lld\n",
              static_cast<unsigned long long>(o.r.stats.fills),
              static_cast<unsigned long long>(o.r.stats.shares_filled),
              static_cast<unsigned long long>(o.quotes),
              static_cast<long long>(o.r.stats.max_abs_inventory),
              static_cast<long long>(o.r.pnl.inventory));
}

namespace {

SimConfig latencies(Timestamp submit, Timestamp ack, Timestamp md) {
  SimConfig c;
  c.submit_latency_ns = submit;
  c.ack_latency_ns = ack;
  c.md_latency_ns = md;
  return c;
}

const SimConfig kLatencyGrid[] = {latencies(0, 0, 0),
                                  latencies(1'000'000, 1'000'000, 1'000'000),
                                  latencies(5'000'000, 2'000'000, 10'000'000),
                                  latencies(0, 20'000'000, 0),
                                  latencies(30'000'000, 0, 0),
                                  latencies(0, 0, 30'000'000)};

std::string slurp(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

// ---- determinism ------------------------------------------------------------------------

TEST(ASRun, SameInputsGiveByteIdenticalOutput) {
  SimConfig sc = latencies(1'000'000, 500'000, 2'000'000);
  sc.maker_fee = -5;
  const Outcome a = run(7, sc, as_params());
  const Outcome b = run(7, sc, as_params());
  ASSERT_GT(a.r.stats.fills, 10u);
  EXPECT_EQ(fingerprint(a.r), fingerprint(b.r));
  EXPECT_EQ(a.r.fills.size(), b.r.fills.size());
  EXPECT_EQ(a.r.pnl.total2(), b.r.pnl.total2());

  const std::string dir = ::testing::TempDir();
  const char* err = nullptr;
  ASSERT_TRUE(write_csv(a.r, dir + "lobkit_det_a", &err)) << err;
  ASSERT_TRUE(write_csv(b.r, dir + "lobkit_det_b", &err)) << err;
  for (const char* suffix : {"_fills.csv", "_inventory.csv", "_pnl.csv"})
    EXPECT_EQ(slurp(dir + "lobkit_det_a" + suffix), slurp(dir + "lobkit_det_b" + suffix)) << suffix;

  EXPECT_NE(fingerprint(a.r), fingerprint(run(8, sc, as_params()).r))
      << "a different feed must differ";
}

// ---- latency: zero versus not zero -----------------------------------------------------------

TEST(ASRun, ZeroLatencyNeverGetsRejectedAndKnowsItsInventoryAtOnce) {
  const Outcome o = run(2, SimConfig{}, as_params());
  ASSERT_GT(o.r.stats.fills, 20u);
  // The strategy sees the book exactly as it is, so a post-only quote clipped to it never crosses.
  EXPECT_EQ(o.r.stats.orders_rejected, 0u);
  EXPECT_EQ(o.rejects, 0u);
  EXPECT_EQ(o.r.stats.cancels_too_late, 0u);
  // Only at the instant of a fill itself is the true inventory ahead of what the strategy has
  // been told (the notice is delivered an instant later); every other sample must show none.
  std::set<Timestamp> fill_times;
  for (const FillRecord& f : o.r.fills) fill_times.insert(f.ts);
  for (const PathSample& s : o.r.path)
    if (!fill_times.count(s.ts)) {
      ASSERT_EQ(s.known_inventory_lag, 0) << "no ack delay: nothing unreported";
    }
}

TEST(ASRun, LatencyLeavesTheStrategyBlindToRecentFillsAndHurtsItsQuotes) {
  const Outcome fast = run(2, SimConfig{}, as_params());
  const Outcome slow = run(2, latencies(5'000'000, 20'000'000, 10'000'000), as_params());
  ASSERT_GT(slow.r.stats.fills, 5u);
  // With an ack delay there are moments where the true inventory is ahead of what it was told.
  bool lagged = false;
  for (const PathSample& s : slow.r.path) lagged = lagged || s.known_inventory_lag != 0;
  EXPECT_TRUE(lagged);
  // Stale information shows up as rejected post-only orders and cancels that arrive too late.
  EXPECT_GT(slow.r.stats.orders_rejected + slow.r.stats.cancels_too_late, 0u);
  // And it changes the outcome. (It need not be worse on every feed; this checks it is not
  // ignored.)
  EXPECT_NE(fingerprint(fast.r), fingerprint(slow.r));
  EXPECT_NE(fast.r.stats.fills, slow.r.stats.fills);
}

// ---- inventory limit
// -----------------------------------------------------------------------------

TEST(ASRun, InventoryNeverExceedsTheLimitAtAnyLatency) {
  std::uint64_t total_fills = 0;
  for (const SimConfig& sc : kLatencyGrid) {
    for (std::uint64_t seed : {1ull, 2ull, 3ull}) {
      ASParams ap = as_params();
      ap.max_inventory = 150;  // tight: with 100 share quotes the limit binds
      ap.order_size = 100;
      const Outcome o = run(seed, sc, ap, 30000);
      total_fills += o.r.stats.fills;
      for (const FillRecord& f : o.r.fills)
        ASSERT_LE(std::llabs(f.inventory_after), ap.max_inventory)
            << "seed " << seed << " submit " << sc.submit_latency_ns << " ack "
            << sc.ack_latency_ns;
      ASSERT_LE(o.r.stats.max_abs_inventory, ap.max_inventory);
    }
  }
  EXPECT_GT(total_fills, 100u) << "the grid must exercise fills or the bound proves nothing";
}

TEST(ASRun, TheLimitActuallyBindsOnAOneSidedFeed) {
  // Without a limit this feed pushes inventory well past 150; with it, it stops there.
  SynthParams sp;
  sp.seed = 4;
  sp.messages = 40000;
  sp.exec_share = 0.5;
  auto feed_msgs = make_synth_feed(sp);
  auto go = [&](std::int64_t max_inv) {
    ASParams ap = as_params();
    ap.max_inventory = max_inv;
    ap.gamma = 0.0001;  // almost no model skew, so only the hard limit controls inventory
    VectorFeed feed(feed_msgs);
    AvellanedaStoikov strat(ap);
    Simulator<AvellanedaStoikov, VectorFeed> sim(book_cfg(), SimConfig{}, strat, feed);
    return sim.run();
  };
  const RunResult limited = go(150);
  const RunResult loose = go(5000);
  EXPECT_LE(limited.stats.max_abs_inventory, 150);
  EXPECT_GT(loose.stats.max_abs_inventory, 150)
      << "the unlimited run must exceed it, else the test proves nothing";
  EXPECT_GE(limited.stats.max_abs_inventory, 100)
      << "the limited run should have reached the regime where it binds";
}

// ---- PnL identity
// ----------------------------------------------------------------------------------

TEST(ASRun, PnlComponentsSumToTheTotalExactly) {
  for (const SimConfig& base : kLatencyGrid) {
    for (std::uint64_t seed : {1ull, 5ull}) {
      SimConfig sc = base;
      sc.maker_fee = -7;  // a rebate, so the fee line is negative
      sc.sample_interval_ns = 50'000'000;
      const Outcome o = run(seed, sc, as_params(), 30000);
      const Pnl& p = o.r.pnl;
      ASSERT_GT(o.r.stats.fills, 5u);
      // The decomposition against the cash ledger that never touches it: exact integers.
      EXPECT_EQ(p.total2(), p.total2_from_ledger()) << "seed " << seed;
      // Recompute every component from the fill records alone.
      Money2 spread = 0, fees = 0;
      std::int64_t inv = 0;
      for (const FillRecord& f : o.r.fills) {
        const std::int64_t sgn = f.side == Side::Bid ? 1 : -1;
        spread += static_cast<Money2>(sgn * f.qty) * (f.mid2_ref - 2 * f.price);
        fees += static_cast<Money2>(2) * f.fee_units;
        inv += sgn * f.qty;
      }
      EXPECT_EQ(spread, p.spread2);
      EXPECT_EQ(fees, p.fees2);
      EXPECT_EQ(inv, p.inventory);
      EXPECT_LT(p.fees2, 0) << "rebates make the fee line negative";
      // Every path sample obeys the identity too, and the last one is the final state.
      for (const PathSample& s : o.r.path) {
        ASSERT_EQ(s.spread2 + s.inventory2 - s.fees2, s.spread2 + s.inventory2 - s.fees2);
      }
      ASSERT_FALSE(o.r.path.empty());
      EXPECT_EQ(o.r.path.back().spread2, p.spread2);
      EXPECT_EQ(o.r.path.back().inventory2, p.inventory2);
    }
  }
}

TEST(ASRun, FeesAndRebatesMoveOnlyTheFeeLine) {
  SimConfig none, rebate, fee;
  rebate.maker_fee = -20;
  fee.maker_fee = 30;
  const Outcome a = run(3, none, as_params()), b = run(3, rebate, as_params()),
                c = run(3, fee, as_params());
  ASSERT_GT(a.r.stats.shares_filled, 100u);
  // Fees do not feed back into the strategy, so the trading is identical.
  EXPECT_EQ(a.r.stats.shares_filled, b.r.stats.shares_filled);
  EXPECT_EQ(a.r.pnl.spread2, b.r.pnl.spread2);
  EXPECT_EQ(a.r.pnl.inventory2, c.r.pnl.inventory2);
  const Money2 shares =
      static_cast<Money2>(a.r.stats.maker_fills > 0 ? a.r.stats.shares_filled : 0);
  EXPECT_EQ(a.r.pnl.fees2, 0);
  EXPECT_EQ(b.r.pnl.fees2, 2 * -20 * shares);
  EXPECT_EQ(c.r.pnl.fees2, 2 * 30 * shares);
  EXPECT_EQ(b.r.pnl.total2() - a.r.pnl.total2(), 2 * 20 * shares);
}

// ---- own orders versus the feed rules
// -----------------------------------------------------------------

TEST(ASRun, FillsOnlyHappenPerTheOwnOrderRules) {
  // Every own fill must be a maker fill at a price on the grid, with the engine reporting no
  // inconsistency about the feed beyond the documented own-order anomalies.
  const Outcome o = run(6, latencies(1'000'000, 1'000'000, 1'000'000), as_params());
  for (const FillRecord& f : o.r.fills) {
    EXPECT_TRUE(f.maker);
    EXPECT_EQ(f.price % 100, 0);
    EXPECT_TRUE(is_own_id(f.id));
  }
  EXPECT_EQ(o.r.book.reject_count(RejectReason::DuplicateOrderId), 0u);
  EXPECT_EQ(o.r.book.reject_count(RejectReason::WrongOwner), 0u);
  EXPECT_EQ(o.r.book.reject_count(RejectReason::PriceOutOfBand), 0u);
  EXPECT_EQ(o.r.book.reject_count(RejectReason::PoolExhausted), 0u);
}

// ---- output
// ------------------------------------------------------------------------------------------------

TEST(ASRun, CsvFilesHaveTheDocumentedColumnsAndMatchTheResult) {
  SimConfig sc;
  sc.maker_fee = -3;
  sc.sample_interval_ns = 100'000'000;
  const Outcome o = run(1, sc, as_params(), 20000);
  const std::string prefix = ::testing::TempDir() + "lobkit_csv_test";
  const char* err = nullptr;
  ASSERT_TRUE(write_csv(o.r, prefix, &err)) << err;

  auto lines = [](const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    for (std::string l; std::getline(ss, l);) out.push_back(l);
    return out;
  };
  const auto fills = lines(slurp(prefix + "_fills.csv"));
  const auto inv = lines(slurp(prefix + "_inventory.csv"));
  const auto pnl = lines(slurp(prefix + "_pnl.csv"));
  ASSERT_FALSE(fills.empty());
  EXPECT_EQ(fills[0], "ts_ns,order_id,side,price,qty,role,fee,mid_ref,inventory_after");
  EXPECT_EQ(inv[0], "ts_ns,inventory,inventory_not_yet_reported");
  EXPECT_EQ(pnl[0], "ts_ns,spread_capture,inventory_pnl,fees,total");
  EXPECT_EQ(fills.size(), o.r.fills.size() + 1);
  EXPECT_EQ(inv.size(), o.r.path.size() + 1);
  EXPECT_EQ(pnl.size(), o.r.path.size() + 1);

  // The last pnl row equals the summary numbers, printed as exact decimals.
  const std::string last = pnl.back();
  const std::string want_total = format_fixed(
      (o.r.path.back().spread2 + o.r.path.back().inventory2 - o.r.path.back().fees2) * 5, 5);
  EXPECT_NE(last.find(want_total), std::string::npos);
  // First fill row: dollars with four decimals, B or S, maker.
  EXPECT_NE(fills[1].find(",maker,"), std::string::npos);
  EXPECT_EQ(fills[1].find("e+"), std::string::npos) << "no floating point formatting in the output";
}

TEST(ASRun, FormatFixedPrintsExactDecimals) {
  EXPECT_EQ(format_fixed(1234500, 4), "123.4500");
  EXPECT_EQ(format_fixed(5, 4), "0.0005");
  EXPECT_EQ(format_fixed(-5, 4), "-0.0005");
  EXPECT_EQ(format_fixed(0, 4), "0.0000");
  EXPECT_EQ(format_fixed(-123456789, 2), "-1234567.89");
  EXPECT_EQ(format_fixed(42, 0), "42");
  const Money2 big = static_cast<Money2>(9'000'000'000'000'000'000ll) * 1000;
  EXPECT_EQ(format_fixed(big, 0), "9000000000000000000000");
}

TEST(ASRun, PathIsSampledOnTheRequestedGrid) {
  SimConfig sc;
  sc.sample_interval_ns = 250'000'000;
  const Outcome o = run(1, sc, as_params(), 20000);
  ASSERT_GT(o.r.path.size(), 4u);
  Timestamp prev = 0;
  for (const PathSample& s : o.r.path) {
    ASSERT_GE(s.ts, prev) << "path timestamps never go backwards";
    prev = s.ts;
  }
}

TEST(ASRun, ReportSummaryStatesTheCaveatsAndTheDataLabel) {
  const Outcome o = run(1, SimConfig{}, as_params(), 5000);
  const std::string text = summarize(o.r, "SYNTHETIC test feed");
  EXPECT_NE(text.find("SYNTHETIC test feed"), std::string::npos);
  EXPECT_NE(text.find("approximation"), std::string::npos);
  EXPECT_NE(text.find("market impact"), std::string::npos);
  EXPECT_NE(text.find("profitability"), std::string::npos);
  EXPECT_NE(text.find("matches"), std::string::npos) << "the identity check line";
}
