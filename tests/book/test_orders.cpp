#include "test_support.hpp"

using namespace lobkit;
using namespace lobkit::testing;

class OrdersTest : public BookTest {};

TEST_F(OrdersTest, LimitRestsOnEmptyBook) {
  limit(1, Side::Bid, 500, 10);
  ASSERT_EQ(ev().size(), 1u);
  EXPECT_EQ(ev()[0].type, EventType::Ack);
  EXPECT_EQ(ev()[0].id, 1u);
  EXPECT_EQ(ev()[0].price, px(500));
  ASSERT_TRUE(book().has_bid());
  EXPECT_EQ(book().best_bid(), px(500));
  EXPECT_FALSE(book().has_ask());
  EXPECT_EQ(book().depth_at(px(500)), 10u);
  expect_valid();
}

TEST_F(OrdersTest, SequenceNumbersAreContiguousFromOne) {
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Bid, 510, 5);
  cancel(99);
  ASSERT_GE(ev().size(), 4u);
  for (std::size_t i = 0; i < ev().size(); ++i) EXPECT_EQ(ev()[i].seq, i + 1);
}

TEST_F(OrdersTest, PriceTimePriorityAtOneLevel) {
  limit(1, Side::Ask, 510, 10);
  limit(2, Side::Ask, 510, 10);
  limit(3, Side::Ask, 510, 10);
  clear();
  limit(9, Side::Bid, 510, 25);
  const auto trades = of_type(EventType::Trade);
  ASSERT_EQ(trades.size(), 3u);
  EXPECT_EQ(trades[0].other, 1u);
  EXPECT_EQ(trades[0].qty, 10u);
  EXPECT_EQ(trades[1].other, 2u);
  EXPECT_EQ(trades[1].qty, 10u);
  EXPECT_EQ(trades[2].other, 3u);
  EXPECT_EQ(trades[2].qty, 5u);
  for (const Event& tr : trades) {
    EXPECT_EQ(tr.id, 9u);
    EXPECT_EQ(tr.price, px(510));
    EXPECT_EQ(tr.side, Side::Bid);  // aggressor side
  }
  EXPECT_EQ(queue(510), (std::vector<OrderId>{3}));
  EXPECT_EQ(book().find_order(3)->qty, 5u);  // partial fill keeps head priority
  EXPECT_FALSE(book().has_bid());
  expect_valid();
}

TEST_F(OrdersTest, SweepsLevelsAtMakerPriceAndRestsRemainder) {
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Ask, 511, 5);
  limit(3, Side::Ask, 513, 5);
  clear();
  limit(9, Side::Bid, 512, 20);
  const auto trades = of_type(EventType::Trade);
  ASSERT_EQ(trades.size(), 2u);
  EXPECT_EQ(trades[0].price, px(510));  // price improvement for the taker
  EXPECT_EQ(trades[1].price, px(511));
  EXPECT_EQ(book().best_bid(), px(512));
  EXPECT_EQ(book().depth_at(px(512)), 10u);
  EXPECT_EQ(book().best_ask(), px(513));
  expect_valid();
}

TEST_F(OrdersTest, NonMarketableLimitDoesNotTrade) {
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Bid, 509, 5);
  EXPECT_TRUE(of_type(EventType::Trade).empty());
  EXPECT_EQ(book().best_bid(), px(509));
  EXPECT_EQ(book().best_ask(), px(510));
  expect_valid();
}

TEST_F(OrdersTest, ZeroQuantityRejected) {
  limit(1, Side::Bid, 500, 0);
  EXPECT_EQ(last_reject(), RejectReason::InvalidQty);
  EXPECT_EQ(book().live_orders(), 0u);
  EXPECT_EQ(book().stats().reject_count(RejectReason::InvalidQty), 1u);
  EXPECT_TRUE(of_type(EventType::Ack).empty());
}

TEST_F(OrdersTest, DuplicateLiveIdRejectedForEveryType) {
  limit(1, Side::Bid, 500, 10);
  for (OrderType ty :
       {OrderType::Limit, OrderType::Market, OrderType::Ioc, OrderType::Fok, OrderType::PostOnly}) {
    clear();
    limit(1, Side::Ask, 520, 5, 0, ty);
    EXPECT_EQ(last_reject(), RejectReason::DuplicateOrderId) << to_string(ty);
  }
  EXPECT_EQ(book().live_orders(), 1u);
  expect_valid();
}

