#pragma once
// LOBSTER file readers.
//
// Message file, one line per event, no header:
//     time,type,order_id,size,price,direction
//   time       seconds after midnight with up to 9 decimals, e.g. 34200.004241176
//   type       1 new limit order, 2 partial cancel (size = shares removed), 3 delete,
//              4 execution of a displayed order, 5 execution of a hidden order,
//              6 cross trade, 7 trading halt
//   price      dollars * 10000, signed
//   direction  1 buy order, -1 sell order (the side of the resting order)
//
// Order book file, one line per message, same order, no header. For level i
// (1 based): ask_price_i, ask_size_i, bid_price_i, bid_size_i. A missing level
// is price 9999999999 (ask) or -9999999999 (bid) with size 0.
//
// These layouts follow the public LOBSTER documentation as I know it. Real files
// have not been available to test against yet (see docs/MARKET_DATA.md), so the
// parser is strict: anything it does not recognize is an error with the line
// number, not a guess.
//
// Both readers work on a std::string_view (typically an mmap), never copy, never
// allocate and never throw. They accept \n and \r\n line ends.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "lobkit/md/feed.hpp"
#include "lobkit/md/status.hpp"

namespace lobkit::md::lobster {

inline constexpr std::int64_t kDummyAskPrice = 9999999999LL;
inline constexpr std::int64_t kDummyBidPrice = -9999999999LL;
inline constexpr std::uint32_t kMaxLevels = 50;

namespace detail {

struct Cursor {
  const char* p;
  const char* end;
};

// Unsigned decimal, at least one digit, no sign, no leading space.
inline ParseStatus parse_u64(Cursor& c, std::uint64_t& out) noexcept {
  if (c.p == c.end || *c.p < '0' || *c.p > '9') return ParseStatus::BadField;
  std::uint64_t v = 0;
  while (c.p != c.end && *c.p >= '0' && *c.p <= '9') {
    const auto d = static_cast<std::uint64_t>(*c.p - '0');
    if (v > (UINT64_MAX - d) / 10) return ParseStatus::Overflow;
    v = v * 10 + d;
    ++c.p;
  }
  out = v;
  return ParseStatus::Ok;
}

inline ParseStatus parse_i64(Cursor& c, std::int64_t& out) noexcept {
  bool neg = false;
  if (c.p != c.end && *c.p == '-') {
    neg = true;
    ++c.p;
  }
  std::uint64_t u = 0;
  if (const ParseStatus s = parse_u64(c, u); s != ParseStatus::Ok) return s;
  const auto limit = static_cast<std::uint64_t>(INT64_MAX) + (neg ? 1u : 0u);
  if (u > limit) return ParseStatus::Overflow;
  out = neg ? static_cast<std::int64_t>(0 - u) : static_cast<std::int64_t>(u);
  return ParseStatus::Ok;
}

// "34200.004241176" -> nanoseconds. Integer seconds, optional fraction of 1 to 9 digits.
inline ParseStatus parse_time_ns(Cursor& c, std::uint64_t& out) noexcept {
  std::uint64_t sec = 0;
  if (c.p == c.end || *c.p < '0' || *c.p > '9') return ParseStatus::BadTimestamp;
  while (c.p != c.end && *c.p >= '0' && *c.p <= '9') {
    sec = sec * 10 + static_cast<std::uint64_t>(*c.p - '0');
    if (sec > 9000000000ull) return ParseStatus::BadTimestamp;  // keeps sec * 1e9 inside uint64
    ++c.p;
  }
  std::uint64_t frac = 0;
  int digits = 0;
  if (c.p != c.end && *c.p == '.') {
    ++c.p;
    while (c.p != c.end && *c.p >= '0' && *c.p <= '9') {
      if (digits == 9) return ParseStatus::BadTimestamp;
      frac = frac * 10 + static_cast<std::uint64_t>(*c.p - '0');
      ++digits;
      ++c.p;
    }
    if (digits == 0) return ParseStatus::BadTimestamp;
  }
  for (; digits < 9; ++digits) frac *= 10;
  out = sec * 1000000000ull + frac;
  return ParseStatus::Ok;
}

inline bool take_comma(Cursor& c) noexcept {
  if (c.p != c.end && *c.p == ',') {
    ++c.p;
    return true;
  }
  return false;
}

// Splits off the next line. Returns false at end of input. A trailing \r is dropped.
struct Line {
  std::string_view text;
  bool terminated;  // had a \n
};
inline bool next_line(std::string_view all, std::size_t& pos, Line& out) noexcept {
  if (pos >= all.size()) return false;
  const char* base = all.data() + pos;
  const std::size_t left = all.size() - pos;
  const void* nl = std::memchr(base, '\n', left);
  std::size_t len;
  if (nl) {
    len = static_cast<std::size_t>(static_cast<const char*>(nl) - base);
    pos += len + 1;
    out.terminated = true;
  } else {
    len = left;
    pos += left;
    out.terminated = false;
  }
  if (len > 0 && base[len - 1] == '\r') --len;
  out.text = std::string_view(base, len);
  return true;
}

}  // namespace detail

// ---- message file ----------------------------------------------------------

class MessageReader {
 public:
  explicit MessageReader(std::string_view text) noexcept : text_(text) {}

