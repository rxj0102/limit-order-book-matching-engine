#pragma once

#include <cstdint>

namespace lobkit::md {

// Result of asking a reader for the next record. Readers never throw and never
// allocate; every failure is one of these codes, and after a failure the reader
// stays failed (further calls return the same code) so a caller cannot read past
// damage by accident.
enum class ParseStatus : std::uint8_t {
  Ok = 0,
  End,           // clean end of input
  Truncated,     // input ends in the middle of a record, or a frame claims more bytes than remain
  ZeroLength,    // a frame declares length 0
  BadLength,     // a frame length does not match the fixed size of its message type
  BadField,      // a field is missing, malformed, or has a value outside its domain
  BadType,       // unknown message type code where one must be known
  BadTimestamp,  // timestamp does not parse or does not fit
  Overflow,      // a number does not fit its destination
  BadLevelCount  // order book file level count is zero, not a multiple of 4 fields, or too large
};

const char* to_string(ParseStatus s) noexcept;

}  // namespace lobkit::md
