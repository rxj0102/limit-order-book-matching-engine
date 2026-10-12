#pragma once
// Comparison queues for the SPSC ring benchmarks. They are deliberately simple
// and share the ring storage scheme (power of two, masked indices), so the
// measured differences come from synchronization and cache layout, not from
// unrelated implementation choices. Both are correct SPSC (the stress test runs
// them too); they are slower, not wrong.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace lobkit::bench {

// A ring protected by a std::mutex. try_push_n and try_pop_n take the lock once
// for the whole batch, which is how a mutex queue is naturally batched.
template <class T>
class MutexQueue {
 public:
  explicit MutexQueue(std::size_t capacity) : mask_(check(capacity) - 1), slots_(new T[capacity]) {}

  bool try_push(const T& v) {
    std::lock_guard<std::mutex> g(m_);
    if (tail_ - head_ == mask_ + 1) return false;
    slots_[tail_++ & mask_] = v;
    return true;
  }
  bool try_pop(T& out) {
    std::lock_guard<std::mutex> g(m_);
    if (tail_ == head_) return false;
    out = slots_[head_++ & mask_];
    return true;
  }
  std::size_t try_push_n(const T* src, std::size_t n) {
    std::lock_guard<std::mutex> g(m_);
    const std::size_t k = std::min(n, mask_ + 1 - (tail_ - head_));
    for (std::size_t i = 0; i < k; ++i) slots_[tail_++ & mask_] = src[i];
    return k;
  }
  std::size_t try_pop_n(T* dst, std::size_t n) {
    std::lock_guard<std::mutex> g(m_);
    const std::size_t k = std::min(n, tail_ - head_);
    for (std::size_t i = 0; i < k; ++i) dst[i] = slots_[head_++ & mask_];
    return k;
  }
  std::size_t capacity() const { return mask_ + 1; }

 private:
  static std::size_t check(std::size_t c) {
    if (c < 2 || !std::has_single_bit(c)) throw std::invalid_argument("capacity");
    return c;
  }
  std::mutex m_;
  std::size_t head_ = 0, tail_ = 0;
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;
};

// What a first attempt at a lock-free SPSC queue usually looks like: both
// indices are plain std::atomic with the default seq_cst ordering, they sit next
// to each other (one cache line, written by two cores), and every operation
// loads the other side index. Batch operations are loops of single operations,
// because there is no cached view to amortize.
template <class T>
class NaiveAtomicQueue {
 public:
  explicit NaiveAtomicQueue(std::size_t capacity)
      : mask_(check(capacity) - 1), slots_(new T[capacity]) {}

  bool try_push(const T& v) {
    const std::size_t t = tail_.load();
    if (t - head_.load() == mask_ + 1) return false;
    slots_[t & mask_] = v;
    tail_.store(t + 1);
    return true;
  }
  bool try_pop(T& out) {
    const std::size_t h = head_.load();
    if (h == tail_.load()) return false;
    out = slots_[h & mask_];
    head_.store(h + 1);
    return true;
  }
  std::size_t try_push_n(const T* src, std::size_t n) {
    std::size_t i = 0;
    while (i < n && try_push(src[i])) ++i;
    return i;
  }
  std::size_t try_pop_n(T* dst, std::size_t n) {
    std::size_t i = 0;
    while (i < n && try_pop(dst[i])) ++i;
    return i;
  }
  std::size_t capacity() const { return mask_ + 1; }

 private:
  static std::size_t check(std::size_t c) {
    if (c < 2 || !std::has_single_bit(c)) throw std::invalid_argument("capacity");
    return c;
  }
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
  const std::size_t mask_;
  std::unique_ptr<T[]> slots_;
};

}  // namespace lobkit::bench

namespace lobkit::bench {

// The SPSC ring with each design decision switchable, to measure what each one
// is worth on a given machine. Pad is the alignment of the producer and consumer
// groups (8 means they are adjacent in one line), Cache enables the cached copy
// of the other side index, SeqCst replaces acquire/release by seq_cst. With
// Pad=128, Cache=true, SeqCst=false it is the production ring minus batching.
template <class T, std::size_t Pad, bool Cache, bool SeqCst>
class AblationRing {
  static constexpr auto kLoadOrder = SeqCst ? std::memory_order_seq_cst : std::memory_order_acquire;
  static constexpr auto kStoreOrder =
      SeqCst ? std::memory_order_seq_cst : std::memory_order_release;
  static constexpr auto kOwnOrder = SeqCst ? std::memory_order_seq_cst : std::memory_order_relaxed;

 public:
  explicit AblationRing(std::size_t capacity) : mask_(capacity - 1), slots_(new T[capacity]) {}

  bool try_push(const T& v) {
    const std::size_t t = tail_.load(kOwnOrder);
    if (!Cache || t - cached_head_ >= mask_ + 1) {
      cached_head_ = head_.load(kLoadOrder);
      if (t - cached_head_ >= mask_ + 1) return false;
    }
    slots_[t & mask_] = v;
    tail_.store(t + 1, kStoreOrder);
    return true;
  }
  bool try_pop(T& out) {
    const std::size_t h = head_.load(kOwnOrder);
    if (!Cache || h == cached_tail_) {
      cached_tail_ = tail_.load(kLoadOrder);
      if (h == cached_tail_) return false;
    }
    out = slots_[h & mask_];
    head_.store(h + 1, kStoreOrder);
    return true;
  }
  std::size_t try_push_n(const T* src, std::size_t n) {
    std::size_t i = 0;
    while (i < n && try_push(src[i])) ++i;
    return i;
  }
  std::size_t try_pop_n(T* dst, std::size_t n) {
    std::size_t i = 0;
    while (i < n && try_pop(dst[i])) ++i;
    return i;
  }

 private:
  alignas(Pad) std::atomic<std::size_t> tail_{0};
  std::size_t cached_head_ = 0;
  alignas(Pad) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_ = 0;
  alignas(Pad) const std::size_t mask_;
  std::unique_ptr<T[]> slots_;
};

}  // namespace lobkit::bench
