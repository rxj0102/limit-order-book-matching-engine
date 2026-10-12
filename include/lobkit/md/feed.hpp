#pragma once

#include "lobkit/book/types.hpp"

namespace lobkit::md {

// What a parsed market data message asks the book to do. Both parsers produce
// this one record, so the replay code does not care which wire format it reads.
enum class FeedType : std::uint8_t {
  Add,            // new displayed limit order
  Reduce,         // partial cancel: qty is the quantity removed
  Delete,         // remove the whole order
  Execute,        // displayed order executed for qty
  Replace,        // ITCH U: delete id, add new_id with price and qty on the same side
  ExecuteHidden,  // execution of a non-displayed order: does not touch the visible book
  Trade,          // ITCH P: trade of a non-displayed order: does not touch the visible book
  CrossTrade,     // LOBSTER type 6: auction cross, not applied
  Halt            // LOBSTER type 7: trading halt indicator; price holds the raw halt code
};

struct FeedMsg {
  Timestamp ts;  // nanoseconds since midnight
  OrderId id;
  OrderId new_id;  // Replace only
  Price price;     // native fixed point, dollars * 10000 for both LOBSTER and ITCH
  Qty qty;
  Side side;
  FeedType type;
  std::uint8_t aux;  // wire type code, kept for reports
};

}  // namespace lobkit::md
