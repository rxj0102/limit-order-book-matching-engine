#include <gtest/gtest.h>

#include <set>
#include <unordered_map>

#include "lobkit/book/id_map.hpp"
#include "lobkit/book/level_bitmap.hpp"
#include "lobkit/book/order_pool.hpp"
#include "workload.hpp"

using namespace lobkit;
using lobkit::testing::SplitMix64;

TEST(Layout, HotStructSizes) {
  EXPECT_EQ(sizeof(Order), 32u);
  EXPECT_EQ(alignof(Order), 32u);
  EXPECT_EQ(sizeof(Level), 32u);
  EXPECT_EQ(sizeof(Event), 48u);
}

TEST(LevelBitmap, MatchesStdSetUnderRandomOps) {
  constexpr std::uint32_t kBits = 65536;
  LevelBitmap bm(kBits);
  std::set<Index> ref;
  SplitMix64 rng(7);
  for (int i = 0; i < 200000; ++i) {
    // Cluster most operations so words empty and refill, but keep far-apart
    // entries so the summary level is exercised.
    const Index v = rng.below(4) == 0 ? static_cast<Index>(rng.below(kBits))
                                      : static_cast<Index>(30000 + rng.below(300));
    if (rng.below(2)) {
      bm.set(v);
      ref.insert(v);
    } else {
      bm.clear(v);
      ref.erase(v);
    }
    if (i % 7 == 0) {
      const Index q = static_cast<Index>(rng.below(kBits));
      auto ge = ref.lower_bound(q);
      EXPECT_EQ(bm.next_at_or_above(q), ge == ref.end() ? kNil : *ge);
      auto gt = ref.upper_bound(q);
      const Index expect_le = gt == ref.begin() ? kNil : *std::prev(gt);
      EXPECT_EQ(bm.prev_at_or_below(q), expect_le);
    }
    ASSERT_EQ(bm.lowest(), ref.empty() ? kNil : *ref.begin());
    ASSERT_EQ(bm.highest(), ref.empty() ? kNil : *ref.rbegin());
  }
}

TEST(LevelBitmap, EdgesAndEmpty) {
  LevelBitmap bm(64);
  EXPECT_EQ(bm.lowest(), kNil);
  EXPECT_EQ(bm.highest(), kNil);
  EXPECT_EQ(bm.next_at_or_above(64), kNil);
  bm.set(0);
  bm.set(63);
  EXPECT_EQ(bm.lowest(), 0u);
  EXPECT_EQ(bm.highest(), 63u);
  EXPECT_EQ(bm.next_at_or_above(1), 63u);
  EXPECT_EQ(bm.prev_at_or_below(62), 0u);
  bm.clear(0);
  EXPECT_EQ(bm.prev_at_or_below(62), kNil);
}

TEST(IdMap, MatchesUnorderedMapIncludingZeroKeyAndClusters) {
  IdMap m(1000);
  std::unordered_map<OrderId, Index> ref;
  SplitMix64 rng(11);
  for (int i = 0; i < 300000; ++i) {
    // Small key space forces long probe clusters and many backward shifts.
    const OrderId k = rng.below(3) == 0 ? rng.below(40) * 0x100000000ull : rng.below(900);
    if (rng.below(2)) {
      if (!ref.count(k) && ref.size() < 1000) {
        m.insert(k, static_cast<Index>(i));
        ref[k] = static_cast<Index>(i);
      }
    } else {
      m.erase(k);
      ref.erase(k);
    }
    const OrderId probe = rng.below(900);
    auto it = ref.find(probe);
    ASSERT_EQ(m.find(probe), it == ref.end() ? kNil : it->second);
    ASSERT_EQ(m.size(), ref.size());
  }
  for (auto& [k, v] : ref) ASSERT_EQ(m.find(k), v);
}

TEST(OrderPool, AccountingAndExhaustion) {
  OrderPool p(8);
  std::vector<Index> got;
  for (int i = 0; i < 8; ++i) got.push_back(p.allocate());
  EXPECT_TRUE(p.full());
  EXPECT_EQ(p.allocate(), kNil);
  EXPECT_EQ(p.in_use(), 8u);
  p.release(got[3]);
  EXPECT_FALSE(p.full());
  EXPECT_EQ(p.allocate(), got[3]);  // LIFO reuse keeps hot slots hot
  for (Index i : got) p.release(i);
  EXPECT_EQ(p.in_use(), 0u);
}

TEST(Config, ValidationRejectsBadGrids) {
  BookConfig c;
  c.num_levels = 1000;
  EXPECT_THROW(c.validate(), std::invalid_argument);
  c = BookConfig{};
  c.tick_size = 0;
  EXPECT_THROW(c.validate(), std::invalid_argument);
  c = BookConfig{};
  c.tick_size = INT64_MAX / 2;
  EXPECT_THROW(c.validate(), std::invalid_argument);
  c = BookConfig{};
  c.base_price = -1;
  EXPECT_THROW(c.validate(), std::invalid_argument);
  c = BookConfig{};
  c.max_orders = 0;
  EXPECT_THROW(c.validate(), std::invalid_argument);
  c = BookConfig{};
  c.num_levels = 1u << 25;
  EXPECT_THROW(c.validate(), std::invalid_argument);
  EXPECT_NO_THROW(BookConfig{}.validate());
}
