#pragma once
// Nasdaq TotalView-ITCH 5.0 decoder for the subset the book needs.
//
// Framing is the BinaryFILE layout: each message is preceded by a 2 byte big
// endian length that does not include itself. All integers are big endian;
// timestamps are 6 bytes (nanoseconds since midnight); prices are 4 byte
// integers with 4 implied decimals (dollars * 10000, the same scale as LOBSTER).
//
// Decoded message types (sizes include the leading type byte):
//   S system event 12      R stock directory 39     A add order 36
//   F add order with MPID 40                         E order executed 31
//   C order executed with price 36                   X order cancel 23
//   D order delete 19                                U order replace 35
//   P trade, non-cross 44
// Every other type is skipped using its length prefix and counted. A known type
// whose frame length differs from the fixed size above is an error (BadLength):
// ITCH sizes are fixed, so a different length means damage, and trusting it
// could desynchronize everything after.
//
// The decoder can filter to one stock locate. R messages are always inspected so
// a symbol can be resolved to its locate (find_locate).
//
// The layouts are from the public Nasdaq ITCH 5.0 specification. No real file
// has been decoded yet (see docs/MARKET_DATA.md); the unit tests use a
// hand-built fixture written from the same specification.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "lobkit/md/feed.hpp"
#include "lobkit/md/status.hpp"

namespace lobkit::md::itch {

namespace detail {

// Byte-wise big endian reads. Never an unaligned pointer cast, so no UB and no
// alignment fault on any target. Callers have already checked the bounds.
inline std::uint16_t be16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
inline std::uint64_t be48(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(be16(p)) << 32) | be32(p + 2);
}
inline std::uint64_t be64(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint64_t>(be32(p)) << 32) | be32(p + 4);
}

// Fixed sizes by type code; 0 for types this decoder does not interpret.
constexpr std::uint16_t known_size(std::uint8_t t) noexcept {
  switch (t) {
    case 'S':
      return 12;
    case 'R':
      return 39;
    case 'A':
      return 36;
    case 'F':
      return 40;
    case 'E':
      return 31;
    case 'C':
      return 36;
    case 'X':
      return 23;
    case 'D':
      return 19;
    case 'U':
      return 35;
    case 'P':
      return 44;
    default:
      return 0;
  }
}

}  // namespace detail

struct DecoderStats {
  std::uint64_t frames = 0;           // every length-prefixed frame consumed
  std::uint64_t skipped_unknown = 0;  // types outside the subset
  std::uint64_t other_locate = 0;     // known book messages for a different stock locate
  std::uint64_t system_events = 0;
  std::uint64_t directory_entries = 0;
  std::uint64_t delivered = 0;  // messages returned by next()
};

class Decoder {
 public:
  // locate_filter < 0 means deliver every stock locate.
  explicit Decoder(std::span<const std::uint8_t> data, std::int32_t locate_filter = -1) noexcept
      : data_(data), filter_(locate_filter) {}

