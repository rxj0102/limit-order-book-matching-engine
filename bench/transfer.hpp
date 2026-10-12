#pragma once
// One saturated producer to consumer transfer, shared by the Google Benchmark
// throughput suite and the interleaved comparison tool.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

#include "common.hpp"

namespace lobkit::bench {

template <std::size_t N>
struct Payload {
  std::uint64_t seq;
  unsigned char pad[N - 8];
};
template <>
struct Payload<8> {
  std::uint64_t seq;
};

inline void relax() {
#if defined(__x86_64__)
  _mm_pause();
#endif
}

struct Transfer {
  double seconds = 0;
  bool in_order = true;
};

// Moves `items` payloads from the calling thread (pinned to cpu_p) to a new
// thread (pinned to cpu_c) through a fresh queue of `capacity` slots, in
// batches of `batch` (1 means the single-item calls). The consumer verifies the
// sequence so a broken queue cannot post a good number.
template <class Q, class P>
Transfer run_transfer(std::size_t capacity, std::size_t batch, std::uint64_t items, int cpu_p,
                      int cpu_c) {
  Q q(capacity);
  std::atomic<bool> go{false}, bad{false};
  std::thread consumer([&] {
    pin_to_cpu(cpu_c);
    while (!go.load(std::memory_order_acquire)) relax();
    P buf[64];
    std::uint64_t expect = 0;
    while (expect < items) {
      const std::size_t got = (batch == 1) ? (q.try_pop(buf[0]) ? 1 : 0) : q.try_pop_n(buf, batch);
      if (got == 0) {
        relax();
        continue;
      }
      for (std::size_t i = 0; i < got; ++i)
        if (buf[i].seq != expect++) bad.store(true, std::memory_order_relaxed);
    }
  });
  pin_to_cpu(cpu_p);
  const auto t0 = std::chrono::steady_clock::now();
  go.store(true, std::memory_order_release);
  P buf[64] = {};
  std::uint64_t next = 0;
  while (next < items) {
    const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(batch, items - next));
    for (std::size_t i = 0; i < want; ++i) buf[i].seq = next + i;
    std::size_t sent = 0;
    while (sent < want) {
      const std::size_t k =
          (batch == 1) ? (q.try_push(buf[sent]) ? 1 : 0) : q.try_push_n(buf + sent, want - sent);
      if (k == 0) relax();
      sent += k;
    }
    next += want;
  }
  consumer.join();
  const auto t1 = std::chrono::steady_clock::now();
  return {std::chrono::duration<double>(t1 - t0).count(), !bad.load()};
}

}  // namespace lobkit::bench
