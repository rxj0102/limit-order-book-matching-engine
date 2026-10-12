#pragma once
// Avellaneda-Stoikov market maker (Avellaneda and Stoikov, 2008, High-frequency
// trading in a limit order book).
//
// FLOATING POINT. This file is the one place in the project where prices and
// quantities are converted to double: the model is defined on real prices and a
// logarithm, and the engine and the money accounting stay integer. The doubles
// are used for one decision (what price and size to quote), the result is rounded
// to the integer tick grid immediately, and nothing downstream sees a double.
// Results are bit-for-bit reproducible with the same binary on the same machine;
// across compilers or libm versions a quote that lands within rounding error of a
// tick boundary could move by one tick.
//
// MODEL (prices in dollars, time in seconds):
//   reservation price  r      = s - q * gamma * sigma^2 * (T - t)
//   total optimal spread delta = gamma * sigma^2 * (T - t) + (2 / gamma) * ln(1 + gamma / k)
//   bid = r - delta / 2, ask = r + delta / 2
// s is the market mid (own orders excluded), q the inventory in shares, gamma the
// risk aversion (per dollar), k the order book depth parameter (per dollar: the
// arrival intensity of market orders falls as exp(-k * distance from the mid)),
// sigma the volatility in dollars per square root second. The default horizon is
// the constant (T - t) = horizon_s, the usual choice for a strategy that runs all
// day; a shrinking horizon to a session end is available.
//
// WHAT THE STRATEGY DOES AROUND THE FORMULA:
//   - sigma comes from an EWMA of squared mid changes sampled on a fixed interval.
//   - Quotes are rounded to the tick grid: bids down, asks up.
//   - Passive clipping: the bid is kept below the best ask the strategy last saw, the
//     ask above the best bid. Orders are sent post-only anyway, so a stale view makes
//     the book reject the order instead of trading.
//   - Self-crossing guard: the own ask is always at least one tick above the own bid.
//   - Inventory limit: the quantity on a side is capped so that the known inventory
//     plus every share still working on that side can never exceed max_inventory in
//     absolute value. "Working" includes orders in flight and fills the strategy has
//     not been told about yet, so the bound holds under latency.
//   - size_skew shrinks the size on the side that would add to a position.
//   - Requoting is throttled and only happens when the desired price moved by at
//     least requote_threshold_ticks, so queue position is not thrown away for nothing.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "lobkit/book/types.hpp"
#include "lobkit/sim/types.hpp"

