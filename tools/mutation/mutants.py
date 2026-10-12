"""Hand written mutants of include/lobkit/book/book.hpp.

Each mutant replaces one exact text fragment (it must occur in the file) with a
plausible bug. expect is either killed (the differential test must fail or
crash) or equivalent (the mutant cannot change any observable behavior, and why
holds the one line argument). A mutant that survives while marked killed, or is
killed while marked equivalent, is reported as a mismatch by run.py.

Fragments match the clang-format output of the source. If the engine is
reformatted and a fragment no longer matches, run.py --check says which.
"""

FILE = "include/lobkit/book/book.hpp"          # default target of a mutant
SIM = "include/lobkit/sim/simulator.hpp"
AS = "include/lobkit/sim/avellaneda_stoikov.hpp"

# A mutant may name another file and which test suite must catch it:
#   suite "diff" (default): the differential test, for the engine
#   suite "sim": the simulator and strategy tests (AS* and Sim*)

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
         old="      if (q_left > 0) {\n        const Qty fill = std::min(o.qty, q_left);\n        emit(EventType::Trade, ts, 0, o.id, px,",
         new="      if (false) {\n        const Qty fill = std::min(o.qty, q_left);\n        emit(EventType::Trade, ts, 0, o.id, px,"),
    dict(name="feed-execute-feed-ahead-not-flagged", expect="killed",
         old="        feed_ahead = true;\n        break;", new="        break;"),
    dict(name="feed-execute-walk-continues-past-feed-order", expect="killed",
         old="        feed_ahead = true;\n        break;",
         new="        feed_ahead = true;\n        cur = o.next;\n        continue;"),
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

    # ---- simulator and strategy (suite "sim") ----
    dict(name="sim-inventory-room-cap-removed", file=AS, suite="sim", expect="killed",
         old="n = std::min(n, room);", new="n = std::min(n, room + 1000000);"),
    dict(name="sim-working-shares-not-counted-against-room", file=AS, suite="sim", expect="killed",
         old="(bid_working - live_bid_qty)", new="(0 * (bid_working - live_bid_qty))"),
    dict(name="sim-submit-latency-ignored", file=SIM, suite="sim", expect="killed",
         old="e.time = now_ + cfg_.submit_latency_ns;\n    e.kind = Kind::Send;",
         new="e.time = now_;\n    e.kind = Kind::Send;"),
    dict(name="sim-market-data-latency-ignored", file=SIM, suite="sim", expect="killed",
         old="e.time = now_ + cfg_.md_latency_ns;", new="e.time = now_;"),
    dict(name="sim-fill-notice-latency-ignored", file=SIM, suite="sim", expect="killed",
         old="n.time = now_ + cfg_.ack_latency_ns;\n    n.id = id;\n    n.side = side;",
         new="n.time = now_;\n    n.id = id;\n    n.side = side;"),
    dict(name="sim-spread-capture-sign-flipped", file=SIM, suite="sim", expect="killed",
         old="pnl_.spread2 += static_cast<Money2>(sgn * q) * (ref - p2);",
         new="pnl_.spread2 += static_cast<Money2>(sgn * q) * (p2 - ref);"),
    dict(name="sim-fee-sign-flipped", file=SIM, suite="sim", expect="killed",
         old="pnl_.fees2 += static_cast<Money2>(2) * fee_units;", new="pnl_.fees2 -= static_cast<Money2>(2) * fee_units;"),
    dict(name="sim-inventory-pnl-uses-wrong-inventory", file=SIM, suite="sim", expect="killed",
         old="pnl_.inventory2 += static_cast<Money2>(inventory_) * (m2 - mid2_);",
         new="pnl_.inventory2 += static_cast<Money2>(inventory_ / 2) * (m2 - mid2_);"),
    dict(name="sim-own-orders-left-in-market-view", file=SIM, suite="sim", expect="killed",
         old="if (lv[j].price == px) lv[j].qty -= std::min<std::uint64_t>(lv[j].qty, o->qty);",
         new="if (lv[j].price == px) lv[j].qty -= 0;"),
    dict(name="sim-orders-not-post-only", file=SIM, suite="sim", expect="killed",
         old="OrderType::PostOnly, kStrategyOwner", new="OrderType::Limit, kStrategyOwner"),
    dict(name="sim-maker-flag-flipped", file=SIM, suite="sim", expect="killed",
         old="n.flag = maker_own ? 1 : 0;", new="n.flag = maker_own ? 0 : 1;"),
    dict(name="sim-aggressor-side-flipped", file=SIM, suite="sim", expect="killed",
         old="const Side side = maker_own ? opposite(e.side) : e.side;",
         new="const Side side = maker_own ? e.side : opposite(e.side);"),
    dict(name="sim-feed-wins-timestamp-ties", file=SIM, suite="sim", expect="killed",
         old="if (!heap_.empty() && (!have || heap_.front().time <= pending.ts)) {",
         new="if (!heap_.empty() && (!have || heap_.front().time < pending.ts)) {"),
]
