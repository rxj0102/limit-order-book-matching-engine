#pragma once
// Single producer single consumer lock-free ring buffer.
//
// Exactly one thread may call the try_push* functions and exactly one thread
// (possibly a different one) may call the try_pop* functions. Nothing enforces
// that; violating it is a data race. Capacity is a power of two, fixed at
// construction. The ring never allocates after construction and never blocks:
// a full ring makes try_push fail, an empty ring makes try_pop fail, and the
// caller decides whether to spin, yield or drop.
//
// Design in one paragraph. Two monotonically increasing 64 bit counters, tail
// (next slot to write, owned by the producer) and head (next slot to read,
// owned by the consumer). They are never wrapped, only masked when indexing, so
// full is tail - head == capacity and empty is tail == head with no wasted slot
// and no ambiguity. Unsigned subtraction keeps that correct even when the
// counters themselves overflow, which the tests exercise. Each thread keeps a
// private cached copy of the other thread counter and only reloads the shared
// one when the cached value says the ring looks full or empty. In the steady
// state a thread therefore touches only its own cache line; the cross-core
// transfer happens once per wrap of the cached view instead of once per item.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>

#include "lobkit/infra/cache_line.hpp"

namespace lobkit {

template <class T>
class SpscRing {
  // Slots are copied with plain assignment and memcpy and are never destroyed
  // individually. Events and messages in this project are PODs.
  static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>,
                "SpscRing stores trivially copyable types only");

 public:
  // capacity must be a power of two and at least 2. initial_index exists for
  // tests: it starts both counters near the 64 bit limit to exercise overflow.
  explicit SpscRing(std::size_t capacity, std::size_t initial_index = 0)
      : mask_(check_capacity(capacity) - 1), slots_(allocate(capacity)) {
    tail_.store(initial_index, std::memory_order_relaxed);
    head_.store(initial_index, std::memory_order_relaxed);
    cached_head_ = initial_index;
    cached_tail_ = initial_index;
  }

  SpscRing(const SpscRing&) = delete;
  SpscRing& operator=(const SpscRing&) = delete;

  std::size_t capacity() const noexcept { return mask_ + 1; }

  // ---- producer side -----------------------------------------------------

  bool try_push(const T& v) noexcept {
    // Relaxed: tail_ is written only by this thread, so we read our own latest
    // value and no ordering with any other thread is involved.
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    if (t - cached_head_ >= capacity()) {
      // Acquire: pairs with the consumer release store of head_. It makes
      // everything the consumer did before releasing (in particular copying the
      // slot out) happen before our overwrite of that slot. With a weaker load
      // we could overwrite a slot the consumer is still reading.
      cached_head_ = head_.load(std::memory_order_acquire);
      if (t - cached_head_ >= capacity()) return false;
    }
    slots_[t & mask_] = v;
    // Release: publishes the slot write above. A consumer that observes the new
    // tail_ with an acquire load is guaranteed to see the stored value.
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Pushes up to n items and returns how many were pushed (0 if full). The
  // whole batch is published with one release store, so the consumer sees all
  // of it or none of it, and the cross-core traffic is paid once per batch.
  std::size_t try_push_n(const T* src, std::size_t n) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);  // own counter, see try_push
    std::size_t free = capacity() - (t - cached_head_);
    if (free < n) {
      cached_head_ = head_.load(std::memory_order_acquire);  // see try_push
      free = capacity() - (t - cached_head_);
    }
    const std::size_t k = std::min(n, free);
    if (k == 0) return 0;
    const std::size_t first = std::min(k, capacity() - (t & mask_));  // up to the physical end
    std::memcpy(slots_.get() + (t & mask_), src, first * sizeof(T));
    std::memcpy(slots_.get(), src + first, (k - first) * sizeof(T));  // wrapped remainder
    tail_.store(t + k, std::memory_order_release);  // publishes all k slots, see try_push
    return k;
  }

  // ---- consumer side -----------------------------------------------------

  bool try_pop(T& out) noexcept {
    // Relaxed: head_ is written only by this thread.
    const std::size_t h = head_.load(std::memory_order_relaxed);
    if (h == cached_tail_) {
      // Acquire: pairs with the producer release store of tail_. It makes the
      // slot contents written before that store visible to our read below.
      cached_tail_ = tail_.load(std::memory_order_acquire);
      if (h == cached_tail_) return false;
    }
    out = slots_[h & mask_];
    // Release: tells the producer the slot is free. Orders our read of the slot
    // above before the producer next write to it (the producer acquires head_).
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

  // Pops up to n items into dst and returns how many were popped (0 if empty).
  std::size_t try_pop_n(T* dst, std::size_t n) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);  // own counter, see try_pop
    std::size_t avail = cached_tail_ - h;
    if (avail < n) {
      cached_tail_ = tail_.load(std::memory_order_acquire);  // see try_pop
      avail = cached_tail_ - h;
    }
    const std::size_t k = std::min(n, avail);
    if (k == 0) return 0;
    const std::size_t first = std::min(k, capacity() - (h & mask_));
    std::memcpy(dst, slots_.get() + (h & mask_), first * sizeof(T));
    std::memcpy(dst + first, slots_.get(), (k - first) * sizeof(T));
    head_.store(h + k, std::memory_order_release);  // frees all k slots, see try_pop
    return k;
  }

  // ---- either side -------------------------------------------------------

  // A snapshot that may be stale by the time it is used; fine for statistics
  // and tests, not for deciding whether try_pop will succeed. Relaxed loads:
  // no data is accessed based on the result, so no synchronization is needed.
  std::size_t size_approx() const noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t d = t - h;
    return d > capacity() ? capacity() : d;  // the two loads are not atomic together
  }

 private:
  struct AlignedDeleter {
    void operator()(T* p) const noexcept { ::operator delete(p, std::align_val_t(kCacheLine)); }
  };

  static std::size_t check_capacity(std::size_t c) {
    if (c < 2 || !std::has_single_bit(c))
      throw std::invalid_argument("SpscRing capacity must be a power of two >= 2");
    return c;
  }
  static std::unique_ptr<T[], AlignedDeleter> allocate(std::size_t c) {
    // Cache line aligned so the first and last slots never share a line with an
    // unrelated heap object that another thread writes.
    void* p = ::operator new(c * sizeof(T), std::align_val_t(kCacheLine));
    return std::unique_ptr<T[], AlignedDeleter>(static_cast<T*>(p));
  }

  // Each group below lives on its own kCacheLine-aligned block, and the next
  // alignas pads the block out, so no two groups share a line.

  // Written by the producer, read by the consumer.
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
  // Producer private: the last value of head_ it saw. Same line as tail_ because
  // only the producer touches either, so they cost one line, not two.
  std::size_t cached_head_ = 0;

  // Written by the consumer, read by the producer.
  alignas(kCacheLine) std::atomic<std::size_t> head_{0};
  std::size_t cached_tail_ = 0;  // consumer private, mirror of cached_head_

  // Read only after construction, read by both threads: shared in every cache,
  // never invalidated, kept off the lines above so they do not drag it along.
  alignas(kCacheLine) const std::size_t mask_;
  std::unique_ptr<T[], AlignedDeleter> slots_;
};

}  // namespace lobkit