namespace lobkit::sim {

struct ASParams {
  double gamma = 0.05;   // risk aversion, per dollar
  double k = 50.0;       // depth parameter, per dollar (50: intensity falls by e per 2 cents)
  double sigma0 = 0.02;  // starting volatility, dollars per sqrt(second)
  double sigma_floor = 1e-4;
  double horizon_s = 10.0;  // (T - t) when not finite horizon
  bool finite_horizon = false;
  double session_end_s = 0;  // seconds after midnight, used when finite_horizon
  double min_horizon_s = 1.0;
  Qty order_size = 100;
  std::int64_t max_inventory = 500;
  double size_skew =
      0.0;  // 0..1: fraction of the size removed at full inventory on the adding side
  Price tick = 100;
  Price min_price = 1;
  Price max_price = 1'000'000'000;
  Timestamp requote_interval_ns = 100'000'000;
  int requote_threshold_ticks = 1;
  Timestamp sigma_interval_ns = 1'000'000'000;
  double sigma_alpha = 0.1;
};

struct ASQuote {
  double reservation;  // dollars
  double spread;       // dollars, total
  double bid, ask;     // dollars
};

// The formulas alone, so they can be tested against hand computed values.
inline ASQuote as_quote(double mid, double inventory, double sigma2, double tau, double gamma,
                        double k) {
  const double r = mid - inventory * gamma * sigma2 * tau;
  const double spread = gamma * sigma2 * tau + (2.0 / gamma) * std::log1p(gamma / k);
  return {r, spread, r - spread / 2.0, r + spread / 2.0};
}

struct PlannedQuote {
  bool active = false;
  Price price = 0;
  Qty qty = 0;
};
struct PlannedQuotes {
  PlannedQuote bid, ask;
};

// Turns the model output and the state into integer quotes. Pure function.
//   known_inv        inventory as the strategy has been told
//   bid_working      shares still working on the bid side (including the live bid itself)
//   live_bid_qty     remaining quantity of the live bid (0 if none), excluded from the room
inline PlannedQuotes plan_quotes(const MarketView& v, double mid_dollars, double sigma2, double tau,
                                 std::int64_t known_inv, std::int64_t bid_working,
                                 std::int64_t live_bid_qty, std::int64_t ask_working,
                                 std::int64_t live_ask_qty, const ASParams& p) {
  PlannedQuotes out;
  if (!v.two_sided) return out;
  const ASQuote q =
      as_quote(mid_dollars, static_cast<double>(known_inv), sigma2, tau, p.gamma, p.k);
  if (!std::isfinite(q.bid) || !std::isfinite(q.ask)) return out;

  const double tick = static_cast<double>(p.tick);
  auto to_native = [](double dollars) { return dollars * kUnitsPerDollar; };
  Price bid = static_cast<Price>(std::floor(to_native(q.bid) / tick)) * p.tick;
  Price ask = static_cast<Price>(std::ceil(to_native(q.ask) / tick)) * p.tick;

  // Passive clipping against the book as last seen.
  bid = std::min(bid, v.ask - p.tick);
  ask = std::max(ask, v.bid + p.tick);
  // Self-crossing guard: our own quotes never touch.
  if (ask <= bid) ask = bid + p.tick;
  // The band edges are rounded inward so quotes stay on the tick grid.
  const Price lo = (p.min_price + p.tick - 1) / p.tick * p.tick;
  const Price hi = p.max_price / p.tick * p.tick;
  bid = std::max(bid, lo);
  ask = std::min(ask, hi);
  if (ask <= bid) return out;

  // Size and inventory room. Room counts everything that could still become
  // inventory, but not the live order itself, since a replacement would cancel it.
  const std::int64_t room_bid = p.max_inventory - (known_inv + (bid_working - live_bid_qty));
  const std::int64_t room_ask = p.max_inventory + known_inv - (ask_working - live_ask_qty);
  const double frac = p.max_inventory > 0
                          ? static_cast<double>(known_inv) / static_cast<double>(p.max_inventory)
                          : 0.0;
  auto sized = [&](double adding_fraction, std::int64_t room) -> Qty {
    const double scale = 1.0 - p.size_skew * std::clamp(adding_fraction, 0.0, 1.0);
    std::int64_t n =
        static_cast<std::int64_t>(std::floor(static_cast<double>(p.order_size) * scale));
    n = std::min(n, room);
    return n > 0 ? static_cast<Qty>(n) : 0;
  };
  out.bid = {true, bid, sized(frac, room_bid)};
  out.ask = {true, ask, sized(-frac, room_ask)};
  if (out.bid.qty == 0) out.bid.active = false;
  if (out.ask.qty == 0) out.ask.active = false;
  return out;
}

class AvellanedaStoikov {
 public:
  explicit AvellanedaStoikov(const ASParams& p) : p_(p), sigma2_(p.sigma0 * p.sigma0) {}

  // ---- simulator callbacks -------------------------------------------------

  template <class G>
  void on_market(const MarketView& v, G& gw) {
    view_ = v;
    have_view_ = true;
    if (!v.two_sided) return;
    update_sigma(v);
    if (gw.now() < next_decision_) return;
    next_decision_ = gw.now() + p_.requote_interval_ns;
    replan(gw);
  }

  template <class G>
  void on_ack(OrderId id, G&) {
    for (Leg* l : {&bid_, &ask_})
      if (l->id == id && l->state == State::Sending) l->state = State::Live;
  }

  template <class G>
  void on_reject(OrderId id, RejectReason, G&) {
    for (Leg* l : {&bid_, &ask_})
      if (l->id == id) {
        l->state = State::Idle;
        l->remaining = 0;
        l->blocked = true;
        l->blocked_view_ts = l->sent_view_ts;  // do not resend on the same stale view
        ++rejects_;
      }
  }

  template <class G>
  void on_fill(const FillNotice& f, G& gw) {
    known_inv_ += (f.side == Side::Bid ? 1 : -1) * static_cast<std::int64_t>(f.qty);
    for (Leg* l : {&bid_, &ask_})
      if (l->id == f.id) {
        l->remaining -= std::min<Qty>(l->remaining, f.qty);
        if (l->remaining == 0) l->state = State::Idle;
      }
    if (have_view_ && view_.two_sided) replan(gw);  // inventory changed: limits may bind now
  }

  template <class G>
  void on_cancel_ack(OrderId id, Qty, G& gw) {
    for (Leg* l : {&bid_, &ask_})
      if (l->id == id) {
        l->state = State::Idle;
        l->remaining = 0;
      }
    if (have_view_ && view_.two_sided) replan(gw);
  }

