#pragma once

#include <gtest/gtest.h>

#include <vector>

#include "lobkit/book/engine.hpp"
#include "lobkit/book/invariants.hpp"
#include "lobkit/book/names.hpp"

namespace lobkit::testing {

// Tick grid used by the unit tests: price = 100000 + 100 * tick, 1024 levels.
// A non-trivial base and tick_size make sure no code path assumes tick == 1.
inline BookConfig small_cfg(std::uint32_t max_orders = 4096) {
  BookConfig c;
  c.tick_size = 100;
  c.base_price = 100000;
  c.num_levels = 1024;
  c.max_orders = max_orders;
  return c;
}

inline Price px(int tick) {
  return 100000 + 100 * static_cast<Price>(tick);
}

class BookTest : public ::testing::Test {
 protected:
  explicit BookTest(BookConfig cfg = small_cfg()) : e(cfg) {}

  Engine<VectorSink> e;
  Timestamp t = 0;

  std::vector<Event>& ev() { return e.sink().events; }
  void clear() { ev().clear(); }
  const Book<VectorSink>& book() const { return e.book(); }

  void limit(OrderId id, Side s, int tick, Qty q, OwnerId owner = 0,
             OrderType type = OrderType::Limit) {
    e.submit({id, px(tick), q, s, type, owner, ++t});
  }
  void market(OrderId id, Side s, Qty q, OwnerId owner = 0) {
    e.submit({id, 0, q, s, OrderType::Market, owner, ++t});
  }
  void cancel(OrderId id) { e.cancel(id, ++t); }
  void modify(OrderId id, int tick, Qty q) { e.modify({id, px(tick), q, ++t}); }

  std::vector<Event> of_type(EventType ty) const {
    std::vector<Event> out;
    for (const Event& x : e.book().sink().events)
      if (x.type == ty) out.push_back(x);
    return out;
  }
  std::vector<OrderId> queue(int tick) const { return book().queue_ids(px(tick)); }

  void expect_valid() {
    const auto err = check_invariants(book());
    EXPECT_FALSE(err.has_value()) << err.value_or("");
  }
  RejectReason last_reject() const {
    for (auto it = book().sink().events.rbegin(); it != book().sink().events.rend(); ++it)
      if (it->type == EventType::Reject) return static_cast<RejectReason>(it->reason);
    return RejectReason::None;
  }
};

}  // namespace lobkit::testing
