#pragma once

#include "lobkit/book/book.hpp"

namespace lobkit {

// Applies exogenous market data messages to feed orders (owner id 0) without
// running the matching loop for them. It operates on the same Book as the
// Engine it came from, so own orders submitted through the engine and feed
// orders applied here live in one set of structures. Semantics are in
// MATCHING_RULES.md sections 7 and 9.
template <EventSink Sink>
class ReplayAdapter {
 public:
  explicit ReplayAdapter(Book<Sink>& book) noexcept : book_(&book) {}

  void apply_add(OrderId id, Side side, Price price, Qty qty, Timestamp ts) noexcept {
    book_->feed_add(id, side, price, qty, ts);
  }
  void apply_reduce(OrderId id, Qty qty, Timestamp ts) noexcept { book_->feed_reduce(id, qty, ts); }
  void apply_delete(OrderId id, Timestamp ts) noexcept { book_->feed_delete(id, ts); }
  void apply_execute(OrderId id, Qty qty, Timestamp ts) noexcept {
    book_->feed_execute(id, qty, ts);
  }

 private:
  Book<Sink>* book_;
};

}  // namespace lobkit
