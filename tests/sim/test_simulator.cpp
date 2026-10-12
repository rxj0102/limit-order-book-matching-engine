// Simulator mechanics with a scripted strategy and hand written SYNTHETIC feeds:
// latency timeline, queue position under latency, own-order fill rules under
// replay, cancel races, post-only rejects, the market view, and the PnL
// decomposition on a case worked out by hand.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "lobkit/sim/simulator.hpp"
#include "synth_feed.hpp"

using namespace lobkit;
using namespace lobkit::sim;
using lobkit::testing::VectorFeed;

namespace {

md::FeedMsg add(Timestamp t, OrderId id, Side s, Price px, Qty q) {
  md::FeedMsg m{};
  m.ts = t;
  m.id = id;
  m.side = s;
  m.price = px;
  m.qty = q;
  m.type = md::FeedType::Add;
  m.aux = 1;
  return m;
}
md::FeedMsg exec(Timestamp t, OrderId id, Side s, Price px, Qty q) {
  md::FeedMsg m = add(t, id, s, px, q);
  m.type = md::FeedType::Execute;
  m.aux = 4;
  return m;
}
md::FeedMsg del(Timestamp t, OrderId id, Side s, Price px) {
  md::FeedMsg m = add(t, id, s, px, 0);
  m.type = md::FeedType::Delete;
  m.aux = 3;
  return m;
}

BookConfig book_cfg() {
  BookConfig c;
  c.tick_size = 100;
  c.base_price = 900000;
  c.num_levels = 4096;
  c.max_orders = 4096;
  return c;
}

struct Entry {
  std::string what;
  Timestamp t;
  OrderId id = 0;
  Qty qty = 0;
  Price a = 0, b = 0;
  RejectReason reason = RejectReason::None;
  bool maker = false;
};

// Sends the planned orders, one per two sided market view, and logs every callback.
struct Scripted {
  struct Plan {
    Side side;
    Price price;
    Qty qty;
  };
  std::vector<Plan> plan;
  std::size_t next = 0;
  int cancel_at_view = -1;  // cancel the first order when this many views have arrived
  int views = 0;
  OrderId first_id = 0;
  std::vector<Entry> log;
  MarketView last{};

  template <class G>
  void on_market(const MarketView& v, G& gw) {
    ++views;
    last = v;
    log.push_back({"market", gw.now(), 0, 0, v.bid, v.ask});
    if (next < plan.size() && v.two_sided) {
      const Plan& p = plan[next++];
      const OrderId id = gw.send(p.side, p.price, p.qty);
      if (first_id == 0) first_id = id;
      log.push_back({"send", gw.now(), id, p.qty, p.price, 0});
    }
    if (views == cancel_at_view && first_id != 0) {
      gw.cancel(first_id);
      log.push_back({"cancel", gw.now(), first_id});
    }
  }
  template <class G>
  void on_ack(OrderId id, G& gw) {
    log.push_back({"ack", gw.now(), id});
  }
  template <class G>
  void on_reject(OrderId id, RejectReason r, G& gw) {
    log.push_back({"reject", gw.now(), id, 0, 0, 0, r});
  }
  template <class G>
  void on_fill(const FillNotice& f, G& gw) {
    log.push_back({"fill", gw.now(), f.id, f.qty, f.price, static_cast<Price>(f.exchange_ts),
                   RejectReason::None, f.maker});
  }
  template <class G>
  void on_cancel_ack(OrderId id, Qty q, G& gw) {
    log.push_back({"cancel_ack", gw.now(), id, q});
  }
  template <class G>
  void on_cancel_reject(OrderId id, G& gw) {
    log.push_back({"cancel_reject", gw.now(), id});
  }

  const Entry* find(const std::string& what, std::size_t nth = 0) const {
    for (const Entry& e : log)
      if (e.what == what && nth-- == 0) return &e;
    return nullptr;
  }
};

RunResult run(Scripted& s, std::vector<md::FeedMsg> feed, const SimConfig& cfg) {
  VectorFeed f(std::move(feed));
  Simulator<Scripted, VectorFeed> sim(book_cfg(), cfg, s, f);
  return sim.run();
}

// A two sided market: bid 999900 x100, ask 1000100 x100, both there by t = 1050.
std::vector<md::FeedMsg> base_market() {
  return {add(1000, 1, Side::Bid, 999900, 100), add(1050, 2, Side::Ask, 1000100, 100)};
}

}  // namespace

// ---- latency timeline --------------------------------------------------------------

