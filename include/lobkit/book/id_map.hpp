#pragma once

#include <bit>
#include <vector>

#include "lobkit/book/types.hpp"

namespace lobkit {

// Open addressing, linear probing, power of two capacity. Deletion uses
// backward shift instead of tombstones so probe lengths do not degrade over a
// long session. Capacity is fixed and sized for at most 50 percent load at the
// configured maximum number of live orders, so it never rehashes.
class IdMap {
 public:
  explicit IdMap(std::uint32_t max_live) {
    std::uint64_t want = static_cast<std::uint64_t>(max_live) * 2;
    if (want < 16) want = 16;
    const auto cap = static_cast<std::uint32_t>(std::bit_ceil(want));
    slots_.assign(cap, Slot{0, kNil, 0});
    mask_ = cap - 1;
    shift_ = 64 - std::countr_zero(cap);
  }

  [[nodiscard]] Index find(OrderId key) const noexcept {
    for (std::uint32_t i = home(key);; i = (i + 1) & mask_) {
      const Slot& s = slots_[i];
      if (s.idx == kNil) return kNil;
      if (s.key == key) return s.idx;
    }
  }

  // Caller guarantees the key is absent and the live count is within max_live.
  void insert(OrderId key, Index idx) noexcept {
    std::uint32_t i = home(key);
    while (slots_[i].idx != kNil) i = (i + 1) & mask_;
    slots_[i].key = key;
    slots_[i].idx = idx;
    ++size_;
  }

  void erase(OrderId key) noexcept {
    std::uint32_t i = home(key);
    while (slots_[i].key != key || slots_[i].idx == kNil) {
      if (slots_[i].idx == kNil) return;
      i = (i + 1) & mask_;
    }
    // Backward shift: pull later entries of the cluster into the hole unless
    // their home slot lies cyclically in (hole, candidate], where moving them
    // would break their probe chain.
    std::uint32_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (slots_[j].idx == kNil) break;
      const std::uint32_t k = home(slots_[j].key);
      const bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
      if (stays) continue;
      slots_[i] = slots_[j];
      i = j;
    }
    slots_[i].idx = kNil;
    --size_;
  }

  std::uint32_t size() const noexcept { return size_; }
  std::uint32_t capacity() const noexcept { return mask_ + 1; }

 private:
  // 16 bytes: four slots per cache line. Occupancy is encoded in idx so that
  // every 64 bit value, including 0, is a valid order id.
  struct Slot {
    OrderId key;
    Index idx;
    std::uint32_t pad;
  };
  static_assert(sizeof(Slot) == 16);

  // Fibonacci hashing: multiply by 2^64 / golden ratio and take the top bits.
  // Cheap, and it spreads sequential ids, which are the common case.
  std::uint32_t home(OrderId key) const noexcept {
    return static_cast<std::uint32_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
  }

  std::vector<Slot> slots_;
  std::uint32_t mask_ = 0;
  int shift_ = 0;
  std::uint32_t size_ = 0;
};

}  // namespace lobkit
