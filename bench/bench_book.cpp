// Book engine benchmarks (Google Benchmark with manual time) and the std::map baseline under the
// same harness. All workloads are SYNTHETIC. See docs/PERFORMANCE.md for methodology.
//
// Each benchmark runs one iteration per repetition. A repetition does a warm-up run (discarded), a
// throughput run (one timer read around the batch; reported as items_per_second) and a latency run
// (a timer read around every operation; reported as p50/p90/p99/p999/max counters in nanoseconds).
// Use --benchmark_repetitions=N --benchmark_enable_random_interleaving=true --benchmark_out=FILE
// --benchmark_out_format=json and summarize with bench/summarize.py (median and range over
// repetitions). A single repetition is not a result.

#include <benchmark/benchmark.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

#include "book_harness.hpp"
#include "lobkit/book/sinks.hpp"

using namespace lobkit;
using namespace lobkit::bench;

namespace {

TimerInfo g_timer;
std::vector<std::unique_ptr<Scenario>> g_scenarios;

// Fails loudly if the baseline and the engine disagree on any event. Run before any benchmark: a
// baseline that computes something different would make the comparison meaningless.
template <class Book>
std::pair<std::uint64_t, std::uint64_t> hash_run(const Scenario& sc) {
  Book book = [&] {
    if constexpr (std::is_same_v<Book, Engine<HashSink>>)
      return Book(sc.cfg, HashSink{});
    else
      return Book(sc.cfg, HashSink{});
  }();
  for (const Op& op : sc.setup) testing::apply(book, op);
  for (const Op& op : sc.measured) testing::apply(book, op);
  return {book.sink().hash, book.sink().count};
}

bool check_equivalence(const Scenario& sc) {
  const auto a = hash_run<Engine<HashSink>>(sc);
  const auto b = hash_run<StdMapBook<HashSink>>(sc);
  if (a != b) {
    std::fprintf(
        stderr,
        "baseline differs from engine on scenario %s (hash %llx vs %llx, events %llu vs %llu)\n",
        sc.name.c_str(), static_cast<unsigned long long>(a.first),
        static_cast<unsigned long long>(b.first), static_cast<unsigned long long>(a.second),
        static_cast<unsigned long long>(b.second));
    return false;
  }
  return true;
}

template <class Kind>
void BM_Scenario(benchmark::State& state, const Scenario* sc) {
  for (auto _ : state) {
    (void)run_once<Kind>(*sc, nullptr);  // warm-up, discarded
    const RunStats thr = run_once<Kind>(*sc, nullptr);
    std::vector<std::uint64_t> lat;
    const RunStats lr = run_once<Kind>(*sc, &lat);
    const Percentiles p = percentiles_ns(lat, g_timer.tsc_per_ns);
    state.SetIterationTime(static_cast<double>(thr.ticks) / (g_timer.tsc_per_ns * 1e9));
    state.SetItemsProcessed(static_cast<std::int64_t>(thr.ops));
    state.counters["p50_ns"] = p.p50;
    state.counters["p90_ns"] = p.p90;
    state.counters["p99_ns"] = p.p99;
    state.counters["p999_ns"] = p.p999;
    state.counters["max_ns"] = p.max;
    state.counters["faults_thr"] = static_cast<double>(thr.faults);
    state.counters["faults_lat"] = static_cast<double>(lr.faults);
    state.counters["rejects"] = static_cast<double>(thr.rejects);
    state.counters["events"] = static_cast<double>(thr.events);
    state.counters["samples"] = static_cast<double>(lat.size());
  }
}

}  // namespace

