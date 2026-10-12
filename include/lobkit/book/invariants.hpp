#pragma once
// Structural invariant checker. O(levels + orders), meant for tests and debug
// builds, never for the hot path. It returns a description of the first
// violation instead of asserting so tests can also prove it detects damage.

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "lobkit/book/book.hpp"

namespace lobkit {
namespace detail {

struct BookInspector {
  template <class S>
  static auto& levels(Book<S>& b) {
    return b.levels_;
  }
  template <class S>
  static auto& pool(Book<S>& b) {
    return b.pool_;
  }
  template <class S>
  static Index& best_bid(Book<S>& b) {
    return b.best_bid_;
  }
  template <class S>
  static Index& best_ask(Book<S>& b) {
    return b.best_ask_;
  }
  template <class S>
  static auto& bids(Book<S>& b) {
    return b.bids_;
  }
  template <class S>
  static auto& asks(Book<S>& b) {
    return b.asks_;
  }
  template <class S>
  static auto& map(Book<S>& b) {
    return b.map_;
  }

  template <class S>
  static std::optional<std::string> check(const Book<S>& b) {
    auto fail = [](std::string m) { return std::optional<std::string>(std::move(m)); };
    const std::uint32_t n = b.cfg_.num_levels;
    std::uint32_t live = 0;

    for (Index l = 0; l < n; ++l) {
      const Level& lv = b.levels_[l];
      const bool in_bid = b.bids_.test(l);
      const bool in_ask = b.asks_.test(l);
      if (in_bid && in_ask) return fail("level " + std::to_string(l) + " in both bitmaps");
      const bool occupied = lv.head != kNil;
      if (occupied != (in_bid || in_ask))
        return fail("bitmap disagrees with occupancy at level " + std::to_string(l));
      if (!occupied) {
        if (lv.tail != kNil || lv.count != 0 || lv.total_qty != 0)
          return fail("empty level " + std::to_string(l) + " has residue");
        continue;
      }
      const Side side = in_bid ? Side::Bid : Side::Ask;
      Index prev = kNil;
      std::uint64_t sum = 0;
      std::uint32_t cnt = 0;
      for (Index c = lv.head; c != kNil; c = b.pool_[c].next) {
        if (++cnt > b.pool_.capacity()) return fail("cycle in level " + std::to_string(l));
        const Order& o = b.pool_[c];
        if (!(o.flags & kOrderLive)) return fail("dead order linked at level " + std::to_string(l));
        if (o.prev != prev) return fail("broken prev link at level " + std::to_string(l));
        if (o.level != l) return fail("order level mismatch at level " + std::to_string(l));
        if (o.side != side) return fail("order side mismatch at level " + std::to_string(l));
        if (o.qty == 0) return fail("zero quantity order at level " + std::to_string(l));
        if (b.map_.find(o.id) != c)
          return fail("id map does not point at order " + std::to_string(o.id));
        sum += o.qty;
        prev = c;
      }
      if (prev != lv.tail) return fail("tail mismatch at level " + std::to_string(l));
      if (cnt != lv.count) return fail("count mismatch at level " + std::to_string(l));
      if (sum != lv.total_qty)
        return fail("aggregate quantity mismatch at level " + std::to_string(l));
      live += cnt;
    }

    const Index hb = b.bids_.highest();
    const Index la = b.asks_.lowest();
    if (b.best_bid_ != hb) return fail("cached best bid is stale");
    if (b.best_ask_ != la) return fail("cached best ask is stale");
    if (hb != kNil && la != kNil && hb >= la) return fail("book is crossed");

    if (live != b.pool_.in_use()) return fail("pool in_use disagrees with linked orders");
    if (b.map_.size() != live) return fail("id map size disagrees with live orders");

    std::uint32_t free_len = 0;
    for (Index f = b.pool_.free_head(); f != kNil; f = b.pool_[f].next) {
      if (b.pool_[f].flags & kOrderLive) return fail("live order on free list");
      if (++free_len > b.pool_.capacity()) return fail("cycle in free list");
    }
    if (free_len + live != b.pool_.capacity())
      return fail("pool accounting: free + live != capacity");

    std::uint32_t mapped = 0;
    bool map_ok = true;
    b.map_.for_each([&](OrderId id, Index idx) {
      ++mapped;
      if (idx >= b.pool_.capacity() || b.pool_[idx].id != id || !(b.pool_[idx].flags & kOrderLive))
        map_ok = false;
    });
    if (!map_ok || mapped != live) return fail("id map contains a stale entry");
    return std::nullopt;
  }
};

}  // namespace detail

template <class S>
std::optional<std::string> check_invariants(const Book<S>& b) {
  return detail::BookInspector::check(b);
}

}  // namespace lobkit

// Aborts with a message in builds without NDEBUG. Costs O(book) per use.
#ifndef NDEBUG
#define LOBKIT_ASSERT_INVARIANTS(book)                                               \
  do {                                                                               \
    if (auto lobkit_err_ = ::lobkit::check_invariants(book)) {                       \
      std::fprintf(stderr, "lobkit invariant violated: %s\n", lobkit_err_->c_str()); \
      std::abort();                                                                  \
    }                                                                                \
  } while (0)
#else
#define LOBKIT_ASSERT_INVARIANTS(book) ((void)0)
#endif
