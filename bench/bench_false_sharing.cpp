// Measures the distance at which two variables written by two cores stop
// interfering. Two pinned threads each increment their own counter as fast as
// they can; the counters are placed `stride` bytes apart. If they share a line
// (or a prefetched line pair) each write steals the line from the other core and
// throughput collapses. The smallest stride at which throughput recovers is the
// padding the ring needs. This is how kCacheLine was chosen, instead of trusting
// std::hardware_destructive_interference_size.
//
// Caveat that matters: this runs in a guest. The guest cannot see whether its
// vCPUs map to different physical cores, SMT siblings or the same core, and a
// hypervisor can hide prefetcher behaviour effects only partly. The result is a
// measurement of this machine, not a constant of the architecture.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <new>
#include <thread>
#include <vector>

#include "common.hpp"

namespace {

using lobkit::bench::pin_to_cpu;

double run_once(std::size_t stride, int cpu_a, int cpu_b, double seconds, bool rmw) {
  // One buffer, 4 KiB aligned; the two counters sit stride bytes apart.
  alignas(4096) static unsigned char buf[8192];
  auto* a = new (buf) std::atomic<std::uint64_t>(0);
  auto* b = new (buf + stride) std::atomic<std::uint64_t>(0);
  std::atomic<bool> go{false}, stop{false};
  std::uint64_t na = 0, nb = 0;
  auto body = [&](std::atomic<std::uint64_t>* c, int cpu, std::uint64_t& n) {
    pin_to_cpu(cpu);
    while (!go.load(std::memory_order_acquire)) {
    }
    std::uint64_t i = 0;
    // load + store, not fetch_add: a plain store keeps the line in Modified state
    // and makes the coherence cost, not the lock prefix, the thing measured.
    while (!stop.load(std::memory_order_relaxed)) {
      if (rmw) {
        // Locked read-modify-write: needs exclusive ownership of the line on every
        // operation and cannot be absorbed by the store buffer, so a shared line
        // is a far more sensitive probe than the plain store variant.
        for (int k = 0; k < 256; ++k) c->fetch_add(1, std::memory_order_relaxed);
      } else {
        for (int k = 0; k < 256; ++k)
          c->store(c->load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      }
      i += 256;
    }
    n = i;
  };
  std::thread ta(body, a, cpu_a, std::ref(na)), tb(body, b, cpu_b, std::ref(nb));
  go.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
  stop.store(true, std::memory_order_relaxed);
  ta.join();
  tb.join();
  return static_cast<double>(na + nb) / seconds / 2.0;  // per-thread increments per second
}

}  // namespace

int main() {
  int a, b;
  lobkit::bench::choose_cpus(a, b);
  std::printf("false sharing study: threads pinned to cpu %d and cpu %d, %d cpus visible\n", a, b,
              lobkit::bench::cpu_count());
#ifdef __cpp_lib_hardware_interference_size
  std::printf("std::hardware_destructive_interference_size = %zu (reported, not trusted)\n",
              std::hardware_destructive_interference_size);
#endif
  const std::size_t strides[] = {8, 16, 32, 56, 64, 72, 120, 128, 136, 192, 256, 512};
  constexpr int kReps = 7;
  for (bool rmw : {false, true}) {
    std::printf("\n%s\n", rmw ? "locked fetch_add (sensitive probe)" : "load + store (plain)");
    std::printf("%8s %18s %10s %10s\n", "stride", "M incr/s/thread", "vs 512", "min..max");
    std::vector<double> med, lo, hi;
    for (std::size_t s : strides) {
      std::vector<double> r;
      for (int i = 0; i < kReps; ++i) r.push_back(run_once(s, a, b, 0.25, rmw));
      std::sort(r.begin(), r.end());
      med.push_back(r[r.size() / 2]);
      lo.push_back(r.front());
      hi.push_back(r.back());
    }
    const double base = med.back();
    for (std::size_t i = 0; i < med.size(); ++i)
      std::printf("%8zu %18.1f %9.2fx %5.2f..%.2f\n", strides[i], med[i] / 1e6, med[i] / base,
                  lo[i] / base, hi[i] / base);
  }
  std::printf(
      "\n(median of %d runs of 0.25 s per stride; min..max relative to the stride 512 median)\n",
      kReps);
  return 0;
}
