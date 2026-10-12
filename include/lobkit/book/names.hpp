#pragma once

#include <string>

#include "lobkit/book/types.hpp"

namespace lobkit {

const char* to_string(EventType t) noexcept;
const char* to_string(RejectReason r) noexcept;
const char* to_string(CancelReason r) noexcept;
const char* to_string(OrderType t) noexcept;
const char* to_string(Side s) noexcept;

// One line, human readable. Cold path, allocates.
std::string describe(const Event& e);

}  // namespace lobkit