  template <class G>
  void on_cancel_reject(OrderId id, G&) {
    // The order was already gone; its fill notice came first and set the leg idle.
    for (Leg* l : {&bid_, &ask_})
      if (l->id == id) {
        l->state = State::Idle;
        l->remaining = 0;
      }
  }

  // ---- inspection ------------------------------------------------------------

  std::int64_t known_inventory() const { return known_inv_; }
  double sigma() const { return std::sqrt(sigma2_); }
  std::uint64_t quotes_sent() const { return quotes_sent_; }
  std::uint64_t cancels_sent() const { return cancels_sent_; }
  std::uint64_t rejects() const { return rejects_; }
  const ASParams& params() const { return p_; }

 private:
  enum class State : std::uint8_t { Idle, Sending, Live, Cancelling };
  struct Leg {
    State state = State::Idle;
    OrderId id = 0;
    Price price = 0;
    Qty remaining = 0;
    Timestamp sent_view_ts = 0;
    Timestamp blocked_view_ts = 0;
    bool blocked = false;
  };

  void update_sigma(const MarketView& v) {
    const double mid = static_cast<double>(v.bid + v.ask) / (2.0 * kUnitsPerDollar);
    if (!sigma_started_) {
      sigma_started_ = true;
      last_mid_ = mid;
      last_sigma_ts_ = v.ts;
      return;
    }
    if (v.ts - last_sigma_ts_ < p_.sigma_interval_ns) return;
    const double dt = static_cast<double>(v.ts - last_sigma_ts_) * 1e-9;
    const double dm = mid - last_mid_;
    sigma2_ = (1.0 - p_.sigma_alpha) * sigma2_ + p_.sigma_alpha * (dm * dm / dt);
    sigma2_ = std::max(sigma2_, p_.sigma_floor * p_.sigma_floor);
    last_mid_ = mid;
    last_sigma_ts_ = v.ts;
  }

  double tau(Timestamp ts) const {
    if (!p_.finite_horizon) return p_.horizon_s;
    return std::max(p_.min_horizon_s, p_.session_end_s - static_cast<double>(ts) * 1e-9);
  }

  template <class G>
  void replan(G& gw) {
    const double mid = static_cast<double>(view_.bid + view_.ask) / (2.0 * kUnitsPerDollar);
    const std::int64_t bid_work = leg_working(bid_), ask_work = leg_working(ask_);
    const PlannedQuotes pq = plan_quotes(view_, mid, sigma2_, tau(view_.ts), known_inv_, bid_work,
                                         bid_.state == State::Idle ? 0 : bid_.remaining, ask_work,
                                         ask_.state == State::Idle ? 0 : ask_.remaining, p_);
    handle(bid_, Side::Bid, pq.bid, gw);
    handle(ask_, Side::Ask, pq.ask, gw);
  }

  static std::int64_t leg_working(const Leg& l) {
    return l.state == State::Idle ? 0 : static_cast<std::int64_t>(l.remaining);
  }

  template <class G>
  void handle(Leg& l, Side side, const PlannedQuote& want, G& gw) {
    switch (l.state) {
      case State::Idle:
        if (!want.active) return;
        if (l.blocked && view_.ts <= l.blocked_view_ts) return;  // same view that got us rejected
        l.blocked = false;
        l.id = gw.send(side, want.price, want.qty);
        l.price = want.price;
        l.remaining = want.qty;
        l.state = State::Sending;
        l.sent_view_ts = view_.ts;
        ++quotes_sent_;
        return;
      case State::Live: {
        const bool keep = want.active && l.remaining <= want.qty &&
                          std::llabs(l.price - want.price) < p_.requote_threshold_ticks * p_.tick;
        if (keep) return;
        gw.cancel(l.id);
        l.state = State::Cancelling;
        ++cancels_sent_;
        return;
      }
      case State::Sending:
      case State::Cancelling:
        return;  // wait for the exchange to answer
    }
  }

  ASParams p_;
  double sigma2_;
  bool sigma_started_ = false;
  double last_mid_ = 0;
  Timestamp last_sigma_ts_ = 0;
  Timestamp next_decision_ = 0;
  MarketView view_{};
  bool have_view_ = false;
  Leg bid_, ask_;
  std::int64_t known_inv_ = 0;
  std::uint64_t quotes_sent_ = 0, cancels_sent_ = 0, rejects_ = 0;
};

}  // namespace lobkit::sim
