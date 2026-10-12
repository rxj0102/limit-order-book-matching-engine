#pragma once
// Event-driven backtester. A replayed feed and one strategy share a single book.
//
// WHAT IS SIMULATED. Feed messages are applied to the same Engine the strategy
// trades in, at their recorded timestamps, through the ReplayAdapter. The
// strategy submits PostOnly orders through the Engine, so they rest at the back of
// the queue at their price and fill only under the own-order rules of
// MATCHING_RULES.md section 9. Three one-way delays are explicit:
//
//   submit latency   strategy -> exchange. An order or cancel decided at time t
//                    reaches the book at t + submit_latency.
//   ack latency      exchange -> strategy. Acks, rejects, fills and cancel results
//                    reach the strategy at (exchange time) + ack_latency.
//   market data      exchange -> strategy. The strategy sees the book as of time
//   latency          t at t + md_latency (a snapshot taken at t, not a live view).
//
// WHAT IS NOT. The feed does not react to the strategy: there is no market impact,
// other participants do not change behavior when a quote appears, and the feed
// executions that fill a strategy order are the historical ones. The fill model is
// an approximation, see docs/SIMULATOR.md for its biases.
//
// EVENT ORDER. One queue of simulator events ordered by (time, sequence), plus the
// feed stream. On equal times queued events run before the next feed message, so
// with zero latency the strategy reacts to every feed message before the next one
// is applied. Everything is deterministic given the feed and the strategy.
//
// Money is integer (types.hpp). The strategy may use floating point internally.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lobkit/book/engine.hpp"
#include "lobkit/md/replay.hpp"
#include "lobkit/sim/types.hpp"

namespace lobkit::sim {

// Strategy callbacks (all take a gateway to act through):
//   on_market(const MarketView&, G&)            market data snapshot arrived
//   on_ack(OrderId, G&)                         order accepted by the book
//   on_reject(OrderId, RejectReason, G&)        order refused (post-only would cross, ...)
//   on_fill(const FillNotice&, G&)              order traded
//   on_cancel_ack(OrderId, Qty cancelled, G&)   cancel done
//   on_cancel_reject(OrderId, G&)               cancel arrived after the order was gone
template <class Strategy, class Reader>
class Simulator {
 public:
  class Gateway {
   public:
    // Decide to place a passive order. Returns its id. It reaches the book
    // submit_latency_ns from now.
    OrderId send(Side side, Price price, Qty qty) { return sim_->gw_send(side, price, qty); }
    void cancel(OrderId id) { sim_->gw_cancel(id); }
    Timestamp now() const { return sim_->now_; }

   private:
    friend class Simulator;
    explicit Gateway(Simulator* s) : sim_(s) {}
    Simulator* sim_;
  };

  Simulator(const BookConfig& book, const SimConfig& cfg, Strategy& strategy, Reader& feed)
      : cfg_(cfg), engine_(book), strat_(strategy), feed_(feed), gw_(this) {
    heap_.reserve(1024);
    engine_.sink().events.reserve(256);
  }

  RunResult run() {
    md::FeedMsg pending{};
    bool have = pull_feed(pending);
    have_any_ = have;
    if (have) {
      first_ts_ = pending.ts;
      next_sample_ = pending.ts + cfg_.sample_interval_ns;
    }
    for (;;) {
      if (!heap_.empty() && (!have || heap_.front().time <= pending.ts)) {
        std::pop_heap(heap_.begin(), heap_.end(), later);
        const Ev e = heap_.back();
        heap_.pop_back();
        advance(e.time);
        dispatch(e);
      } else if (have) {
        if (pending.ts < now_) ++stats_.time_regressions;  // feed went backwards: apply at now_
        advance(std::max(now_, pending.ts));
        process_feed(pending);
        have = pull_feed(pending);
      } else {
        break;
      }
    }
    if (first_ts_ != 0 || have_any_) record_sample(now_);  // final state
    RunResult r;
    r.stats = stats_;
    r.pnl = pnl_;
    r.pnl.inventory = inventory_;
    r.pnl.mid2_final = mid2_;
    r.fills = std::move(fills_);
    r.path = std::move(path_);
    r.book = engine_.book().stats();
    r.first_ts = first_ts_;
    r.last_ts = now_;
    r.feed_status = feed_status_;
    return r;
  }