TEST(SimLatency, ZeroLatencyEverythingHappensAtTheFeedTime) {
  Scripted s;
  s.plan = {{Side::Bid, 999800, 20}};
  auto feed = base_market();
  feed.push_back(add(5000, 3, Side::Bid, 999000, 1));  // a later message so the run continues
  const RunResult r = run(s, feed, SimConfig{});
  const Entry* send = s.find("send");
  const Entry* ack = s.find("ack");
  ASSERT_NE(send, nullptr);
  ASSERT_NE(ack, nullptr);
  EXPECT_EQ(send->t, 1050u);  // decided on the first two sided view
  EXPECT_EQ(ack->t, 1050u);   // arrived and acknowledged at the same instant
  EXPECT_EQ(r.stats.orders_accepted, 1u);
}

TEST(SimLatency, SubmitAckAndMarketDataDelaysAreEachApplied) {
  Scripted s;
  s.plan = {{Side::Bid, 999800, 20}};
  SimConfig cfg;
  cfg.md_latency_ns = 70;
  cfg.submit_latency_ns = 300;
  cfg.ack_latency_ns = 50;
  auto feed = base_market();
  feed.push_back(add(9000, 3, Side::Bid, 999000, 1));
  run(s, feed, cfg);
  // The two sided book exists at 1050; the strategy sees it at 1050 + 70.
  ASSERT_NE(s.find("send"), nullptr);
  EXPECT_EQ(s.find("send")->t, 1120u);
  // The order reaches the book at 1120 + 300 and the ack reaches the strategy 50 later.
  EXPECT_EQ(s.find("ack")->t, 1120u + 300u + 50u);
  // The view the strategy got is the snapshot taken at 1050, labeled with that time.
  EXPECT_EQ(s.last.ts, 1050u + 0u) << "last view is the one from the final feed message";
}

TEST(SimLatency, MarketDataSnapshotIsStaleByExactlyTheDelay) {
  Scripted s;
  SimConfig cfg;
  cfg.md_latency_ns = 400;
  auto feed = base_market();
  feed.push_back(add(1200, 3, Side::Ask, 1000000, 50));  // improves the ask at t = 1200
  feed.push_back(add(9000, 4, Side::Bid, 999000, 1));
  run(s, feed, cfg);
  // The strategy learns of the improved ask at 1200 + 400, not before.
  Timestamp when = 0;
  for (const Entry& e : s.log)
    if (e.what == "market" && e.b == 1000000) {
      when = e.t;
      break;
    }
  EXPECT_EQ(when, 1600u);
  // And at 1500 it still saw the old ask.
  for (const Entry& e : s.log)
    if (e.what == "market" && e.t < 1600 && e.t >= 1450) {
      EXPECT_EQ(e.b, 1000100);
    }
}

// ---- queue position and the own-order fill rules under replay ------------------------

namespace {
// Feed for the queue tests. Ask side level 1000100: X1 (10) first, then later adds.
std::vector<md::FeedMsg> queue_feed(Timestamp x2_time) {
  return {add(1000, 1, Side::Bid, 999900, 10),     add(1050, 10, Side::Ask, 1000100, 10),
          add(x2_time, 11, Side::Ask, 1000100, 8), add(2000, 12, Side::Ask, 1000100, 4),
          exec(2100, 10, Side::Ask, 1000100, 10),  exec(2200, 11, Side::Ask, 1000100, 8),
          exec(2300, 12, Side::Ask, 1000100, 2),   add(9000, 2, Side::Bid, 999000, 1)};
}
}  // namespace

TEST(SimFillRules, OwnOrderQueuesBehindEarlierFeedOrdersAndFillsOnlyWhenFlowReachesPastIt) {
  Scripted s;
  s.plan = {{Side::Ask, 1000100, 5}};  // join the ask queue at the touch
  SimConfig cfg;
  cfg.submit_latency_ns =
      500;  // arrives at 1050 + 500 = 1550, after X2 (t = 1300) and before X3 (t = 2000)
  const RunResult r = run(s, queue_feed(1300), cfg);
  // Queue at 2000: X1(10), X2(8), own(5), X3(4).
  // exec X1 at 2100: own is behind X1 and X2, untouched.
  // exec X2 at 2200: own is at the front now but nothing behind it has traded yet, so no fill.
  // exec X3 at 2300 for 2: X3 is behind the own order, whose 5 shares sit ahead of it, so it
  // fills 2.
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].ts, 2300u);
  EXPECT_EQ(r.fills[0].qty, 2u);
  EXPECT_EQ(r.fills[0].side, Side::Ask);
  EXPECT_EQ(r.fills[0].price, 1000100);
  EXPECT_TRUE(r.fills[0].maker);
  EXPECT_EQ(r.pnl.inventory, -2);
  EXPECT_EQ(r.book.queue_order_anomalies, 0u);  // the feed itself was consistent
}

