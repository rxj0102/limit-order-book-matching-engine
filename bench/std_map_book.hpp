#pragma once
// Baseline order book for the benchmarks: the textbook design that most people write first.
//
//   std::map<Price, std::list<Order>>   one tree per side, one list node per order
//   std::unordered_map<OrderId, Loc>    id to list iterator, for O(1) cancel
//
// It is NOT the ReferenceBook in tests/support. That one is deliberately O(n) (linear
// searches, native prices) so that it is obviously correct, which would make any comparison
// with it meaningless. This one is the fastest straightforward std:: design: cancels are O(1)
// through stored iterators, the id map is reserved up front so it never rehashes, and the
// best price is the first element of the map.
//
// Differences from lobkit::Book, which are what the comparison measures:
//   * one heap allocation per resting order (list node) and per level (map node) and per id
//     (unordered_map node); the engine allocates nothing after construction
//   * levels are found by tree descent (O(log levels)); the engine indexes a ladder directly
//   * the touch is a map begin(); the engine keeps it in a field and uses bitmaps to find the
//     next level
// It implements the same semantics as lobkit::Book for owner 0 flow and for the engine
// operations with owner ids. It does NOT implement own order queue rules for feed executes
// (MATCHING_RULES.md section 9): the benchmarks never mix own orders with feed operations, and
// bench_book checks that the baseline and the engine produce identical event streams before any
// number is taken.

#include <functional>
#include <list>
#include <map>
#include <unordered_map>

#include "lobkit/book/config.hpp"
#include "lobkit/book/types.hpp"

namespace lobkit::bench {

template <class Sink>
class StdMapBook {
 public:
  StdMapBook(const BookConfig& cfg, Sink sink) : cfg_(cfg), sink_(std::move(sink)) {
    cfg_.validate();
    where_.reserve(cfg_.max_orders);
  }

  Sink& sink() { return sink_; }
  std::uint64_t rejects() const { return rejects_; }
  std::uint32_t live_orders() const { return static_cast<std::uint32_t>(where_.size()); }

  // ---- engine operations ----
  void submit(const NewOrder& o) { (void)submit_impl(o); }

  void cancel(OrderId id, Timestamp ts) {
    auto it = where_.find(id);
    if (it == where_.end()) return reject(ts, id, RejectReason::UnknownOrderId, Side::Bid, 0, 0);
    const Ord& o = *it->second.it;
    emit(EventType::Cancel, ts, o.id, 0, it->second.price, o.qty, o.side,
         static_cast<std::uint8_t>(CancelReason::UserRequest), 0);
    remove(it);
  }

  void modify(const ModifyOrder& m) {
    auto it = where_.find(m.id);
    if (it == where_.end())
      return reject(m.ts, m.id, RejectReason::UnknownOrderId, Side::Bid, m.new_price, m.new_qty);
    Ord& o = *it->second.it;
    if (m.new_qty == 0) return reject(m.ts, m.id, RejectReason::InvalidQty, o.side, m.new_price, 0);
    if (const RejectReason r = check_price(m.new_price); r != RejectReason::None)
      return reject(m.ts, m.id, r, o.side, m.new_price, m.new_qty);
    if (m.new_price == it->second.price && m.new_qty <= o.qty) {
      o.qty = m.new_qty;
      emit(EventType::Modify, m.ts, o.id, 0, m.new_price, o.qty, o.side, 0, 0);
      return;
    }
    const Side side = o.side;
    const OwnerId owner = o.owner;
    remove(it);
    emit(EventType::Modify, m.ts, m.id, 0, m.new_price, m.new_qty, side, 0, 0);
    const Qty rem = match(side, owner, m.id, true, m.new_price, m.new_qty, m.ts);
    if (rem > 0) rest(m.id, side, owner, m.new_price, rem);
  }

