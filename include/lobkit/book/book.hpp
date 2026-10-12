#pragma once
// Book<Sink>: all matching state and every mutation primitive. Engine and
// ReplayAdapter are thin facades over this one class, so they necessarily share
// the same price ladder, order pool, id map and invariant checker.
//
// Nothing here allocates after construction, calls a virtual function, or uses
// floating point. Messages are processed to completion, one at a time.

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

#include "lobkit/book/config.hpp"
#include "lobkit/book/id_map.hpp"
#include "lobkit/book/level_bitmap.hpp"
#include "lobkit/book/order.hpp"
#include "lobkit/book/order_pool.hpp"
#include "lobkit/book/sinks.hpp"
#include "lobkit/book/types.hpp"

namespace lobkit {

namespace detail {
struct BookInspector;
}

template <EventSink Sink>
class Book {
 public:
  Book(const BookConfig& cfg, Sink sink)
      : cfg_((cfg.validate(), cfg)),
        sink_(std::move(sink)),
        levels_(cfg.num_levels),
        bids_(cfg.num_levels),
        asks_(cfg.num_levels),
        pool_(cfg.max_orders),
        map_(cfg.max_orders) {}

  // ---- Engine operations -------------------------------------------------

  void submit(const NewOrder& o) noexcept { (void)submit_impl(o); }

  void cancel(OrderId id, Timestamp ts) noexcept {
    const Index oi = map_.find(id);
    if (oi == kNil) return reject(ts, id, RejectReason::UnknownOrderId, Side::Bid, 0, 0);
    const Order& o = pool_[oi];
    emit_cancel(ts, o, o.qty, CancelReason::UserRequest);
    remove_order(oi);
  }

  void modify(const ModifyOrder& m) noexcept {
    const Index oi = map_.find(m.id);
    if (oi == kNil)
      return reject(m.ts, m.id, RejectReason::UnknownOrderId, Side::Bid, m.new_price, m.new_qty);
    Order& o = pool_[oi];
    if (m.new_qty == 0) return reject(m.ts, m.id, RejectReason::InvalidQty, o.side, m.new_price, 0);
    Index idx = kNil;
    if (const RejectReason r = to_index(m.new_price, idx); r != RejectReason::None)
      return reject(m.ts, m.id, r, o.side, m.new_price, m.new_qty);

    if (idx == o.level && m.new_qty <= o.qty) {
      // Same price, size not increased: edit in place. Queue position is kept
      // because the order never leaves the list.
      levels_[o.level].total_qty -= o.qty - m.new_qty;
      o.qty = m.new_qty;
      emit(EventType::Modify, m.ts, o.id, 0, m.new_price, o.qty, o.side, 0, 0);
      return;
    }
    // Price change or size increase: cancel and replace under the same id.
    const Side side = o.side;
    const OwnerId owner = o.owner;
    remove_order(oi);
    emit(EventType::Modify, m.ts, m.id, 0, m.new_price, m.new_qty, side, 0, 0);
    const Qty rem = match(side, owner, m.id, idx, m.new_qty, m.ts);
    // Cannot fail: remove_order above freed a pool slot.
    if (rem > 0) rest(m.id, side, owner, idx, rem);
  }

  // ---- Replay adapter operations (feed owner) ----------------------------

  void feed_add(OrderId id, Side side, Price price, Qty qty, Timestamp ts) noexcept {
    const NewOrder o{id, price, qty, side, OrderType::Limit, kFeedOwner, ts};
    const Qty traded = submit_impl(o);
    stats_.feed_add_displaced_qty += traded;
  }

  void feed_reduce(OrderId id, Qty qty, Timestamp ts) noexcept {
    Index oi = kNil;
    if (!feed_target(id, qty, ts, oi)) return;
    Order& o = pool_[oi];
    if (qty >= o.qty) {
      emit_cancel(ts, o, o.qty, CancelReason::FeedReduce);
      remove_order(oi);
    } else {
      emit_cancel(ts, o, qty, CancelReason::FeedReduce);
      o.qty -= qty;
      levels_[o.level].total_qty -= qty;
    }
  }

  void feed_delete(OrderId id, Timestamp ts) noexcept {
    Index oi = kNil;
    if (!feed_target(id, 1, ts, oi)) return;
    emit_cancel(ts, pool_[oi], pool_[oi].qty, CancelReason::FeedDelete);
    remove_order(oi);
  }

