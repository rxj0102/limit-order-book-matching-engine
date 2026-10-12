#pragma once
// Deterministic synthetic operation streams. The generator is independent of
// engine state (it never asks the book anything), so the same stream can later
// be fed to the engine and to the reference book. It uses its own PRNG because
// std::uniform_int_distribution is implementation defined and would make golden
// hashes differ between standard libraries.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "lobkit/book/engine.hpp"

namespace lobkit::testing {

struct SplitMix64 {
  std::uint64_t s;
  explicit SplitMix64(std::uint64_t seed) : s(seed) {}
  std::uint64_t next() {
    std::uint64_t z = (s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  std::uint64_t below(std::uint64_t n) { return next() % n; }
};

struct Op {
  enum Kind : std::uint8_t { Submit, Cancel, Modify, FeedAdd, FeedReduce, FeedDelete, FeedExecute };
  Kind kind;
  NewOrder order;   // Submit, FeedAdd
  ModifyOrder mod;  // Modify
  OrderId id;       // Cancel, Feed*
  Qty qty;          // FeedReduce, FeedExecute
  Timestamp ts;
};

struct WorkloadParams {
  std::uint64_t seed = 1;
  std::size_t n = 10000;
  Price base_price = 100000;
  Price tick_size = 100;
  std::uint32_t num_levels = 1024;
  bool feed_ops = false;
  bool bad_input = true;  // zero qty, off-tick, out-of-band, duplicate ids
};

inline std::vector<Op> make_workload(const WorkloadParams& p) {
  SplitMix64 rng(p.seed);
  std::vector<Op> ops;
  ops.reserve(p.n);
  const auto px = [&](std::int64_t tick) { return p.base_price + p.tick_size * tick; };
  std::int64_t mid = p.num_levels / 2;
  OrderId next_id = 1;

  auto pick_id = [&]() -> OrderId {
    if (next_id == 1) return 1;
    if (rng.below(10) < 9) {
      const std::uint64_t window = std::min<std::uint64_t>(next_id - 1, 2000);
      return next_id - 1 - rng.below(window);
    }
    return 1 + rng.below(next_id - 1);
  };
  auto pick_tick = [&](Side side) -> std::int64_t {
    // Geometric-ish distance from mid: mostly near the touch, fat tail.
    std::int64_t d = 0;
    while (rng.below(100) < 70 && d < 40) ++d;
    if (rng.below(50) == 0) d += static_cast<std::int64_t>(rng.below(100));
    std::int64_t t = side == Side::Bid ? mid - d : mid + 1 + d;
    return std::clamp<std::int64_t>(t, 0, static_cast<std::int64_t>(p.num_levels) - 1);
  };

  for (std::size_t i = 0; i < p.n; ++i) {
    if (rng.below(8) == 0) mid += static_cast<std::int64_t>(rng.below(3)) - 1;
    mid = std::clamp<std::int64_t>(mid, 60, static_cast<std::int64_t>(p.num_levels) - 60);
    const Timestamp ts = i + 1;
    Op op{};
    op.ts = ts;
    const unsigned r = static_cast<unsigned>(rng.below(100));

    auto new_order = [&](OrderType type) {
      NewOrder o{};
      o.id = next_id++;
      o.side = rng.below(2) ? Side::Bid : Side::Ask;
      o.type = type;
      o.owner = static_cast<OwnerId>(rng.below(4));
      o.qty = static_cast<Qty>(1 + rng.below(100));
      o.price = px(pick_tick(o.side));
      o.ts = ts;
      if (p.bad_input) {
        const auto bad = rng.below(200);
        if (bad == 0)
          o.qty = 0;
        else if (bad == 1)
          o.price += 1;  // off tick
        else if (bad == 2)
          o.price = p.base_price - p.tick_size;
        else if (bad == 3)
          o.price = px(p.num_levels);
        else if (bad == 4 && next_id > 2)
          o.id = pick_id();  // likely duplicate
      }
      return o;
    };

    // Cumulative percentage thresholds. Feed operations only appear when asked.
    //            limit mkt ioc fok post cancel modify fadd fred fdel fexec
    const unsigned w_feed[] = {34, 37, 42, 45, 50, 64, 76, 82, 86, 89, 100};
    const unsigned w_plain[] = {45, 48, 53, 56, 62, 80, 100, 100, 100, 100, 100};
    const unsigned* w = p.feed_ops ? w_feed : w_plain;
    int k = 0;
    while (r >= w[k]) ++k;
    switch (k) {
      case 0:
        op.kind = Op::Submit;
        op.order = new_order(OrderType::Limit);
        break;
      case 1:
        op.kind = Op::Submit;
        op.order = new_order(OrderType::Market);
        break;
      case 2:
        op.kind = Op::Submit;
        op.order = new_order(OrderType::Ioc);
        break;
      case 3:
        op.kind = Op::Submit;
        op.order = new_order(OrderType::Fok);
        break;
      case 4:
        op.kind = Op::Submit;
        op.order = new_order(OrderType::PostOnly);
        break;
      case 5:
        op.kind = Op::Cancel;
        op.id = pick_id();
        break;
      case 6:
        op.kind = Op::Modify;
        op.mod.id = pick_id();
        op.mod.new_qty = static_cast<Qty>(rng.below(120));  // zero is possible: rejected
        op.mod.new_price = px(pick_tick(rng.below(2) ? Side::Bid : Side::Ask));
        op.mod.ts = ts;
        break;
      case 7:
        op.kind = Op::FeedAdd;
        op.order = new_order(OrderType::Limit);
        op.order.owner = kFeedOwner;
        break;
      case 8:
        op.kind = Op::FeedReduce;
        op.id = pick_id();
        op.qty = static_cast<Qty>(1 + rng.below(50));
        break;
      case 9:
        op.kind = Op::FeedDelete;
        op.id = pick_id();
        break;
      default:
        op.kind = Op::FeedExecute;
        op.id = pick_id();
        op.qty = static_cast<Qty>(1 + rng.below(50));
        break;
    }
    ops.push_back(op);
  }
  return ops;
}

template <class E>
inline void apply(E& e, const Op& op) {
  switch (op.kind) {
    case Op::Submit:
      e.submit(op.order);
      break;
    case Op::Cancel:
      e.cancel(op.id, op.ts);
      break;
    case Op::Modify:
      e.modify(op.mod);
      break;
    case Op::FeedAdd: {
      auto a = e.replay_adapter();
      a.apply_add(op.order.id, op.order.side, op.order.price, op.order.qty, op.ts);
      break;
    }
    case Op::FeedReduce:
      e.replay_adapter().apply_reduce(op.id, op.qty, op.ts);
      break;
    case Op::FeedDelete:
      e.replay_adapter().apply_delete(op.id, op.ts);
      break;
    case Op::FeedExecute:
      e.replay_adapter().apply_execute(op.id, op.qty, op.ts);
      break;
  }
}

}  // namespace lobkit::testing
