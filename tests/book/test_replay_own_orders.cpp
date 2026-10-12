// Pins MATCHING_RULES.md section 9: how own (simulated) orders interact with
// feed executions applied through the replay adapter.
#include "test_support.hpp"

using namespace lobkit;
using namespace lobkit::testing;

namespace {
constexpr OwnerId kMe = 1;
constexpr OrderId kX1 = 1, kX2 = 2, kOwn = 1000;
}  // namespace

class OwnOrderTest : public BookTest {
 protected:
  ReplayAdapter<VectorSink> feed() { return e.replay_adapter(); }

  // Level at tick 500, front to back: feed X1 (10), own O (5), feed X2 (8).
  void setup_example() {
    feed().apply_add(kX1, Side::Ask, px(500), 10, ++t);
    limit(kOwn, Side::Ask, 500, 5, kMe);
    feed().apply_add(kX2, Side::Ask, px(500), 8, ++t);
    clear();
  }
  Qty own_qty() { return book().find_order(kOwn) ? book().find_order(kOwn)->qty : 0; }
};

TEST_F(OwnOrderTest, OwnOrderRestsAtBackBehindExistingFeedOrders) {
  feed().apply_add(kX1, Side::Ask, px(500), 10, ++t);
  feed().apply_add(kX2, Side::Ask, px(500), 8, ++t);
  limit(kOwn, Side::Ask, 500, 5, kMe);
  EXPECT_EQ(queue(500), (std::vector<OrderId>{kX1, kX2, kOwn}));
  feed().apply_add(3, Side::Ask, px(500), 1, ++t);
  EXPECT_EQ(queue(500),
            (std::vector<OrderId>{kX1, kX2, kOwn, 3}));  // later feed orders queue behind
}

TEST_F(OwnOrderTest, ExecutingAFeedOrderAheadNeverTouchesTheOwnOrder) {
  setup_example();
  feed().apply_execute(kX1, 4, ++t);
  EXPECT_EQ(own_qty(), 5u);
  EXPECT_EQ(book().find_order(kX1)->qty, 6u);
  ASSERT_EQ(ev().size(), 1u);  // only the feed trade
  expect_valid();
}

TEST_F(OwnOrderTest, DrainingFeedOrdersAheadDoesNotFillByItself) {
  setup_example();
  feed().apply_execute(kX1, 10, ++t);  // X1 gone, own order now at the front
  EXPECT_EQ(queue(500), (std::vector<OrderId>{kOwn, kX2}));
  EXPECT_EQ(own_qty(), 5u);
  feed().apply_reduce(kX2, 3, ++t);  // cancels and reductions never fill us
  feed().apply_delete(kX2, ++t);
  EXPECT_EQ(own_qty(), 5u);
  for (const Event& x : ev())
    if (x.type == EventType::Trade) {
      EXPECT_NE(x.other, kOwn);
    }
  expect_valid();
}

TEST_F(OwnOrderTest, FillsWhenFlowTradesThroughToAnOrderBehindIt) {
  setup_example();
  feed().apply_execute(kX1, 10, ++t);
  clear();
  feed().apply_execute(kX2, 3, ++t);
  ASSERT_EQ(ev().size(), 2u);
  // Own fill first (it has priority), then the feed order.
  EXPECT_EQ(ev()[0].type, EventType::Trade);
  EXPECT_EQ(ev()[0].other, kOwn);
  EXPECT_EQ(ev()[0].qty, 3u);
  EXPECT_EQ(ev()[0].id, 0u);
  EXPECT_EQ(ev()[0].flags & kTradeExternalTaker, kTradeExternalTaker);
  EXPECT_EQ(ev()[1].other, kX2);
  EXPECT_EQ(ev()[1].qty, 3u);
  EXPECT_EQ(own_qty(), 2u);
  EXPECT_EQ(book().find_order(kX2)->qty, 5u);  // feed order reduced by exactly the feed qty
  expect_valid();
}

TEST_F(OwnOrderTest, OwnFillIsBoundedByTheExecutedQuantityAndRemovesFilledOrder) {
  setup_example();
  feed().apply_execute(kX1, 10, ++t);
  clear();
  feed().apply_execute(kX2, 8, ++t);  // more than our 5
  EXPECT_EQ(ev()[0].qty, 5u);
  EXPECT_EQ(book().find_order(kOwn), nullptr);
  EXPECT_EQ(book().find_order(kX2), nullptr);
  expect_valid();
}

TEST_F(OwnOrderTest, FeedOrderAheadOfOwnOrderBlocksTheFillAndIsCountedAsAnomaly) {
  setup_example();  // X1 is still ahead of the own order
  feed().apply_execute(kX2, 3, ++t);
  EXPECT_EQ(own_qty(), 5u);
  EXPECT_EQ(book().stats().queue_order_anomalies, 1u);
  EXPECT_EQ(book().find_order(kX2)->qty, 5u);  // the feed itself is still applied
  expect_valid();
}

TEST_F(OwnOrderTest, OwnBidsFollowTheSameRulesOnTheOtherSide) {
  feed().apply_add(kX1, Side::Bid, px(400), 10, ++t);
  limit(kOwn, Side::Bid, 400, 5, kMe);
  feed().apply_add(kX2, Side::Bid, px(400), 8, ++t);
  feed().apply_delete(kX1, ++t);
  clear();
  feed().apply_execute(kX2, 2, ++t);
  EXPECT_EQ(ev()[0].other, kOwn);
  EXPECT_EQ(ev()[0].side, Side::Ask);  // aggressor sold into the bid
  EXPECT_EQ(own_qty(), 3u);
}

TEST_F(OwnOrderTest, FeedAddCrossingAnOwnRestingOrderTradesAndDisplacesFeedQuantity) {
  limit(kOwn, Side::Bid, 500, 5, kMe);  // own bid, nothing on the ask side
  clear();
  feed().apply_add(kX1, Side::Ask, px(500), 8, ++t);  // feed ask at our price
  const auto trades = of_type(EventType::Trade);
  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].other, kOwn);
  EXPECT_EQ(trades[0].qty, 5u);
  EXPECT_EQ(trades[0].id, kX1);  // the feed order is the aggressor here
  EXPECT_EQ(book().find_order(kX1)->qty, 3u);
  EXPECT_EQ(book().stats().feed_add_displaced_qty, 5u);
  EXPECT_EQ(book().stats().feed_crossed_fills, 0u);
  expect_valid();
}

TEST_F(OwnOrderTest, OwnAggressiveOrderMatchesFeedOrdersByPriceTimePriority) {
  feed().apply_add(kX1, Side::Ask, px(500), 10, ++t);
  feed().apply_add(kX2, Side::Ask, px(500), 8, ++t);
  clear();
  limit(kOwn, Side::Bid, 500, 12, kMe);
  const auto trades = of_type(EventType::Trade);
  ASSERT_EQ(trades.size(), 2u);
  EXPECT_EQ(trades[0].other, kX1);
  EXPECT_EQ(trades[0].qty, 10u);
  EXPECT_EQ(trades[1].other, kX2);
  EXPECT_EQ(trades[1].qty, 2u);
  EXPECT_EQ(trades[0].flags & kTradeExternalTaker, 0);
  expect_valid();
}