  // See MATCHING_RULES.md section 9 for the rules this implements.
  void feed_execute(OrderId id, Qty qty, Timestamp ts) noexcept {
    Index yi = kNil;
    if (!feed_target(id, qty, ts, yi)) return;
    Order& y = pool_[yi];
    Level& lvl = levels_[y.level];
    const Price px = price_of(y.level);
    const Side aggressor = opposite(y.side);
    if (qty > y.qty) {
      qty = y.qty;
      ++stats_.feed_exec_overflow;
    }

    Qty q_left = qty;
    bool feed_ahead = false;
    for (Index cur = lvl.head; cur != yi;) {
      Order& o = pool_[cur];
      const Index nxt = o.next;
      if (o.owner == kFeedOwner) {
        feed_ahead = true;
      } else if (!feed_ahead && q_left > 0) {
        const Qty fill = std::min(o.qty, q_left);
        emit(EventType::Trade, ts, 0, o.id, px, fill, aggressor, 0, kTradeExternalTaker);
        ++stats_.trades;
        o.qty -= fill;
        lvl.total_qty -= fill;
        q_left -= fill;
        if (o.qty == 0) remove_order(cur);
      }
      cur = nxt;
    }
    if (feed_ahead) ++stats_.queue_order_anomalies;

    // The feed order is always reduced by the full feed quantity so that the
    // feed side of the book stays an exact replica of the feed.
    emit(EventType::Trade, ts, 0, y.id, px, qty, aggressor, 0, kTradeExternalTaker);
    ++stats_.trades;
    y.qty -= qty;
    lvl.total_qty -= qty;
    if (y.qty == 0) remove_order(yi);
  }

  // ---- Read side ---------------------------------------------------------

  const BookConfig& config() const noexcept { return cfg_; }
  const BookStats& stats() const noexcept { return stats_; }
  Sink& sink() noexcept { return sink_; }
  const Sink& sink() const noexcept { return sink_; }
  Seq last_seq() const noexcept { return seq_; }
  std::uint32_t live_orders() const noexcept { return pool_.in_use(); }

  bool has_bid() const noexcept { return best_bid_ != kNil; }
  bool has_ask() const noexcept { return best_ask_ != kNil; }
  Price best_bid() const noexcept { return price_of(best_bid_); }
  Price best_ask() const noexcept { return price_of(best_ask_); }

  // Aggregate resting quantity at a price, 0 if the price is off grid or empty.
  Qty depth_at(Price p) const noexcept {
    Index idx = kNil;
    if (to_index(p, idx) != RejectReason::None) return 0;
    return static_cast<Qty>(levels_[idx].total_qty);
  }

  // Returns nullptr if the id is not live. The pointer is invalidated by the
  // next mutation.
  [[nodiscard]] const Order* find_order(OrderId id) const noexcept {
    const Index oi = map_.find(id);
    return oi == kNil ? nullptr : &pool_[oi];
  }

  Price price_of(Index level) const noexcept {
    return cfg_.base_price + static_cast<Price>(level) * cfg_.tick_size;
  }

  // Cold path convenience for tests: order ids at a price, front to back.
  std::vector<OrderId> queue_ids(Price p) const {
    std::vector<OrderId> out;
    Index idx = kNil;
    if (to_index(p, idx) != RejectReason::None) return out;
    for (Index c = levels_[idx].head; c != kNil; c = pool_[c].next) out.push_back(pool_[c].id);
    return out;
  }

 private:
  friend struct detail::BookInspector;

  // ---- Event emission ----------------------------------------------------

  void emit(EventType t, Timestamp ts, OrderId id, OrderId other, Price price, Qty qty, Side side,
            std::uint8_t reason, std::uint8_t flags) noexcept {
    Event e{};  // zero init so no stray bytes reach the hash
    e.seq = ++seq_;
    e.ts = ts;
    e.id = id;
    e.other = other;
    e.price = price;
    e.qty = qty;
    e.type = t;
    e.reason = reason;
    e.side = side;
    e.flags = flags;
    sink_(e);
  }

  void reject(Timestamp ts, OrderId id, RejectReason r, Side side, Price price, Qty qty) noexcept {
    ++stats_.rejects[static_cast<std::size_t>(r)];
    emit(EventType::Reject, ts, id, 0, price, qty, side, static_cast<std::uint8_t>(r), 0);
  }

  void emit_cancel(Timestamp ts, const Order& o, Qty qty, CancelReason why) noexcept {
    emit(EventType::Cancel, ts, o.id, 0, price_of(o.level), qty, o.side,
         static_cast<std::uint8_t>(why), 0);
  }

