// Proves the hot path never touches the heap. This binary replaces the global
// allocation functions; any call while the guard is armed is counted, and the
// test fails if the count is not zero. Kept in its own executable because the
// replacement is process wide.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>

#include "lobkit/book/engine.hpp"
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
