#pragma once
// Fundamental value types and the event record. Everything here is a plain
// integer or POD so that the engine never touches floating point and event
// streams can be compared and hashed bytewise.

#include <cstdint>
#include <type_traits>

namespace lobkit {

// Native fixed-point price (for LOBSTER: dollars * 10000). int64 so that the
// grid arithmetic cannot overflow for any realistic instrument.
using Price = std::int64_t;
using Qty = std::uint32_t;
using OrderId = std::uint64_t;
using Seq = std::uint64_t;
// Caller supplied, copied into events. The engine never reads a clock.
using Timestamp = std::uint64_t;
using OwnerId = std::uint16_t;
// Index into the order pool or the price ladder. 32 bits halve link size
// compared to pointers, which is what lets an Order fit in 32 bytes.
using Index = std::uint32_t;

inline constexpr Index kNil = 0xFFFFFFFFu;
// Owner 0 is the market data feed. Any other owner is a simulated participant.
inline constexpr OwnerId kFeedOwner = 0;

enum class Side : std::uint8_t { Bid = 0, Ask = 1 };

constexpr Side opposite(Side s) noexcept {
  return s == Side::Bid ? Side::Ask : Side::Bid;
}

enum class OrderType : std::uint8_t { Limit = 0, Market, Ioc, Fok, PostOnly };

enum class EventType : std::uint8_t { Ack = 0, Reject, Trade, Cancel, Modify };

enum class RejectReason : std::uint8_t {
  None = 0,
  InvalidQty,
  PriceOutOfBand,
  PriceOffTick,
  DuplicateOrderId,
  UnknownOrderId,
  PoolExhausted,
  FokNotFillable,
  PostOnlyWouldCross,
  WrongOwner,
  kCount
};

enum class CancelReason : std::uint8_t {
  UserRequest = 0,
  IocRemainder,
  MarketRemainder,
  FeedReduce,
  FeedDelete,
};

// Trade event flag bits.
inline constexpr std::uint8_t kTradeSelf = 1;           // taker and maker share a non-feed owner
inline constexpr std::uint8_t kTradeExternalTaker = 2;  // taker is a feed aggressor, id is 0

struct NewOrder {
  OrderId id = 0;
  Price price = 0;  // ignored for Market
  Qty qty = 0;
  Side side = Side::Bid;
  OrderType type = OrderType::Limit;
  OwnerId owner = kFeedOwner;
  Timestamp ts = 0;
};

struct ModifyOrder {
  OrderId id = 0;
  Price new_price = 0;
  Qty new_qty = 0;  // new total remaining quantity, not a delta
  Timestamp ts = 0;
};

// 48 bytes, no padding, so the bytes are a canonical encoding of the event.
// The determinism test hashes them directly.
struct Event {
  Seq seq;
  Timestamp ts;
  OrderId id;     // Ack/Reject/Cancel/Modify: the order. Trade: the taker (0 if external).
  OrderId other;  // Trade: the maker. Otherwise 0.
  Price price;    // native units
  Qty qty;
  EventType type;
  std::uint8_t reason;  // Reject: RejectReason. Cancel: CancelReason. Ack: OrderType.
  Side side;            // Trade: aggressor side. Otherwise the order side.
  std::uint8_t flags;   // Trade: kTrade* bits
};
static_assert(sizeof(Event) == 48);
static_assert(std::has_unique_object_representations_v<Event>,
              "Event must have no padding: it is hashed bytewise");

}  // namespace lobkit