  // ---- feed operations (owner 0 only) ----
  struct Adapter {
    StdMapBook* b;
    void apply_add(OrderId id, Side side, Price price, Qty qty, Timestamp ts) {
      (void)b->submit_impl(NewOrder{id, price, qty, side, OrderType::Limit, kFeedOwner, ts});
    }
    void apply_reduce(OrderId id, Qty qty, Timestamp ts) {
      auto it = b->feed_target(id, qty, ts);
      if (it == b->where_.end()) return;
      Ord& o = *it->second.it;
      if (qty >= o.qty) {
        b->emit(EventType::Cancel, ts, o.id, 0, it->second.price, o.qty, o.side,
                static_cast<std::uint8_t>(CancelReason::FeedReduce), 0);
        b->remove(it);
      } else {
        b->emit(EventType::Cancel, ts, o.id, 0, it->second.price, qty, o.side,
                static_cast<std::uint8_t>(CancelReason::FeedReduce), 0);
        o.qty -= qty;
      }
    }
    void apply_delete(OrderId id, Timestamp ts) {
      auto it = b->feed_target(id, 1, ts);
      if (it == b->where_.end()) return;
      const Ord& o = *it->second.it;
      b->emit(EventType::Cancel, ts, o.id, 0, it->second.price, o.qty, o.side,
              static_cast<std::uint8_t>(CancelReason::FeedDelete), 0);
      b->remove(it);
    }
    void apply_execute(OrderId id, Qty qty, Timestamp ts) {
      auto it = b->feed_target(id, qty, ts);
      if (it == b->where_.end()) return;
      Ord& y = *it->second.it;
      if (qty > y.qty) qty = y.qty;
      b->emit(EventType::Trade, ts, 0, y.id, it->second.price, qty,
              y.side == Side::Bid ? Side::Ask : Side::Bid, 0, kTradeExternalTaker);
      y.qty -= qty;
      if (y.qty == 0) b->remove(it);
    }
  };
  Adapter replay_adapter() { return Adapter{this}; }

 private:
  struct Ord {
    OrderId id;
    Qty qty;
    Side side;
    OwnerId owner;
  };
  using Queue = std::list<Ord>;
  struct Loc {
    Queue::iterator it;
    Price price;
  };
  using Where = std::unordered_map<OrderId, Loc>;
  using Bids = std::map<Price, Queue, std::greater<Price>>;
  using Asks = std::map<Price, Queue>;