int main(int argc, char** argv) {
  keep_heap_memory();
  int cpu = 2;
  if (const char* v = std::getenv("LOBKIT_BENCH_CPU")) cpu = std::atoi(v);
  const bool pinned = pin_to_cpu(cpu);

  using Maker = std::pair<std::string, std::function<Scenario()>>;
  const std::vector<Maker> makers = {{"add", make_add},
                                     {"cancel", make_cancel},
                                     {"modify_reduce", [] { return make_modify(false); }},
                                     {"modify_reprice", [] { return make_modify(true); }},
                                     {"match_1", [] { return make_match(1); }},
                                     {"match_4", [] { return make_match(4); }},
                                     {"match_16", [] { return make_match(16); }},
                                     {"match_64", [] { return make_match(64); }},
                                     {"mixed", make_mixed},
                                     {"mixed_strided_ids", make_mixed_strided},
                                     {"feed_replay", make_feed}};

  // Profiling mode for perf, valgrind and similar tools: LOBKIT_BENCH_PROFILE=engine/mixed:30
  // builds one scenario, runs its throughput pass 30 times (setup included) and exits, without
  // Google Benchmark.
  if (const char* pv = std::getenv("LOBKIT_BENCH_PROFILE")) {
    const std::string spec(pv);
    const auto colon = spec.rfind(':');
    const int n = colon == std::string::npos ? 10 : std::atoi(spec.c_str() + colon + 1);
    const std::string which = spec.substr(0, colon);
    for (const auto& [name, make] : makers) {
      const bool eng = which == "engine/" + name, base = which == "stdmap/" + name;
      if (!eng && !base) continue;
      const Scenario sc = make();
      for (int i = 0; i < n; ++i) {
        if (eng)
          (void)run_once<EngineKind>(sc, nullptr);
        else
          (void)run_once<StdMapKind>(sc, nullptr);
      }
      return 0;
    }
    std::fprintf(stderr, "unknown profile target %s\n", which.c_str());
    return 2;
  }
  g_timer = calibrate_timer();
  for (const auto& m : makers) g_scenarios.emplace_back(new Scenario(m.second()));

  bool skip_check = false;
  for (int i = 1; i < argc; ++i)
    if (std::string(argv[i]) == "--no-equivalence-check") skip_check = true;
  if (!skip_check) {
    for (const auto& s : g_scenarios)
      if (!check_equivalence(*s)) return 2;
    // The test workload with invalid input and all five order types, as a stronger check on the
    // baseline.
    testing::WorkloadParams wp;
    wp.seed = 99;
    wp.n = 400000;
    Scenario bad{"equivalence_bad_input", "", bench_config(), {}, {}};
    wp.base_price = bad.cfg.base_price;
    wp.tick_size = bad.cfg.tick_size;
    wp.num_levels = bad.cfg.num_levels;
    wp.bad_input = true;
    wp.feed_ops = false;
    bad.measured = testing::make_workload(wp);
    if (!check_equivalence(bad)) return 2;
    std::printf(
        "equivalence: baseline and engine event streams identical on all scenarios and on a 400k "
        "op workload with invalid input\n");
  }

  benchmark::AddCustomContext("data",
                              "SYNTHETIC (see scenario descriptions in bench/book_scenarios.hpp)");
  benchmark::AddCustomContext("pinned_cpu", pinned ? std::to_string(cpu) : "pinning failed");
  benchmark::AddCustomContext("tsc_per_ns", std::to_string(g_timer.tsc_per_ns));
  benchmark::AddCustomContext("tsc_per_ns_range", std::to_string(g_timer.tsc_per_ns_min) + " to " +
                                                      std::to_string(g_timer.tsc_per_ns_max));
  benchmark::AddCustomContext("tsc_invariant_cpuid", g_timer.cpuid_invariant ? "yes" : "no");
  benchmark::AddCustomContext("tsc_constant_nonstop_cpuinfo",
                              std::string(g_timer.cpuinfo_constant ? "constant " : "") +
                                  (g_timer.cpuinfo_nonstop ? "nonstop" : ""));
  benchmark::AddCustomContext("clocksource", g_timer.clocksource);
  benchmark::AddCustomContext(
      "timer_overhead_ns_min_median",
      std::to_string(g_timer.overhead_ns_min) + " " + std::to_string(g_timer.overhead_ns_median));
#ifdef LOBKIT_BENCH_FLAGS
  benchmark::AddCustomContext("flags", LOBKIT_BENCH_FLAGS);
#endif

  for (const auto& s : g_scenarios) {
    for (bool eng : {true, false}) {
      const std::string name = std::string(eng ? "engine/" : "stdmap/") + s->name;
      auto* b = eng ? benchmark::RegisterBenchmark(name.c_str(), BM_Scenario<EngineKind>, s.get())
                    : benchmark::RegisterBenchmark(name.c_str(), BM_Scenario<StdMapKind>, s.get());
      b->UseManualTime()->Iterations(1)->Unit(benchmark::kMillisecond);
    }
  }
  std::vector<char*> args(argv, argv + argc);
  std::vector<char*> filtered;
  for (char* a : args)
    if (std::string(a) != "--no-equivalence-check") filtered.push_back(a);
  int n = static_cast<int>(filtered.size());
  benchmark::Initialize(&n, filtered.data());
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
