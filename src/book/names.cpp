#include "lobkit/book/names.hpp"

#include <cinttypes>
#include <cstdio>

namespace lobkit {

const char* to_string(EventType t) noexcept {
  switch (t) {
    case EventType::Ack:
      return "Ack";
    case EventType::Reject:
      return "Reject";
    case EventType::Trade:
      return "Trade";
    case EventType::Cancel:
      return "Cancel";
    case EventType::Modify:
      return "Modify";
  }
  return "?";
}

const char* to_string(RejectReason r) noexcept {
  switch (r) {
    case RejectReason::None:
      return "None";
    case RejectReason::InvalidQty:
      return "InvalidQty";
    case RejectReason::PriceOutOfBand:
      return "PriceOutOfBand";
    case RejectReason::PriceOffTick:
      return "PriceOffTick";
    case RejectReason::DuplicateOrderId:
      return "DuplicateOrderId";
    case RejectReason::UnknownOrderId:
      return "UnknownOrderId";
    case RejectReason::PoolExhausted:
      return "PoolExhausted";
    case RejectReason::FokNotFillable:
      return "FokNotFillable";
    case RejectReason::PostOnlyWouldCross:
      return "PostOnlyWouldCross";
    case RejectReason::WrongOwner:
      return "WrongOwner";
    case RejectReason::kCount:
      break;
  }
  return "?";
}

const char* to_string(CancelReason r) noexcept {
  switch (r) {
    case CancelReason::UserRequest:
      return "UserRequest";
    case CancelReason::IocRemainder:
      return "IocRemainder";
    case CancelReason::MarketRemainder:
      return "MarketRemainder";
    case CancelReason::FeedReduce:
      return "FeedReduce";
    case CancelReason::FeedDelete:
      return "FeedDelete";
  }
  return "?";
}

const char* to_string(OrderType t) noexcept {
  switch (t) {
    case OrderType::Limit:
      return "Limit";
    case OrderType::Market:
      return "Market";
    case OrderType::Ioc:
      return "Ioc";
    case OrderType::Fok:
      return "Fok";
    case OrderType::PostOnly:
      return "PostOnly";
  }
  return "?";
}

const char* to_string(Side s) noexcept {
  return s == Side::Bid ? "Bid" : "Ask";
}

std::string describe(const Event& e) {
  char buf[256];
  const char* detail = "";
  switch (e.type) {
    case EventType::Reject:
      detail = to_string(static_cast<RejectReason>(e.reason));
      break;
    case EventType::Cancel:
      detail = to_string(static_cast<CancelReason>(e.reason));
      break;
    case EventType::Ack:
      detail = to_string(static_cast<OrderType>(e.reason));
      break;
    default:
      break;
  }
  std::snprintf(buf, sizeof buf,
                "#%" PRIu64 " t=%" PRIu64 " %s %s id=%" PRIu64 " other=%" PRIu64 " px=%" PRId64
                " qty=%u flags=%u %s",
                e.seq, e.ts, to_string(e.type), to_string(e.side), e.id, e.other, e.price, e.qty,
                e.flags, detail);
  return buf;
}

}  // namespace lobkit
