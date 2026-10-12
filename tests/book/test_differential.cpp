// Differential test: engine versus the std::map/std::deque reference book on
// millions of randomized operations. Set LOBKIT_DIFF_OPS to change the length
// of each scenario (default 1,000,000) for a longer nightly run.
#include <gtest/gtest.h>

#include "diff_runner.hpp"

using namespace lobkit;
using namespace lobkit::testing;

namespace {

struct Scenario {
  const char* name;
  BookConfig cfg;
  WorkloadParams wp;
};

BookConfig make_cfg(Price tick, Price base, std::uint32_t levels, std::uint32_t pool) {
  BookConfig c;
  c.tick_size = tick;
  c.base_price = base;
  c.num_levels = levels;
  c.max_orders = pool;
  return c;
}

WorkloadParams make_wp(std::uint64_t seed, const BookConfig& c, bool feed) {
  WorkloadParams p;
  p.seed = seed;
  p.base_price = c.base_price;
  p.tick_size = c.tick_size;
  p.num_levels = c.num_levels;
  p.feed_ops = feed;
  return p;
}

std::vector<Scenario> scenarios() {
  std::vector<Scenario> v;
  auto add = [&](const char* name, BookConfig c, std::uint64_t seed, bool feed) {
    v.push_back({name, c, make_wp(seed, c, feed)});
  };
  // Tick 100 and a non-zero base make sure nothing assumes tick == 1.
  add("lobster-like grid, roomy pool", make_cfg(100, 100000, 1024, 16384), 101, true);
  // A pool this small is exhausted constantly: exercises PoolExhausted paths.
  add("tiny pool", make_cfg(100, 100000, 1024, 24), 202, true);
  // Base 0 and tick 1 are the other extreme of the grid arithmetic.
  add("unit tick, zero base, narrow band", make_cfg(1, 0, 256, 2048), 303, true);
  add("engine ops only", make_cfg(5, 7000, 512, 4096), 404, false);
  // A fat tick and a mid-sized pool with feed ops, different seed again.
  add("wide tick", make_cfg(2500, 2500, 1024, 512), 505, true);
  return v;
}

}  // namespace

class Differential : public ::testing::TestWithParam<std::size_t> {};

TEST_P(Differential, EngineMatchesReferenceBook) {
  const Scenario sc = scenarios()[GetParam()];
  WorkloadParams wp = sc.wp;
  wp.n = diff_op_count(1000000);
  const auto ops = make_workload(wp);
  const DiffResult r = run_differential(sc.cfg, ops);
  ASSERT_TRUE(r.ok) << sc.name << "\n" << r.message;

  // Guard against a degenerate stream that would make agreement meaningless.
  if (wp.n >= 100000) {
    EXPECT_GT(r.trades, wp.n / 40) << "too few trades";
    const BookStats& s = r.stats;
    for (RejectReason why : {RejectReason::InvalidQty, RejectReason::PriceOutOfBand,
                             RejectReason::DuplicateOrderId, RejectReason::UnknownOrderId,
                             RejectReason::FokNotFillable, RejectReason::PostOnlyWouldCross})
      EXPECT_GT(s.reject_count(why), 0u) << to_string(why);
    // With tick_size 1 every in-band price is on the grid.
    if (sc.cfg.tick_size > 1) {
      EXPECT_GT(s.reject_count(RejectReason::PriceOffTick), 0u);
    }
    if (sc.cfg.max_orders < 100) {
      EXPECT_GT(s.reject_count(RejectReason::PoolExhausted), 1000u);
    }
    if (wp.feed_ops) {
      EXPECT_GT(r.ext_trades, wp.n / 5000);
      EXPECT_GT(s.reject_count(RejectReason::WrongOwner), 0u);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Scenarios, Differential, ::testing::Values(0, 1, 2, 3, 4),
                         [](const ::testing::TestParamInfo<std::size_t>& pi) {
                           return "s" + std::to_string(pi.param);
                         });
