#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <stdexcept>

#include "lobkit/book/types.hpp"

namespace lobkit {

enum class SelfTradePolicy : std::uint8_t {
  Allow = 0,  // execute and flag in the Trade event
  // CancelNewest is reserved for a later release.
};

struct BookConfig {
  // Instrument grid. Ladder index = (price - base_price) / tick_size, so the
  // ladder spans num_levels real ticks regardless of the native price scale.
  Price tick_size = 1;
  Price base_price = 0;
  std::uint32_t num_levels = 65536;  // power of two, multiple of 64
  std::uint32_t max_orders = 1u << 20;
  SelfTradePolicy self_trade = SelfTradePolicy::Allow;

  void validate() const {
    if (tick_size <= 0) throw std::invalid_argument("tick_size must be positive");
    if (base_price < 0) throw std::invalid_argument("base_price must be non-negative");
    if (num_levels < 64 || num_levels > (1u << 24) || !std::has_single_bit(num_levels))
      throw std::invalid_argument("num_levels must be a power of two in [64, 2^24]");
    if (max_orders == 0 || max_orders >= (1u << 30))
      throw std::invalid_argument("max_orders must be in [1, 2^30)");
    // The whole band must be representable in Price.
    if (tick_size > (INT64_MAX - base_price) / static_cast<Price>(num_levels))
      throw std::invalid_argument("price band overflows int64");
  }
};

struct BookStats {
  std::array<std::uint64_t, static_cast<std::size_t>(RejectReason::kCount)> rejects{};
  std::uint64_t trades = 0;
  // Replay diagnostics, see MATCHING_RULES.md section 9.
  std::uint64_t feed_crossed_fills = 0;      // a feed add traded against a feed order
  std::uint64_t feed_crossed_own_fills = 0;  // a feed add traded against an own resting order
  std::uint64_t feed_add_displaced_qty = 0;  // feed add quantity consumed by crossing own orders
  std::uint64_t feed_exec_overflow = 0;      // feed execute larger than remaining quantity
  std::uint64_t queue_order_anomalies = 0;   // feed order ahead of an own order was not drained

  // Replay divergences. Plain replay and simulation count and report them;
  // snapshot verification treats any non-zero value as a failure, together with
  // out-of-band and off-tick rejects (MATCHING_RULES.md section 9).
  std::uint64_t anomalies() const noexcept {
    return feed_crossed_fills + feed_crossed_own_fills + feed_exec_overflow + queue_order_anomalies;
  }

  std::uint64_t reject_count(RejectReason r) const noexcept {
    return rejects[static_cast<std::size_t>(r)];
  }
};

}  // namespace lobkit
