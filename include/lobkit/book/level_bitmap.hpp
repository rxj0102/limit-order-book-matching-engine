#pragma once

#include <bit>
#include <vector>

#include "lobkit/book/types.hpp"

namespace lobkit {

// Two level occupancy bitmap over the ladder. l0 has one bit per price level,
// l1 has one bit per l0 word (set iff the word is non-zero). Finding the next
// occupied level is at most a few word scans, independent of how sparse the
// book is, and the summary level (16 words for 65536 levels) stays in L1.
class LevelBitmap {
 public:
  explicit LevelBitmap(std::uint32_t nbits)
      : nbits_(nbits), l0_(nbits / 64, 0), l1_((nbits / 64 + 63) / 64, 0) {}

  void set(Index i) noexcept {
    const Index w = i >> 6;
    l0_[w] |= 1ull << (i & 63);
    l1_[w >> 6] |= 1ull << (w & 63);
  }

  void clear(Index i) noexcept {
    const Index w = i >> 6;
    l0_[w] &= ~(1ull << (i & 63));
    if (l0_[w] == 0) l1_[w >> 6] &= ~(1ull << (w & 63));
  }

  bool test(Index i) const noexcept { return (l0_[i >> 6] >> (i & 63)) & 1; }

  // Lowest set index >= i, or kNil.
  Index next_at_or_above(Index i) const noexcept {
    if (i >= nbits_) return kNil;
    const Index w = i >> 6;
    const std::uint64_t m = l0_[w] & (~0ull << (i & 63));
    if (m) return (w << 6) | static_cast<Index>(std::countr_zero(m));
    return first_in_words_from(w + 1);
  }

  // Highest set index <= i, or kNil.
  Index prev_at_or_below(Index i) const noexcept {
    if (i >= nbits_) i = nbits_ - 1;
    const Index w = i >> 6;
    const std::uint64_t m = l0_[w] & (~0ull >> (63 - (i & 63)));
    if (m) return (w << 6) | static_cast<Index>(63 - std::countl_zero(m));
    if (w == 0) return kNil;
    return last_in_words_upto(w - 1);
  }

  Index lowest() const noexcept { return next_at_or_above(0); }
  Index highest() const noexcept { return prev_at_or_below(nbits_ - 1); }

  std::uint32_t size_bits() const noexcept { return nbits_; }

 private:
  Index first_in_words_from(Index w) const noexcept {
    if (w >= l0_.size()) return kNil;
    Index j = w >> 6;
    std::uint64_t m = l1_[j] & (~0ull << (w & 63));
    while (!m) {
      if (++j >= l1_.size()) return kNil;
      m = l1_[j];
    }
    const Index wi = (j << 6) | static_cast<Index>(std::countr_zero(m));
    return (wi << 6) | static_cast<Index>(std::countr_zero(l0_[wi]));
  }

  Index last_in_words_upto(Index w) const noexcept {
    Index j = w >> 6;
    std::uint64_t m = l1_[j] & (~0ull >> (63 - (w & 63)));
    while (!m) {
      if (j == 0) return kNil;
      m = l1_[--j];
    }
    const Index wi = (j << 6) | static_cast<Index>(63 - std::countl_zero(m));
    return (wi << 6) | static_cast<Index>(63 - std::countl_zero(l0_[wi]));
  }

  std::uint32_t nbits_;
  std::vector<std::uint64_t> l0_;
  std::vector<std::uint64_t> l1_;
};

}  // namespace lobkit