TEST(SimFillRules, LessLatencyMeansABetterQueuePositionAndAnEarlierFill) {
  Scripted s;
  s.plan = {{Side::Ask, 1000100, 5}};
  SimConfig cfg;  // zero latency: the order arrives at 1050, directly behind X1 and ahead of X2
  const RunResult r = run(s, queue_feed(1300), cfg);
  // Queue: X1(10), own(5), X2(8), X3(4). exec X1 at 2100: nothing for us. exec X2 at
  // 2200 for 8: X2 is behind the own order, which is at the front, so it fills all 5.
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].ts, 2200u);
  EXPECT_EQ(r.fills[0].qty, 5u);
  EXPECT_EQ(r.pnl.inventory, -5);
}

TEST(SimFillRules, FeedCancelsAheadMoveUsUpButNeverFillUs) {
  Scripted s;
  s.plan = {{Side::Ask, 1000100, 5}};
  std::vector<md::FeedMsg> feed = {add(1000, 1, Side::Bid, 999900, 10),
                                   add(1050, 10, Side::Ask, 1000100, 10),
                                   add(1500, 12, Side::Ask, 1000100, 4),
                                   del(2000, 10, Side::Ask, 1000100),  // the order ahead leaves
                                   add(9000, 2, Side::Bid, 999000, 1)};
  const RunResult r = run(s, feed, SimConfig{});
  EXPECT_TRUE(r.fills.empty());
  EXPECT_EQ(r.stats.orders_accepted, 1u);
}

// ---- the market view excludes own orders -----------------------------------------------

TEST(SimView, OwnQuotesNeverAppearInTheMarketView) {
  Scripted s;
  s.plan = {{Side::Bid, 1000000, 40}, {Side::Ask, 1000100, 40}};
  // A wide market (bid 999000, ask 1001000) so both quotes sit inside the spread. Later
  // bids improve the feed bid step by step, which makes the strategy see new views.
  std::vector<md::FeedMsg> feed = {
      add(1000, 1, Side::Bid, 999000, 100), add(1050, 2, Side::Ask, 1001000, 100),
      add(2000, 3, Side::Bid, 999100, 1), add(3000, 4, Side::Bid, 999200, 1),
      add(4000, 5, Side::Bid, 999300, 1)};
  const RunResult r = run(s, feed, SimConfig{});
  EXPECT_EQ(r.stats.orders_accepted, 2u);
  int seen = 0;
  for (const Entry& e : s.log)
    if (e.what == "market" && e.b != 0) {  // skip the first view, which has no ask yet
      ++seen;
      EXPECT_LT(e.a, 1000000) << "own bid leaked into the view";
      EXPECT_EQ(e.b, 1001000) << "own ask leaked into the view";
    }
  EXPECT_EQ(seen, 4);
}

// ---- cancel races, rejects ---------------------------------------------------------------

TEST(SimRaces, CancelThatArrivesAfterTheFillIsReportedTooLate) {
  Scripted s;
  s.plan = {{Side::Bid, 999800, 20}};
  s.cancel_at_view = 3;  // views: 1 at 1000 (one sided), 2 at 1050 (we send), 3 at 2150 (we cancel)
  SimConfig cfg;
  cfg.submit_latency_ns = 1000;
  cfg.ack_latency_ns = 10;
  std::vector<md::FeedMsg> feed = {
      add(1000, 1, Side::Bid, 999900, 10),
      add(1050, 2, Side::Ask, 1000100, 10),  // view 1 at 1050: we send
      add(1500, 20, Side::Bid, 999800, 30),  // Y ahead of us (we arrive 2050)
      add(2100, 21, Side::Bid, 999800, 30),  // Z behind us
      add(2150, 5, Side::Ask, 1000000, 1),   // improves the ask: view 3 at 2150
      exec(2200, 20, Side::Bid, 999800, 30),
      exec(2300, 21, Side::Bid, 999800, 30),  // drains Y, then Z fills us
      add(9000, 6, Side::Bid, 990000, 1)};
  const RunResult r = run(s, feed, cfg);
  // The cancel decided at 2150 arrives at 3150; the order was fully filled at 2300.
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].qty, 20u);
  EXPECT_EQ(r.stats.cancels_sent, 1u);
  EXPECT_EQ(r.stats.cancels_too_late, 1u);
  EXPECT_EQ(r.stats.cancels_confirmed, 0u);
  ASSERT_NE(s.find("cancel_reject"), nullptr);
  // The fill notice reached the strategy before the answer to its late cancel.
  EXPECT_LT(s.find("fill")->t, s.find("cancel_reject")->t);
}

