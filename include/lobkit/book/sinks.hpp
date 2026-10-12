#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <vector>

#include "lobkit/book/types.hpp"

namespace lobkit {

// Any callable taking a const Event& qualifies. The engine is a template over
// the sink so the call is inlined: no virtual dispatch, no std::function.
template <class S>
concept EventSink = requires(S& s, const Event& e) { s(e); };

struct NullSink {
  void operator()(const Event&) noexcept {}
};

// Allocates. Tests and tools only, never on a path that must be allocation free.
struct VectorSink {
  std::vector<Event> events;
  void operator()(const Event& e) { events.push_back(e); }
};

struct CountingSink {
  std::array<std::uint64_t, 5> by_type{};
  std::uint64_t total = 0;
  void operator()(const Event& e) noexcept {
    ++by_type[static_cast<std::size_t>(e.type)];
    ++total;
  }
};

// FNV-1a over the raw event bytes. Not cryptographic: it only has to make an
// accidental divergence between two runs overwhelmingly visible. The bytes are
// little endian on every platform we build for, so golden hashes are portable
// across the compilers in CI.
struct HashSink {
  std::uint64_t hash = 14695981039346656037ull;
  std::uint64_t count = 0;
  void operator()(const Event& e) noexcept {
    const auto* p = reinterpret_cast<const unsigned char*>(&e);
    for (std::size_t i = 0; i < sizeof(Event); ++i) {
      hash ^= p[i];
      hash *= 1099511628211ull;
    }
    ++count;
  }
};

}  // namespace lobkit
