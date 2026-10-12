"""Hand written mutants of include/lobkit/book/book.hpp.

Each mutant replaces one exact text fragment (it must occur in the file) with a
plausible bug. expect is either killed (the differential test must fail or
crash) or equivalent (the mutant cannot change any observable behavior, and why
holds the one line argument). A mutant that survives while marked killed, or is
killed while marked equivalent, is reported as a mismatch by run.py.

Fragments match the clang-format output of the source. If the engine is
reformatted and a fragment no longer matches, run.py --check says which.
"""

FILE = "include/lobkit/book/book.hpp"

MUTANTS = [
    dict(name="fifo-insert-at-head", expect="killed",
         old="""      o.prev = lvl.tail;
      pool_[lvl.tail].next = oi;
      lvl.tail = oi;""",
         new="""      o.next = lvl.head;
      pool_[lvl.head].prev = oi;
      lvl.head = oi;"""),
    dict(name="modify-keeps-priority-on-size-increase", expect="killed",
         old="m.new_qty <= o.qty) {", new="true) {"),
    dict(name="post-only-trades-at-equal-price", expect="killed",
         old="best_ask_ != kNil && best_ask_ <= idx)", new="best_ask_ != kNil && best_ask_ < idx)"),
    dict(name="fok-availability-off-by-one", expect="killed",
         old="available(o.side, idx, o.qty) < o.qty)", new="available(o.side, idx, o.qty) + 1 < o.qty)"),
    dict(name="feed-execute-never-fills-own", expect="killed",
         old="else if (!feed_ahead && q_left > 0) {", new="else if (false) {"),
    dict(name="feed-execute-ignores-feed-ahead", expect="killed",
         old="else if (!feed_ahead && q_left > 0) {", new="else if (q_left > 0) {"),
    dict(name="self-trade-flag-for-feed-owner", expect="killed",
         old="if (owner != kFeedOwner && owner == m.owner) flags |= kTradeSelf;",
         new="if (owner == m.owner) flags |= kTradeSelf;"),
    dict(name="market-ack-carries-price", expect="killed",
         old="o.type == OrderType::Market ? 0 : o.price", new="o.price"),
    dict(name="ioc-remainder-wrong-reason", expect="killed",
         old="CancelReason::IocRemainder", new="CancelReason::MarketRemainder"),
    dict(name="pool-check-skipped-for-post-only", expect="killed",
         old="if (may_rest && pool_.full())",
         new="if (may_rest && pool_.full() && o.type == OrderType::Limit)"),
    dict(name="feed-reduce-reports-wrong-qty", expect="killed",
         old="""emit_cancel(ts, o, qty, CancelReason::FeedReduce);
      o.qty -= qty;""",
         new="""emit_cancel(ts, o, qty + 1, CancelReason::FeedReduce);
      o.qty -= qty;"""),
    dict(name="feed-execute-aggregate-not-updated", expect="killed",
         old="""lvl.total_qty -= qty;
    if (y.qty == 0)""", new="""if (y.qty == 0)"""),
    dict(name="modify-emits-event-before-removal", expect="equivalent",
         why="remove_order emits no events and the Modify event is built from locals copied before the "
             "removal, so swapping the two statements changes neither the stream nor the state.",
         old="""remove_order(oi);
    emit(EventType::Modify, m.ts, m.id, 0, m.new_price, m.new_qty, side, 0, 0);""",
         new="""emit(EventType::Modify, m.ts, m.id, 0, m.new_price, m.new_qty, side, 0, 0);
    remove_order(oi);"""),
    dict(name="next-ask-searched-from-level-plus-one", expect="equivalent",
         why="the bit for o.level was cleared on the line above, so searching from level or from level + 1 "
             "finds the same next occupied index.",
         old="if (best_ask_ == o.level) best_ask_ = asks_.next_at_or_above(o.level);",
         new="if (best_ask_ == o.level) best_ask_ = asks_.next_at_or_above(o.level + 1);"),
]
