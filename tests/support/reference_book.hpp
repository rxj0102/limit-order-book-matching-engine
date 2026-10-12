#pragma once
// A deliberately slow, obviously correct order book used as the oracle for
// differential testing. It shares no code with the engine: sorted std::map of
// price to std::deque, linear searches for removal, native prices instead of
// ladder indices, live order count taken from an unordered_map. It implements
// the semantics of docs/MATCHING_RULES.md directly from the text.
//
// Performance is irrelevant here. Where a choice exists, it prefers the
// simplest formulation, even at O(n).

#include <algorithm>
#include <deque>
#include <map>
#include <unordered_map>
#include <vector>

#include "lobkit/book/config.hpp"
#include "lobkit/book/types.hpp"

namespace lobkit::testing {

class ReferenceBook {
 public:
  explicit ReferenceBook(const BookConfig& cfg) : cfg_(cfg) { cfg_.validate(); }

  std::vector<Event> events;
  BookStats stats;

  // ---- engine operations ----

  void submit(const NewOrder& o) { (void)submit_impl(o); }

  void cancel(OrderId id, Timestamp ts) {
    auto it = where_.find(id);
    if (it == where_.end()) return reject(ts, id, RejectReason::UnknownOrderId, Side::Bid, 0, 0);
    const Loc loc = it->second;
    const Resting& r = *find_in_level(loc, id);
    emit(EventType::Cancel, ts, id, 0, loc.price, r.qty, loc.side,
         static_cast<std::uint8_t>(CancelReason::UserRequest), 0);
    remove(id);
  }

  void modify(const ModifyOrder& m) {
    auto it = where_.find(m.id);
    if (it == where_.end())
      return reject(m.ts, m.id, RejectReason::UnknownOrderId, Side::Bid, m.new_price, m.new_qty);
    const Loc loc = it->second;
    Resting& cur = *find_in_level(loc, m.id);
    if (m.new_qty == 0)
      return reject(m.ts, m.id, RejectReason::InvalidQty, loc.side, m.new_price, 0);
    if (RejectReason why = check_price(m.new_price); why != RejectReason::None)
      return reject(m.ts, m.id, why, loc.side, m.new_price, m.new_qty);

    if (m.new_price == loc.price && m.new_qty <= cur.qty) {
      cur.qty = m.new_qty;
      emit(EventType::Modify, m.ts, m.id, 0, m.new_price, m.new_qty, loc.side, 0, 0);
      return;
    }
    const Side side = loc.side;
    const OwnerId owner = cur.owner;
    remove(m.id);
    emit(EventType::Modify, m.ts, m.id, 0, m.new_price, m.new_qty, side, 0, 0);
    const Qty rem = match(side, owner, m.id, m.new_price, true, m.new_qty, m.ts);
    if (rem > 0) rest(m.id, side, owner, m.new_price, rem);
  }

  // ---- feed operations ----

  void feed_add(OrderId id, Side side, Price price, Qty qty, Timestamp ts) {
    const NewOrder o{id, price, qty, side, OrderType::Limit, kFeedOwner, ts};
    stats.feed_add_displaced_qty += submit_impl(o);
  }

  void feed_reduce(OrderId id, Qty qty, Timestamp ts) {
    const Loc* loc = feed_target(id, qty, ts);
    if (!loc) return;
    Resting& r = *find_in_level(*loc, id);
    const Qty removed = std::min(qty, r.qty);
    emit(EventType::Cancel, ts, id, 0, loc->price, removed, loc->side,
         static_cast<std::uint8_t>(CancelReason::FeedReduce), 0);
    if (qty >= r.qty)
      remove(id);
    else
      r.qty -= qty;
  }

  void feed_delete(OrderId id, Timestamp ts) {
    const Loc* loc = feed_target(id, 1, ts);
    if (!loc) return;
    emit(EventType::Cancel, ts, id, 0, loc->price, find_in_level(*loc, id)->qty, loc->side,
         static_cast<std::uint8_t>(CancelReason::FeedDelete), 0);
    remove(id);
  }