  ParseStatus next(FeedMsg& out) noexcept {
    if (status_ != ParseStatus::Ok) return status_;
    detail::Line ln;
    if (!detail::next_line(text_, pos_, ln)) return status_ = ParseStatus::End;
    ++line_no_;
    line_ = ln.text;
    return status_ = parse_line(ln, out);
  }

  // Raw text of the line most recently read (also the offending one after an error).
  std::string_view last_line() const noexcept { return line_; }
  std::size_t line_number() const noexcept { return line_no_; }  // 1 based
  std::size_t offset() const noexcept { return pos_; }
  ParseStatus status() const noexcept { return status_; }

 private:
  static ParseStatus parse_line(const detail::Line& ln, FeedMsg& out) noexcept {
    using detail::take_comma;
    detail::Cursor c{ln.text.data(), ln.text.data() + ln.text.size()};
    // A last line cut off before its end is a truncation, not a malformed field.
    const ParseStatus short_status = ln.terminated ? ParseStatus::BadField : ParseStatus::Truncated;

    // Running out of input where a field was expected is Truncated on an
    // unterminated last line and BadField otherwise. Real errors (overflow, bad
    // timestamp) keep their own code.
    auto fail = [&](ParseStatus s) noexcept {
      return (s == ParseStatus::BadField && c.p == c.end) ? short_status : s;
    };

    std::uint64_t ts = 0, type = 0, id = 0, size = 0;
    std::int64_t price = 0, dir = 0;
    if (const auto s = detail::parse_time_ns(c, ts); s != ParseStatus::Ok) return s;
    if (!take_comma(c)) return fail(ParseStatus::BadField);
    if (const auto s = detail::parse_u64(c, type); s != ParseStatus::Ok) return fail(s);
    if (!take_comma(c)) return fail(ParseStatus::BadField);
    if (const auto s = detail::parse_u64(c, id); s != ParseStatus::Ok) return fail(s);
    if (!take_comma(c)) return fail(ParseStatus::BadField);
    if (const auto s = detail::parse_u64(c, size); s != ParseStatus::Ok) return fail(s);
    if (!take_comma(c)) return fail(ParseStatus::BadField);
    if (const auto s = detail::parse_i64(c, price); s != ParseStatus::Ok) return fail(s);
    if (!take_comma(c)) return fail(ParseStatus::BadField);
    if (const auto s = detail::parse_i64(c, dir); s != ParseStatus::Ok) return fail(s);
    if (c.p != c.end) return ParseStatus::BadField;  // trailing garbage or a seventh field

    if (type < 1 || type > 7) return ParseStatus::BadType;
    if (size > UINT32_MAX) return ParseStatus::Overflow;

    FeedMsg m{};
    m.ts = ts;
    m.id = id;
    m.price = price;
    m.qty = static_cast<Qty>(size);
    m.aux = static_cast<std::uint8_t>(type);
    switch (type) {
      case 1:
        m.type = FeedType::Add;
        break;
      case 2:
        m.type = FeedType::Reduce;
        break;
      case 3:
        m.type = FeedType::Delete;
        break;
      case 4:
        m.type = FeedType::Execute;
        break;
      case 5:
        m.type = FeedType::ExecuteHidden;
        break;
      case 6:
        m.type = FeedType::CrossTrade;
        break;
      default:
        m.type = FeedType::Halt;
        break;
    }
    if (type <= 5) {
      if (dir != 1 && dir != -1) return ParseStatus::BadField;
      m.side = dir == 1 ? Side::Bid : Side::Ask;
    } else {
      m.side = Side::Bid;  // meaningless for cross trades and halts; the raw value is not kept
    }
    out = m;
    return ParseStatus::Ok;
  }