  // Returns Ok with a book-relevant or trade message in out, End at a clean end
  // of input, or an error. Frames that are not delivered (skipped types, other
  // stocks, system events, directory entries) are consumed internally.
  ParseStatus next(FeedMsg& out) noexcept {
    using namespace detail;
    if (status_ != ParseStatus::Ok) return status_;
    for (;;) {
      if (pos_ == data_.size()) return status_ = ParseStatus::End;
      if (data_.size() - pos_ < 2) return status_ = ParseStatus::Truncated;  // half a length prefix
      const std::uint16_t len = be16(data_.data() + pos_);
      if (len == 0) return status_ = ParseStatus::ZeroLength;
      if (data_.size() - pos_ - 2 < len)
        return status_ = ParseStatus::Truncated;  // frame runs past the end
      const std::uint8_t* m = data_.data() + pos_ + 2;
      frame_offset_ = pos_;
      pos_ += 2u + len;
      ++stats_.frames;

      const std::uint8_t type = m[0];
      const std::uint16_t want = known_size(type);
      if (want == 0) {
        ++stats_.skipped_unknown;
        continue;
      }
      if (len != want) return status_ = ParseStatus::BadLength;
      const std::uint16_t locate = be16(m + 1);  // m[3..4] is the tracking number, unused
      const std::uint64_t ts = be48(m + 5);

      switch (type) {
        case 'S':
          ++stats_.system_events;
          continue;
        case 'R':
          ++stats_.directory_entries;
          continue;
        default:
          break;
      }
      if (filter_ >= 0 && locate != static_cast<std::uint16_t>(filter_)) {
        ++stats_.other_locate;
        continue;
      }

      FeedMsg f{};
      f.ts = ts;
      f.aux = type;
      switch (type) {
        case 'A':
        case 'F': {
          const std::uint8_t bs = m[19];
          if (bs != 'B' && bs != 'S') return status_ = ParseStatus::BadField;
          f.type = FeedType::Add;
          f.id = be64(m + 11);
          f.side = bs == 'B' ? Side::Bid : Side::Ask;
          f.qty = be32(m + 20);
          f.price = static_cast<Price>(be32(m + 32));
          break;
        }
        case 'E':
        case 'C':
          f.type = FeedType::Execute;
          f.id = be64(m + 11);
          f.qty = be32(m + 19);
          // C carries an execution price (m + 32) that may differ from the order
          // price; it is not needed to reduce the displayed order.
          break;
        case 'X':
          f.type = FeedType::Reduce;
          f.id = be64(m + 11);
          f.qty = be32(m + 19);
          break;
        case 'D':
          f.type = FeedType::Delete;
          f.id = be64(m + 11);
          break;
        case 'U':
          f.type = FeedType::Replace;
          f.id = be64(m + 11);
          f.new_id = be64(m + 19);
          f.qty = be32(m + 27);
          f.price = static_cast<Price>(be32(m + 31));
          break;
        default:  // 'P'
        {
          const std::uint8_t bs = m[19];
          if (bs != 'B' && bs != 'S') return status_ = ParseStatus::BadField;
          f.type = FeedType::Trade;
          f.id = be64(m + 11);
          f.side = bs == 'B' ? Side::Bid : Side::Ask;
          f.qty = be32(m + 20);
          f.price = static_cast<Price>(be32(m + 32));
          break;
        }
      }
      ++stats_.delivered;
      out = f;
      return ParseStatus::Ok;
    }
  }

  const DecoderStats& stats() const noexcept { return stats_; }
  ParseStatus status() const noexcept { return status_; }
  std::size_t offset() const noexcept { return pos_; }
  std::size_t frame_offset() const noexcept {
    return frame_offset_;
  }  // start of the last frame read

 private:
  std::span<const std::uint8_t> data_;
  std::int32_t filter_;
  std::size_t pos_ = 0;
  std::size_t frame_offset_ = 0;
  DecoderStats stats_{};
  ParseStatus status_ = ParseStatus::Ok;
};

// Scans R (stock directory) messages for a symbol. The symbol is the 8 byte
// right-padded field; pass it without padding. Returns the locate in out_locate
// and true if found. Stops at the first framing error.
inline bool find_locate(std::span<const std::uint8_t> data, const char* symbol,
                        std::uint16_t& out_locate) noexcept {
  using namespace detail;
  const std::size_t sl = std::strlen(symbol);
  if (sl == 0 || sl > 8) return false;
  std::size_t pos = 0;
  while (data.size() - pos >= 2) {
    const std::uint16_t len = be16(data.data() + pos);
    if (len == 0 || data.size() - pos - 2 < len) return false;
    const std::uint8_t* m = data.data() + pos + 2;
    pos += 2u + len;
    if (m[0] != 'R' || len != 39) continue;
    // stock symbol at offset 11, 8 bytes, space padded
    bool match = true;
    for (std::size_t i = 0; i < 8; ++i) {
      const std::uint8_t want = i < sl ? static_cast<std::uint8_t>(symbol[i]) : ' ';
      if (m[11 + i] != want) {
        match = false;
        break;
      }
    }
    if (match) {
      out_locate = be16(m + 1);
      return true;
    }
  }
  return false;
}

}  // namespace lobkit::md::itch
