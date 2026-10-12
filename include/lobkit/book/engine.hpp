#pragma once

#include "lobkit/book/book.hpp"
#include "lobkit/book/replay_adapter.hpp"

namespace lobkit {

// Public matching engine. Results are delivered only through the sink, in
// processing order, each with a sequence number. Single threaded by design.
// A throwing sink terminates the process (the operations are noexcept).
template <EventSink Sink = NullSink>
class Engine {
 public:
  explicit Engine(const BookConfig& cfg, Sink sink = Sink{}) : book_(cfg, std::move(sink)) {}

  void submit(const NewOrder& o) noexcept { book_.submit(o); }
  void cancel(OrderId id, Timestamp ts) noexcept { book_.cancel(id, ts); }
  void modify(const ModifyOrder& m) noexcept { book_.modify(m); }

  ReplayAdapter<Sink> replay_adapter() noexcept { return ReplayAdapter<Sink>(book_); }

  Book<Sink>& book() noexcept { return book_; }
  const Book<Sink>& book() const noexcept { return book_; }
  Sink& sink() noexcept { return book_.sink(); }

 private:
  Book<Sink> book_;
};

}  // namespace lobkit
