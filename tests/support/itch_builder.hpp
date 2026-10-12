#pragma once
// Builds ITCH 5.0 frames byte by byte for decoder tests. Written independently of
// the decoder (explicit shifts, no shared helpers) so the tests do not just echo it.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lobkit::testing::itch {

using Bytes = std::vector<std::uint8_t>;

inline void put(Bytes& b, std::uint64_t v, int nbytes) {
  for (int i = nbytes - 1; i >= 0; --i)
    b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}
inline void put_stock(Bytes& b, const char* s) {
  std::size_t n = std::strlen(s);
  for (std::size_t i = 0; i < 8; ++i) b.push_back(i < n ? static_cast<std::uint8_t>(s[i]) : ' ');
}

// type, locate, tracking number, 6 byte timestamp
inline Bytes header(char type, std::uint16_t locate, std::uint64_t ts) {
  Bytes b;
  b.push_back(static_cast<std::uint8_t>(type));
  put(b, locate, 2);
  put(b, 0, 2);
  put(b, ts, 6);
  return b;
}
inline Bytes framed(const Bytes& payload) {
  Bytes f;
  put(f, payload.size(), 2);
  f.insert(f.end(), payload.begin(), payload.end());
  return f;
}
inline void append(Bytes& out, const Bytes& f) {
  out.insert(out.end(), f.begin(), f.end());
}

inline Bytes add(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, char bs,
                 std::uint32_t shares, std::uint32_t price, char type = 'A') {
  Bytes b = header(type, loc, ts);
  put(b, ref, 8);
  b.push_back(static_cast<std::uint8_t>(bs));
  put(b, shares, 4);
  put_stock(b, "TEST");
  put(b, price, 4);
  if (type == 'F') {
    for (char c : std::string("MPID")) b.push_back(static_cast<std::uint8_t>(c));
  }
  return b;
}
inline Bytes executed(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref,
                      std::uint32_t shares) {
  Bytes b = header('E', loc, ts);
  put(b, ref, 8);
  put(b, shares, 4);
  put(b, 77, 8);
  return b;
}
inline Bytes cancel(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref, std::uint32_t shares) {
  Bytes b = header('X', loc, ts);
  put(b, ref, 8);
  put(b, shares, 4);
  return b;
}
inline Bytes del(std::uint16_t loc, std::uint64_t ts, std::uint64_t ref) {
  Bytes b = header('D', loc, ts);
  put(b, ref, 8);
  return b;
}
inline Bytes replace(std::uint16_t loc, std::uint64_t ts, std::uint64_t old_ref,
                     std::uint64_t new_ref, std::uint32_t shares, std::uint32_t price) {
  Bytes b = header('U', loc, ts);
  put(b, old_ref, 8);
  put(b, new_ref, 8);
  put(b, shares, 4);
  put(b, price, 4);
  return b;
}

}  // namespace lobkit::testing::itch
