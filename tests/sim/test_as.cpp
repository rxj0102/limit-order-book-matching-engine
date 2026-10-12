// Strategy math and planning. All data here is synthetic and hand picked.
#include <gtest/gtest.h>

#include <cmath>

#include "lobkit/sim/avellaneda_stoikov.hpp"
#include "workload.hpp"

using namespace lobkit;
using namespace lobkit::sim;

namespace {
MarketView view(Price bid, Price ask) {
  MarketView v;
  v.bid = bid;
  v.ask = ask;
  v.bid_qty = v.ask_qty = 1000;
  v.two_sided = true;
  return v;
}
double dollars(Price p) {
  return static_cast<double>(p) / kUnitsPerDollar;
}
}  // namespace

TEST(ASMath, ReservationPriceAndSpreadMatchTheFormulas) {
  // s = 100, sigma = 0.02 $/sqrt(s), tau = 10 s, gamma = 0.05 /$, k = 50 /$
  const double sigma2 = 0.02 * 0.02;
  const ASQuote flat = as_quote(100.0, 0, sigma2, 10.0, 0.05, 50.0);
  const double spread = 0.05 * sigma2 * 10.0 + (2.0 / 0.05) * std::log(1.0 + 0.05 / 50.0);
  EXPECT_DOUBLE_EQ(flat.reservation, 100.0);  // no inventory: reservation is the mid
  EXPECT_NEAR(flat.spread, spread, 1e-12);
  EXPECT_NEAR(flat.spread, 0.04018001, 1e-8);  // hand value: 0.0002 + 40 * 0.00099950033
  EXPECT_NEAR(flat.bid, 100.0 - spread / 2, 1e-12);
  EXPECT_NEAR(flat.ask, 100.0 + spread / 2, 1e-12);

  // Long 200 shares: the reservation price drops by q * gamma * sigma^2 * tau = 0.04
  const ASQuote lng = as_quote(100.0, 200, sigma2, 10.0, 0.05, 50.0);
  EXPECT_NEAR(lng.reservation, 99.96, 1e-12);
  EXPECT_NEAR(lng.spread, flat.spread, 1e-12);  // the spread does not depend on inventory
  const ASQuote sht = as_quote(100.0, -200, sigma2, 10.0, 0.05, 50.0);
  EXPECT_NEAR(sht.reservation, 100.04, 1e-12);
}

TEST(ASMath, RiskAversionWidensTheInventoryTermAndNarrowsTheDepthTerm) {
  const double sigma2 = 4e-4;
  const ASQuote lo = as_quote(100.0, 100, sigma2, 10.0, 0.01, 50.0);
  const ASQuote hi = as_quote(100.0, 100, sigma2, 10.0, 0.5, 50.0);
  EXPECT_GT(100.0 - lo.reservation, 0.0);
  EXPECT_GT(100.0 - hi.reservation, 100.0 - lo.reservation);  // more risk aversion, stronger skew
  // Larger k (a thinner tail of arrivals) tightens the spread.
  EXPECT_LT(as_quote(100.0, 0, sigma2, 10.0, 0.05, 200.0).spread,
            as_quote(100.0, 0, sigma2, 10.0, 0.05, 20.0).spread);
}

TEST(ASPlan, QuotesAreRoundedOutwardToTheTickGrid) {
  ASParams p;  // gamma .05, k 50, order 100, max inventory 500, tick 100
  const PlannedQuotes q = plan_quotes(view(999900, 1000100), 100.0, 4e-4, 10.0, 0, 0, 0, 0, 0, p);
  // half spread = 0.04018001 / 2 = 0.020090007 dollars
  // bid 99.979909993 dollars -> 999799.0999 native -> floor to the 100 grid = 999700
  // ask 100.020090007 dollars -> 1000200.9001 native -> ceil to the 100 grid = 1000300
  ASSERT_TRUE(q.bid.active);
  ASSERT_TRUE(q.ask.active);
  EXPECT_EQ(q.bid.price, 999700);
  EXPECT_EQ(q.ask.price, 1000300);
  EXPECT_EQ(q.bid.qty, 100u);
  EXPECT_EQ(q.ask.qty, 100u);
}

TEST(ASPlan, InventorySkewsBothQuotesDown) {
  ASParams p;
  p.gamma = 0.5;  // strong, to see the effect with a coarse tick
  const PlannedQuotes flat =
      plan_quotes(view(999900, 1000100), 100.0, 4e-4, 10.0, 0, 0, 0, 0, 0, p);
  const PlannedQuotes lng =
      plan_quotes(view(999900, 1000100), 100.0, 4e-4, 10.0, 300, 0, 0, 0, 0, p);
  EXPECT_LT(lng.bid.price, flat.bid.price);
  EXPECT_LT(lng.ask.price, flat.ask.price);
}

TEST(ASPlan, QuotesStayPassiveAgainstTheViewedBook) {
  ASParams p;
  // A very wide model spread would never cross, so make the model want to cross: huge
  // negative inventory pushes the reservation price up through the ask.
  p.gamma = 5.0;
  const MarketView v = view(999900, 1000100);
  const PlannedQuotes q = plan_quotes(v, 100.0, 4e-2, 100.0, -400, 0, 0, 0, 0, p);
  ASSERT_TRUE(q.bid.active);
  EXPECT_LE(q.bid.price, v.ask - p.tick);  // bid never at or through the viewed ask
  EXPECT_GE(q.ask.price, v.bid + p.tick);  // ask never at or through the viewed bid
}