  std::string_view text_;
  std::string_view line_;
  std::size_t pos_ = 0;
  std::size_t line_no_ = 0;
  ParseStatus status_ = ParseStatus::Ok;
};

// ---- order book file -------------------------------------------------------

struct BookLevel {
  std::int64_t price;
  std::int64_t size;
};

struct BookRow {
  std::uint32_t levels = 0;
  BookLevel ask[kMaxLevels];
  BookLevel bid[kMaxLevels];
};

// Number of price levels in a book file, from the first line (4 fields per
// level). 0 if the text is empty or the first line is not a multiple of 4 fields.
inline std::uint32_t detect_levels(std::string_view text) noexcept {
  std::size_t pos = 0;
  detail::Line ln;
  if (!detail::next_line(text, pos, ln) || ln.text.empty()) return 0;
  std::size_t fields = 1;
  for (char ch : ln.text)
    if (ch == ',') ++fields;
  if (fields % 4 != 0 || fields / 4 > kMaxLevels) return 0;
  return static_cast<std::uint32_t>(fields / 4);
}

class BookRowReader {
 public:
  BookRowReader(std::string_view text, std::uint32_t levels) noexcept
      : text_(text), levels_(levels) {
    if (levels == 0 || levels > kMaxLevels) status_ = ParseStatus::BadLevelCount;
  }

  ParseStatus next(BookRow& out) noexcept {
    if (status_ != ParseStatus::Ok) return status_;
    detail::Line ln;
    if (!detail::next_line(text_, pos_, ln)) return status_ = ParseStatus::End;
    ++line_no_;
    line_ = ln.text;
    detail::Cursor c{ln.text.data(), ln.text.data() + ln.text.size()};
    const ParseStatus short_status = ln.terminated ? ParseStatus::BadField : ParseStatus::Truncated;
    out.levels = levels_;
    for (std::uint32_t i = 0; i < levels_; ++i) {
      std::int64_t f[4];
      for (int k = 0; k < 4; ++k) {
        if (const auto s = detail::parse_i64(c, f[k]); s != ParseStatus::Ok)
          return status_ = (s == ParseStatus::BadField && c.p == c.end ? short_status : s);
        const bool last = i + 1 == levels_ && k == 3;
        if (!last && !detail::take_comma(c))
          return status_ = (c.p == c.end ? short_status : ParseStatus::BadField);
      }
      out.ask[i] = {f[0], f[1]};
      out.bid[i] = {f[2], f[3]};
    }
    if (c.p != c.end) return status_ = ParseStatus::BadField;
    return ParseStatus::Ok;
  }

  std::string_view last_line() const noexcept { return line_; }
  std::size_t line_number() const noexcept { return line_no_; }
  ParseStatus status() const noexcept { return status_; }
  std::uint32_t levels() const noexcept { return levels_; }

 private:
  std::string_view text_;
  std::string_view line_;
  std::uint32_t levels_;
  std::size_t pos_ = 0;
  std::size_t line_no_ = 0;
  ParseStatus status_ = ParseStatus::Ok;
};

}  // namespace lobkit::md::lobster