  const Engine<VectorSink>& engine() const { return engine_; }
  const md::ReplayCounters& counters() const { return counters_; }
  std::int64_t inventory() const { return inventory_; }

 private:
  enum class Kind : std::uint8_t {
    Send,
    Cancel,
    Market,
    NotifyAck,
    NotifyReject,
    NotifyFill,
    NotifyCancelAck,
    NotifyCancelReject
  };
  struct Ev {
    Timestamp time;
    std::uint64_t seq;
    Kind kind;
    Side side;
    std::uint8_t flag;  // Fill: maker. Reject: RejectReason. Market: two_sided
    Qty qty;
    Qty qty2;
    Price price;
    Price price2;
    OrderId id;
    Timestamp ts2;
  };
  static bool later(const Ev& a, const Ev& b) {
    return a.time != b.time ? a.time > b.time : a.seq > b.seq;  // min-heap on (time, seq)
  }

  void push(Ev e) {
    e.seq = ++seq_;
    heap_.push_back(e);
    std::push_heap(heap_.begin(), heap_.end(), later);
  }

  // ---- gateway ------------------------------------------------------------

  OrderId gw_send(Side side, Price price, Qty qty) {
    const OrderId id = kOwnIdBase + (++own_counter_);
    Ev e{};
    e.time = now_ + cfg_.submit_latency_ns;
    e.kind = Kind::Send;
    e.side = side;
    e.price = price;
    e.qty = qty;
    e.id = id;
    push(e);
    ++stats_.orders_sent;
    return id;
  }
  void gw_cancel(OrderId id) {
    Ev e{};
    e.time = now_ + cfg_.submit_latency_ns;
    e.kind = Kind::Cancel;
    e.id = id;
    push(e);
    ++stats_.cancels_sent;
  }

  // ---- time and sampling --------------------------------------------------

  void advance(Timestamp t) {
    if (t <= now_) return;
    while (next_sample_ < t) {
      record_sample(next_sample_);
      next_sample_ += cfg_.sample_interval_ns;
    }
    now_ = t;
  }

  void record_sample(Timestamp ts) {
    path_.push_back({ts, inventory_, inventory_ - known_inventory_, pnl_.spread2, pnl_.inventory2,
                     pnl_.fees2, mid_valid_ ? mid2_ : 0});
  }

  // ---- feed ---------------------------------------------------------------

  bool pull_feed(md::FeedMsg& m) {
    for (;;) {
      const md::ParseStatus st = feed_.next(m);
      if (st != md::ParseStatus::Ok) {
        feed_status_ = st;
        return false;
      }
      if ((m.id >> 62) != 0 || (m.type == md::FeedType::Replace && (m.new_id >> 62) != 0)) {
        ++stats_.feed_id_collisions;  // would collide with the own id range
        continue;
      }
      ++stats_.feed_messages;
      return true;
    }
  }

  void process_feed(const md::FeedMsg& m) {
    now_ = std::max(now_, m.ts);
    md::apply_feed(engine_, counters_, m);
    post_op(/*cancel_op=*/false);
    emit_market_update();
  }

  // ---- engine events and accounting ----------------------------------------

  void post_op(bool cancel_op) {
    auto& evs = engine_.sink().events;
    for (const Event& e : evs) {
      switch (e.type) {
        case EventType::Trade:
          on_trade(e);
          break;
        case EventType::Ack:
          if (is_own_id(e.id)) {
            own_live_.push_back(e.id);
            ++stats_.orders_accepted;
            notify(Kind::NotifyAck, e.id);
          }
          break;
        case EventType::Reject:
          if (is_own_id(e.id)) {
            Ev n{};
            n.id = e.id;
            n.flag = e.reason;
            if (cancel_op && static_cast<RejectReason>(e.reason) == RejectReason::UnknownOrderId) {
              ++stats_.cancels_too_late;
              n.kind = Kind::NotifyCancelReject;
            } else {
              ++stats_.orders_rejected;
              n.kind = Kind::NotifyReject;
            }
            n.time = now_ + cfg_.ack_latency_ns;
            push(n);
          }
          break;
        case EventType::Cancel:
          if (is_own_id(e.id)) {
            erase_live(e.id);
            ++stats_.cancels_confirmed;
            Ev n{};
            n.kind = Kind::NotifyCancelAck;
            n.id = e.id;
            n.qty = e.qty;
            n.time = now_ + cfg_.ack_latency_ns;
            push(n);
          }
          break;
        default:
          break;
      }
    }
    evs.clear();
    update_mid();
  }

