#pragma once
// Measurement harness for the book benchmarks.
//
// Per run of a scenario (see run_once):
//   * a fresh book is built and the scenario setup applied untimed (the book is warm),
//   * the measured operations are applied either with one timer read around the whole batch
//     (throughput pass) or with a timer read around every operation (latency pass),
//   * page faults taken inside the measured loop are counted and reported, because a first
//     touch of a 4 KiB page costs far more than any book operation. glibc is told to keep freed
//     memory in the heap (mallopt) so that the warm-up pass leaves pages mapped for the timed
//     pass; the count shows whether that worked.
// Timer: lfence; rdtsc; lfence (common.hpp). Converting to nanoseconds uses a TSC frequency
// calibrated against CLOCK_MONOTONIC_RAW, and the cost of reading the timer itself is measured
// and reported separately. It is NOT subtracted from the percentiles: per operation latencies
// below roughly 100 ns include a timer overhead of the size reported in the run context.

#include <malloc.h>
#include <sys/resource.h>
#include <time.h>

#if defined(__x86_64__)
#include <cpuid.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "book_scenarios.hpp"
#include "common.hpp"
#include "lobkit/book/engine.hpp"
#include "std_map_book.hpp"

namespace lobkit::bench {

// ---- sink: cheap, but not removable by the optimizer -----------------------------------------------
struct BenchSink {
  std::uint64_t events = 0;
  std::uint64_t qty = 0;
  void operator()(const Event& e) noexcept {
    ++events;
    qty += e.qty;
  }
};

// ---- timer calibration --------------------------------------------------------------------------------
struct TimerInfo {
  double tsc_per_ns = 0;
  double tsc_per_ns_min = 0, tsc_per_ns_max = 0;  // spread over three calibrations
  double overhead_ns_min = 0, overhead_ns_median = 0;
  bool cpuid_invariant = false;
  bool cpuinfo_constant = false, cpuinfo_nonstop = false;
  std::string clocksource;
};

inline double now_raw_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

inline TimerInfo calibrate_timer() {
  TimerInfo t;
#if defined(__x86_64__)
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (__get_cpuid(0x80000007, &a, &b, &c, &d)) t.cpuid_invariant = (d >> 8) & 1u;
#endif
  std::ifstream cpuinfo("/proc/cpuinfo");
  for (std::string line; std::getline(cpuinfo, line);) {
    if (line.rfind("flags", 0) == 0) {
      t.cpuinfo_constant = line.find(" constant_tsc") != std::string::npos;
      t.cpuinfo_nonstop = line.find(" nonstop_tsc") != std::string::npos;
      break;
    }
  }
  std::ifstream cs("/sys/devices/system/clocksource/clocksource0/current_clocksource");
  std::getline(cs, t.clocksource);
  double r[3];
  for (double& x : r) {
    const double t0 = now_raw_ns();
    const std::uint64_t c0 = rdtsc();
    while (now_raw_ns() - t0 < 150e6) {
    }
    const std::uint64_t c1 = rdtsc();
    const double t1 = now_raw_ns();
    x = static_cast<double>(c1 - c0) / (t1 - t0);
  }
  std::sort(r, r + 3);
  t.tsc_per_ns = r[1];
  t.tsc_per_ns_min = r[0];
  t.tsc_per_ns_max = r[2];
  std::vector<std::uint64_t> d2(200000);
  for (auto& x : d2) {
    const std::uint64_t t0 = rdtsc();
    const std::uint64_t t1 = rdtsc();
    x = t1 - t0;
  }
  std::sort(d2.begin(), d2.end());
  t.overhead_ns_min = static_cast<double>(d2.front()) / t.tsc_per_ns;
  t.overhead_ns_median = static_cast<double>(d2[d2.size() / 2]) / t.tsc_per_ns;
  return t;
}

inline void keep_heap_memory() {
  // Keep freed blocks in the heap instead of returning them to the kernel, so a later pass does
  // not take page faults again. 30 MiB is just under glibc's 32 MiB maximum for this setting.
  mallopt(M_MMAP_THRESHOLD, 30 * 1024 * 1024);
  mallopt(M_TRIM_THRESHOLD, 1 << 30);
  mallopt(M_TOP_PAD, 64 << 20);
}

inline std::uint64_t minor_faults() {
  rusage ru;
  getrusage(RUSAGE_THREAD, &ru);
  return static_cast<std::uint64_t>(ru.ru_minflt);
}

// ---- engines under test ---------------------------------------------------------------------------------------
struct EngineKind {
  static constexpr const char* name = "engine";
  using Book = Engine<BenchSink>;
  static Book make(const BookConfig& c) { return Book(c, BenchSink{}); }
  static std::uint64_t rejects(Book& b) {
    std::uint64_t n = 0;
    for (auto r : b.book().stats().rejects) n += r;
    return n;
  }
  static std::uint64_t events(Book& b) { return b.sink().events; }
};
struct StdMapKind {
  static constexpr const char* name = "stdmap";
  using Book = StdMapBook<BenchSink>;
  static Book make(const BookConfig& c) { return Book(c, BenchSink{}); }
  static std::uint64_t rejects(Book& b) { return b.rejects(); }
  static std::uint64_t events(Book& b) { return b.sink().events; }
};

struct RunStats {
  std::uint64_t ticks = 0;  // time in the measured loops, TSC ticks
  std::uint64_t ops = 0;
  std::uint64_t faults = 0;  // minor page faults inside the measured loops
  std::uint64_t rejects = 0;
  std::uint64_t events = 0;
};

// per_op != nullptr selects the latency pass and fills it with one tick delta per operation.
template <class Kind>
RunStats run_once(const Scenario& sc, std::vector<std::uint64_t>* per_op) {
  auto book = Kind::make(sc.cfg);
  RunStats rs;
  if (per_op) {
    per_op->clear();
    per_op->reserve(sc.measured.size() * static_cast<std::size_t>(sc.passes));
  }
  for (int p = 0; p < sc.passes; ++p) {
    for (const Op& op : sc.setup) testing::apply(book, op);
    const std::uint64_t f0 = minor_faults();
    if (per_op) {
      for (const Op& op : sc.measured) {
        const std::uint64_t t0 = rdtsc();
        testing::apply(book, op);
        const std::uint64_t t1 = rdtsc();
        per_op->push_back(t1 - t0);
        rs.ticks += t1 - t0;
      }
    } else {
      const std::uint64_t t0 = rdtsc();
      for (const Op& op : sc.measured) testing::apply(book, op);
      const std::uint64_t t1 = rdtsc();
      rs.ticks += t1 - t0;
    }
    rs.faults += minor_faults() - f0;
    rs.ops += sc.measured.size();
  }
  rs.rejects = Kind::rejects(book);
  rs.events = Kind::events(book);
  return rs;
}

struct Percentiles {
  double p50, p90, p99, p999, max;  // nanoseconds
};

inline Percentiles percentiles_ns(std::vector<std::uint64_t>& v, double tsc_per_ns) {
  std::sort(v.begin(), v.end());
  auto at = [&](double q) { return static_cast<double>(v[std::min(v.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.size())))]) / tsc_per_ns; };
  return {at(0.50), at(0.90), at(0.99), at(0.999), static_cast<double>(v.back()) / tsc_per_ns};
}

}  // namespace lobkit::bench
