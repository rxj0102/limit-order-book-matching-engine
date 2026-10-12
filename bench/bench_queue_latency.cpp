// One-way hand-off latency of each queue at a fixed, sub-saturation arrival rate.
//
// The producer creates one item every `period` ns (busy-waiting on the TSC) and
// stamps it at creation. With batch > 1 it holds items until a batch is full and
// then pushes them together, so the latency of the early items in a batch
// includes the wait for the batch to fill: that is the real price of batching,
// and it is why batch rows have larger latency than single-item rows. The
// consumer busy-polls and stamps each pop call. Latency is receive minus
// creation, in nanoseconds, from a TSC calibrated against steady_clock.
//
// Cross-core TSC skew would show up as negative differences; they are counted
// and printed (it should be zero).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"
#include "lobkit/infra/spsc_ring.hpp"
#include "queues.hpp"

namespace {

using namespace lobkit;
using namespace lobkit::bench;

struct Stamped {
  std::uint64_t seq;
  std::uint64_t t_create;  // when the item was generated
  std::uint64_t t_push;    // when the batch containing it was handed to the queue
};

inline void relax() {
#if defined(__x86_64__)
  _mm_pause();
#endif
}

struct Result {
  double p50, p90, p99, p999, max, mean;
  std::uint64_t negatives;
};
struct Both {
  Result e2e;      // receive - create: includes the wait for a batch to fill
  Result handoff;  // receive - push: the cost of the queue itself
};

template <class Q>
Both run_once(std::size_t batch, double period_ns, std::size_t n, std::size_t warmup, double tpn,
              int cpu_p, int cpu_c) {
  Q q(4096);
  std::vector<std::int64_t> lat(n, 0), lat_push(n, 0);
  std::atomic<bool> go{false};
  const auto period_ticks = static_cast<std::uint64_t>(period_ns * tpn);

  std::thread consumer([&] {
    pin_to_cpu(cpu_c);
    while (!go.load(std::memory_order_acquire)) relax();
    Stamped buf[64];
    std::size_t got_total = 0;
    while (got_total < n) {
      std::size_t got = (batch == 1) ? (q.try_pop(buf[0]) ? 1 : 0) : q.try_pop_n(buf, 64);
      if (got == 0) {
        relax();
        continue;
      }
      const std::uint64_t now = rdtsc();
      for (std::size_t i = 0; i < got; ++i) {
        lat[buf[i].seq] =
            static_cast<std::int64_t>(now) - static_cast<std::int64_t>(buf[i].t_create);
        lat_push[buf[i].seq] =
            static_cast<std::int64_t>(now) - static_cast<std::int64_t>(buf[i].t_push);
      }
      got_total += got;
    }
  });

  pin_to_cpu(cpu_p);
  go.store(true, std::memory_order_release);
  Stamped pending[64];
  std::size_t npend = 0;
  std::uint64_t deadline = rdtsc() + period_ticks * 10;
  for (std::size_t i = 0; i < n; ++i) {
    while (rdtsc() < deadline) relax();
    pending[npend++] = {i, rdtsc(), 0};
    // A producer that fell behind does not try to catch up: that would create a
    // burst and measure queueing, not hand-off.
    deadline = std::max(deadline + period_ticks, pending[npend - 1].t_create);
    if (npend == batch || i + 1 == n) {
      const std::uint64_t tp = rdtsc();
      for (std::size_t j = 0; j < npend; ++j) pending[j].t_push = tp;
      std::size_t sent = 0;
      while (sent < npend) {
        const std::size_t k = (batch == 1) ? (q.try_push(pending[sent]) ? 1 : 0)
                                           : q.try_push_n(pending + sent, npend - sent);
        if (k == 0) relax();
        sent += k;
      }
      npend = 0;
    }
  }
  consumer.join();

  auto summarize = [&](const std::vector<std::int64_t>& l) {
    std::vector<std::uint64_t> v;
    v.reserve(n);
    std::uint64_t neg = 0;
    double sum = 0;
    for (std::size_t i = warmup; i < n; ++i) {
      if (l[i] < 0) {
        ++neg;
        continue;
      }
      v.push_back(static_cast<std::uint64_t>(l[i]));
      sum += static_cast<double>(l[i]);
    }
    std::sort(v.begin(), v.end());
    auto ns = [&](double p) { return percentile(v, p) / tpn; };
    return Result{ns(0.50),
                  ns(0.90),
                  ns(0.99),
                  ns(0.999),
                  static_cast<double>(v.back()) / tpn,
                  sum / static_cast<double>(v.size()) / tpn,
                  neg};
  };
  return {summarize(lat), summarize(lat_push)};
}

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

struct Row {
  std::string text;
};

template <class Q>
void report(const char* name, std::size_t batch, double period_ns, std::size_t n, double tpn, int a,
            int b, std::vector<Row>& e2e_rows, std::vector<Row>& handoff_rows) {
  constexpr int kRuns = 5;
  std::vector<double> e[6], h[6];
  std::uint64_t neg = 0;
  for (int r = 0; r < kRuns; ++r) {
    const Both x = run_once<Q>(batch, period_ns, n, n / 10, tpn, a, b);
    const Result* rs[2] = {&x.e2e, &x.handoff};
    std::vector<double>* out[2] = {e, h};
    for (int k = 0; k < 2; ++k) {
      out[k][0].push_back(rs[k]->p50);
      out[k][1].push_back(rs[k]->p90);
      out[k][2].push_back(rs[k]->p99);
      out[k][3].push_back(rs[k]->p999);
      out[k][4].push_back(rs[k]->max);
      out[k][5].push_back(rs[k]->mean);
      neg += rs[k]->negatives;
    }
  }
  auto fmt = [&](std::vector<double>* v) {
    char buf[256];
    std::snprintf(buf, sizeof buf, "%-13s %5zu %8.0f %9.0f %9.0f %9.0f %10.0f %11.0f %9.0f", name,
                  batch, median(v[0]), median(v[1]), median(v[2]), median(v[3]), median(v[4]),
                  *std::max_element(v[4].begin(), v[4].end()), median(v[5]));
    return std::string(buf);
  };
  e2e_rows.push_back({fmt(e)});
  handoff_rows.push_back({fmt(h) + "  neg=" + std::to_string(neg)});
}

}  // namespace