TEST_F(OrdersTest, IdMayBeReusedAfterOrderIsGone) {
  limit(1, Side::Bid, 500, 10);
  cancel(1);
  clear();
  limit(1, Side::Bid, 501, 10);
  EXPECT_EQ(ev()[0].type, EventType::Ack);
  expect_valid();
}

TEST_F(OrdersTest, PriceBoundariesAndGridChecks) {
  limit(1, Side::Bid, 0, 1);     // first tick of the band
  limit(2, Side::Ask, 1023, 1);  // last tick of the band
  EXPECT_EQ(book().live_orders(), 2u);
  clear();
  limit(3, Side::Ask, 1024, 1);  // one past the band
  EXPECT_EQ(last_reject(), RejectReason::PriceOutOfBand);
  e.submit({4, px(0) - 100, 1, Side::Bid, OrderType::Limit, 0, ++t});  // one tick below base
  EXPECT_EQ(last_reject(), RejectReason::PriceOutOfBand);
  e.submit({5, px(10) + 1, 1, Side::Bid, OrderType::Limit, 0, ++t});  // inside the band, off grid
  EXPECT_EQ(last_reject(), RejectReason::PriceOffTick);
  e.submit({6, px(1023) + 99, 1, Side::Bid, OrderType::Limit, 0, ++t});  // inside the last tick
  EXPECT_EQ(last_reject(), RejectReason::PriceOffTick);
  e.submit({7, px(1023) + 100, 1, Side::Bid, OrderType::Limit, 0, ++t});  // exactly the band end
  EXPECT_EQ(last_reject(), RejectReason::PriceOutOfBand);
  e.submit({8, -5, 1, Side::Bid, OrderType::Limit, 0, ++t});
  EXPECT_EQ(last_reject(), RejectReason::PriceOutOfBand);
  e.submit({9, INT64_MAX, 1, Side::Bid, OrderType::Limit, 0, ++t});
  EXPECT_EQ(last_reject(), RejectReason::PriceOutOfBand);
  EXPECT_EQ(book().stats().reject_count(RejectReason::PriceOutOfBand), 5u);
  EXPECT_EQ(book().stats().reject_count(RejectReason::PriceOffTick), 2u);
  EXPECT_EQ(book().live_orders(), 2u);
  expect_valid();
}

TEST_F(OrdersTest, EventsCarryNativePrices) {
  limit(1, Side::Ask, 700, 5);
  limit(2, Side::Bid, 700, 5);
  const auto trades = of_type(EventType::Trade);
  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].price, 100000 + 100 * 700);
}

// ---- Market ------------------------------------------------------------

TEST_F(OrdersTest, MarketSweepsAndCancelsRemainder) {
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Ask, 700, 5);
  clear();
  market(9, Side::Bid, 20);
  EXPECT_EQ(of_type(EventType::Trade).size(), 2u);
  const auto cancels = of_type(EventType::Cancel);
  ASSERT_EQ(cancels.size(), 1u);
  EXPECT_EQ(cancels[0].qty, 10u);
  EXPECT_EQ(cancels[0].reason, static_cast<std::uint8_t>(CancelReason::MarketRemainder));
  EXPECT_EQ(book().live_orders(), 0u);
  expect_valid();
}

TEST_F(OrdersTest, MarketOnEmptyBookIsAckedThenCancelled) {
  market(1, Side::Ask, 7);
  ASSERT_EQ(ev().size(), 2u);
  EXPECT_EQ(ev()[0].type, EventType::Ack);
  EXPECT_EQ(ev()[1].type, EventType::Cancel);
  EXPECT_EQ(ev()[1].qty, 7u);
  expect_valid();
}

// ---- IOC ---------------------------------------------------------------

TEST_F(OrdersTest, IocFillsWhatItCanAndNeverRests) {
  limit(1, Side::Ask, 510, 4);
  clear();
  limit(2, Side::Bid, 510, 10, 0, OrderType::Ioc);
  EXPECT_EQ(of_type(EventType::Trade).size(), 1u);
  const auto c = of_type(EventType::Cancel);
  ASSERT_EQ(c.size(), 1u);
  EXPECT_EQ(c[0].qty, 6u);
  EXPECT_EQ(c[0].reason, static_cast<std::uint8_t>(CancelReason::IocRemainder));
  EXPECT_FALSE(book().has_bid());
  expect_valid();
}

