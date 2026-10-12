#include "test_support.hpp"

using namespace lobkit;
using namespace lobkit::testing;

class ModifyTest : public BookTest {};

TEST_F(ModifyTest, SizeDecreaseKeepsQueuePosition) {
  limit(1, Side::Bid, 500, 10);
  limit(2, Side::Bid, 500, 10);
  limit(3, Side::Bid, 500, 10);
  clear();
  modify(1, 500, 4);
  EXPECT_EQ(queue(500), (std::vector<OrderId>{1, 2, 3}));
  EXPECT_EQ(book().find_order(1)->qty, 4u);
  EXPECT_EQ(book().depth_at(px(500)), 24u);
  ASSERT_EQ(ev().size(), 1u);
  EXPECT_EQ(ev()[0].type, EventType::Modify);
  EXPECT_EQ(ev()[0].qty, 4u);
  // Priority is real: a seller hits order 1 first.
  clear();
  limit(9, Side::Ask, 500, 4);
  EXPECT_EQ(of_type(EventType::Trade)[0].other, 1u);
  expect_valid();
}

TEST_F(ModifyTest, SameSizeSamePriceIsAcceptedAndKeepsPriority) {
  limit(1, Side::Bid, 500, 10);
  limit(2, Side::Bid, 500, 10);
  modify(1, 500, 10);
  EXPECT_EQ(queue(500), (std::vector<OrderId>{1, 2}));
  EXPECT_EQ(ev().back().type, EventType::Modify);
}

TEST_F(ModifyTest, SizeIncreaseLosesPriority) {
  limit(1, Side::Bid, 500, 10);
  limit(2, Side::Bid, 500, 10);
  modify(1, 500, 11);
  EXPECT_EQ(queue(500), (std::vector<OrderId>{2, 1}));
  EXPECT_EQ(book().find_order(1)->qty, 11u);
  EXPECT_EQ(book().depth_at(px(500)), 21u);
  expect_valid();
}

TEST_F(ModifyTest, PriceChangeLosesPriorityAndMovesLevel) {
  limit(1, Side::Bid, 500, 10);
  limit(2, Side::Bid, 501, 10);
  modify(1, 502, 10);
  EXPECT_EQ(book().best_bid(), px(502));
  EXPECT_EQ(book().depth_at(px(500)), 0u);
  EXPECT_EQ(queue(502), (std::vector<OrderId>{1}));
  modify(1, 501, 10);
  EXPECT_EQ(queue(501), (std::vector<OrderId>{2, 1}));
  EXPECT_EQ(book().best_bid(), px(501));
  expect_valid();
}

TEST_F(ModifyTest, ModifyToCrossingPriceTradesImmediately) {
  limit(1, Side::Ask, 510, 6);
  limit(2, Side::Bid, 500, 10);
  clear();
  modify(2, 510, 10);
  ASSERT_GE(ev().size(), 2u);
  EXPECT_EQ(ev()[0].type, EventType::Modify);
  EXPECT_EQ(ev()[1].type, EventType::Trade);
  EXPECT_EQ(ev()[1].qty, 6u);
  EXPECT_EQ(book().find_order(2)->qty, 4u);  // remainder rests under the same id
  EXPECT_EQ(book().best_bid(), px(510));
  EXPECT_FALSE(book().has_ask());
  expect_valid();
}

TEST_F(ModifyTest, ModifyThatFullyMatchesLeavesNoOrderBehind) {
  limit(1, Side::Ask, 510, 20);
  limit(2, Side::Bid, 500, 10);
  modify(2, 510, 10);
  EXPECT_EQ(book().find_order(2), nullptr);
  EXPECT_EQ(book().depth_at(px(510)), 10u);
  expect_valid();
}

TEST_F(ModifyTest, ModifyAfterFullFillIsRejectedAsUnknown) {
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Bid, 510, 5);
  clear();
  modify(1, 510, 3);
  EXPECT_EQ(last_reject(), RejectReason::UnknownOrderId);
}

TEST_F(ModifyTest, ModifyAfterPartialFillSetsRemainingQuantity) {
  limit(1, Side::Ask, 510, 10);
  limit(2, Side::Bid, 510, 4);  // 6 left on order 1
  modify(1, 510, 8);            // increase relative to the remaining 6: loses priority
  EXPECT_EQ(book().find_order(1)->qty, 8u);
  modify(1, 510, 2);  // decrease: priority kept
  EXPECT_EQ(book().find_order(1)->qty, 2u);
  expect_valid();
}

TEST_F(ModifyTest, RejectsAreSideEffectFree) {
  limit(1, Side::Bid, 500, 10);
  limit(2, Side::Bid, 500, 10);
  clear();
  modify(1, 500, 0);
  EXPECT_EQ(last_reject(), RejectReason::InvalidQty);
  modify(1, 5000, 10);
  EXPECT_EQ(last_reject(), RejectReason::PriceOutOfBand);
  e.modify({1, px(500) + 1, 10, ++t});
  EXPECT_EQ(last_reject(), RejectReason::PriceOffTick);
  modify(77, 500, 10);
  EXPECT_EQ(last_reject(), RejectReason::UnknownOrderId);
  EXPECT_EQ(queue(500), (std::vector<OrderId>{1, 2}));
  EXPECT_EQ(book().find_order(1)->qty, 10u);
  expect_valid();
}

TEST_F(ModifyTest, ModifyOnSellSideMirrors) {
  limit(1, Side::Ask, 510, 10);
  limit(2, Side::Ask, 510, 10);
  modify(1, 509, 10);
  EXPECT_EQ(book().best_ask(), px(509));
  modify(1, 510, 10);
  EXPECT_EQ(queue(510), (std::vector<OrderId>{2, 1}));
  expect_valid();
}
