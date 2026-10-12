#include <chrono>

#include "test_support.hpp"

using namespace lobkit;
using namespace lobkit::testing;

class ReplayTest : public BookTest {
 protected:
  ReplayAdapter<VectorSink> feed() { return e.replay_adapter(); }
};

TEST_F(ReplayTest, AddRestsAtBackAndEmitsAck) {
  feed().apply_add(1, Side::Bid, px(500), 10, ++t);
  feed().apply_add(2, Side::Bid, px(500), 5, ++t);
  EXPECT_EQ(queue(500), (std::vector<OrderId>{1, 2}));
  EXPECT_EQ(ev()[0].type, EventType::Ack);
  EXPECT_EQ(book().find_order(1)->owner, kFeedOwner);
  expect_valid();
}

TEST_F(ReplayTest, ReducePreservesPriorityAndDeletesWhenFullyReduced) {
  feed().apply_add(1, Side::Bid, px(500), 10, ++t);
  feed().apply_add(2, Side::Bid, px(500), 10, ++t);
  clear();
  feed().apply_reduce(1, 4, ++t);
  EXPECT_EQ(book().find_order(1)->qty, 6u);
  EXPECT_EQ(queue(500), (std::vector<OrderId>{1, 2}));
  EXPECT_EQ(ev()[0].type, EventType::Cancel);
  EXPECT_EQ(ev()[0].qty, 4u);
  EXPECT_EQ(ev()[0].reason, static_cast<std::uint8_t>(CancelReason::FeedReduce));
  feed().apply_reduce(1, 100, ++t);  // at least the remainder: removed
  EXPECT_EQ(ev().back().qty, 6u);
  EXPECT_EQ(queue(500), (std::vector<OrderId>{2}));
  expect_valid();
}

TEST_F(ReplayTest, DeleteRemovesOrder) {
  feed().apply_add(1, Side::Ask, px(510), 10, ++t);
  clear();
  feed().apply_delete(1, ++t);
  EXPECT_EQ(ev()[0].reason, static_cast<std::uint8_t>(CancelReason::FeedDelete));
  EXPECT_FALSE(book().has_ask());
  expect_valid();
}

TEST_F(ReplayTest, ExecuteReducesFeedOrderWithExternalTaker) {
  feed().apply_add(1, Side::Ask, px(510), 10, ++t);
  clear();
  feed().apply_execute(1, 4, ++t);
  ASSERT_EQ(ev().size(), 1u);
  EXPECT_EQ(ev()[0].type, EventType::Trade);
  EXPECT_EQ(ev()[0].id, 0u);
  EXPECT_EQ(ev()[0].other, 1u);
  EXPECT_EQ(ev()[0].qty, 4u);
  EXPECT_EQ(ev()[0].price, px(510));
  EXPECT_EQ(ev()[0].side, Side::Bid);  // aggressor bought from a resting ask
  EXPECT_EQ(ev()[0].flags & kTradeExternalTaker, kTradeExternalTaker);
  EXPECT_EQ(book().find_order(1)->qty, 6u);
  feed().apply_execute(1, 6, ++t);
  EXPECT_EQ(book().find_order(1), nullptr);
  expect_valid();
}

TEST_F(ReplayTest, ExecuteLargerThanRemainingIsClampedAndCounted) {
  feed().apply_add(1, Side::Ask, px(510), 5, ++t);
  clear();
  feed().apply_execute(1, 9, ++t);
  EXPECT_EQ(ev()[0].qty, 5u);
  EXPECT_EQ(book().stats().feed_exec_overflow, 1u);
  EXPECT_EQ(book().find_order(1), nullptr);
}

TEST_F(ReplayTest, RejectsUnknownZeroAndOwnOrderIds) {
  feed().apply_reduce(9, 1, ++t);
  EXPECT_EQ(last_reject(), RejectReason::UnknownOrderId);
  feed().apply_delete(9, ++t);
  EXPECT_EQ(last_reject(), RejectReason::UnknownOrderId);
  feed().apply_execute(9, 1, ++t);
  EXPECT_EQ(last_reject(), RejectReason::UnknownOrderId);
  feed().apply_add(1, Side::Bid, px(500), 0, ++t);
  EXPECT_EQ(last_reject(), RejectReason::InvalidQty);
  feed().apply_add(1, Side::Bid, px(500), 5, ++t);
  feed().apply_execute(1, 0, ++t);
  EXPECT_EQ(last_reject(), RejectReason::InvalidQty);

  limit(100, Side::Bid, 400, 5, /*owner=*/1);  // own order
  feed().apply_reduce(100, 1, ++t);
  EXPECT_EQ(last_reject(), RejectReason::WrongOwner);
  feed().apply_delete(100, ++t);
  EXPECT_EQ(last_reject(), RejectReason::WrongOwner);
  feed().apply_execute(100, 1, ++t);
  EXPECT_EQ(last_reject(), RejectReason::WrongOwner);
  EXPECT_EQ(book().find_order(100)->qty, 5u);
  EXPECT_EQ(book().stats().reject_count(RejectReason::WrongOwner), 3u);
  expect_valid();
}

TEST_F(ReplayTest, OutOfBandAndOffTickAreCountedForTheReplayReport) {
  feed().apply_add(1, Side::Bid, px(5000), 5, ++t);
  feed().apply_add(2, Side::Bid, px(5) + 7, 5, ++t);
  EXPECT_EQ(book().stats().reject_count(RejectReason::PriceOutOfBand), 1u);
  EXPECT_EQ(book().stats().reject_count(RejectReason::PriceOffTick), 1u);
  EXPECT_EQ(book().live_orders(), 0u);
}

TEST_F(ReplayTest, FeedAddCrossingFeedOrdersIsCountedAsAnomaly) {
  feed().apply_add(1, Side::Ask, px(510), 5, ++t);
  feed().apply_add(2, Side::Bid, px(510), 5, ++t);  // a faithful feed never does this
  EXPECT_EQ(book().stats().feed_crossed_fills, 1u);
  expect_valid();  // the book stays uncrossed regardless
}

// A feed execute used to walk the whole queue ahead of the target to look for own
// orders, so executing a deep feed order cost O(position). With no own orders
// that is wasted work; the walk now stops at the first feed order ahead. Without
// the early exit this test does about 3.75e9 link hops (tens of seconds in
// release, minutes under sanitizers); with it, it is a few milliseconds.
TEST(ReplayPerformanceGuard, ExecutingDeepFeedOrdersDoesNotWalkTheQueue) {
  BookConfig cfg = small_cfg(120000);
  Engine<NullSink> eng(cfg);
  auto fa = eng.replay_adapter();
  constexpr OrderId kN = 100000;
  for (OrderId id = 1; id <= kN; ++id) fa.apply_add(id, Side::Ask, px(500), 1, id);
  const auto t0 = std::chrono::steady_clock::now();
  for (OrderId id = kN; id > kN / 2; --id) fa.apply_execute(id, 1, kN + id);
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(secs, 5.0);
  EXPECT_EQ(eng.book().stats().queue_order_anomalies, kN / 2);  // each had feed orders ahead
  EXPECT_EQ(eng.book().live_orders(), kN / 2);
}