TEST_F(OrdersTest, IocRespectsLimitPrice) {
  limit(1, Side::Ask, 512, 4);
  clear();
  limit(2, Side::Bid, 511, 4, 0, OrderType::Ioc);
  EXPECT_TRUE(of_type(EventType::Trade).empty());
  EXPECT_EQ(of_type(EventType::Cancel).size(), 1u);
}

// ---- FOK ---------------------------------------------------------------

TEST_F(OrdersTest, FokFillsAcrossLevelsWhenFullyAvailable) {
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Ask, 511, 5);
  clear();
  limit(9, Side::Bid, 511, 10, 0, OrderType::Fok);
  EXPECT_EQ(of_type(EventType::Trade).size(), 2u);
  EXPECT_TRUE(of_type(EventType::Cancel).empty());
  EXPECT_EQ(book().live_orders(), 0u);
  expect_valid();
}

TEST_F(OrdersTest, FokRejectsWithoutAnyStateChange) {
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Ask, 511, 5);
  clear();
  limit(9, Side::Bid, 511, 11, 0, OrderType::Fok);  // one lot short
  ASSERT_EQ(ev().size(), 1u);
  EXPECT_EQ(last_reject(), RejectReason::FokNotFillable);
  EXPECT_EQ(book().depth_at(px(510)), 5u);
  EXPECT_EQ(book().depth_at(px(511)), 5u);
  EXPECT_EQ(book().live_orders(), 2u);
  expect_valid();
}

TEST_F(OrdersTest, FokDoesNotCountLiquidityBeyondItsLimit) {
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Ask, 520, 100);
  clear();
  limit(9, Side::Bid, 515, 10, 0, OrderType::Fok);
  EXPECT_EQ(last_reject(), RejectReason::FokNotFillable);
}

TEST_F(OrdersTest, FokSellSideMirror) {
  limit(1, Side::Bid, 500, 5);
  limit(2, Side::Bid, 499, 5);
  clear();
  limit(9, Side::Ask, 499, 10, 0, OrderType::Fok);
  EXPECT_EQ(of_type(EventType::Trade).size(), 2u);
  expect_valid();
}

// ---- Post only ---------------------------------------------------------

TEST_F(OrdersTest, PostOnlyRestsWhenItDoesNotCross) {
  limit(1, Side::Ask, 510, 5);
  clear();
  limit(2, Side::Bid, 509, 5, 0, OrderType::PostOnly);
  ASSERT_EQ(ev().size(), 1u);
  EXPECT_EQ(ev()[0].type, EventType::Ack);
  EXPECT_EQ(book().best_bid(), px(509));
}

TEST_F(OrdersTest, PostOnlyRejectedWhenItWouldTradeIncludingEqualPrice) {
  limit(1, Side::Ask, 510, 5);
  clear();
  limit(2, Side::Bid, 510, 5, 0, OrderType::PostOnly);  // equal price crosses
  EXPECT_EQ(last_reject(), RejectReason::PostOnlyWouldCross);
  limit(3, Side::Bid, 515, 5, 0, OrderType::PostOnly);
  EXPECT_EQ(last_reject(), RejectReason::PostOnlyWouldCross);
  EXPECT_EQ(book().depth_at(px(510)), 5u);  // not slid, not traded
  EXPECT_FALSE(book().has_bid());
  expect_valid();
}

// ---- Cancel ------------------------------------------------------------

TEST_F(OrdersTest, CancelRemovesFromAnyQueuePositionKeepingFifo) {
  for (OrderId i = 1; i <= 5; ++i) limit(i, Side::Bid, 500, 10);
  cancel(3);  // middle
  cancel(1);  // head
  cancel(5);  // tail
  EXPECT_EQ(queue(500), (std::vector<OrderId>{2, 4}));
  EXPECT_EQ(book().depth_at(px(500)), 20u);
  const auto c = of_type(EventType::Cancel);
  ASSERT_EQ(c.size(), 3u);
  EXPECT_EQ(c[0].qty, 10u);
  EXPECT_EQ(c[0].reason, static_cast<std::uint8_t>(CancelReason::UserRequest));
  expect_valid();
}

