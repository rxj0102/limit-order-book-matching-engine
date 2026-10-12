#pragma once
// Timing and pinning helpers shared by the benchmarks. x86-64 Linux oriented;
// other platforms fall back to steady_clock and no pinning, and say so.

#include <pthread.h>
#include <sched.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#if defined(__x86_64__)
#include <x86intrin.h>
#endif

namespace lobkit::bench {

// Reads the time stamp counter. The lfence stops the CPU from executing rdtsc
// before earlier instructions finish, which is what we want when stamping the
// moment an item is created or received. On non-x86 it is a nanosecond clock.
inline std::uint64_t rdtsc() {
#if defined(__x86_64__)
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
#else
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
#endif
}

// TSC ticks per nanosecond, measured against steady_clock over ~100 ms. Only
// meaningful with an invariant TSC (constant_tsc and nonstop_tsc in cpuinfo).
inline double tsc_per_ns() {
  using clk = std::chrono::steady_clock;
  const auto t0 = clk::now();
  const std::uint64_t c0 = rdtsc();
  while (clk::now() - t0 < std::chrono::milliseconds(100)) {
  }
  const std::uint64_t c1 = rdtsc();
  const auto t1 = clk::now();
  return static_cast<double>(c1 - c0) /
         static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
}

// Pins the calling thread to one CPU. Returns false (and the caller should say
// so in its output) if the kernel refuses.
inline bool pin_to_cpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(static_cast<std::size_t>(cpu), &set);
  return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

inline int cpu_count() {
  return static_cast<int>(std::thread::hardware_concurrency());
}

// Two distinct CPUs for the producer and the consumer. Overridable with
// LOBKIT_BENCH_CPUS=a,b. This does not know the physical topology; the guest
// view of it is recorded in the results (see docs/INFRA.md).
inline void choose_cpus(int& a, int& b) {
  a = 1;
  b = 2;
  if (const char* v = std::getenv("LOBKIT_BENCH_CPUS")) std::sscanf(v, "%d,%d", &a, &b);
  const int n = std::max(cpu_count(), 1);
  a %= n;
  b %= n;
  if (a == b) b = (a + 1) % n;
}

inline double percentile(std::vector<std::uint64_t>& sorted, double p) {
  if (sorted.empty()) return 0;
  const std::size_t i =
      std::min(sorted.size() - 1, static_cast<std::size_t>(p * static_cast<double>(sorted.size())));
  return static_cast<double>(sorted[i]);
}

}  // namespace lobkit::bench
