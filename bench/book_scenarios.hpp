#pragma once
// Deterministic operation streams for the book benchmarks. Every stream is SYNTHETIC. If real
// market data is supplied later, the mixed scenario should be re-derived from it (see
// docs/PERFORMANCE.md); nothing here claims to resemble a real market beyond "most orders near
// the touch with a fat tail", which is the same shape tests/support/workload.hpp uses.

#include <algorithm>
#include <string>
#include <vector>

#include "lobkit/book/config.hpp"
#include "lobkit/md/replay.hpp"
#include "lobkit/sim/synthetic.hpp"
#include "workload.hpp"  // tests/support: Op, SplitMix64, make_workload, apply

namespace lobkit::bench {

using testing::Op;
using testing::SplitMix64;

struct Scenario {
  std::string name;
  std::string what;  // one line, for the report
  BookConfig cfg;
  std::vector<Op> setup;     // applied untimed before every pass
  std::vector<Op> measured;  // timed
  int passes = 1;            // setup+measured repeated this many times per run, for sample count
};

inline BookConfig bench_config() {
  BookConfig c;
  c.tick_size = 100;
  c.base_price = 100000;
  c.num_levels = 16384;
  c.max_orders = 1u << 19;  // 16 MiB of orders, below the 32 MiB malloc mmap ceiling (see harness)
  return c;
}

inline Price px(const BookConfig& c, std::int64_t tick) {
  return c.base_price + c.tick_size * tick;
}

namespace detail {
struct Gen {
  explicit Gen(const BookConfig& c, std::uint64_t seed) : cfg(c), rng(seed) {}
  BookConfig cfg;
  SplitMix64 rng;
  std::int64_t mid() const { return cfg.num_levels / 2; }
  // Same shape as tests/support/workload.hpp: mostly near the touch, fat tail.
  std::int64_t tick_for(Side side) {
    std::int64_t d = 0;
    while (rng.below(100) < 70 && d < 40) ++d;
    if (rng.below(50) == 0) d += static_cast<std::int64_t>(rng.below(100));
    return side == Side::Bid ? mid() - d : mid() + 1 + d;
  }
  Op add(OrderId id, Timestamp ts) {
    Op op{};
    op.kind = Op::Submit;
    op.ts = ts;
    op.order.id = id;
    op.order.side = rng.below(2) ? Side::Bid : Side::Ask;
    op.order.type = OrderType::Limit;
    op.order.owner = 0;
    op.order.qty = static_cast<Qty>(1 + rng.below(100));
    op.order.price = px(cfg, tick_for(op.order.side));
    op.order.ts = ts;
    return op;
  }
};
}  // namespace detail

inline std::vector<Op> resting_orders(const BookConfig& cfg, std::uint64_t seed, std::size_t n,
                                      OrderId first_id, std::vector<Op>* keep = nullptr) {
  detail::Gen g(cfg, seed);
  std::vector<Op> ops;
  ops.reserve(n);
  for (std::size_t i = 0; i < n; ++i) ops.push_back(g.add(first_id + i, i + 1));
  if (keep) *keep = ops;
  return ops;
}

inline Scenario make_add() {
  Scenario s{"add",
             "200k non-crossing limit adds into a book already holding 100k orders",
             bench_config(),
             {},
             {}};
  s.setup = resting_orders(s.cfg, 11, 100000, 1);
  s.measured = resting_orders(s.cfg, 12, 200000, 1000001);
  return s;
}

inline Scenario make_cancel() {
  Scenario s{
      "cancel", "200k cancels, random order, from a book of 300k orders", bench_config(), {}, {}};
  s.setup = resting_orders(s.cfg, 21, 300000, 1);
  std::vector<OrderId> ids(300000);
  for (std::size_t i = 0; i < ids.size(); ++i) ids[i] = i + 1;
  SplitMix64 r(22);
  for (std::size_t i = ids.size() - 1; i > 0; --i) std::swap(ids[i], ids[r.below(i + 1)]);
  for (std::size_t i = 0; i < 200000; ++i) {
    Op op{};
    op.kind = Op::Cancel;
    op.id = ids[i];
    op.ts = i + 1;
    s.measured.push_back(op);
  }
  return s;
}

inline Scenario make_modify(bool reprice) {
  Scenario s{
      reprice ? "modify_reprice" : "modify_reduce",
      reprice ? "200k price changes (cancel and replace, loses priority) in a book of 300k orders"
              : "200k in place quantity reductions in a book of 300k orders",
      bench_config(),
      {},
      {}};
  s.setup = resting_orders(s.cfg, 31, 300000, 1);
  detail::Gen g(s.cfg, 32);
  // 200k distinct orders, visited in random order. Visiting them in id order would let a hash table
  // with an identity hash walk memory sequentially, which says nothing about a real feed.
  std::vector<std::size_t> order(s.setup.size());
  for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
  SplitMix64 shuf(33);
  for (std::size_t i = order.size() - 1; i > 0; --i) std::swap(order[i], order[shuf.below(i + 1)]);
  for (std::size_t i = 0; i < 200000; ++i) {
    const Op& orig = s.setup[order[i]];
    Op op{};
    op.kind = Op::Modify;
    op.ts = i + 1;
    op.mod.id = orig.order.id;
    op.mod.ts = op.ts;
    if (reprice) {
      op.mod.new_qty = orig.order.qty;
      op.mod.new_price = px(s.cfg, g.tick_for(orig.order.side));
      if (op.mod.new_price == orig.order.price)
        op.mod.new_price += orig.order.side == Side::Bid ? -s.cfg.tick_size : s.cfg.tick_size;
    } else {
      op.mod.new_price = orig.order.price;
      op.mod.new_qty =
          std::max<Qty>(1, orig.order.qty / 2);  // 1 stays 1: a no-change modify, still in place
    }
    s.measured.push_back(op);
  }
  return s;
}

// A buy IOC that consumes exactly n_levels levels of 4 orders each (10 shares per order).
inline Scenario make_match(int n_levels) {
  Scenario s{"match_" + std::to_string(n_levels),
             "IOC buy sweeping " + std::to_string(n_levels) +
                 " price levels of 4 orders each, per operation",
             bench_config(),
             {},
             {}};
  const std::int64_t total_levels = 16000;
  const int sweeps = static_cast<int>(std::min<std::int64_t>(4000, total_levels / n_levels));
  const std::int64_t first = 100;
  OrderId id = 1;
  for (std::int64_t l = 0; l < static_cast<std::int64_t>(sweeps) * n_levels; ++l)
    for (int k = 0; k < 4; ++k) {
      Op op{};
      op.kind = Op::Submit;
      op.ts = id;
      op.order = NewOrder{id, px(s.cfg, first + l), 10, Side::Ask, OrderType::Limit, 0, id};
      s.setup.push_back(op);
      ++id;
    }
  for (int k = 0; k < sweeps; ++k) {
    Op op{};
    op.kind = Op::Submit;
    op.ts = 1000000 + static_cast<Timestamp>(k);
    op.order = NewOrder{1000000 + static_cast<OrderId>(k),
                        px(s.cfg, first + static_cast<std::int64_t>(k + 1) * n_levels - 1),
                        static_cast<Qty>(40 * n_levels),
                        Side::Bid,
                        OrderType::Ioc,
                        0,
                        op.ts};
    s.measured.push_back(op);
  }
  // Enough repetitions for a few thousand latency samples (p99.9 needs at least ~1000).
  s.passes = std::max(1, 4000 / sweeps);
  return s;
}

// Plain engine message mix from tests/support/workload.hpp: limit, market, IOC, FOK and post-only
// submits, cancels and modifies, with owner ids 0 to 3. The first 100k operations build the book
// and are applied untimed (they are the setup); the next 500k are timed.
inline Scenario make_mixed() {
  Scenario s{"mixed",
             "500k mixed messages (limit 45%, market 3%, IOC 5%, FOK 3%, post-only 6%, cancel 18%, "
             "modify 20%) after 100k to build the book",
             bench_config(),
             {},
             {}};
  testing::WorkloadParams wp;
  wp.seed = 41;
  wp.n = 600000;
  wp.base_price = s.cfg.base_price;
  wp.tick_size = s.cfg.tick_size;
  wp.num_levels = s.cfg.num_levels;
  wp.feed_ops = false;
  wp.bad_input = false;
  auto ops = testing::make_workload(wp);
  s.setup.assign(ops.begin(), ops.begin() + 100000);
  s.measured.assign(ops.begin() + 100000, ops.end());
  return s;
}

// The synthetic feed generator's messages applied through the replay adapter (adds, reductions,
// deletes, executions). This is the shape of the Phase 4 replay workload. SYNTHETIC.
inline Scenario make_feed() {
  Scenario s{"feed_replay",
             "500k synthetic feed messages (add, reduce, delete, execute) through the replay "
             "adapter after 100k to build the book",
             bench_config(),
             {},
             {}};
  sim::synthetic::SynthParams sp;
  sp.seed = 51;
  sp.messages = 600000;
  const auto msgs = sim::synthetic::make_synth_feed(sp);
  Price lo = msgs.front().price, hi = lo;
  for (const auto& m : msgs)
    if (m.type == md::FeedType::Add && m.price > 0) {
      lo = std::min(lo, m.price);
      hi = std::max(hi, m.price);
    }
  s.cfg.base_price = md::suggest_base_price(lo, hi, s.cfg.tick_size, s.cfg.num_levels);
  for (std::size_t i = 0; i < msgs.size(); ++i) {
    const md::FeedMsg& m = msgs[i];
    Op op{};
    op.ts = m.ts;
    op.id = m.id;
    op.qty = m.qty;
    switch (m.type) {
      case md::FeedType::Add:
        op.kind = Op::FeedAdd;
        op.order = NewOrder{m.id, m.price, m.qty, m.side, OrderType::Limit, 0, m.ts};
        break;
      case md::FeedType::Reduce:
        op.kind = Op::FeedReduce;
        break;
      case md::FeedType::Delete:
        op.kind = Op::FeedDelete;
        break;
      case md::FeedType::Execute:
        op.kind = Op::FeedExecute;
        break;
      default:
        continue;  // the generator emits only the four above
    }
    (i < 100000 ? s.setup : s.measured).push_back(op);
  }
  return s;
}

// The mixed scenario with every order id multiplied by 1024. Real order ids are not guaranteed to
// be dense; this checks how sensitive the id hash is to structure in the ids (strided ids are the
// classic weakness of a hash that keeps nearby ids nearby). SYNTHETIC.
inline Scenario make_mixed_strided() {
  Scenario s = make_mixed();
  s.name = "mixed_strided_ids";
  s.what = "the mixed scenario with every order id multiplied by 1024";
  auto remap = [](std::vector<Op>& v) {
    for (Op& op : v) {
      op.order.id *= 1024;
      op.id *= 1024;
      op.mod.id *= 1024;
    }
  };
  remap(s.setup);
  remap(s.measured);
  return s;
}

}  // namespace lobkit::bench
