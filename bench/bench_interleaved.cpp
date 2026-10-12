// Round-robin comparison of queue variants. Each round runs every variant once,
// in the same order, so slow changes in the host (noisy neighbours, frequency,
// vCPU placement) hit all variants alike instead of whichever ran last. Reports
// the median, min and max items per second per variant over all rounds.
//
// Two groups:
//   headline : the SPSC ring, mutex queue and naive queue, batch 1 and 64
//   ablation : the ring with each design decision switched off in turn
// The ablation ring (AblationRing in queues.hpp) is a single-item copy of the
// production ring with switches, so the production ring is included as a
// control: if it and the ablation baseline disagree, the harness, not the design,
// is the explanation.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "lobkit/infra/spsc_ring.hpp"
#include "queues.hpp"
#include "transfer.hpp"

namespace {

using namespace lobkit;
using namespace lobkit::bench;
using P = Payload<8>;

struct Variant {
  std::string name;
  std::function<Transfer(int, int, std::uint64_t)> run;
  std::vector<double> rate;  // items per second, one entry per round
};

template <class Q>
Variant make(std::string name, std::size_t batch) {
  return {std::move(name),
          [batch](int a, int b, std::uint64_t items) {
            return run_transfer<Q, P>(4096, batch, items, a, b);
          },
          {}};
}

void run_group(const char* title, std::vector<Variant>& vs, int rounds, std::uint64_t items, int a,
               int b) {
  bool ok = true;
  for (int r = 0; r < rounds; ++r)
    for (Variant& v : vs) {
      const Transfer t = v.run(a, b, items);
      ok = ok && t.in_order;
      v.rate.push_back(static_cast<double>(items) / t.seconds);
    }
  std::printf("\n%s (%d interleaved rounds, %llu items per transfer)%s\n", title, rounds,
              static_cast<unsigned long long>(items), ok ? "" : "  ORDER VIOLATION");
  std::printf("%-44s %10s %10s %10s\n", "variant", "median M/s", "min", "max");
  for (Variant& v : vs) {
    std::sort(v.rate.begin(), v.rate.end());
    std::printf("%-44s %10.2f %10.2f %10.2f\n", v.name.c_str(), v.rate[v.rate.size() / 2] / 1e6,
                v.rate.front() / 1e6, v.rate.back() / 1e6);
  }
}

}  // namespace

int main() {
  int a, b;
  choose_cpus(a, b);
  const int rounds = std::getenv("LOBKIT_ROUNDS") ? std::atoi(std::getenv("LOBKIT_ROUNDS")) : 21;
  const std::uint64_t items = 2'000'000;
  std::printf("producer cpu %d, consumer cpu %d (%d visible), payload 8 bytes, capacity 4096\n", a,
              b, cpu_count());

  std::vector<Variant> head;
  for (std::size_t batch : {1u, 64u}) {
    const std::string s = " batch " + std::to_string(batch);
    head.push_back(make<SpscRing<P>>("spsc_ring" + s, batch));
    head.push_back(make<MutexQueue<P>>("mutex_queue" + s, batch));
    head.push_back(make<NaiveAtomicQueue<P>>("naive_atomic" + s, batch));
  }
  run_group("headline", head, rounds, items, a, b);

  std::vector<Variant> abl;
  abl.push_back(make<SpscRing<P>>("control: production SpscRing", 1));
  abl.push_back(
      make<AblationRing<P, 128, true, false>>("pad128, cached index, acquire/release", 1));
  abl.push_back(make<AblationRing<P, 64, true, false>>("pad64,  cached index, acquire/release", 1));
  abl.push_back(
      make<AblationRing<P, 8, true, false>>("no padding, cached index, acquire/release", 1));
  abl.push_back(
      make<AblationRing<P, 128, false, false>>("pad128, no cached index, acquire/release", 1));
  abl.push_back(make<AblationRing<P, 128, true, true>>("pad128, cached index, seq_cst", 1));
  abl.push_back(
      make<AblationRing<P, 8, false, true>>("no padding, no cache, seq_cst (naive layout)", 1));
  run_group("ablation, batch 1", abl, rounds, items, a, b);
  return 0;
}