  // ---- Grid --------------------------------------------------------------

  RejectReason to_index(Price p, Index& out) const noexcept {
    if (p < cfg_.base_price) return RejectReason::PriceOutOfBand;
    // p >= base_price, so the unsigned difference is exact.
    const auto off = static_cast<std::uint64_t>(p) - static_cast<std::uint64_t>(cfg_.base_price);
    const auto tick = static_cast<std::uint64_t>(cfg_.tick_size);
    const std::uint64_t ticks = off / tick;
    if (ticks >= cfg_.num_levels) return RejectReason::PriceOutOfBand;
    if (off % tick != 0) return RejectReason::PriceOffTick;
    out = static_cast<Index>(ticks);
    return RejectReason::None;
  }

  // ---- Validation shared by the feed operations --------------------------

  bool feed_target(OrderId id, Qty qty, Timestamp ts, Index& oi) noexcept {
    if (qty == 0) {
      reject(ts, id, RejectReason::InvalidQty, Side::Bid, 0, 0);
      return false;
    }
    oi = map_.find(id);
    if (oi == kNil) {
      reject(ts, id, RejectReason::UnknownOrderId, Side::Bid, 0, qty);
      return false;
    }
    if (pool_[oi].owner != kFeedOwner) {
      reject(ts, id, RejectReason::WrongOwner, pool_[oi].side, price_of(pool_[oi].level), qty);
      return false;
    }
    return true;
  }

  // ---- New order path ----------------------------------------------------

  // Returns the quantity that traded on arrival.
  [[nodiscard]] Qty submit_impl(const NewOrder& o) noexcept {
    if (o.qty == 0) {
      reject(o.ts, o.id, RejectReason::InvalidQty, o.side, o.price, 0);
      return 0;
    }
    Index idx = kNil;
    if (o.type != OrderType::Market) {
      if (const RejectReason r = to_index(o.price, idx); r != RejectReason::None) {
        reject(o.ts, o.id, r, o.side, o.price, o.qty);
        return 0;
      }
    }
    if (map_.find(o.id) != kNil) {
      reject(o.ts, o.id, RejectReason::DuplicateOrderId, o.side, o.price, o.qty);
      return 0;
    }
    const bool may_rest = o.type == OrderType::Limit || o.type == OrderType::PostOnly;
    // Checked up front, even if the order would match completely, so the
    // outcome does not depend on the match result.
    if (may_rest && pool_.full()) {
      reject(o.ts, o.id, RejectReason::PoolExhausted, o.side, o.price, o.qty);
      return 0;
    }
    if (o.type == OrderType::PostOnly && crosses(o.side, idx)) {
      reject(o.ts, o.id, RejectReason::PostOnlyWouldCross, o.side, o.price, o.qty);
      return 0;
    }
    if (o.type == OrderType::Fok && available(o.side, idx, o.qty) < o.qty) {
      reject(o.ts, o.id, RejectReason::FokNotFillable, o.side, o.price, o.qty);
      return 0;
    }

    emit(EventType::Ack, o.ts, o.id, 0, o.type == OrderType::Market ? 0 : o.price, o.qty, o.side,
         static_cast<std::uint8_t>(o.type), 0);
    const Qty rem = match(o.side, o.owner, o.id, idx, o.qty, o.ts);
    if (rem > 0) {
      switch (o.type) {
        case OrderType::Limit:
        case OrderType::PostOnly:
          rest(o.id, o.side, o.owner, idx, rem);
          break;
        case OrderType::Ioc:
          emit(EventType::Cancel, o.ts, o.id, 0, o.price, rem, o.side,
               static_cast<std::uint8_t>(CancelReason::IocRemainder), 0);
          break;
        case OrderType::Market:
          emit(EventType::Cancel, o.ts, o.id, 0, 0, rem, o.side,
               static_cast<std::uint8_t>(CancelReason::MarketRemainder), 0);
          break;
        case OrderType::Fok:
          break;  // unreachable: availability was checked
      }
    }
    return o.qty - rem;
  }

  bool crosses(Side side, Index idx) const noexcept {
    return side == Side::Bid ? (best_ask_ != kNil && best_ask_ <= idx)
                             : (best_bid_ != kNil && best_bid_ >= idx);
  }

