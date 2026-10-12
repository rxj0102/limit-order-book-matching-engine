// LOBSTER fuzz target. Input: 2 byte little endian length M, then M bytes of
// message file text, then the rest as order book file text. The target parses
// both, replays the messages (plain and verified) into a small book, and checks
// the book invariants. Any crash, sanitizer report, hang or invariant failure is
// a finding.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "lobkit/book/invariants.hpp"
#include "lobkit/md/lobster.hpp"
#include "lobkit/md/replay.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace lobkit;
  using namespace lobkit::md;
  if (size < 2) return 0;
  std::size_t m_len = static_cast<std::size_t>(data[0]) | (static_cast<std::size_t>(data[1]) << 8);
  const std::size_t rest = size - 2;
  if (m_len > rest) m_len = rest;
  const std::string_view msgs(reinterpret_cast<const char*>(data) + 2, m_len);
  const std::string_view rows(reinterpret_cast<const char*>(data) + 2 + m_len, rest - m_len);

  BookConfig cfg;
  cfg.tick_size = 100;
  cfg.base_price = 0;
  cfg.num_levels = 1024;  // small grid: construction stays cheap per execution
  cfg.max_orders = 1024;

  {
    Engine<NullSink> eng(cfg);
    lobster::MessageReader rd(msgs);
    ReplayCounters c;
    replay(eng, rd, c);
    if (auto err = check_invariants(eng.book())) {
      std::fprintf(stderr, "invariant violated after plain replay: %s\n", err->c_str());
      std::abort();
    }
  }
  {
    std::uint32_t levels = lobster::detect_levels(rows);
    if (levels == 0) levels = 1;
    Engine<NullSink> eng(cfg);
    lobster::MessageReader rd(msgs);
    lobster::BookRowReader br(rows, levels);
    ReplayCounters c;
    (void)replay_verify(eng, rd, br, c);
    if (auto err = check_invariants(eng.book())) {
      std::fprintf(stderr, "invariant violated after verified replay: %s\n", err->c_str());
      std::abort();
    }
  }
  return 0;
}