  void feed_execute(OrderId id, Qty qty, Timestamp ts) {
    const Loc* locp = feed_target(id, qty, ts);
    if (!locp) return;
    const Loc loc = *locp;
    auto& q = level(loc.side).at(loc.price);
    std::size_t ypos = 0;
    while (q[ypos].id != id) ++ypos;
    if (qty > q[ypos].qty) {
      qty = q[ypos].qty;
      ++stats.feed_exec_overflow;
    }
    const Side aggressor = opposite(loc.side);
    Qty q_left = qty;
    bool feed_ahead = false;
    std::vector<OrderId> filled_out;
    for (std::size_t i = 0; i < ypos; ++i) {
      Resting& r = q[i];
      if (r.owner == kFeedOwner) {
        feed_ahead = true;
      } else if (!feed_ahead && q_left > 0) {
        const Qty fill = std::min(r.qty, q_left);
        emit(EventType::Trade, ts, 0, r.id, loc.price, fill, aggressor, 0, kTradeExternalTaker);
        ++stats.trades;
        r.qty -= fill;
        q_left -= fill;
        if (r.qty == 0) filled_out.push_back(r.id);
      }
    }
    if (feed_ahead) ++stats.queue_order_anomalies;
    Resting& y = *find_in_level(loc, id);
    emit(EventType::Trade, ts, 0, id, loc.price, qty, aggressor, 0, kTradeExternalTaker);
    ++stats.trades;
    y.qty -= qty;
    const bool y_gone = y.qty == 0;
    for (OrderId f : filled_out) remove(f);
    if (y_gone) remove(id);
  }

  // ---- read side, for state comparison ----

  Qty depth_at(Price p) const {
    Qty sum = 0;
    for (const auto* m : {&bids_, &asks_}) {
      auto it = m->find(p);
      if (it != m->end())
        for (const Resting& r : it->second) sum += r.qty;
    }
    return sum;
  }
  std::vector<OrderId> queue_ids(Price p) const {
    std::vector<OrderId> out;
    for (const auto* m : {&bids_, &asks_}) {
      auto it = m->find(p);
      if (it != m->end())
        for (const Resting& r : it->second) out.push_back(r.id);
    }
    return out;
  }
  bool has_bid() const { return !bids_.empty(); }
  bool has_ask() const { return !asks_.empty(); }
  Price best_bid() const { return bids_.rbegin()->first; }
  Price best_ask() const { return asks_.begin()->first; }
  std::size_t live() const { return where_.size(); }

 private:
  struct Resting {
    OrderId id;
    Qty qty;
    OwnerId owner;
  };
  struct Loc {
    Side side;
    Price price;
  };
  using Ladder = std::map<Price, std::deque<Resting>>;

  Ladder& level(Side s) { return s == Side::Bid ? bids_ : asks_; }

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
    events.push_back(e);
  }

  void reject(Timestamp ts, OrderId id, RejectReason r, Side side, Price price, Qty qty) {
    ++stats.rejects[static_cast<std::size_t>(r)];
    emit(EventType::Reject, ts, id, 0, price, qty, side, static_cast<std::uint8_t>(r), 0);
  }

  RejectReason check_price(Price p) const {
    const Price end = cfg_.base_price + static_cast<Price>(cfg_.num_levels) * cfg_.tick_size;
    if (p < cfg_.base_price || p >= end) return RejectReason::PriceOutOfBand;
    if ((p - cfg_.base_price) % cfg_.tick_size != 0) return RejectReason::PriceOffTick;
    return RejectReason::None;
  }

  Resting* find_in_level(const Loc& loc, OrderId id) {
    auto& dq = level(loc.side).at(loc.price);
    for (Resting& r : dq)
      if (r.id == id) return &r;
    return nullptr;
  }

  const Loc* feed_target(OrderId id, Qty qty, Timestamp ts) {
    if (qty == 0) {
      reject(ts, id, RejectReason::InvalidQty, Side::Bid, 0, 0);
      return nullptr;
    }
    auto it = where_.find(id);
    if (it == where_.end()) {
      reject(ts, id, RejectReason::UnknownOrderId, Side::Bid, 0, qty);
      return nullptr;
    }
    if (find_in_level(it->second, id)->owner != kFeedOwner) {
      reject(ts, id, RejectReason::WrongOwner, it->second.side, it->second.price, qty);
      return nullptr;
    }
    return &it->second;
  }

  void rest(OrderId id, Side side, OwnerId owner, Price price, Qty qty) {
    level(side)[price].push_back({id, qty, owner});
    where_[id] = {side, price};
  }

