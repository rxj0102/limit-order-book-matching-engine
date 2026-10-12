#include <string>

#include "test_support.hpp"

using namespace lobkit;
using namespace lobkit::testing;

TEST(Names, EveryEnumeratorHasAName) {
  for (auto t :
       {EventType::Ack, EventType::Reject, EventType::Trade, EventType::Cancel, EventType::Modify})
    EXPECT_STRNE(to_string(t), "?");
  for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(RejectReason::kCount); ++i)
    EXPECT_STRNE(to_string(static_cast<RejectReason>(i)), "?") << int{i};
  for (auto r : {CancelReason::UserRequest, CancelReason::IocRemainder,
                 CancelReason::MarketRemainder, CancelReason::FeedReduce, CancelReason::FeedDelete})
    EXPECT_STRNE(to_string(r), "?");
  for (auto t :
       {OrderType::Limit, OrderType::Market, OrderType::Ioc, OrderType::Fok, OrderType::PostOnly})
    EXPECT_STRNE(to_string(t), "?");
  EXPECT_STREQ(to_string(Side::Bid), "Bid");
  EXPECT_STREQ(to_string(Side::Ask), "Ask");
  // Out-of-range values must not crash the formatter.
  EXPECT_STREQ(to_string(RejectReason::kCount), "?");
  EXPECT_STREQ(to_string(static_cast<EventType>(99)), "?");
  EXPECT_STREQ(to_string(static_cast<CancelReason>(99)), "?");
  EXPECT_STREQ(to_string(static_cast<OrderType>(99)), "?");
}

class NamesBookTest : public BookTest {};

TEST_F(NamesBookTest, DescribeShowsTypeAndReason) {
  limit(1, Side::Ask, 510, 5);
  limit(2, Side::Bid, 510, 5);
  cancel(77);
  market(3, Side::Bid, 1);
  e.replay_adapter().apply_delete(99, ++t);
  std::string all;
  for (const Event& x : ev()) all += describe(x) + "\n";
  for (const char* want : {"Ack", "Trade", "Reject", "UnknownOrderId", "Cancel", "MarketRemainder"})
    EXPECT_NE(all.find(want), std::string::npos) << want;
}

TEST_F(NamesBookTest, QueueQueryOffGridIsEmptyAndReadersAreConsistent) {
  limit(1, Side::Bid, 500, 5);
  EXPECT_TRUE(book().queue_ids(px(5000)).empty());
  EXPECT_EQ(book().depth_at(px(5000)), 0u);
  EXPECT_EQ(book().depth_at(px(500) + 1), 0u);
  EXPECT_EQ(book().last_seq(), ev().size());
  EXPECT_EQ(book().config().tick_size, 100);
}
