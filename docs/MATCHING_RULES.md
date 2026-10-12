# Matching rules

Exact semantics of lobkit v0.1. If code and this document disagree, that is a bug in one of them; the tests in tests/book pin the behavior described here.

## 1. Model

- One instrument per book. One book per thread.
- Prices are int64 native fixed-point values on a tick grid defined by `tick_size` and `base_price`. Quantities are uint32.
- Time is an input. Every message carries a caller supplied timestamp that is copied to the events it produces. The engine reads no clock.
- Every event has a sequence number from a single counter, starting at 1, incremented by one per event, including rejects.
- Order ids are unique among live orders. The caller may reuse the id of an order that is no longer live. The engine keeps no history of dead ids.

## 2. Validation, in this order

For `submit`:

1. `qty == 0` rejects with `InvalidQty`.
2. For all types except Market: price below `base_price` or at or above `base_price + num_levels * tick_size` rejects with `PriceOutOfBand`. A price inside the band that is not a multiple of `tick_size` from base rejects with `PriceOffTick`. The Market price field is ignored.
3. An id that is currently live rejects with `DuplicateOrderId`.
4. For Limit and PostOnly: if the order pool has no free slot, rejects with `PoolExhausted`. This is checked before matching, even if the order would have fully matched, so the outcome does not depend on the match result. It is conservative on purpose.

A rejected message changes no book state. It emits exactly one Reject event, and increments the matching counter in `BookStats`.

## 3. Order types

Priority is price first, then arrival order within a price. A trade executes at the price of the resting (maker) order. The aggressor is the incoming order.

Event order for an accepted submit: one `Ack`, then zero or more `Trade`, then at most one `Cancel` (for an unfilled remainder that does not rest).

| Type | Behavior |
|---|---|
| Limit | Matches against the opposite side at prices at or better than its limit. Any remainder rests at the back of the queue at its limit price. |
| Market | Matches against the opposite side without a price limit. Any remainder is cancelled with reason `MarketRemainder`; it never rests. On an empty opposite side, the whole order is cancelled after the Ack. |
| IOC | Like Limit, but any remainder is cancelled with reason `IocRemainder`. |
| FOK | Before any mutation, sums the quantity available at prices at or better than the limit. If it is at least the order quantity, the order fills completely. Otherwise it rejects with `FokNotFillable` and changes nothing. The check walks occupied levels and stops as soon as enough is found. |
| PostOnly | If it would match at all (the opposite best price is at or better than its limit), it rejects with `PostOnlyWouldCross`. It is not slid to the touch. Otherwise it rests like a Limit. |

## 4. Cancel

`cancel(id)` removes a live order and emits `Cancel` with the remaining quantity and reason `UserRequest`. An id that is unknown, already filled, or already cancelled rejects with `UnknownOrderId`. There is no race to resolve inside the engine: messages are processed in arrival order, so a cancel that arrives after the fill sees the order gone.

## 5. Modify

`modify(id, new_price, new_qty)`:

1. Unknown id rejects with `UnknownOrderId`. `new_qty == 0` rejects with `InvalidQty` (use cancel to remove an order). A bad price rejects as in section 2.
2. Same price and `new_qty < current remaining quantity`: the quantity is reduced in place. Queue priority is preserved. Emits `Modify` with the new quantity.
3. Same price and `new_qty == current remaining quantity`: no change, priority preserved. Emits `Modify`.
4. Anything else (a price change, or a quantity increase at the same price): cancel and replace with the same id. The order loses priority. It is removed from its queue, emits `Modify`, and is then processed as a new Limit order with the new price and quantity: it may match immediately (emitting `Trade` events), and any remainder rests at the back of the queue. There is no separate Cancel event for the removal.

The quantity in a modify is the new total remaining quantity, not the new original quantity and not a delta. Filled quantity is not remembered, so a modify after a partial fill sets the remaining quantity directly.

Race cases: a modify of an order that was filled before the modify arrived rejects with `UnknownOrderId`. A modify of a partly filled order operates on the remaining quantity at the time of processing.

## 6. Self-trade

Config `SelfTradePolicy`. v0.1 implements `Allow`. A trade where taker and maker have the same non-zero owner id sets the self-trade flag in the Trade event. Owner id 0 is the feed and is never flagged. `CancelNewest` is reserved and not implemented.

## 7. Replay adapter operations

These apply exogenous messages to feed orders (owner id 0). They share the same book state and invariants as the engine.

| Call | Behavior |
|---|---|
| `apply_add(id, side, price, qty)` | Validated like a Limit submit. Emits `Ack`. In a faithful replica it rests without trading. If it would cross a resting own order it trades as an incoming limit order (section 9). |
| `apply_reduce(id, qty)` | Removes `qty` from a feed order, priority preserved. If `qty` is at least the remaining quantity the order is removed. Emits `Cancel` with the removed quantity and reason `FeedReduce`. |
| `apply_delete(id)` | Removes a feed order. Emits `Cancel` with reason `FeedDelete`. |
| `apply_execute(id, qty)` | A feed order traded for `qty` against an external aggressor. Emits `Trade` with taker id 0 and the external-taker flag. See section 9. |

All four reject with `UnknownOrderId` for an unknown id, `InvalidQty` for zero, and `WrongOwner` if the id belongs to an own order. A quantity larger than the remaining quantity on an execute is clamped to the remaining quantity and counted in `feed_exec_overflow`.

## 8. Price levels and the touch

The best bid is the highest occupied bid level and the best ask the lowest occupied ask level. The book is never crossed after any operation completes. A level that becomes empty is removed from the occupancy bitmap in the same operation.

## 9. Own orders and feed executions

