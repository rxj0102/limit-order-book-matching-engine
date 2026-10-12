#include "test_support.hpp"
#include "workload.hpp"

using namespace lobkit;
using namespace lobkit::testing;

namespace {
struct Digest {
  std::uint64_t hash;
  std::uint64_t events;
};

Digest run(std::uint64_t seed, std::size_t n, bool feed) {
  Engine<HashSink> e(small_cfg(8192));
  WorkloadParams p;
  p.seed = seed;
  p.n = n;
  p.feed_ops = feed;
  for (const Op& op : make_workload(p)) apply(e, op);
  return {e.sink().hash, e.sink().count};
}
}  // namespace

TEST(Determinism, SameStreamGivesIdenticalEventBytes) {
  const Digest a = run(42, 200000, true);
  const Digest b = run(42, 200000, true);
  EXPECT_EQ(a.hash, b.hash);
  EXPECT_EQ(a.events, b.events);
  EXPECT_GT(a.events, 200000u);
}

TEST(Determinism, DifferentStreamsDiffer) {
  EXPECT_NE(run(42, 20000, true).hash, run(43, 20000, true).hash);
}

// Golden values pin the whole behavior: any change to matching semantics, event
// layout or ordering changes them and must be a conscious decision. They must
// be identical under GCC and Clang, debug and release.
TEST(Determinism, GoldenHashes) {
  EXPECT_EQ(run(42, 200000, false).hash, 0xe14308c86488b287ull) << "plain";
  EXPECT_EQ(run(42, 200000, true).hash, 0x6e76bc207d5d5d24ull) << "with feed ops";
}