TEST(SimRaces, CancelOfARestingOrderIsConfirmedWithTheRemainingQuantity) {
  Scripted s;
  s.plan = {{Side::Bid, 999800, 20}};
  s.cancel_at_view = 2;
  std::vector<md::FeedMsg> feed = base_market();
  feed.push_back(add(3000, 3, Side::Bid, 998000, 1));  // second view
  feed.push_back(add(9000, 4, Side::Bid, 997000, 1));
  const RunResult r = run(s, feed, SimConfig{});
  EXPECT_EQ(r.stats.cancels_confirmed, 1u);
  ASSERT_NE(s.find("cancel_ack"), nullptr);
  EXPECT_EQ(s.find("cancel_ack")->qty, 20u);
}

TEST(SimRaces, PostOnlyOrderThatWouldCrossOnArrivalIsRejected) {
  Scripted s;
  s.plan = {{Side::Bid, 1000300, 10}};  // below the ask it saw (1000500)
  SimConfig cfg;
  cfg.submit_latency_ns = 1000;  // arrives at 2050
  std::vector<md::FeedMsg> feed = {
      add(1000, 1, Side::Bid, 999900, 10), add(1050, 2, Side::Ask, 1000500, 10),
      add(1800, 3, Side::Ask, 1000200, 10),  // the ask drops below our bid
      add(9000, 4, Side::Bid, 990000, 1)};
  const RunResult r = run(s, feed, cfg);
  ASSERT_NE(s.find("reject"), nullptr);
  EXPECT_EQ(s.find("reject")->reason, RejectReason::PostOnlyWouldCross);
  EXPECT_EQ(r.stats.orders_rejected, 1u);
  EXPECT_TRUE(r.fills.empty());  // a post-only order never takes liquidity
}

// ---- PnL decomposition, worked by hand -------------------------------------------------------

TEST(SimPnl, DecompositionMatchesAHandComputedExample) {
  Scripted s;
  s.plan = {{Side::Bid, 999800, 50}};
  SimConfig cfg;
  cfg.maker_fee = -10;  // a rebate of 10 native units (a tenth of a cent) per share
  std::vector<md::FeedMsg> feed = {
      add(1000, 1, Side::Bid, 999900, 100),  add(1050, 2, Side::Ask, 1000100, 100),  // mid 100.00
      add(1500, 3, Side::Bid, 999800, 40),    // queues behind our bid (we joined first at 1050)
      exec(2000, 3, Side::Bid, 999800, 30),   // order 3 trades 30; ours is ahead and fills 30
      del(3000, 2, Side::Ask, 1000100),       // the ask moves up: ...
      add(3000, 4, Side::Ask, 1000300, 100),  // ... mid becomes (999900 + 1000300) / 2
      add(4000, 5, Side::Bid, 990000, 1)};
  const RunResult r = run(s, feed, cfg);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].qty, 30u);
  // Bought 30 at 99.98 against a mid of 100.00: capture = 30 * (200000 - 199960 ... in half units
  //   mid2 = 999900 + 1000100 = 2000000, price2 = 2 * 999800 = 1999600, diff 400
  EXPECT_EQ(r.fills[0].mid2_ref, 2000000);
  EXPECT_EQ(r.pnl.spread2, static_cast<Money2>(30) * 400);  // 12000 half units = $0.60
  // The mid then rises to (999900 + 1000300): mid2 = 2000200, +200 half units on 30 shares.
  EXPECT_EQ(r.pnl.inventory2, static_cast<Money2>(30) * 200);  // 6000 half units = $0.30
  // Fees: 30 shares at -10 native units = -300 native = -600 half units, a rebate.
  EXPECT_EQ(r.pnl.fees2, -600);
  EXPECT_EQ(r.pnl.total2(), 12000 + 6000 + 600);  // $0.93
  // The independent ledger agrees: cash -30 * 1999600, inventory 30 marked at 2000200, minus fees.
  EXPECT_EQ(r.pnl.cash2, static_cast<Money2>(-30) * 1999600);
  EXPECT_EQ(r.pnl.total2_from_ledger(), r.pnl.total2());
  EXPECT_EQ(r.pnl.inventory, 30);
  EXPECT_EQ(r.fills[0].fee_units, -300);
}

