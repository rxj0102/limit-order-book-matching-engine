#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <vector>

#include "lobkit/infra/spsc_ring.hpp"

using lobkit::SpscRing;

TEST(SpscRingLayout, CountersAndPrivateCopiesLiveOnSeparateLines) {
  // The false-sharing defense is only real if the members are where we think.
  static_assert(alignof(SpscRing<int>) == lobkit::kCacheLine);
  static_assert(sizeof(SpscRing<int>) % lobkit::kCacheLine == 0);
  SUCCEED();
}

TEST(SpscRing, RejectsBadCapacity) {
  EXPECT_THROW(SpscRing<int>(0), std::invalid_argument);
  EXPECT_THROW(SpscRing<int>(1), std::invalid_argument);
  EXPECT_THROW(SpscRing<int>(3), std::invalid_argument);
  EXPECT_THROW(SpscRing<int>(100), std::invalid_argument);
  EXPECT_NO_THROW(SpscRing<int>(2));
  EXPECT_EQ(SpscRing<int>(64).capacity(), 64u);
}

TEST(SpscRing, EmptyPopFailsAndLeavesOutputAlone) {
  SpscRing<int> r(4);
  int v = 123;
  EXPECT_FALSE(r.try_pop(v));
  EXPECT_EQ(v, 123);
  int buf[3] = {7, 7, 7};
  EXPECT_EQ(r.try_pop_n(buf, 3), 0u);
  EXPECT_EQ(buf[0], 7);
  EXPECT_EQ(r.size_approx(), 0u);
}

TEST(SpscRing, FillsToExactlyCapacityThenRejects) {
  SpscRing<int> r(8);
  for (int i = 0; i < 8; ++i) ASSERT_TRUE(r.try_push(i)) << i;  // no slot is wasted
  EXPECT_FALSE(r.try_push(99));
  EXPECT_EQ(r.size_approx(), 8u);
  int v;
  ASSERT_TRUE(r.try_pop(v));
  EXPECT_EQ(v, 0);
  EXPECT_TRUE(r.try_push(8));  // one slot freed, exactly one more fits
  EXPECT_FALSE(r.try_push(9));
  for (int i = 1; i <= 8; ++i) {
    ASSERT_TRUE(r.try_pop(v));
    EXPECT_EQ(v, i);
  }
  EXPECT_FALSE(r.try_pop(v));
}

TEST(SpscRing, FifoAcrossManyWraparounds) {
  SpscRing<std::uint32_t> r(4);
  std::uint32_t next_in = 0, next_out = 0;
  // Interleave irregularly so head and tail pass the physical end at every offset.
  for (int round = 0; round < 10000; ++round) {
    const int pushes = 1 + round % 4;
    for (int i = 0; i < pushes; ++i)
      if (r.try_push(next_in)) ++next_in;
    const int pops = 1 + (round * 7) % 4;
    for (int i = 0; i < pops; ++i) {
      std::uint32_t v;
      if (r.try_pop(v)) {
        ASSERT_EQ(v, next_out);
        ++next_out;
      }
    }
  }
  std::uint32_t v;
  while (r.try_pop(v)) {
    ASSERT_EQ(v, next_out);
    ++next_out;
  }
  EXPECT_EQ(next_in, next_out);
  EXPECT_GT(next_in, 10000u);
}

TEST(SpscRing, CountersMayOverflowTheirType) {
  // Start 5 below 2^64 so tail passes zero in the middle of the test. Full and
  // empty detection must keep working through the overflow.
  const std::size_t start = std::numeric_limits<std::size_t>::max() - 5;
  SpscRing<int> r(4, start);
  int next_in = 0, next_out = 0;
  for (int round = 0; round < 200; ++round) {
    while (r.try_push(next_in)) ++next_in;
    EXPECT_EQ(r.size_approx(), 4u);
    int v;
    while (r.try_pop(v)) {
      ASSERT_EQ(v, next_out);
      ++next_out;
    }
    EXPECT_EQ(r.size_approx(), 0u);
  }
  EXPECT_EQ(next_in, next_out);
  EXPECT_EQ(next_in, 800);
}

TEST(SpscRingBatch, PartialPushAndPopReportTheActualCount) {
  SpscRing<int> r(8);
  std::vector<int> in(20);
  for (int i = 0; i < 20; ++i) in[static_cast<std::size_t>(i)] = i;
  EXPECT_EQ(r.try_push_n(in.data(), 20), 8u);  // only 8 fit
  EXPECT_EQ(r.try_push_n(in.data(), 1), 0u);   // full
  int out[20] = {};
  EXPECT_EQ(r.try_pop_n(out, 5), 5u);
  for (int i = 0; i < 5; ++i) EXPECT_EQ(out[i], i);
  EXPECT_EQ(r.try_pop_n(out, 20), 3u);  // only 3 left
  for (int i = 0; i < 3; ++i) EXPECT_EQ(out[i], 5 + i);
  EXPECT_EQ(r.try_pop_n(out, 1), 0u);
}

TEST(SpscRingBatch, BatchesThatStraddleThePhysicalEndKeepOrder) {
  SpscRing<int> r(8);
  int next_in = 0, next_out = 0;
  // Batch sizes 3 and 5 against capacity 8 put the split point everywhere.
  for (int round = 0; round < 5000; ++round) {
    int src[5];
    const std::size_t want = (round % 2) ? 3 : 5;
    for (std::size_t i = 0; i < want; ++i) src[i] = next_in + static_cast<int>(i);
    next_in += static_cast<int>(r.try_push_n(src, want));
    int dst[4];
    const std::size_t got = r.try_pop_n(dst, static_cast<std::size_t>(round % 3) + 2);
    for (std::size_t i = 0; i < got; ++i) ASSERT_EQ(dst[i], next_out++);
  }
  int dst[8];
  for (std::size_t got; (got = r.try_pop_n(dst, 8)) > 0;)
    for (std::size_t i = 0; i < got; ++i) ASSERT_EQ(dst[i], next_out++);
  EXPECT_EQ(next_in, next_out);
}

TEST(SpscRingBatch, SingleAndBatchOperationsMix) {
  SpscRing<int> r(4);
  const int a[3] = {1, 2, 3};
  EXPECT_EQ(r.try_push_n(a, 3), 3u);
  int v;
  ASSERT_TRUE(r.try_pop(v));
  EXPECT_EQ(v, 1);
  EXPECT_TRUE(r.try_push(4));
  EXPECT_TRUE(r.try_push(5));  // wraps
  EXPECT_FALSE(r.try_push(6));
  int out[4];
  EXPECT_EQ(r.try_pop_n(out, 4), 4u);
  EXPECT_EQ(out[0], 2);
  EXPECT_EQ(out[3], 5);
}

TEST(SpscRingBatch, ZeroLengthBatchesAreNoOps) {
  SpscRing<int> r(4);
  int x = 0;
  EXPECT_EQ(r.try_push_n(&x, 0), 0u);
  EXPECT_EQ(r.try_pop_n(&x, 0), 0u);
  EXPECT_EQ(r.size_approx(), 0u);
}

TEST(SpscRing, WorksWithStructPayloads) {
  struct Msg {
    std::uint64_t a, b, c;
  };
  SpscRing<Msg> r(2);
  EXPECT_TRUE(r.try_push({1, 2, 3}));
  Msg m{};
  ASSERT_TRUE(r.try_pop(m));
  EXPECT_EQ(m.c, 3u);
}
