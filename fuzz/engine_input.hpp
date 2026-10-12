#pragma once
// Decodes arbitrary bytes into a book configuration and a bounded operation
// stream. Every byte string is a valid input; the decoder never rejects, so the
// fuzzer spends its time on the engine and not on a format gate.
//
// Layout: byte 0 selects the grid and pool size, then 8 byte records:
//   [kind][side/type][owner][tick lo][tick hi][qty][id lo][id hi]
// Ids come from a 10 bit domain so cancels, modifies and duplicates hit live
// orders often. High bits of the tick field select off-grid prices on purpose.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "lobkit/book/engine.hpp"
#include "workload.hpp"

namespace lobkit::fuzz {

struct Input {
  BookConfig cfg;
  std::vector<testing::Op> ops;
};

inline Input decode(const std::uint8_t* data, std::size_t size) {
  Input in;
  const std::uint8_t sel = size ? data[0] : 0;
  // Small grids keep each execution fast and make collisions at the touch common.
  static constexpr struct {
    Price tick, base;
    std::uint32_t levels, pool;
  } kGrids[] = {{1, 0, 64, 16}, {100, 100000, 128, 64}, {5, 7000, 256, 32}, {1, 0, 128, 4096}};
  const auto& g = kGrids[sel & 3];
  in.cfg.tick_size = g.tick;
  in.cfg.base_price = g.base;
  in.cfg.num_levels = g.levels;
  in.cfg.max_orders = g.pool;

  constexpr std::size_t kRec = 8;
  constexpr std::size_t kMaxOps = 4096;
  Timestamp ts = 0;
  for (std::size_t off = 1; off + kRec <= size && in.ops.size() < kMaxOps; off += kRec) {
    const std::uint8_t* r = data + off;
    testing::Op op{};
    op.ts = ++ts;
    const unsigned tick_raw = static_cast<unsigned>(r[3]) | (static_cast<unsigned>(r[4]) << 8);
    // Mostly in band; two selector bits at the top occasionally push it out or off grid.
    std::int64_t tick = tick_raw % g.levels;
    Price price = g.base + g.tick * tick;
    switch ((tick_raw >> 14) & 3) {
      case 1:
        price += (g.tick > 1 ? 1 : 0);
        break;  // off tick (when a grid exists)
      case 2:
        price = g.base + g.tick * static_cast<Price>(g.levels) + (tick_raw & 7);
        break;  // above
      case 3:
        if (tick_raw & 0x100) price = g.base - 1 - (tick_raw & 7);
        break;  // below
      default:
        break;
    }
    const OrderId id = static_cast<OrderId>(r[6] | ((r[7] & 3) << 8));
    const Qty qty = r[5];  // zero is possible on purpose
    const OwnerId owner = static_cast<OwnerId>(r[2] & 3);
    const unsigned k = r[0] % 11;
    auto fill_order = [&](OrderType t) {
      op.order = NewOrder{id, price, qty, (r[1] & 1) ? Side::Ask : Side::Bid, t, owner, op.ts};
    };
    switch (k) {
      case 0:
        op.kind = testing::Op::Submit;
        fill_order(OrderType::Limit);
        break;
      case 1:
        op.kind = testing::Op::Submit;
        fill_order(OrderType::Market);
        break;
      case 2:
        op.kind = testing::Op::Submit;
        fill_order(OrderType::Ioc);
        break;
      case 3:
        op.kind = testing::Op::Submit;
        fill_order(OrderType::Fok);
        break;
      case 4:
        op.kind = testing::Op::Submit;
        fill_order(OrderType::PostOnly);
        break;
      case 5:
        op.kind = testing::Op::Cancel;
        op.id = id;
        break;
      case 6:
        op.kind = testing::Op::Modify;
        op.mod = ModifyOrder{id, price, qty, op.ts};
        break;
      case 7:
        op.kind = testing::Op::FeedAdd;
        fill_order(OrderType::Limit);
        op.order.owner = kFeedOwner;
        break;
      case 8:
        op.kind = testing::Op::FeedReduce;
        op.id = id;
        op.qty = qty;
        break;
      case 9:
        op.kind = testing::Op::FeedDelete;
        op.id = id;
        break;
      default:
        op.kind = testing::Op::FeedExecute;
        op.id = id;
        op.qty = qty;
        break;
    }
    in.ops.push_back(op);
  }
  return in;
}

}  // namespace lobkit::fuzz