TEST(ASPlan, OwnQuotesNeverCrossWhateverTheInputs) {
  lobkit::testing::SplitMix64 rng(5);
  for (int i = 0; i < 20000; ++i) {
    ASParams p;
    p.gamma = 0.001 + 0.001 * static_cast<double>(rng.below(2000));
    p.k = 5.0 + static_cast<double>(rng.below(300));
    p.order_size = static_cast<Qty>(1 + rng.below(300));
    p.max_inventory = static_cast<std::int64_t>(1 + rng.below(2000));
    p.size_skew = static_cast<double>(rng.below(101)) / 100.0;
    const Price bid = 900000 + 100 * static_cast<Price>(rng.below(200));
    const Price ask = bid + 100 * static_cast<Price>(1 + rng.below(10));
    const std::int64_t inv = static_cast<std::int64_t>(rng.below(4001)) - 2000;
    const double sigma2 = 1e-6 * static_cast<double>(1 + rng.below(1000));
    const double tau = 1.0 + static_cast<double>(rng.below(100));
    const PlannedQuotes q =
        plan_quotes(view(bid, ask), dollars(bid + ask) / 2, sigma2, tau, inv, 0, 0, 0, 0, p);
    if (q.bid.active && q.ask.active) {
      ASSERT_LT(q.bid.price, q.ask.price) << i;
    }
    if (q.bid.active) {
      ASSERT_LT(q.bid.price, ask);
      ASSERT_EQ(q.bid.price % p.tick, 0);
    }
    if (q.ask.active) {
      ASSERT_GT(q.ask.price, bid);
      ASSERT_EQ(q.ask.price % p.tick, 0);
    }
  }
}

TEST(ASPlan, InventoryLimitCapsTheSizeAndSwitchesTheSideOff) {
  ASParams p;
  p.max_inventory = 500;
  p.order_size = 100;
  const MarketView v = view(999900, 1000100);
  auto plan = [&](std::int64_t inv, std::int64_t bid_work = 0, std::int64_t ask_work = 0) {
    return plan_quotes(v, 100.0, 4e-4, 10.0, inv, bid_work, 0, ask_work, 0, p);
  };
  EXPECT_EQ(plan(0).bid.qty, 100u);
  EXPECT_EQ(plan(450).bid.qty, 50u);   // room for 50 more
  EXPECT_FALSE(plan(500).bid.active);  // at the limit: no bid at all
  EXPECT_TRUE(plan(500).ask.active);   // but selling is welcome
  EXPECT_EQ(plan(500).ask.qty, 100u);
  EXPECT_FALSE(plan(-500).ask.active);
  EXPECT_TRUE(plan(-500).bid.active);
  // Shares already working on a side count against the room.
  EXPECT_EQ(plan(300, /*bid_work=*/150).bid.qty, 50u);
  EXPECT_FALSE(plan(300, /*bid_work=*/200).bid.active);
  EXPECT_EQ(plan(-300, 0, /*ask_work=*/150).ask.qty, 50u);
}

TEST(ASPlan, SizeSkewShrinksTheSideThatAddsToThePosition) {
  ASParams p;
  p.size_skew = 0.5;
  p.max_inventory = 1000;
  const MarketView v = view(999900, 1000100);
  const PlannedQuotes q =
      plan_quotes(v, 100.0, 4e-4, 10.0, 500, 0, 0, 0, 0, p);  // half way to the limit
  EXPECT_EQ(q.bid.qty, 75u);                                  // 100 * (1 - 0.5 * 0.5)
  EXPECT_EQ(q.ask.qty, 100u);                                 // the reducing side keeps full size
  const PlannedQuotes s = plan_quotes(v, 100.0, 4e-4, 10.0, -500, 0, 0, 0, 0, p);
  EXPECT_EQ(s.ask.qty, 75u);
  EXPECT_EQ(s.bid.qty, 100u);
}

TEST(ASPlan, OneSidedOrNonFiniteInputProducesNoQuotes) {
  ASParams p;
  MarketView v = view(999900, 1000100);
  v.two_sided = false;
  EXPECT_FALSE(plan_quotes(v, 100.0, 4e-4, 10.0, 0, 0, 0, 0, 0, p).bid.active);
  v.two_sided = true;
  EXPECT_FALSE(plan_quotes(v, std::nan(""), 4e-4, 10.0, 0, 0, 0, 0, 0, p).bid.active);
  p.gamma = 0.0;  // the model divides by gamma: the result is infinite, so no quotes
  EXPECT_FALSE(plan_quotes(v, 100.0, 4e-4, 10.0, 0, 0, 0, 0, 0, p).bid.active);
}

TEST(ASPlan, BandEdgesAreRespected) {
  ASParams p;
  p.min_price = 999900;
  p.max_price = 1000100;
  const PlannedQuotes q = plan_quotes(view(999900, 1000100), 100.0, 4e-4, 10.0, 0, 0, 0, 0, 0, p);
  if (q.bid.active) {
    EXPECT_GE(q.bid.price, p.min_price);
  }
  if (q.ask.active) {
    EXPECT_LE(q.ask.price, p.max_price);
  }
}
