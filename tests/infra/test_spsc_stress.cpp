// Timed two-thread stress test. A producer emits a strictly increasing sequence
// and a consumer checks it, so reordering, loss, duplication and torn reads all
// show up. The capacities are tiny on purpose: full, empty and wraparound happen
// constantly. Duration per configuration is LOBKIT_STRESS_SECONDS (default 0.4).
// The same test runs the comparison queues, which both validates the benchmark
// baselines and confirms the harness can see a correct queue as correct.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

#include "lobkit/infra/spsc_ring.hpp"
#include "queues.hpp"

namespace {

using lobkit::SpscRing;
using lobkit::bench::MutexQueue;
using lobkit::bench::NaiveAtomicQueue;

struct Item {
  std::uint64_t seq;
  std::uint64_t check;  // function of seq: catches a half-written slot
};
constexpr std::uint64_t kMix = 0x9E3779B97F4A7C15ull;
inline Item make(std::uint64_t s) {
  return {s, (s * kMix) ^ 0xA5A5A5A5A5A5A5A5ull};
}
inline bool valid(const Item& i) {
  return i.check == ((i.seq * kMix) ^ 0xA5A5A5A5A5A5A5A5ull);
}

double stress_seconds() {
  if (const char* v = std::getenv("LOBKIT_STRESS_SECONDS")) return std::atof(v);
  return 0.4;
}

struct Outcome {
  std::uint64_t pushed = 0, popped = 0;
  std::string error;
};

template <class Q>
Outcome run(Q& q, bool batch, std::uint64_t seed) {
  std::atomic<bool> stop{false}, producer_done{false};
  std::atomic<std::uint64_t> pushed{0};
  Outcome out;

  std::thread producer([&] {
    std::uint64_t next = 0, rng = seed;
    Item buf[32];
    auto rnd = [&] { return rng = rng * 6364136223846793005ull + 1442695040888963407ull; };
    while (!stop.load(std::memory_order_relaxed)) {
      if (!batch) {
        if (q.try_push(make(next)))
          ++next;
        else
          std::this_thread::yield();
      } else {
        const std::size_t want = 1 + (rnd() >> 33) % 32;
        for (std::size_t i = 0; i < want; ++i) buf[i] = make(next + i);
        const std::size_t got = q.try_push_n(buf, want);
        next += got;  // a partial push keeps its prefix; the rest is regenerated
        if (got == 0) std::this_thread::yield();
      }
    }
    pushed.store(next, std::memory_order_relaxed);
    producer_done.store(true, std::memory_order_release);
  });

  std::thread consumer([&] {
    std::uint64_t expect = 0, rng = seed ^ 0xFFFF;
    Item buf[32];
    auto rnd = [&] { return rng = rng * 6364136223846793005ull + 1442695040888963407ull; };
    auto check = [&](const Item& it) {
      if (it.seq != expect || !valid(it)) {
        if (out.error.empty())
          out.error = "expected seq " + std::to_string(expect) + " got " + std::to_string(it.seq) +
                      (valid(it) ? "" : " (corrupt payload)");
        expect = it.seq;  // resynchronize so one fault is reported once
      }
      ++expect;
    };
    bool draining = false;
    for (;;) {
      std::size_t got = 0;
      if (!batch) {
        Item it;
        if (q.try_pop(it)) {
          check(it);
          got = 1;
        }
      } else {
        got = q.try_pop_n(buf, 1 + (rnd() >> 33) % 32);
        for (std::size_t i = 0; i < got; ++i) check(buf[i]);
      }
      if (got == 0) {
        // Only after the producer has finished (acquire) is an empty pop final.
        if (draining) break;
        draining = producer_done.load(std::memory_order_acquire);
        if (!draining) std::this_thread::yield();
      } else {
        draining = false;
      }
    }
    out.popped = expect;
  });

  std::this_thread::sleep_for(std::chrono::duration<double>(stress_seconds()));
  stop.store(true, std::memory_order_relaxed);
  producer.join();
  consumer.join();
  out.pushed = pushed.load(std::memory_order_relaxed);
  return out;
}

template <class Q>
void run_matrix() {
  for (std::size_t cap : {2u, 8u, 1024u}) {
    for (bool batch : {false, true}) {
      Q q(cap);
      const Outcome o = run(q, batch, 0x1234 + cap);
      EXPECT_TRUE(o.error.empty()) << "cap " << cap << " batch " << batch << ": " << o.error;
      EXPECT_EQ(o.pushed, o.popped) << "loss or duplication, cap " << cap << " batch " << batch;
      EXPECT_GT(o.pushed, 1000u) << "stress made no progress, cap " << cap;
    }
  }
}

}  // namespace

TEST(SpscStress, RingKeepsOrderWithoutLossOrDuplication) {
  run_matrix<SpscRing<Item>>();
}
TEST(SpscStress, MutexQueueBaselineIsCorrect) {
  run_matrix<MutexQueue<Item>>();
}
TEST(SpscStress, NaiveAtomicBaselineIsCorrect) {
  run_matrix<NaiveAtomicQueue<Item>>();
}