int main() {
  int a, b;
  choose_cpus(a, b);
  const double tpn = tsc_per_ns();
  const double period_ns =
      std::getenv("LOBKIT_PERIOD_NS") ? std::atof(std::getenv("LOBKIT_PERIOD_NS")) : 2000.0;
  const std::size_t n = std::getenv("LOBKIT_LAT_ITEMS")
                            ? std::strtoull(std::getenv("LOBKIT_LAT_ITEMS"), nullptr, 10)
                            : 100000;

  // Cost of the timer itself, so readers can judge the lowest believable value.
  std::vector<std::uint64_t> d;
  for (int i = 0; i < 100000; ++i) {
    const auto t0 = rdtsc();
    const auto t1 = rdtsc();
    d.push_back(t1 - t0);
  }
  std::sort(d.begin(), d.end());
  std::printf("producer cpu %d, consumer cpu %d (%d visible); tsc %.4f GHz\n", a, b, cpu_count(),
              tpn);
  std::printf("timer overhead (lfence+rdtsc+lfence back to back): median %.0f ns, p99 %.0f ns\n",
              static_cast<double>(d[d.size() / 2]) / tpn,
              static_cast<double>(d[d.size() * 99 / 100]) / tpn);
  std::printf(
      "arrival period %.0f ns, %zu items per run, first 10%% discarded, 5 runs, median across "
      "runs\n\n",
      period_ns, n);
  std::vector<Row> e2e, handoff;
  for (std::size_t batch : {1u, 16u, 64u}) {
    report<SpscRing<Stamped>>("spsc_ring", batch, period_ns, n, tpn, a, b, e2e, handoff);
    report<MutexQueue<Stamped>>("mutex_queue", batch, period_ns, n, tpn, a, b, e2e, handoff);
    report<NaiveAtomicQueue<Stamped>>("naive_atomic", batch, period_ns, n, tpn, a, b, e2e, handoff);
  }
  const char* head =
      "queue         batch   p50 ns    p90 ns    p99 ns  p99.9 ns     max ns   worst max   mean ns";
  std::printf("end to end: receive minus creation (includes waiting for a batch to fill)\n%s\n",
              head);
  for (const Row& r : e2e) std::printf("%s\n", r.text.c_str());
  std::printf("\nhand off: receive minus the moment the batch was pushed (the queue itself)\n%s\n",
              head);
  for (const Row& r : handoff) std::printf("%s\n", r.text.c_str());
  return 0;
}
