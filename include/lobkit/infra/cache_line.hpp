#pragma once

#include <cstddef>

#ifndef LOBKIT_CACHE_LINE
#define LOBKIT_CACHE_LINE 128
#endif

namespace lobkit {

// Distance in bytes that keeps two independently written variables from
// interfering with each other across cores.
//
// This is deliberately NOT std::hardware_destructive_interference_size. That
// value is a compile-time guess that GCC reports as 64 for x86-64 and that
// changes with -mtune, which is an ABI hazard in headers (GCC warns about it).
// On many Intel cores the L2 spatial prefetcher pulls the adjacent line of a
// 128 byte aligned pair, so two variables 64 bytes apart can still ping-pong.
// The value used here is chosen from measurement, see docs/INFRA.md section 3
// and bench/bench_false_sharing.cpp, and can be overridden at configure time
// with -DLOBKIT_CACHE_LINE=<bytes>.
inline constexpr std::size_t kCacheLine = LOBKIT_CACHE_LINE;

static_assert(kCacheLine >= 64 && (kCacheLine & (kCacheLine - 1)) == 0,
              "cache line size must be a power of two of at least 64");

}  // namespace lobkit
