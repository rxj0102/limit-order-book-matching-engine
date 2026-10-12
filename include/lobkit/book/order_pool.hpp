#pragma once

#include <vector>

#include "lobkit/book/order.hpp"

namespace lobkit {

// Fixed capacity slab with an intrusive free list. allocate and release are a
// pop and a push; nothing here can call the system allocator after construction.
class OrderPool {
 public:
  explicit OrderPool(std::uint32_t capacity) : slots_(capacity) {
    for (std::uint32_t i = 0; i < capacity; ++i) slots_[i].next = i + 1;
    if (capacity > 0) slots_[capacity - 1].next = kNil;
    free_head_ = capacity > 0 ? 0 : kNil;
  }

  Index allocate() noexcept {
    const Index i = free_head_;
    if (i == kNil) return kNil;
    free_head_ = slots_[i].next;
    ++in_use_;
    return i;
  }

  void release(Index i) noexcept {
    slots_[i].flags = 0;
    slots_[i].next = free_head_;
    free_head_ = i;
    --in_use_;
  }

  Order& operator[](Index i) noexcept { return slots_[i]; }
  const Order& operator[](Index i) const noexcept { return slots_[i]; }

  bool full() const noexcept { return free_head_ == kNil; }
  std::uint32_t in_use() const noexcept { return in_use_; }
  std::uint32_t capacity() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }
  Index free_head() const noexcept { return free_head_; }

 private:
  std::vector<Order> slots_;
  Index free_head_ = kNil;
  std::uint32_t in_use_ = 0;
};

}  // namespace lobkit
