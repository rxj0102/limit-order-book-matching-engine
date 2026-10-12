# Strategy simulator

An event-driven backtester that replays a feed into the matching engine and lets one strategy trade in the same book, plus an Avellaneda-Stoikov market maker. Code: include/lobkit/sim, src/sim, tools/lobkit_sim.cpp.

READ THIS FIRST. Everything produced so far was run on SYNTHETIC data, and nothing in this document is evidence that any strategy is profitable. The fill model is an approximation. The feed does not react to the strategy. This is a research and engineering tool, not a live trading system, and it has no connection to one.

## 1. What is simulated

The feed (LOBSTER or ITCH messages) is applied to the engine at its recorded timestamps through the ReplayAdapter. The strategy submits orders through the Engine into the same book, so its orders rest at the back of the queue at their price and fill only under the own-order rules of MATCHING_RULES.md section 9: an own order fills when a feed execution reaches an order behind it and no feed order is still ahead of it. Cancels and reductions of feed orders ahead of it move it forward and never fill it.

The strategy only sends post-only orders. A quote that would cross the book on arrival is rejected by the engine instead of trading, so the strategy never takes liquidity by accident. (Taker accounting exists for completeness; with post-only quotes the taker count is zero, and a test checks it.)

## 2. Time, latency and event order

Three one-way delays are configured separately, all nanoseconds of simulated time and zero by default:

| Setting | Direction | Meaning |
|---|---|---|
| submit latency | strategy to exchange | an order or cancel decided at time t reaches the book at t plus this |
| ack latency | exchange to strategy | acks, rejects, fills and cancel results reach the strategy at (exchange time) plus this |
| market data latency | exchange to strategy | the strategy sees a snapshot of the book taken at t at time t plus this |

So with a submit latency of 300 and an ack latency of 50, an order decided at 1120 reaches the book at 1420 and its ack reaches the strategy at 1470. Tests pin these numbers.

Events are ordered by (time, sequence number). The feed is merged into that order with one rule: at equal times, events already queued run before the next feed message. With zero latency the strategy therefore reacts to a feed message before the next one is applied, and its order lands in the queue ahead of a feed order with the same timestamp. This is a convention, tested, and it matters only for ties.

Because delays are modeled, queue position depends on latency: an order that reaches the book later lines up behind feed orders that arrived in between. A test shows the same strategy action filling at different times and quantities at two latencies.

## 3. What the strategy sees

The market view is the top of the book with the strategy own displayed orders subtracted, so the strategy does not chase its own quote. It is a snapshot, delivered after the market data delay, and it is only sent when the best prices or sizes change. Own fills reach the strategy after the ack delay, so the strategy knows a lagged inventory. The simulator tracks the true inventory separately and reports the difference.

## 4. Accounting

All money is integer, in half units of the feed price (the feed price unit is 1e-4 dollars, so a half unit is 0.5e-4 dollars), held in 128 bit integers. Half units make a half-tick mid exact. Nothing in the accounting uses floating point; CSV output is formatted from integers.

For every fill with side s (+1 buy, -1 sell), quantity q, price p, and the market mid m just before the operation that caused the fill (own orders excluded):

    spread capture   += s * q * (m - p)
    cash             += -s * q * p            (kept as an independent ledger)
    fees             += q * (maker fee or taker fee)   (a negative fee is a rebate)

Whenever the mid changes (feed messages and strategy order arrivals can both change it):

    inventory PnL    += inventory * (new mid - old mid)

Then

    total = spread capture + inventory PnL - fees paid

and the identity is checked exactly, in integers, against a number computed another way that never uses the decomposition: cash ledger plus final inventory marked at the final mid, minus fees. The check runs in every test run and appears in the summary as matches or MISMATCH. The marking uses the mid of the other participants' book, so a fill is not measured against a mid that the strategy own quote created.

Meaning of the pieces: spread capture is what the strategy earned relative to the mid at the instant of each fill (positive for buying below it); inventory PnL is what the position gained or lost as the mid moved while it was held; fees are the maker rebates or fees. Losing money on inventory while earning the spread is the usual picture of adverse selection, but this decomposition does not separate adverse selection from ordinary price drift.