  void notify(Kind k, OrderId id) {
    Ev n{};
    n.kind = k;
    n.id = id;
    n.time = now_ + cfg_.ack_latency_ns;
    push(n);
  }

  void erase_live(OrderId id) {
    own_live_.erase(std::remove(own_live_.begin(), own_live_.end(), id), own_live_.end());
  }

  void on_trade(const Event& e) {
    const bool maker_own = is_own_id(e.other);
    const bool taker_own = is_own_id(e.id);
    if (!maker_own && !taker_own) return;
    if (maker_own && taker_own) {  // cannot happen with PostOnly quotes; counted if it ever does
      ++stats_.self_trades;
      return;
    }
    const OrderId id = maker_own ? e.other : e.id;
    const Side side = maker_own ? opposite(e.side) : e.side;  // e.side is the aggressor side
    const std::int64_t sgn = side == Side::Bid ? 1 : -1;
    const std::int64_t q = e.qty;
    const std::int64_t p2 = 2 * e.price;

    // Spread capture is measured against the market mid just before this operation
    // changed anything. If no two sided market was ever seen, the fill price is the
    // reference and the capture is zero; the mark then starts from there.
    if (!mid_valid_) {
      mid2_ = p2;
      mid_valid_ = true;
    }
    const std::int64_t ref = mid2_;
    pnl_.spread2 += static_cast<Money2>(sgn * q) * (ref - p2);
    pnl_.cash2 += static_cast<Money2>(-sgn * q) * p2;
    const std::int64_t fee_units = q * (maker_own ? cfg_.maker_fee : cfg_.taker_fee);
    pnl_.fees2 += static_cast<Money2>(2) * fee_units;
    inventory_ += sgn * q;
    stats_.max_abs_inventory =
        std::max<std::int64_t>(stats_.max_abs_inventory, std::abs(inventory_));
    ++stats_.fills;
    stats_.shares_filled += e.qty;
    (maker_own ? stats_.maker_fills : stats_.taker_fills)++;
    if (cfg_.record_fills)
      fills_.push_back({e.ts, id, side, e.price, e.qty, maker_own, fee_units, ref, inventory_});

    record_sample(now_);  // path point at every fill

    Ev n{};
    n.kind = Kind::NotifyFill;
    n.time = now_ + cfg_.ack_latency_ns;
    n.id = id;
    n.side = side;
    n.price = e.price;
    n.qty = e.qty;
    n.flag = maker_own ? 1 : 0;
    n.ts2 = e.ts;
    push(n);
  }

  // The book as everyone else shows it: top levels with the strategy own displayed
  // orders subtracted. Own orders are found by id, so this stays exact however the
  // strategy behaves.
  MarketView compute_view() {
    const auto& book = engine_.book();
    Book<VectorSink>::LevelView a[4], b[4];
    std::size_t na = book.top_levels(Side::Ask, a, 4), nb = book.top_levels(Side::Bid, b, 4);
    for (std::size_t i = 0; i < own_live_.size();) {
      const Order* o = book.find_order(own_live_[i]);
      if (!o) {  // fully filled since it was added
        own_live_.erase(own_live_.begin() + static_cast<std::ptrdiff_t>(i));
        continue;
      }
      const Price px = book.price_of(o->level);
      auto* lv = o->side == Side::Ask ? a : b;
      const std::size_t n = o->side == Side::Ask ? na : nb;
      for (std::size_t j = 0; j < n; ++j)
        if (lv[j].price == px) lv[j].qty -= std::min<std::uint64_t>(lv[j].qty, o->qty);
      ++i;
    }
    MarketView v;
    v.ts = now_;
    auto first_nonempty = [](const Book<VectorSink>::LevelView* lv, std::size_t n, Price& px,
                             Qty& q) {
      for (std::size_t j = 0; j < n; ++j)
        if (lv[j].qty > 0) {
          px = lv[j].price;
          q = static_cast<Qty>(std::min<std::uint64_t>(lv[j].qty, UINT32_MAX));
          return true;
        }
      return false;
    };
    const bool has_a = first_nonempty(a, na, v.ask, v.ask_qty);
    const bool has_b = first_nonempty(b, nb, v.bid, v.bid_qty);
    v.two_sided = has_a && has_b;
    return v;
  }