This section defines how orders submitted by a simulated strategy (owner id 1 or more, called own orders) interact with feed executions applied by the replay adapter. The goal is that an own order fills only when real flow would have traded through its queue position.

Definitions. A level queue is ordered from front (highest priority) to back. A feed order has owner id 0.

Rules:

1. Placement. An own order that rests joins the back of the queue at its price. It is behind every order already there, feed or own. Feed orders that arrive afterwards queue behind it.
2. Feed adds, reductions and deletes never fill an own order. When a feed order ahead of an own order is reduced or deleted, the own order moves toward the front and nothing else happens.
3. Feed execute. `apply_execute(Y, q)` walks the queue of Y level from the front toward Y. Let q_left start at q.
   - For each own order O ahead of Y, while no feed order has been seen ahead of it in this walk: fill O for `min(O.remaining, q_left)`, emit a `Trade` (taker external, maker O), and reduce q_left by that amount. Fully filled orders are removed.
   - When the walk meets a feed order ahead of Y, set the anomaly flag. Own orders behind that feed order are not filled, and `queue_order_anomalies` is incremented once for the call.
   - Y is then reduced by exactly q (clamped to its remaining quantity), emitting its own `Trade`.
4. Consequence of rule 3: an own order fills only when every feed order that was ahead of it at placement time has been drained (executed, reduced to zero, or deleted) and a feed execute then reaches an order behind it. Merely being at the front does not fill an order.
5. Replica fidelity. Feed orders are reduced by exactly the feed quantity, so removing the own orders from the book yields the feed book exactly. The price of this is that own fills are counterfactual extra volume: total traded volume in the simulator can exceed feed volume by the own fills. Own fills are also bounded by the quantity of the feed execute that reached them, which makes the fill model somewhat conservative about aggressor size and optimistic about the absence of market impact (see LIMITATIONS.md).
6. Feed add that crosses an own order. A feed order cannot cross feed orders in a faithful replica, so the only possible crossing is against own resting orders (for example, an own bid above a newly arrived feed ask). The feed order is processed as an incoming limit order: it trades against the crossed own orders at the own order price, and its remainder rests. The traded amount is subtracted from the feed order, so the replica deviates from the feed by that amount, counted in `feed_add_displaced_qty`. Every such fill is an anomaly, counted in `feed_crossed_own_fills`.
7. Own aggressive orders (marketable limit, market, IOC, FOK) match against feed orders by normal price-time priority, with the rules of section 3.

Worked example. Level at price 100, queue front to back: feed X1 (10), own O (5), feed X2 (8).

| Step | Result |
|---|---|
| `apply_execute(X1, 4)` | O is behind X1, so it is not touched. X1 has 6 left. |
| `apply_execute(X1, 6)` | X1 is gone. O is now at the front, still unfilled (rule 4). |
| `apply_execute(X2, 3)` | O is ahead of X2 and no feed order is ahead of O. O fills 3 (2 left). X2 is reduced by 3 (5 left). |
| Same state but X1 not yet drained, `apply_execute(X2, 3)` | A feed order is ahead of O, so this is the anomaly case. O is not filled and the anomaly counter increments. |

## 10. Anomalies and how replay reports them

Some situations mean that the book no longer matches the feed exactly. The engine never hides them: each has a counter in `BookStats`, and the sum of the first four below is `BookStats::anomalies()`.

| Counter | Meaning |
|---|---|
| `queue_order_anomalies` | A feed order was still ahead of the executed feed order, so the execute was not at the front of its queue (section 9, rule 3). This is counted whether or not own orders are present, because on a price-time priority venue it means the replica has diverged from the feed. When own orders sit behind that feed order they are not filled. |
| `feed_crossed_own_fills` | A feed add crossed a resting own order and traded with it (section 9, rule 6). |
| `feed_crossed_fills` | A feed add crossed a resting feed order. A faithful feed never does this. |
| `feed_exec_overflow` | A feed execute named more quantity than the order had left. |
| Rejects `PriceOutOfBand`, `PriceOffTick`, `UnknownOrderId`, `InvalidQty`, `WrongOwner`, `DuplicateOrderId` on feed operations | The message could not be applied. |

Reporting policy, enforced by the replay layer (Phase 4):

- Plain replay and simulation: all counters are printed in the final report, and the run is marked degraded if any anomaly or feed-operation reject count is non-zero. The run continues.
- Snapshot-verification mode: any anomaly, any out-of-band or off-tick message, and any rejected feed operation is a hard failure. The first occurrence is reported with its message number, and the process exits non-zero. In this mode there are no own orders, so the own-order anomalies cannot occur by construction, and seeing one indicates a bug.

## 11. How feed formats map onto the replay adapter

Both parsers produce the same feed message and the replay applies it through the calls in section 7. The mapping, with what is deliberately not applied:

| Source message | Adapter call or action |
|---|---|
| LOBSTER 1 new limit order | apply_add |
| LOBSTER 2 partial cancel (size is the removed quantity) | apply_reduce |
| LOBSTER 3 delete | apply_delete |
| LOBSTER 4 execution of a displayed order | apply_execute |
| LOBSTER 5 hidden execution | not applied, counted with its shares |
| LOBSTER 6 cross trade, 7 halt | not applied, counted |
| ITCH A, F add | apply_add |
| ITCH E, C executed | apply_execute |
| ITCH X cancel | apply_reduce |
| ITCH D delete | apply_delete |
| ITCH U replace | apply_delete of the old reference then apply_add of the new one on the same side: it loses time priority, which is how ITCH replaces behave. A replace of an unknown reference is counted and produces an UnknownOrderId reject |
| ITCH P non-cross trade | not applied, counted |

Order ids from a feed live in the same namespace as own order ids. The simulator must pick its own ids from a range the feed does not use (the top bit set is the convention proposed for Phase 5).