  void emit(EventType t, Timestamp ts, OrderId id, OrderId other, Price price, Qty qty, Side side,
            std::uint8_t reason, std::uint8_t flags) {
    Event e{};
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

  void reject(Timestamp ts, OrderId id, RejectReason r, Side side, Price price, Qty qty) {
    ++rejects_;
    emit(EventType::Reject, ts, id, 0, price, qty, side, static_cast<std::uint8_t>(r), 0);
  }

  RejectReason check_price(Price p) const {
    if (p < cfg_.base_price) return RejectReason::PriceOutOfBand;
    const auto off = static_cast<std::uint64_t>(p) - static_cast<std::uint64_t>(cfg_.base_price);
    const auto tick = static_cast<std::uint64_t>(cfg_.tick_size);
    if (off / tick >= cfg_.num_levels) return RejectReason::PriceOutOfBand;
    if (off % tick != 0) return RejectReason::PriceOffTick;
    return RejectReason::None;
  }

  Where::iterator feed_target(OrderId id, Qty qty, Timestamp ts) {
    if (qty == 0) {
      reject(ts, id, RejectReason::InvalidQty, Side::Bid, 0, 0);
      return where_.end();
    }
    auto it = where_.find(id);
    if (it == where_.end()) {
      reject(ts, id, RejectReason::UnknownOrderId, Side::Bid, 0, qty);
      return where_.end();
    }
    if (it->second.it->owner != kFeedOwner) {
      reject(ts, id, RejectReason::WrongOwner, it->second.it->side, it->second.price, qty);
      return where_.end();
    }
    return it;
  }

  bool crosses(Side side, Price p) const {
    return side == Side::Bid ? (!asks_.empty() && asks_.begin()->first <= p)
                             : (!bids_.empty() && bids_.begin()->first >= p);
  }

  std::uint64_t available(Side side, Price p, Qty need) const {
    std::uint64_t sum = 0;
    if (side == Side::Bid) {
      for (auto it = asks_.begin(); it != asks_.end() && it->first <= p && sum < need; ++it)
        sum += level_qty(it->second);
    } else {
      for (auto it = bids_.begin(); it != bids_.end() && it->first >= p && sum < need; ++it)
        sum += level_qty(it->second);
    }
    return sum;
  }
  static std::uint64_t level_qty(const Queue& q) {
    std::uint64_t s = 0;
    for (const Ord& o : q) s += o.qty;
    return s;
  }

  Qty submit_impl(const NewOrder& o) {
    if (o.qty == 0) {
      reject(o.ts, o.id, RejectReason::InvalidQty, o.side, o.price, 0);
      return 0;
    }
    if (o.type != OrderType::Market) {
      if (const RejectReason r = check_price(o.price); r != RejectReason::None) {
        reject(o.ts, o.id, r, o.side, o.price, o.qty);
        return 0;
      }
    }
    if (where_.count(o.id)) {
      reject(o.ts, o.id, RejectReason::DuplicateOrderId, o.side, o.price, o.qty);
      return 0;
    }
    const bool may_rest = o.type == OrderType::Limit || o.type == OrderType::PostOnly;
    if (may_rest && where_.size() >= cfg_.max_orders) {
      reject(o.ts, o.id, RejectReason::PoolExhausted, o.side, o.price, o.qty);
      return 0;
    }
    const bool market = o.type == OrderType::Market;
    if (o.type == OrderType::PostOnly && crosses(o.side, o.price)) {
      reject(o.ts, o.id, RejectReason::PostOnlyWouldCross, o.side, o.price, o.qty);
      return 0;
    }
    if (o.type == OrderType::Fok && available(o.side, o.price, o.qty) < o.qty) {
      reject(o.ts, o.id, RejectReason::FokNotFillable, o.side, o.price, o.qty);
      return 0;
    }
    emit(EventType::Ack, o.ts, o.id, 0, market ? 0 : o.price, o.qty, o.side,
         static_cast<std::uint8_t>(o.type), 0);
    const Qty rem = match(o.side, o.owner, o.id, !market, o.price, o.qty, o.ts);
    if (rem > 0) {
      switch (o.type) {
        case OrderType::Limit:
        case OrderType::PostOnly:
          rest(o.id, o.side, o.owner, o.price, rem);
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
          break;
      }
    }
    return o.qty - rem;
  }

  template <class Map>
  Qty match_side(Map& book, Side side, OwnerId owner, OrderId id, bool limited, Price limit,
                 Qty qty, Timestamp ts) {
    const bool buy = side == Side::Bid;
    while (qty > 0 && !book.empty()) {
      auto lvl = book.begin();
      const Price px = lvl->first;
      if (limited && (buy ? px > limit : px < limit)) break;
      Queue& q = lvl->second;
      while (qty > 0 && !q.empty()) {
        Ord& m = q.front();
        const Qty fill = std::min(qty, m.qty);
        std::uint8_t flags = 0;
        if (owner != kFeedOwner && owner == m.owner) flags |= kTradeSelf;
        emit(EventType::Trade, ts, id, m.id, px, fill, side, 0, flags);
        m.qty -= fill;
        qty -= fill;
        if (m.qty == 0) {
          where_.erase(m.id);
          q.pop_front();
        }
      }
      if (q.empty()) book.erase(lvl);
    }
    return qty;
  }

  Qty match(Side side, OwnerId owner, OrderId id, bool limited, Price limit, Qty qty,
            Timestamp ts) {
    return side == Side::Bid ? match_side(asks_, side, owner, id, limited, limit, qty, ts)
                             : match_side(bids_, side, owner, id, limited, limit, qty, ts);
  }

  void rest(OrderId id, Side side, OwnerId owner, Price price, Qty qty) {
    Queue& q = side == Side::Bid ? bids_[price] : asks_[price];
    q.push_back(Ord{id, qty, side, owner});
    where_.emplace(id, Loc{std::prev(q.end()), price});
  }

  void remove(Where::iterator it) {
    const Price price = it->second.price;
    const Side side = it->second.it->side;
    if (side == Side::Bid) {
      auto lvl = bids_.find(price);
      lvl->second.erase(it->second.it);
      if (lvl->second.empty()) bids_.erase(lvl);
    } else {
      auto lvl = asks_.find(price);
      lvl->second.erase(it->second.it);
      if (lvl->second.empty()) asks_.erase(lvl);
    }
    where_.erase(it);
  }

  BookConfig cfg_;
  Sink sink_;
  Bids bids_;
  Asks asks_;
  Where where_;
  Seq seq_ = 0;
  std::uint64_t rejects_ = 0;
};

}  // namespace lobkit::bench
