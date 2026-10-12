#pragma once
// Types shared by the simulator and strategies.
//
// Units. The engine and the simulator accounting are integer: prices are the
// feed native units (dollars * 10000), quantities are shares. Money totals are
// kept in half native units (so a half-tick mid is exact) in 128 bit integers.
// Only the strategy math (avellaneda_stoikov.hpp) uses double.

#include <cstdint>
#include <vector>

#include "lobkit/book/config.hpp"
#include "lobkit/book/types.hpp"
#include "lobkit/md/status.hpp"

namespace lobkit::sim {

// Own order ids live in [2^62, 2^63). Feed messages with an id in the upper two
// bits are refused by the simulator so the two namespaces cannot collide.
inline constexpr OrderId kOwnIdBase = 1ull << 62;
inline constexpr OwnerId kStrategyOwner = 1;
constexpr bool is_own_id(OrderId id) noexcept {
  return (id >> 62) == 1;
}

// Native price units per dollar (LOBSTER and ITCH both use 1e-4 dollars).
inline constexpr double kUnitsPerDollar = 10000.0;

// __extension__ keeps -Wpedantic quiet about the compiler's 128 bit integer (GCC and Clang).
__extension__ typedef __int128 Money2;  // half native units times shares

struct SimConfig {
  // Latencies, nanoseconds of simulated time. All default to zero.
  Timestamp submit_latency_ns = 0;  // strategy to exchange: orders and cancels
  Timestamp ack_latency_ns = 0;     // exchange to strategy: acks, rejects, fills, cancel results
  Timestamp md_latency_ns = 0;      // exchange to strategy: market data
  // Fees in native price units per share (10000 = one dollar per share). A negative
  // value is a rebate. Applied by liquidity role.
  std::int64_t maker_fee = 0;
  std::int64_t taker_fee = 0;
  Timestamp sample_interval_ns = 1'000'000'000;  // spacing of the inventory and PnL path samples
  bool record_fills = true;
};

// Top of the book as the strategy sees it, with the strategy's own displayed
// orders removed (the strategy knows its quotes; it must not chase them).
struct MarketView {
  Timestamp ts = 0;  // exchange time of the snapshot, before any market data delay
  Price bid = 0, ask = 0;
  Qty bid_qty = 0, ask_qty = 0;
  bool two_sided = false;
};

struct FillNotice {
  OrderId id;
  Side side;  // side of the strategy order
  Price price;
  Qty qty;
  bool maker;
  Timestamp exchange_ts;  // when the fill happened; the notice arrives ack_latency_ns later
};

enum class NoticeKind : std::uint8_t { Ack, Reject, Fill, CancelAck, CancelReject };

struct FillRecord {
  Timestamp ts;
  OrderId id;
  Side side;
  Price price;
  Qty qty;
  bool maker;
  std::int64_t fee_units;  // total fee for this fill, native units (negative: rebate)
  std::int64_t mid2_ref;   // reference mid in half native units used for spread capture
  std::int64_t inventory_after;
};

struct PathSample {
  Timestamp ts;
  std::int64_t inventory;
  std::int64_t known_inventory_lag;  // true inventory minus what the strategy has been told: 0 with
                                     // no latency
  Money2 spread2, inventory2, fees2;
};

struct SimStats {
  std::uint64_t feed_messages = 0;
  std::uint64_t feed_id_collisions = 0;  // feed ids inside the own id range, skipped
  std::uint64_t time_regressions = 0;    // feed timestamps that went backwards
  std::uint64_t orders_sent = 0, orders_accepted = 0, orders_rejected = 0;
  std::uint64_t cancels_sent = 0, cancels_confirmed = 0, cancels_too_late = 0;
  std::uint64_t fills = 0, shares_filled = 0, maker_fills = 0, taker_fills = 0;
  std::uint64_t self_trades = 0;
  std::int64_t max_abs_inventory = 0;
  std::uint64_t market_updates = 0;
};

// Accounting result. total = spread_capture + inventory - fees, exactly.
struct Pnl {
  Money2 spread2 = 0;     // sum over fills of side * qty * (mid_ref - price)
  Money2 inventory2 = 0;  // sum over mid changes of inventory * change in mid
  Money2 fees2 = 0;       // fees paid (rebates reduce it), same unit
  Money2 cash2 = 0;       // sum over fills of -side * qty * price: the independent ledger
  std::int64_t inventory = 0;
  std::int64_t mid2_final = 0;

  Money2 total2() const noexcept { return spread2 + inventory2 - fees2; }
  // Mark to market straight from the cash ledger and final inventory, which the
  // decomposition above never touches. The identity test compares the two.
  Money2 total2_from_ledger() const noexcept {
    return cash2 + static_cast<Money2>(inventory) * mid2_final - fees2;
  }
};

struct RunResult {
  SimStats stats;
  Pnl pnl;
  std::vector<FillRecord> fills;
  std::vector<PathSample> path;
  BookStats book;  // engine counters at the end (rejects, anomalies)
  Timestamp first_ts = 0, last_ts = 0;
  md::ParseStatus feed_status = md::ParseStatus::End;  // End: the feed ran to its clean end
};

}  // namespace lobkit::sim
