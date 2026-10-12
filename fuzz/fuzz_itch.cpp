// ITCH fuzz target. Input byte 0 selects the stock locate filter (below 128: none,
// otherwise locate = low three bits); the rest is a BinaryFILE stream. The target
// decodes it, replays it into a small book, scans for a symbol, and checks the
// book invariants.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

#include "lobkit/book/invariants.hpp"
#include "lobkit/md/itch.hpp"
#include "lobkit/md/replay.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  using namespace lobkit;
  using namespace lobkit::md;
  if (size < 1) return 0;
  const std::int32_t locate = data[0] < 128 ? -1 : (data[0] & 7);
  const std::span<const std::uint8_t> stream(data + 1, size - 1);

  BookConfig cfg;
  cfg.tick_size = 100;
  cfg.base_price = 1400000;  // the committed fixture trades near 1500000
  cfg.num_levels = 1024;
  cfg.max_orders = 1024;

  Engine<NullSink> eng(cfg);
  itch::Decoder dec(stream, locate);
  ReplayCounters c;
  replay(eng, dec, c);
  if (auto err = check_invariants(eng.book())) {
    std::fprintf(stderr, "invariant violated after ITCH replay: %s\n", err->c_str());
    std::abort();
  }
  std::uint16_t loc;
  (void)itch::find_locate(stream, "AAPL", loc);
  return 0;
}