  // Quantity available at prices at or better than idx, stopping early once
  // `need` is reached. Reads level aggregates only; no mutation.
  std::uint64_t available(Side side, Index idx, Qty need) const noexcept {
    std::uint64_t sum = 0;
    if (side == Side::Bid) {
      for (Index l = best_ask_; l != kNil && l <= idx && sum < need;
           l = asks_.next_at_or_above(l + 1))
        sum += levels_[l].total_qty;
    } else {
      for (Index l = best_bid_; l != kNil && l >= idx && sum < need;
           l = l == 0 ? kNil : bids_.prev_at_or_below(l - 1))
        sum += levels_[l].total_qty;
    }
    return sum;
  }

  // Matches the incoming order against the opposite side and returns the
  // unfilled remainder. limit == kNil means no price limit (market order).
  Qty match(Side side, OwnerId owner, OrderId id, Index limit, Qty qty, Timestamp ts) noexcept {
    const bool buy = side == Side::Bid;
    while (qty > 0) {
      const Index lvl_idx = buy ? best_ask_ : best_bid_;
      if (lvl_idx == kNil) break;
      if (limit != kNil && (buy ? lvl_idx > limit : lvl_idx < limit)) break;
      Level& lvl = levels_[lvl_idx];
      const Price px = price_of(lvl_idx);
      while (qty > 0 && lvl.head != kNil) {
        const Index mi = lvl.head;
        Order& m = pool_[mi];
        const Qty fill = std::min(qty, m.qty);
        std::uint8_t flags = 0;
        if (owner != kFeedOwner && owner == m.owner) flags |= kTradeSelf;
        if (owner == kFeedOwner) {
          if (m.owner == kFeedOwner) {
            ++stats_.feed_crossed_fills;
          } else {
            ++stats_.feed_crossed_own_fills;
          }
        }
        emit(EventType::Trade, ts, id, m.id, px, fill, side, 0, flags);
        ++stats_.trades;
        m.qty -= fill;
        lvl.total_qty -= fill;
        qty -= fill;
        if (m.qty == 0) remove_order(mi);
      }
    }
    return qty;
  }

  // ---- Structural primitives ---------------------------------------------

  void rest(OrderId id, Side side, OwnerId owner, Index level, Qty qty) noexcept {
    const Index oi = pool_.allocate();  // fullness was checked by the caller
    Order& o = pool_[oi];
    o.id = id;
    o.qty = qty;
    o.level = level;
    o.owner = owner;
    o.side = side;
    o.flags = kOrderLive;
    o.reserved = 0;
    o.next = kNil;
    o.prev = kNil;

    Level& lvl = levels_[level];
    if (lvl.tail == kNil) {
      lvl.head = lvl.tail = oi;
      if (side == Side::Bid) {
        bids_.set(level);
        if (best_bid_ == kNil || level > best_bid_) best_bid_ = level;
      } else {
        asks_.set(level);
        if (best_ask_ == kNil || level < best_ask_) best_ask_ = level;
      }
    } else {
      o.prev = lvl.tail;
      pool_[lvl.tail].next = oi;
      lvl.tail = oi;
    }
    ++lvl.count;
    lvl.total_qty += qty;
    map_.insert(id, oi);
  }

  // Unlinks, updates level aggregates and the touch, erases from the id map,
  // returns the slot to the pool. The order's current qty is what is removed
  // from the level aggregate, so callers must not pre-subtract it.
  void remove_order(Index oi) noexcept {
    Order& o = pool_[oi];
    Level& lvl = levels_[o.level];
    if (o.prev != kNil)
      pool_[o.prev].next = o.next;
    else
      lvl.head = o.next;
    if (o.next != kNil)
      pool_[o.next].prev = o.prev;
    else
      lvl.tail = o.prev;
    --lvl.count;
    lvl.total_qty -= o.qty;
    if (lvl.head == kNil) {
      if (o.side == Side::Bid) {
        bids_.clear(o.level);
        if (best_bid_ == o.level) best_bid_ = bids_.prev_at_or_below(o.level);
      } else {
        asks_.clear(o.level);
        if (best_ask_ == o.level) best_ask_ = asks_.next_at_or_above(o.level);
      }
    }
    map_.erase(o.id);
    pool_.release(oi);
  }

  // ---- State -------------------------------------------------------------
  // Hot scalars first so they share a cache line; the vectors' control blocks
  // are cold and live behind them.
  alignas(64) Index best_bid_ = kNil;
  Index best_ask_ = kNil;
  Seq seq_ = 0;
  BookStats stats_{};

  BookConfig cfg_;
  Sink sink_;
  std::vector<Level> levels_;
  LevelBitmap bids_;
  LevelBitmap asks_;
  OrderPool pool_;
  IdMap map_;
};

}  // namespace lobkit
