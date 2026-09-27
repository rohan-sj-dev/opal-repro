// Pessimistic baselines from Section 2.1 / Section 5 "Evaluated locks":
//   StdRWLock  - the platform's blocking reader-writer lock (the paper's STDRW /
//                pthread_rwlock_t). On Windows this is SRWLOCK: MinGW's
//                std::shared_mutex goes through winpthreads' rwlock, which lost
//                updates under this workload in our stress test.
//   MCSRWLock  - centralised atomic reader counter + MCS queue for writers
//
// Both expose the same interface as OpalLock so the index template compiles
// against either. read_lock()/read_unlock() really acquire and release here;
// read_unlock() returning true just means "traversal is still valid", which is
// unconditionally so for a pessimistic lock.
#pragma once
#include "opal.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <shared_mutex>
#endif

namespace lk {

struct alignas(64) MCSNode {
  std::atomic<bool> locked{false};
  std::atomic<uint16_t> next{0};
};
// SMOs hold several node locks at once, so a thread can be enqueued on more
// than one MCS lock at a time. Unlike Opal -- whose release path never touches
// the queue node, so one node per thread suffices -- a plain MCS lock is still
// reading its node in write_unlock(), so each nesting level needs its own.
constexpr int kMaxNesting = 48;
extern MCSNode g_mcs_nodes[kMaxThreads * kMaxNesting];
extern thread_local uint64_t t_mcs_inuse;  // bitmask of this thread's busy slots

class StdRWLock {
 public:
  static constexpr bool kUseFnPtr = false;
  static constexpr bool kOptimistic = false;

#ifdef _WIN32
  bool read_lock(uint64_t& v) { AcquireSRWLockShared(&m_); v = 0; return true; }
  bool read_unlock(uint64_t) { ReleaseSRWLockShared(&m_); return true; }
  void read_abort(uint64_t) { ReleaseSRWLockShared(&m_); }
  void write_lock() { AcquireSRWLockExclusive(&m_); }
  void write_unlock() { ReleaseSRWLockExclusive(&m_); }
#else
  bool read_lock(uint64_t& v) { m_.lock_shared(); v = 0; return true; }
  bool read_unlock(uint64_t) { m_.unlock_shared(); return true; }
  void read_abort(uint64_t) { m_.unlock_shared(); }
  void write_lock() { m_.lock(); }
  void write_unlock() { m_.unlock(); }
#endif
  bool execute_op(OpFn fn, void* in) {
    write_lock();
    bool r = fn(in);
    write_unlock();
    return r;
  }

 private:
#ifdef _WIN32
  SRWLOCK m_ = SRWLOCK_INIT;
#else
  std::shared_mutex m_;
#endif
};

// Writer-preference reader-writer lock: the read side is the centralised
// counter the paper blames for cache-line ping-pong on every tree level
// (Section 2.1), the write side is a plain MCS queue.
class MCSRWLock {
 public:
  static constexpr bool kUseFnPtr = false;
  static constexpr bool kOptimistic = false;

  bool read_lock(uint64_t& v) {
    v = 0;
    for (;;) {
      while (wlocked_.load(acq)) cpu_pause();
      readers_.fetch_add(1, std::memory_order_seq_cst);
      if (!wlocked_.load(std::memory_order_seq_cst)) return true;
      readers_.fetch_sub(1, rel);
    }
  }
  bool read_unlock(uint64_t) { readers_.fetch_sub(1, rel); return true; }
  void read_abort(uint64_t) { readers_.fetch_sub(1, rel); }

  void write_lock() {
    // Locks are not released in acquisition order during an SMO, so take the
    // lowest free slot rather than treating nesting as a stack.
    const int level = __builtin_ctzll(~t_mcs_inuse);
    t_mcs_inuse |= (1ull << level);
    const uint16_t slot = static_cast<uint16_t>(t_tid * kMaxNesting + level);
    MCSNode& me = g_mcs_nodes[slot];
    me.next.store(0, rlx);
    me.locked.store(true, rlx);
    uint16_t prev = tail_.exchange(static_cast<uint16_t>(slot + 1),
                                   std::memory_order_acq_rel);
    if (prev != 0) {
      g_mcs_nodes[prev - 1].next.store(static_cast<uint16_t>(slot + 1), rel);
      while (me.locked.load(acq)) cpu_pause();
    }
    wlocked_.store(1, std::memory_order_seq_cst);
    while (readers_.load(std::memory_order_seq_cst) != 0) cpu_pause();
    my_slot_ = slot;  // only ever written by the current holder
    my_level_ = level;
  }

  void write_unlock() {
    const uint16_t slot = my_slot_;
    t_mcs_inuse &= ~(1ull << my_level_);
    wlocked_.store(0, rel);
    MCSNode& me = g_mcs_nodes[slot];
    uint16_t nx = me.next.load(acq);
    if (nx == 0) {
      uint16_t exp = static_cast<uint16_t>(slot + 1);
      if (tail_.compare_exchange_strong(exp, 0, std::memory_order_acq_rel, rlx))
        return;
      while ((nx = me.next.load(acq)) == 0) cpu_pause();
    }
    g_mcs_nodes[nx - 1].locked.store(false, rel);
  }

  bool execute_op(OpFn fn, void* in) {
    write_lock();
    bool r = fn(in);
    write_unlock();
    return r;
  }

 private:
  std::atomic<int32_t> readers_{0};
  std::atomic<uint16_t> tail_{0};
  std::atomic<uint8_t> wlocked_{0};
  uint16_t my_slot_{0};  // queue slot of the current holder
  int my_level_{0};
};

} // namespace lk