  void remove(OrderId id) {
    const Loc loc = where_.at(id);
    auto& dq = level(loc.side).at(loc.price);
    dq.erase(std::find_if(dq.begin(), dq.end(), [&](const Resting& r) { return r.id == id; }));
    if (dq.empty()) level(loc.side).erase(loc.price);
    where_.erase(id);
  }

  bool crosses(Side side, Price price) const {
    if (side == Side::Bid) return !asks_.empty() && asks_.begin()->first <= price;
    return !bids_.empty() && bids_.rbegin()->first >= price;
  }

  std::uint64_t available(Side side, Price price) const {
    std::uint64_t sum = 0;
    if (side == Side::Bid) {
      for (const auto& [p, dq] : asks_) {
        if (p > price) break;
        for (const Resting& r : dq) sum += r.qty;
      }
    } else {
      for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) {
        if (it->first < price) break;
        for (const Resting& r : it->second) sum += r.qty;
      }
    }
    return sum;
  }

  // Walks the opposite side best price first, front of queue first.
  Qty match(Side side, OwnerId owner, OrderId id, Price limit, bool has_limit, Qty qty,
            Timestamp ts) {
    while (qty > 0) {
      Ladder& opp = level(opposite(side));
      if (opp.empty()) break;
      auto lv = side == Side::Bid ? opp.begin() : std::prev(opp.end());
      const Price px = lv->first;
      if (has_limit && (side == Side::Bid ? px > limit : px < limit)) break;
      auto& dq = lv->second;
      while (qty > 0 && !dq.empty()) {
        Resting& m = dq.front();
        const Qty fill = std::min(qty, m.qty);
        std::uint8_t flags = 0;
        if (owner != kFeedOwner && owner == m.owner) flags |= kTradeSelf;
        if (owner == kFeedOwner) {
          if (m.owner == kFeedOwner)
            ++stats.feed_crossed_fills;
          else
            ++stats.feed_crossed_own_fills;
        }
        emit(EventType::Trade, ts, id, m.id, px, fill, side, 0, flags);
        ++stats.trades;
        m.qty -= fill;
        qty -= fill;
        if (m.qty == 0) remove(m.id);  // may erase the level, so re-fetch below
        if (opp.find(px) == opp.end()) break;
      }
    }
    return qty;
  }

  Qty submit_impl(const NewOrder& o) {
    if (o.qty == 0) {
      reject(o.ts, o.id, RejectReason::InvalidQty, o.side, o.price, 0);
      return 0;
    }
    const bool is_market = o.type == OrderType::Market;
    if (!is_market) {
      if (RejectReason why = check_price(o.price); why != RejectReason::None) {
        reject(o.ts, o.id, why, o.side, o.price, o.qty);
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
    if (o.type == OrderType::PostOnly && crosses(o.side, o.price)) {
      reject(o.ts, o.id, RejectReason::PostOnlyWouldCross, o.side, o.price, o.qty);
      return 0;
    }
    if (o.type == OrderType::Fok && available(o.side, o.price) < o.qty) {
      reject(o.ts, o.id, RejectReason::FokNotFillable, o.side, o.price, o.qty);
      return 0;
    }
    emit(EventType::Ack, o.ts, o.id, 0, is_market ? 0 : o.price, o.qty, o.side,
         static_cast<std::uint8_t>(o.type), 0);
    const Qty rem = match(o.side, o.owner, o.id, o.price, !is_market, o.qty, o.ts);
    if (rem > 0) {
      if (may_rest) {
        rest(o.id, o.side, o.owner, o.price, rem);
      } else if (o.type == OrderType::Ioc) {
        emit(EventType::Cancel, o.ts, o.id, 0, o.price, rem, o.side,
             static_cast<std::uint8_t>(CancelReason::IocRemainder), 0);
      } else if (is_market) {
        emit(EventType::Cancel, o.ts, o.id, 0, 0, rem, o.side,
             static_cast<std::uint8_t>(CancelReason::MarketRemainder), 0);
      }
    }
    return o.qty - rem;
  }

  BookConfig cfg_;
  Ladder bids_, asks_;
  std::unordered_map<OrderId, Loc> where_;
  Seq seq_ = 0;
};

}  // namespace lobkit::testing