TEST(SimPnl, SellsAreSignedCorrectly) {
  Scripted s;
  s.plan = {{Side::Ask, 1000200, 50}};
  SimConfig cfg;
  cfg.maker_fee = 5;  // a fee this time
  std::vector<md::FeedMsg> feed = {
      add(1000, 1, Side::Bid, 999900, 100), add(1050, 2, Side::Ask, 1000100, 100),
      add(1500, 3, Side::Ask, 1000200, 40), exec(2000, 3, Side::Ask, 1000200, 10),
      add(9000, 5, Side::Bid, 990000, 1)};
  const RunResult r = run(s, feed, cfg);
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_EQ(r.fills[0].side, Side::Ask);
  // Sold 10 at 100.02 against a mid of 100.00: capture = 10 * (price2 - mid2) = 10 * 400.
  EXPECT_EQ(r.pnl.spread2, static_cast<Money2>(10) * 400);
  EXPECT_EQ(r.pnl.fees2, 2 * 10 * 5);
  EXPECT_EQ(r.pnl.inventory, -10);
  EXPECT_EQ(r.pnl.total2(), r.pnl.total2_from_ledger());
}

// ---- guards -------------------------------------------------------------------------------------

TEST(SimGuards, FeedIdsInsideTheOwnRangeAreSkippedNotMixedUp) {
  Scripted s;
  std::vector<md::FeedMsg> feed = base_market();
  feed.push_back(
      add(2000, kOwnIdBase + 5, Side::Bid, 999000, 10));  // a feed id in the reserved range
  feed.push_back(add(3000, 3, Side::Bid, 998000, 1));
  const RunResult r = run(s, feed, SimConfig{});
  EXPECT_EQ(r.stats.feed_id_collisions, 1u);
  EXPECT_EQ(r.stats.feed_messages, 3u);
}

TEST(SimGuards, EmptyFeedRunsAndReportsItsStatus) {
  Scripted s;
  const RunResult r = run(s, {}, SimConfig{});
  EXPECT_EQ(r.stats.feed_messages, 0u);
  EXPECT_EQ(r.feed_status, md::ParseStatus::End);
  EXPECT_TRUE(r.fills.empty());
}

// ---- notice latency and the tie rule
// -------------------------------------------------------------

TEST(SimLatency, FillNoticeArrivesExactlyAckLatencyAfterTheFill) {
  Scripted s;
  s.plan = {{Side::Ask, 1000100, 5}};
  SimConfig cfg;
  cfg.ack_latency_ns = 250;
  const RunResult r =
      run(s, queue_feed(1300), cfg);  // zero submit latency: fills at 2200 for 5 (see above)
  ASSERT_EQ(r.fills.size(), 1u);
  const Entry* fill = s.find("fill");
  ASSERT_NE(fill, nullptr);
  EXPECT_EQ(fill->b, 2200);          // exchange time carried in the notice
  EXPECT_EQ(fill->t, 2200u + 250u);  // delivered ack_latency later
  EXPECT_TRUE(fill->maker) << "a resting order that is traded against is the maker";
  EXPECT_EQ(fill->qty, 5u);
  EXPECT_EQ(fill->a, 1000100);
}

TEST(SimOrdering, QueuedEventsRunBeforeTheNextFeedMessageAtTheSameTimestamp) {
  // With zero latency the strategy reacts to a feed message before the next message with the
  // same timestamp is applied, so its order reaches the book first and queues ahead of it.
  Scripted s;
  s.plan = {{Side::Bid, 999800, 10}};
  std::vector<md::FeedMsg> feed = {
      add(1000, 1, Side::Bid, 999900, 10), add(1050, 2, Side::Ask, 1000100, 10),
      add(1050, 20, Side::Bid, 999800, 10)};  // same timestamp as the ask
  VectorFeed f(feed);
  Simulator<Scripted, VectorFeed> sim(book_cfg(), SimConfig{}, s, f);
  sim.run();
  const auto queue = sim.engine().book().queue_ids(999800);
  ASSERT_EQ(queue.size(), 2u);
  EXPECT_EQ(queue[0], kOwnIdBase + 1)
      << "the strategy order was applied before the second feed message";
  EXPECT_EQ(queue[1], 20u);
}