If no two-sided market has ever been seen when the first fill happens (not reachable with the shipped strategy), the fill price serves as the reference and the capture of that fill is zero.

## 5. The Avellaneda-Stoikov strategy

Source: avellaneda_stoikov.hpp. Prices in dollars, time in seconds.

    reservation price   r     = s - q * gamma * sigma^2 * (T - t)
    optimal spread      delta = gamma * sigma^2 * (T - t) + (2 / gamma) * ln(1 + gamma / k)
    quotes              bid = r - delta / 2,   ask = r + delta / 2

| Symbol | Meaning | Unit | Default |
|---|---|---|---|
| s | market mid, own orders excluded | dollars | |
| q | inventory as the strategy has been told | shares | |
| gamma | risk aversion | per dollar | 0.05 |
| k | order book depth: market order intensity falls as exp(-k * distance) | per dollar | 50 |
| sigma | volatility of the mid | dollars per square root second | 0.02 at the start, then estimated |
| T - t | horizon | seconds | 10, constant (the infinite horizon variant); a shrinking horizon to a session end is available |

Estimation: sigma comes from an exponentially weighted average of squared mid changes sampled every second. k is a configured parameter, not fitted; fitting it from data is left out.

Around the formula:

- Quotes are rounded to the tick grid, bids down and asks up.
- Passive clipping: the bid stays at least one tick below the best ask the strategy last saw, the ask at least one tick above the best bid.
- Self-crossing guard: the own ask is always above the own bid.
- Inventory limit: the quantity on a side is capped so that the known inventory plus every share still working on that side never exceeds max inventory in absolute value. Working shares include orders in flight and fills the strategy has not yet been told about, so the bound holds under latency; tests assert it at six latency settings on three seeds.
- Quote skewing: the reservation price shifts quotes against the position (the model term), and an optional size skew shrinks the size on the side that adds to the position.
- Requoting is throttled and happens only if the desired price moved by at least one tick, so queue position is not discarded for nothing. A replacement is a cancel followed by a new order after the cancel is confirmed, so there is never more than one own order per side.

Floating point: this file is the only place in the project where prices and quantities become doubles. The decision, a price and a size, is rounded to integers immediately. Runs are reproducible bit for bit with the same binary on the same machine; across compilers or libm versions a quote within rounding error of a tick boundary could move by one tick.

## 6. Output

lobkit_sim_run writes three CSV files and prints a summary:

| File | Columns |
|---|---|
| PREFIX_fills.csv | ts_ns, order_id, side, price, qty, role, fee, mid_ref, inventory_after |
| PREFIX_inventory.csv | ts_ns, inventory, inventory_not_yet_reported |
| PREFIX_pnl.csv | ts_ns, spread_capture, inventory_pnl, fees, total |

Prices and money are exact decimals in dollars. The inventory and PnL paths are sampled every second of simulated time by default and at every fill. The same inputs give byte-identical files (tested).

    lobkit_sim_run --lobster FILE --out PREFIX --data-label SYNTHETIC-1M [--submit-latency-us N --ack-latency-us N
        --md-latency-us N --maker-fee N --taker-fee N --gamma G --k K --sigma0 S --horizon SEC --size N
        --max-inventory N --size-skew F --requote-ms MS --tick-size N]

The data label is mandatory in spirit: without it the output says the data is unspecified.

## 7. What was run, on synthetic data only

docs/results/phase5/synthetic_runs.txt holds the output of the runs below: one synthetic LOBSTER-format feed (scripts/make_synthetic_lobster.py, 1,000,000 messages, seed 3, about 1,000 seconds of simulated time), default parameters unless stated. One run per row, and the feed is one random draw, so differences between rows are within what a different seed could produce. They are shown to exercise the tool, not to rank settings.