  void update_mid() {
    const MarketView v = compute_view();
    if (!v.two_sided) return;  // keep the last mid until both sides are back
    const std::int64_t m2 = v.bid + v.ask;
    if (mid_valid_) {
      pnl_.inventory2 += static_cast<Money2>(inventory_) * (m2 - mid2_);
    }
    mid2_ = m2;
    mid_valid_ = true;
  }

  void emit_market_update() {
    const MarketView v = compute_view();
    if (have_last_view_ && v.two_sided == last_view_.two_sided && v.bid == last_view_.bid &&
        v.ask == last_view_.ask && v.bid_qty == last_view_.bid_qty &&
        v.ask_qty == last_view_.ask_qty)
      return;
    last_view_ = v;
    have_last_view_ = true;
    Ev e{};
    e.kind = Kind::Market;
    e.time = now_ + cfg_.md_latency_ns;
    e.price = v.bid;
    e.price2 = v.ask;
    e.qty = v.bid_qty;
    e.qty2 = v.ask_qty;
    e.flag = v.two_sided ? 1 : 0;
    e.ts2 = now_;
    push(e);
  }

  // ---- dispatch ------------------------------------------------------------

  void dispatch(const Ev& e) {
    switch (e.kind) {
      case Kind::Send:
        engine_.submit({e.id, e.price, e.qty, e.side, OrderType::PostOnly, kStrategyOwner, now_});
        post_op(false);
        break;
      case Kind::Cancel:
        engine_.cancel(e.id, now_);
        post_op(true);
        break;
      case Kind::Market: {
        MarketView v;
        v.ts = e.ts2;
        v.bid = e.price;
        v.ask = e.price2;
        v.bid_qty = e.qty;
        v.ask_qty = e.qty2;
        v.two_sided = e.flag != 0;
        ++stats_.market_updates;
        strat_.on_market(v, gw_);
        break;
      }
      case Kind::NotifyAck:
        strat_.on_ack(e.id, gw_);
        break;
      case Kind::NotifyReject:
        strat_.on_reject(e.id, static_cast<RejectReason>(e.flag), gw_);
        break;
      case Kind::NotifyFill: {
        known_inventory_ += (e.side == Side::Bid ? 1 : -1) * static_cast<std::int64_t>(e.qty);
        strat_.on_fill(FillNotice{e.id, e.side, e.price, e.qty, e.flag != 0, e.ts2}, gw_);
        break;
      }
      case Kind::NotifyCancelAck:
        strat_.on_cancel_ack(e.id, e.qty, gw_);
        break;
      case Kind::NotifyCancelReject:
        strat_.on_cancel_reject(e.id, gw_);
        break;
    }
  }

  SimConfig cfg_;
  Engine<VectorSink> engine_;
  Strategy& strat_;
  Reader& feed_;
  Gateway gw_;
  md::ReplayCounters counters_{};
  SimStats stats_{};
  Pnl pnl_{};
  std::vector<Ev> heap_;
  std::vector<FillRecord> fills_;
  std::vector<PathSample> path_;
  std::vector<OrderId> own_live_;
  MarketView last_view_{};
  bool have_last_view_ = false;
  std::uint64_t seq_ = 0;
  std::uint64_t own_counter_ = 0;
  Timestamp now_ = 0, first_ts_ = 0, next_sample_ = 0;
  std::int64_t inventory_ = 0, known_inventory_ = 0;
  std::int64_t mid2_ = 0;
  bool mid_valid_ = false;
  bool have_any_ = false;
  md::ParseStatus feed_status_ = md::ParseStatus::End;
};

}  // namespace lobkit::sim
