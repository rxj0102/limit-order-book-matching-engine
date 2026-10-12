#include "test_support.hpp"
#include "workload.hpp"

using namespace lobkit;
using namespace lobkit::testing;

TEST(Invariants, HoldThroughRandomMixedWorkloadWithFeedOps) {
  for (std::uint64_t seed : {1ull, 2ull, 3ull}) {
    Engine<CountingSink> e(small_cfg(512));  // small pool so exhaustion is exercised too
    WorkloadParams p;
    p.seed = seed;
    p.n = 60000;
    p.feed_ops = true;
    const auto ops = make_workload(p);
    for (std::size_t i = 0; i < ops.size(); ++i) {
      apply(e, ops[i]);
      if (i % 61 == 0) {
        const auto err = check_invariants(e.book());
        ASSERT_FALSE(err.has_value()) << "seed " << seed << " op " << i << ": " << *err;
      }
    }
    ASSERT_FALSE(check_invariants(e.book()).has_value());
    // The stream must actually have exercised the interesting paths.
    EXPECT_GT(e.sink().by_type[static_cast<int>(EventType::Trade)], 1000u);
    EXPECT_GT(e.sink().by_type[static_cast<int>(EventType::Cancel)], 1000u);
    EXPECT_GT(e.book().stats().reject_count(RejectReason::PostOnlyWouldCross), 0u);
    EXPECT_GT(e.book().stats().reject_count(RejectReason::FokNotFillable), 0u);
    EXPECT_GT(e.book().stats().reject_count(RejectReason::PoolExhausted), 0u);
  }
}

TEST(Invariants, EmptyBookIsValid) {
  Engine<NullSink> e(small_cfg());
  EXPECT_FALSE(check_invariants(e.book()).has_value());
}

// The checker is only worth something if it fails on damage. Each case breaks
// one structure behind the engine's back and expects a specific complaint.
class CorruptionTest : public BookTest {
 protected:
  void SetUp() override {
    limit(1, Side::Bid, 500, 10);
    limit(2, Side::Bid, 500, 10);
    limit(3, Side::Ask, 600, 10);
    ASSERT_FALSE(check_invariants(book()).has_value());
  }
  Book<VectorSink>& mut() { return e.book(); }
  void expect_complaint(const char* needle) {
    const auto err = check_invariants(book());
    ASSERT_TRUE(err.has_value());
    EXPECT_NE(err->find(needle), std::string::npos) << *err;
  }
};
using detail::BookInspector;

TEST_F(CorruptionTest, DetectsAggregateMismatch) {
  BookInspector::levels(mut())[500].total_qty += 1;
  expect_complaint("aggregate");
}
TEST_F(CorruptionTest, DetectsBrokenBackLink) {
  auto& pool = BookInspector::pool(mut());
  const Index head = BookInspector::levels(mut())[500].head;
  pool[pool[head].next].prev = kNil;
  expect_complaint("prev");
}
TEST_F(CorruptionTest, DetectsStaleBestPrice) {
  BookInspector::best_bid(mut()) = 499;
  expect_complaint("best bid");
}
TEST_F(CorruptionTest, DetectsBitmapOccupancyMismatch) {
  BookInspector::asks(mut()).set(700);
  expect_complaint("bitmap");
}
TEST_F(CorruptionTest, DetectsCrossedBook) {
  // Move the ask level down onto the bid, as a bug in matching would.
  auto& lv = BookInspector::levels(mut());
  lv[400] = lv[600];
  lv[600] = Level{};
  BookInspector::asks(mut()).clear(600);
  BookInspector::asks(mut()).set(400);
  BookInspector::best_ask(mut()) = 400;
  auto& pool = BookInspector::pool(mut());
  pool[lv[400].head].level = 400;
  expect_complaint("crossed");
}
TEST_F(CorruptionTest, DetectsPoolLeak) {
  (void)BookInspector::pool(mut()).allocate();
  expect_complaint("pool");
}
TEST_F(CorruptionTest, DetectsIdMapDamage) {
  BookInspector::map(mut()).erase(2);
  expect_complaint("id map");
}
