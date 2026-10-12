#pragma once
// Runs one operation stream through the engine and the reference book and
// compares event streams after every operation, so a failure points at the
// exact operation instead of at a hash mismatch millions of events later.

#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "lobkit/book/engine.hpp"
#include "lobkit/book/invariants.hpp"
#include "lobkit/book/names.hpp"
#include "reference_book.hpp"
#include "workload.hpp"

namespace lobkit::testing {

inline std::string describe_op(const Op& op) {
  std::ostringstream s;
  switch (op.kind) {
    case Op::Submit:
    case Op::FeedAdd:
      s << (op.kind == Op::Submit ? "submit " : "feed_add ") << to_string(op.order.type) << ' '
        << to_string(op.order.side) << " id=" << op.order.id << " px=" << op.order.price
        << " qty=" << op.order.qty << " owner=" << op.order.owner;
      break;
    case Op::Cancel:
      s << "cancel id=" << op.id;
      break;
    case Op::Modify:
      s << "modify id=" << op.mod.id << " px=" << op.mod.new_price << " qty=" << op.mod.new_qty;
      break;
    case Op::FeedReduce:
      s << "feed_reduce id=" << op.id << " qty=" << op.qty;
      break;
    case Op::FeedDelete:
      s << "feed_delete id=" << op.id;
      break;
    case Op::FeedExecute:
      s << "feed_execute id=" << op.id << " qty=" << op.qty;
      break;
  }
  return s.str();
}

inline void apply_ref(ReferenceBook& r, const Op& op) {
  switch (op.kind) {
    case Op::Submit:
      r.submit(op.order);
      break;
    case Op::Cancel:
      r.cancel(op.id, op.ts);
      break;
    case Op::Modify:
      r.modify(op.mod);
      break;
    case Op::FeedAdd:
      r.feed_add(op.order.id, op.order.side, op.order.price, op.order.qty, op.ts);
      break;
    case Op::FeedReduce:
      r.feed_reduce(op.id, op.qty, op.ts);
      break;
    case Op::FeedDelete:
      r.feed_delete(op.id, op.ts);
      break;
    case Op::FeedExecute:
      r.feed_execute(op.id, op.qty, op.ts);
      break;
  }
}

struct DiffResult {
  bool ok = true;
  std::string message;  // first divergence, empty when ok
  std::uint64_t events = 0;
  std::uint64_t trades = 0;
  std::uint64_t ext_trades = 0;  // trades with an external taker (feed executes)
  BookStats stats;
};

inline std::string dump(const std::vector<Event>& v) {
  std::string s;
  for (const Event& e : v) s += "    " + describe(e) + "\n";
  return s;
}

// Compares the full visible state of both books: touch, depth and queue order
// at every price on the grid.
inline bool states_equal(const Engine<VectorSink>& e, const ReferenceBook& r, const BookConfig& cfg,
                         std::string& why) {
  const auto& b = e.book();
  if (b.has_bid() != r.has_bid() || b.has_ask() != r.has_ask()) {
    why = "touch presence";
    return false;
  }
  if (b.has_bid() && b.best_bid() != r.best_bid()) {
    why = "best bid";
    return false;
  }
  if (b.has_ask() && b.best_ask() != r.best_ask()) {
    why = "best ask";
    return false;
  }
  if (b.live_orders() != r.live()) {
    why = "live order count";
    return false;
  }
  for (std::uint32_t t = 0; t < cfg.num_levels; ++t) {
    const Price p = cfg.base_price + static_cast<Price>(t) * cfg.tick_size;
    if (b.depth_at(p) != r.depth_at(p)) {
      why = "depth at tick " + std::to_string(t);
      return false;
    }
    if (b.depth_at(p) != 0 && b.queue_ids(p) != r.queue_ids(p)) {
      why = "queue order at tick " + std::to_string(t);
      return false;
    }
  }
  return true;
}

inline bool stats_equal(const BookStats& a, const BookStats& b) {
  return a.rejects == b.rejects && a.trades == b.trades &&
         a.feed_crossed_fills == b.feed_crossed_fills &&
         a.feed_crossed_own_fills == b.feed_crossed_own_fills &&
         a.feed_add_displaced_qty == b.feed_add_displaced_qty &&
         a.feed_exec_overflow == b.feed_exec_overflow &&
         a.queue_order_anomalies == b.queue_order_anomalies;
}

inline DiffResult run_differential(const BookConfig& cfg, const std::vector<Op>& ops,
                                   std::size_t state_every = 1009,
                                   std::size_t invariant_every = 997) {
  DiffResult res;
  Engine<VectorSink> eng(cfg);
  ReferenceBook ref(cfg);
  auto fail = [&](std::size_t i, const std::string& what) {
    res.ok = false;
    std::ostringstream s;
    s << "op " << i << " (" << describe_op(ops[i]) << "): " << what << "\n  engine events:\n"
      << dump(eng.sink().events) << "  reference events:\n"
      << dump(ref.events);
    res.message = s.str();
    return res;
  };

  for (std::size_t i = 0; i < ops.size(); ++i) {
    apply(eng, ops[i]);
    apply_ref(ref, ops[i]);
    const auto& a = eng.sink().events;
    const auto& b = ref.events;
    if (a.size() != b.size()) return fail(i, "different number of events");
    if (!a.empty() && std::memcmp(a.data(), b.data(), a.size() * sizeof(Event)) != 0)
      return fail(i, "event mismatch");
    res.events += a.size();
    for (const Event& e : a)
      if (e.type == EventType::Trade) {
        ++res.trades;
        if (e.flags & kTradeExternalTaker) ++res.ext_trades;
      }
    eng.sink().events.clear();
    ref.events.clear();

    if (i % state_every == 0) {
      std::string why;
      if (!states_equal(eng, ref, cfg, why)) return fail(i, "state mismatch: " + why);
    }
    if (i % invariant_every == 0) {
      if (auto err = check_invariants(eng.book())) return fail(i, "invariant: " + *err);
    }
  }
  std::string why;
  if (!states_equal(eng, ref, cfg, why))
    return fail(ops.size() - 1, "final state mismatch: " + why);
  if (auto err = check_invariants(eng.book()))
    return fail(ops.size() - 1, "final invariant: " + *err);
  if (!stats_equal(eng.book().stats(), ref.stats)) return fail(ops.size() - 1, "stats mismatch");
  res.stats = eng.book().stats();
  return res;
}

inline std::size_t diff_op_count(std::size_t dflt) {
  if (const char* v = std::getenv("LOBKIT_DIFF_OPS"))
    return static_cast<std::size_t>(std::strtoull(v, nullptr, 10));
  return dflt;
}

}  // namespace lobkit::testing
