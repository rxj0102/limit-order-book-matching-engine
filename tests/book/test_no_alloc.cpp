// Proves the hot path never touches the heap. This binary replaces the global
// allocation functions; any call while the guard is armed is counted, and the
// test fails if the count is not zero. Kept in its own executable because the
// replacement is process wide.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>

#include "lobkit/book/engine.hpp"
#include "lobkit/md/itch.hpp"
#include "lobkit/md/lobster.hpp"
#include "lobkit/md/mapped_file.hpp"
#include "lobkit/md/replay.hpp"
#include "workload.hpp"

namespace {
std::atomic<bool> g_armed{false};
std::atomic<unsigned long> g_allocs{0};

void* do_alloc(std::size_t n, std::size_t align) {
  if (g_armed.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
  void* p = align > alignof(std::max_align_t)
                ? std::aligned_alloc(align, (n + align - 1) / align * align)
                : std::malloc(n ? n : 1);
  if (!p) throw std::bad_alloc();
  return p;
}
}  // namespace

void* operator new(std::size_t n) {
  return do_alloc(n, 0);
}
void* operator new[](std::size_t n) {
  return do_alloc(n, 0);
}
void* operator new(std::size_t n, std::align_val_t a) {
  return do_alloc(n, static_cast<std::size_t>(a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
  return do_alloc(n, static_cast<std::size_t>(a));
}
// The nothrow forms must be replaced too: the standard library uses them
// (for example in std::stable_sort) and the matching delete would otherwise
// free memory that another allocator handed out.
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return do_alloc(n, 0);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  try {
    return do_alloc(n, 0);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void* p) noexcept {
  std::free(p);
}
void operator delete[](void* p) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
  std::free(p);
}

using namespace lobkit;
using namespace lobkit::testing;

namespace {
struct Guard {
  Guard() {
    g_allocs = 0;
    g_armed = true;
  }
  ~Guard() { g_armed = false; }
  unsigned long count() const { return g_allocs.load(); }
};
}  // namespace

TEST(NoAlloc, GuardActuallyDetectsAllocations) {
  Guard g;
  void* p = ::operator new(16);  // a direct call cannot be elided by the optimizer
  ::operator delete(p);
  EXPECT_EQ(g.count(), 1u);
}

TEST(NoAlloc, MatchingCancelModifyAndReplayDoNotAllocate) {
  BookConfig cfg;
  cfg.tick_size = 100;
  cfg.base_price = 100000;
  cfg.num_levels = 1024;
  cfg.max_orders = 2048;  // small enough that exhaustion rejects occur inside the guard
  Engine<CountingSink> e(cfg);

  WorkloadParams p;
  p.seed = 5;
  p.n = 300000;
  p.feed_ops = true;
  const auto ops = make_workload(p);  // allocates, before the guard is armed

  unsigned long allocs = 0;
  {
    Guard g;
    for (const Op& op : ops) apply(e, op);
    allocs = g.count();
  }
  EXPECT_EQ(allocs, 0u);
  EXPECT_GT(e.sink().by_type[static_cast<int>(EventType::Trade)], 10000u);
  EXPECT_GT(e.book().stats().reject_count(RejectReason::PoolExhausted), 0u);
}

// ---- market data: parsers, replay, verification ------------------------------------
// Files are mapped (and any buffers built) before the guard is armed. Everything
// inside the guard is the replay hot path: parsing, pacing, applying, verifying.

TEST(NoAllocMarketData, ParseReplayAndVerifyDoNotAllocate) {
  using namespace lobkit::md;
  MappedFile msgs, rows, itch_file;
  const char* err = nullptr;
  ASSERT_TRUE(msgs.open(LOBKIT_DATA_DIR "/SYNTH_lobster_1000_message_10.csv", &err)) << err;
  ASSERT_TRUE(rows.open(LOBKIT_DATA_DIR "/SYNTH_lobster_1000_orderbook_10.csv", &err)) << err;
  ASSERT_TRUE(itch_file.open(LOBKIT_DATA_DIR "/itch_fixture.bin", &err)) << err;

  BookConfig cfg;
  cfg.tick_size = 100;
  cfg.base_price = 900000;
  cfg.num_levels = 4096;
  cfg.max_orders = 4096;
  Engine<NullSink> plain(cfg), verified(cfg), paced(cfg);
  BookConfig icfg = cfg;
  icfg.base_price = 1400000;
  Engine<NullSink> itch_eng(icfg);

  unsigned long allocs = 0;
  std::uint64_t n_parse = 0;
  ReplayCounters c1, c2, c3, c4;
  VerifyResult vr;
  RunResult r1, r3, r4, rp;
  {
    Guard g;
    {  // parse only, both formats
      lobster::MessageReader rd(msgs.text());
      rp = parse_only(rd, n_parse);
      itch::Decoder d(itch_file.bytes(), 1);
      std::uint64_t n2 = 0;
      parse_only(d, n2);
    }
    {  // as fast as possible
      lobster::MessageReader rd(msgs.text());
      r1 = replay(plain, rd, c1);
    }
    {  // snapshot verification (top_levels, row parsing, comparison)
      lobster::MessageReader rd(msgs.text());
      lobster::BookRowReader br(rows.text(), 10);
      vr = replay_verify(verified, rd, br, c2);
    }
    {  // paced, at a speed high enough that waits are negligible
      lobster::MessageReader rd(msgs.text());
      Pacer pacer(1e9);
      r3 = replay(paced, rd, c3, &pacer);
    }
    {  // ITCH
      itch::Decoder d(itch_file.bytes(), 1);
      r4 = replay(itch_eng, d, c4);
    }
    allocs = g.count();
  }
  EXPECT_EQ(allocs, 0u);
  // The work must actually have happened, or zero allocations would mean nothing.
  EXPECT_EQ(n_parse, 1000u);
  EXPECT_EQ(r1.status, ParseStatus::End);
  EXPECT_EQ(c1.messages, 1000u);
  EXPECT_TRUE(vr.ok);
  EXPECT_EQ(vr.verified, 1000u);
  EXPECT_EQ(r3.status, ParseStatus::End);
  EXPECT_EQ(c3.messages, 1000u);
  EXPECT_EQ(r4.status, ParseStatus::End);
  EXPECT_EQ(c4.messages, 9u);
}

TEST(NoAllocMarketData, ErrorPathsDoNotAllocateEither) {
  using namespace lobkit::md;
  // Malformed input must produce a code, not an exception or a heap allocation.
  const char bad_text[] = "1.0,1,1,10,1000000,1\nbroken line\n";
  const std::uint8_t bad_bin[] = {0, 5, 'A', 1, 2};  // frame claims 5 bytes, 3 present
  unsigned long allocs = 0;
  ParseStatus a, b;
  {
    Guard g;
    lobster::MessageReader rd(std::string_view(bad_text, sizeof bad_text - 1));
    FeedMsg m;
    rd.next(m);
    a = rd.next(m);
    itch::Decoder d(std::span<const std::uint8_t>(bad_bin, sizeof bad_bin));
    b = d.next(m);
    allocs = g.count();
  }
  EXPECT_EQ(allocs, 0u);
  EXPECT_EQ(a, ParseStatus::BadTimestamp);
  EXPECT_EQ(b, ParseStatus::Truncated);
}