| Run | Fills (shares) | Spread capture | Inventory PnL | Fees paid | Total (dollars) |
|---|---|---|---|---|---|
| baseline | 92 (7,707) | 38.285 | -39.890 | 0 | -1.605 |
| latency 1 ms each way | 33 (2,572) | 18.970 | 8.200 | 0 | 27.170 |
| latency 10 ms each way | 103 (7,684) | 34.905 | -37.825 | 0 | -2.920 |
| latency 50 ms each way | 13 (1,000) | 9.510 | -33.510 | 0 | -24.000 |
| gamma 0.01 | 24 (1,817) | 23.780 | -57.605 | 0 | -33.825 |
| gamma 0.5 | 349 (27,426) | 69.165 | -38.645 | 0 | 30.520 |
| maker rebate 0.2 cent per share | 92 (7,707) | 38.285 | -39.890 | -15.414 | 13.809 |
| max inventory 100 | 93 (7,675) | 33.855 | -47.450 | 0 | -13.595 |

The numbers are not monotonic in latency, which is what one would expect from noise on one feed, and nothing here can be read as an effect of latency or of gamma on real profit. The max inventory 100 run reaches exactly 100 shares, so the limit binds.

## 8. Limitations and biases

Plainly:

- The fill model is an approximation. Fills happen when historical executions reach the queue position the order would have had. Real queue dynamics depend on information the feed does not contain.
- No market impact. The feed does not change because the strategy is there. Other participants do not react to the strategy quotes, and the historical executions that fill it would have been executed against someone else.
- Cancels ahead of the strategy are assumed independent of its presence. In reality participants cancel when they see a large order arrive. This tends to flatter the fill model.
- Hidden liquidity: executions of non-displayed orders (LOBSTER type 5) are not applied to the strategy orders even when they were at its price, which under-fills. Whether the net bias of that and the cancel assumption cancels out is unknown.
- Latency is constant and deterministic. Real latency has jitter, and the simulator does not model contention or queueing inside the exchange gateway.
- Feed timestamps are the venue timestamps. The simulator measures market data delay from them. Real receive times are different and not in these files.
- The venue is assumed price-time priority with the rules of MATCHING_RULES.md. Pro-rata allocation, auctions, odd lots, minimum quantities, self-match prevention groups and order-type nuances of any real venue are not modeled.
- Fees are a flat amount per share by liquidity role. Fee tiers, exchange and regulatory fees, borrow costs for shorts, and financing are not modeled.
- The mark is the mid of the other participants' book. A real position could not be closed at the mid.
- A feed order that arrives at a price that crosses a resting strategy order trades with it as an incoming order (rule 6 of section 9). This displaces the feed order, counted as an anomaly; later feed messages that name a feed order consumed this way are rejected by the engine and counted. In the 1,000,000 message synthetic runs this affected tens of messages (66 anomalies and 24 rejects in the baseline). The replica of the feed is no longer exact after such an event.
- The model assumptions of Avellaneda-Stoikov (a Brownian mid, Poisson order arrivals whose intensity decays exponentially with distance) are not true of real markets. sigma estimation is crude and k is not calibrated.
- Results on synthetic data are not evidence of profitability, and results on a real historical feed would still be an optimistic or at best uncertain estimate for the reasons above.
- Not a live trading system: no gateway, no risk checks, no persistence.

## 9. How it is tested

tests/sim: strategy math against hand computed values and a randomized property test (own quotes never cross, always on the grid, passive); the latency timeline to the nanosecond; queue position and fills under latency and the Phase 1 rules; the market view excluding own orders; cancel races (a cancel that arrives after the fill is reported too late and the fill notice comes first); post-only rejects on arrival; the PnL decomposition on a case worked out by hand (30 shares bought at 99.98 against a 100.00 mid, a mid rise, a rebate: $0.60 + $0.30 + $0.03); exact PnL identity across six latency settings and fee settings; inventory limit at six latency settings and a feed that pushes inventory far past the limit without it; byte-identical CSV on repeated runs; CSV format.

A one-off manual mutation check (tools/mutation, suite sim) applies 13 hand written bugs to the simulator and strategy, among them ignoring each of the three latencies, flipping the sign of spread capture or fees, removing the inventory cap, leaving own orders in the market view, sending limit instead of post-only orders, and swapping the tie rule. All 13 are caught by the tests. Three of these were not caught on the first attempt (fill notice latency, the maker flag on fill notices, and the tie rule) and tests were added for them; a fourth, working shares not counted against the inventory room, first failed to compile as a mutant, was rewritten, and was then caught. See TESTING.md.
