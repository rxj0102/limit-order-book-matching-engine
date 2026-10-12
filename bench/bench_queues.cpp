// Saturated throughput of the SPSC ring against a mutex queue and a naive atomic
// queue, single item and batch, two payload sizes. A producer and a consumer
// thread, pinned to two CPUs, move kItems items through a 4096 slot queue as
// fast as they can. Reported as items per second by Google Benchmark (real time,
// median of repetitions). Latency is measured separately in bench_queue_latency
// because a saturated queue measures queueing delay, not hand-off cost.
//
// The benchmarks run one after another, so slow drift of the host (noisy
// neighbours, frequency) can bias comparisons between rows. bench_interleaved
// runs the same transfers round-robin to cancel that and is the better source
// for close comparisons.
#include <benchmark/benchmark.h>

#include "lobkit/infra/spsc_ring.hpp"
#include "queues.hpp"
#include "transfer.hpp"

namespace {

using namespace lobkit;
using namespace lobkit::bench;

constexpr std::uint64_t kItems = 2'000'000;
constexpr std::size_t kCapacity = 4096;

template <class Q, class P>
void Throughput(benchmark::State& st) {
  const auto batch = static_cast<std::size_t>(st.range(0));
  int cpu_p, cpu_c;
  choose_cpus(cpu_p, cpu_c);
  bool ok = true;
  for (auto _ : st) ok = ok && run_transfer<Q, P>(kCapacity, batch, kItems, cpu_p, cpu_c).in_order;
  if (!ok) st.SkipWithError("consumer saw out of order data");
  st.SetItemsProcessed(static_cast<std::int64_t>(st.iterations()) *
                       static_cast<std::int64_t>(kItems));
}

template <class P>
void register_all(const char* tag) {
  struct Q {
    const char* name;
    void (*fn)(benchmark::State&);
  };
  const Q qs[] = {{"spsc_ring", Throughput<SpscRing<P>, P>},
                  {"mutex_queue", Throughput<MutexQueue<P>, P>},
                  {"naive_atomic", Throughput<NaiveAtomicQueue<P>, P>}};
  for (const Q& q : qs)
    benchmark::RegisterBenchmark((std::string(q.name) + "/" + tag).c_str(), q.fn)
        ->Arg(1)
        ->Arg(16)
        ->Arg(64)
        ->UseRealTime()
        ->Iterations(2)
        ->Repetitions(7)
        ->ReportAggregatesOnly(true);
}

}  // namespace

int main(int argc, char** argv) {
  register_all<Payload<8>>("payload8");
  register_all<Payload<48>>("payload48");
  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
