#pragma once

#include "lobkit/book/types.hpp"

namespace lobkit {

inline constexpr std::uint8_t kOrderLive = 1;

// Cache line reasoning: 32 bytes and 32 byte aligned means two orders per
// 64 byte line and no order ever straddles two lines. The match loop reads the
// head order, writes its quantity, and follows next, so one line fetch usually
// covers the maker and often its successor. Links are 32 bit pool indices, not
// pointers, which is what makes this fit.
struct alignas(32) Order {
  OrderId id;
  Qty qty;  // remaining
  Index level;
  Index prev;
  Index next;  // also the free list link while the slot is free
  OwnerId owner;
  Side side;
  std::uint8_t flags;
  std::uint32_t reserved;
};
static_assert(sizeof(Order) == 32 && alignof(Order) == 32);

// One price level. head/tail delimit an intrusive FIFO. total_qty is the
// aggregate the FOK check and depth queries read without walking the queue.
// 32 bytes for the same two-per-line reason as Order.
struct alignas(32) Level {
  Index head = kNil;
  Index tail = kNil;
  std::uint32_t count = 0;
  std::uint64_t total_qty = 0;
};
static_assert(sizeof(Level) == 32 && alignof(Level) == 32);

}  // namespace lobkit