TEST_F(OrdersTest, CancelReportsRemainingQuantityAfterPartialFill) {
  limit(1, Side::Ask, 510, 10);
  limit(2, Side::Bid, 510, 4);
  clear();
  cancel(1);
  ASSERT_EQ(ev().size(), 1u);
  EXPECT_EQ(ev()[0].qty, 6u);
}

TEST_F(OrdersTest, CancelOfUnknownFilledOrAlreadyCancelledIdRejects) {
  cancel(42);
  EXPECT_EQ(last_reject(), RejectReason::UnknownOrderId);
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Bid, 510, 5);  // fills order 1 completely
  clear();
  cancel(1);
  EXPECT_EQ(last_reject(), RejectReason::UnknownOrderId);
  limit(3, Side::Bid, 400, 5);
  cancel(3);
  clear();
  cancel(3);
  EXPECT_EQ(last_reject(), RejectReason::UnknownOrderId);
  expect_valid();
}

TEST_F(OrdersTest, CancellingTheLastOrderAtTheTouchMovesTheTouchAcrossGaps) {
  limit(1, Side::Bid, 900, 1);
  limit(2, Side::Bid, 3, 1);  // far away, different bitmap word and summary word
  limit(3, Side::Bid, 450, 1);
  EXPECT_EQ(book().best_bid(), px(900));
  cancel(1);
  EXPECT_EQ(book().best_bid(), px(450));
  cancel(3);
  EXPECT_EQ(book().best_bid(), px(3));
  cancel(2);
  EXPECT_FALSE(book().has_bid());
  limit(4, Side::Ask, 1023, 1);
  limit(5, Side::Ask, 64, 1);
  cancel(5);
  EXPECT_EQ(book().best_ask(), px(1023));
  expect_valid();
}

// ---- Capacity and self trade ---------------------------------------------

class TinyPoolTest : public BookTest {
 protected:
  TinyPoolTest() : BookTest(small_cfg(4)) {}
};

TEST_F(TinyPoolTest, ExhaustionRejectsRestingTypesOnlyAndRecovers) {
  for (OrderId i = 1; i <= 4; ++i) limit(i, Side::Bid, 100 + static_cast<int>(i), 1);
  clear();
  limit(5, Side::Bid, 200, 1);
  EXPECT_EQ(last_reject(), RejectReason::PoolExhausted);
  limit(6, Side::Bid, 200, 1, 0, OrderType::PostOnly);
  EXPECT_EQ(last_reject(), RejectReason::PoolExhausted);
  clear();
  market(7, Side::Ask, 1);  // does not need a slot
  EXPECT_EQ(of_type(EventType::Trade).size(), 1u);
  limit(8, Side::Bid, 200, 1);  // a slot was freed by the fill
  EXPECT_EQ(ev().back().type, EventType::Ack);
  expect_valid();
}

TEST_F(OrdersTest, SelfTradeIsAllowedAndFlaggedForNonFeedOwners) {
  limit(1, Side::Ask, 510, 5, /*owner=*/3);
  limit(2, Side::Bid, 510, 5, /*owner=*/3);
  auto trades = of_type(EventType::Trade);
  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].flags & kTradeSelf, kTradeSelf);

  clear();
  limit(3, Side::Ask, 510, 5, /*owner=*/0);
  limit(4, Side::Bid, 510, 5, /*owner=*/0);  // feed owner never counts as self trade
  trades = of_type(EventType::Trade);
  ASSERT_EQ(trades.size(), 1u);
  EXPECT_EQ(trades[0].flags & kTradeSelf, 0);

  clear();
  limit(5, Side::Ask, 510, 5, /*owner=*/1);
  limit(6, Side::Bid, 510, 5, /*owner=*/2);
  EXPECT_EQ(of_type(EventType::Trade)[0].flags & kTradeSelf, 0);
}

TEST_F(OrdersTest, DescribeIsStable) {
  limit(1, Side::Bid, 500, 10);
  EXPECT_NE(describe(ev()[0]).find("Ack"), std::string::npos);
  EXPECT_STREQ(to_string(RejectReason::WrongOwner), "WrongOwner");
}
