// Shared primitives for the Opal reproduction.
#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
static inline void cpu_pause() { _mm_pause(); }
static inline uint64_t rdtsc() { return __rdtsc(); }
#else
static inline void cpu_pause() { std::atomic_thread_fence(std::memory_order_seq_cst); }
static inline uint64_t rdtsc() { return 0; }
#endif

namespace lk {

// Opal's lock word packs the tail into 10 bits, so 1024 threads is the ceiling
// (paper, Section 4 "Lock structure").
constexpr int kMaxThreads = 1024;

// Thread id, registered once per worker. Queue nodes live in a global array and
// are addressed by this id, which is what lets the 8-byte lock word hold a tail
// pointer in 10 bits (paper, Section 4 "Queue node").
extern thread_local int t_tid;
void register_thread(int id);

constexpr std::memory_order acq = std::memory_order_acquire;
constexpr std::memory_order rel = std::memory_order_release;
constexpr std::memory_order rlx = std::memory_order_relaxed;

} // namespace lk
