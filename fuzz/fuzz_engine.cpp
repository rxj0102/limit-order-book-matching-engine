// Engine fuzz target: arbitrary bytes become an operation stream that is run
// through the engine and the reference book. Any event divergence, state
// divergence or invariant violation aborts, which libFuzzer reports as a crash
// and minimizes. Sanitizers catch memory errors on the same inputs.
#include <cstdio>
#include <cstdlib>

#include "diff_runner.hpp"
#include "engine_input.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const lobkit::fuzz::Input in = lobkit::fuzz::decode(data, size);
  const lobkit::testing::DiffResult r =
      lobkit::testing::run_differential(in.cfg, in.ops, /*state_every=*/32, /*invariant_every=*/8);
  if (!r.ok) {
    std::fprintf(stderr, "%s\n", r.message.c_str());
    std::abort();
  }
  return 0;
}
