// Opal: operation-aware hybrid lock.
//
// Reimplementation of Listings 1 and 2 of
//   Gupta, Sanchez Lopez, Laforet, Lozi, Kashyap.
//   "Operation-Aware Hybrid Locking for Modern In-Memory Indexes."
//   PVLDB 19(8): 1804-1817, 2026.  doi:10.14778/3811243.3811253
//
// The class is parameterised on the two design knobs the paper ablates, so a
// single implementation covers three of the evaluated locks:
//
//   OpalLock<true , false>  ~ OptiQL   optimistic reads, MCS writers, no batching
//   OpalLock<true , true >  = Opal     + function-pointer batching for updates
//   OpalLock<false, true >  = Opal-NOR batching, opportunistic reads disabled
//
#pragma once
#include "common.h"
#include <cassert>

namespace lk {

// Lock states (Listing 1, lines 1-2).
enum : uint8_t { G_UNLOCKED = 0, G_LOCKED = 1, G_LOCKED_OPT = 3 };

// Batch cap, keeps the combiner from starving waiters (paper, Section 2.3
// "Fairness and starvation"; the value is the one used in Listing 2).
constexpr uint32_t kWaitersToBatch = 16384;

using OpFn = bool (*)(void*);

// One queue node per thread (Listing 2, queue_node_struct). Unlike MCS/OptiQL
// this node is *not* passed to the release path, which is what allows a single
// node per thread even with nested SMO locking (paper, Section 4 "Queue node").
struct alignas(64) QNode {
  std::atomic<bool>     wait{false};       // spun on by the waiter
  std::atomic<bool>     processed{false};  // PRCSD / UNPRCSD
  std::atomic<uint16_t> next{0};           // qnode id + 1; 0 == NULL
  OpFn                  fn_ptr{nullptr};   // NULL => traditional queueing
  void*                 input_ptr{nullptr};
  bool                  tree_result{false};
};
static_assert(sizeof(QNode) == 64, "queue node should be one cache line");

extern QNode g_qnodes[kMaxThreads];

// Combining statistics, aggregated per thread by the benchmark.
extern thread_local uint64_t t_batched;   // critical sections run by a combiner
extern thread_local uint64_t t_batches;   // number of combining phases

template <bool OPPORTUNISTIC_READS, bool BATCHING>
class OpalLock {
 public:
  // Tells the index whether to route updates through execute_op().
  static constexpr bool kUseFnPtr = BATCHING;
  static constexpr bool kOptimistic = true;

  OpalLock() { w_.raw = 0; }
  OpalLock(const OpalLock&) = delete;

  // ---- Optimistic read lock (Listing 1, lines 10-15; paper Section 3.2.1) ----
  // A single 64-bit load gives state and version together, which is strictly
  // stronger than reading the two fields separately as the pseudocode does.
  bool read_lock(uint64_t& version) {
    uint64_t w = word().load(acq);
    version = w >> 32;
    uint8_t s = static_cast<uint8_t>(w & 0xFF);
    return s == G_UNLOCKED || s == G_LOCKED_OPT;
  }

  bool read_unlock(uint64_t version) {
    uint64_t w = word().load(acq);
    return (static_cast<uint8_t>(w & 0xFF) != G_LOCKED) && ((w >> 32) == version);
  }

  // Abandoning an optimistic read costs nothing; the pessimistic locks need the
  // hook, so the index calls it on every restart path.
  void read_abort(uint64_t) {}

  // ---- Write lock, traditional queueing (Listing 1, lines 17-51) ----
  // This is the path SMOs take: no batching, plain MCS-style handoff.
  void write_lock() {
    uint8_t exp = G_UNLOCKED;                                  // Fastpath
    if (st().compare_exchange_strong(exp, G_LOCKED, acq, rlx)) return;

    QNode& q = g_qnodes[t_tid];                                // Slowpath
    const uint16_t me = static_cast<uint16_t>(t_tid + 1);
    q.wait.store(true, rlx);
    q.next.store(0, rlx);
    q.fn_ptr = nullptr;

    // Phase 1A: join the queue and wait until notified.
    uint16_t qprev = tl().exchange(me, std::memory_order_acq_rel);
    if (qprev != 0) {
      g_qnodes[qprev - 1].next.store(me, rel);
      while (q.wait.load(acq)) cpu_pause();
    }
    acquire_global_and_handoff(q, me);
  }

  // Listing 1, lines 49-51. Bumping the version before unlocking is what makes
  // concurrent optimistic readers fail validation.
  void write_unlock() {
    ver().fetch_add(1, rel);
    st().store(G_UNLOCKED, rel);
  }

  // ---- Write lock with function pointer (Listing 2; paper Section 3.2.3) ----
  // Returns the critical section's own result. false means the index should
  // restart its traversal.
  bool execute_op(OpFn fn, void* input) {
    QNode& q = g_qnodes[t_tid];
    q.fn_ptr = fn;
    q.input_ptr = input;

    uint8_t exp = G_UNLOCKED;                                  // Fastpath
    if (st().compare_exchange_strong(exp, G_LOCKED, acq, rlx))
      return exec_cs_and_release(q);

    const uint16_t me = static_cast<uint16_t>(t_tid + 1);      // Slowpath
    q.wait.store(true, rlx);
    q.processed.store(false, rlx);
    q.next.store(0, rlx);

    // Phase 2A: busy-waiting.
    uint16_t qprev = tl().exchange(me, std::memory_order_acq_rel);
    if (qprev != 0) {
      g_qnodes[qprev - 1].next.store(me, rel);
      while (q.wait.load(acq)) cpu_pause();
      if (q.processed.load(acq)) return q.tree_result;  // a combiner ran it
    }

    // Phase 2B: acquire the global TAS lock.
    acquire_global();

    // Phase 2C: combining-role decision.
    uint16_t exp_tail = me;
    if (tl().load(acq) == me &&
        tl().compare_exchange_strong(exp_tail, 0, std::memory_order_acq_rel, rlx)) {
      disable_opportunistic_reads();
      return exec_cs_and_release(q);                   // only one in the queue
    }
    uint16_t nx;
    while ((nx = q.next.load(acq)) == 0) cpu_pause();  // wait for the successor
    QNode* qnext = &g_qnodes[nx - 1];

    if (BATCHING && !check_batching_condition(qnext)) {
      // Phase 2D: combining. Batch requests while the leaf's cache line stays
      // resident in this core's L1 (paper, Section 3.2.3 Phase 4).
      uint32_t counter = 0;
      for (;;) {
        QNode* qcurr = qnext;
        qnext = node_of(qcurr->next.load(acq));
        ++counter;
        disable_opportunistic_reads();
        exec_cs(*qcurr);
        enable_opportunistic_reads();
        qcurr->processed.store(true, rel);
        qcurr->wait.store(false, rel);
        if (check_batching_condition(qnext) || counter >= kWaitersToBatch) break;
      }
      t_batched += counter;
      ++t_batches;
    }
    // The node we stopped at is never one we executed, so it is safe to hand
    // the queue head to it.
    qnext->wait.store(false, rel);
    disable_opportunistic_reads();
    return exec_cs_and_release(q);                     // combiner runs last
  }

  uint32_t version_now() { return ver().load(rlx); }

 private:
  struct Word {
    union {
      uint64_t raw;
      struct {
        uint8_t  state;    // offset 0, 2 bits used
        uint8_t  pad;
        uint16_t tail;     // offset 2, 10 bits used: qnode id + 1
        uint32_t version;  // offset 4 (paper uses 52 bits of a 64-bit word)
      } f;
    };
  };
  alignas(8) Word w_;
  static_assert(sizeof(Word) == 8, "lock word must stay 8 bytes");

  std::atomic_ref<uint64_t> word() { return std::atomic_ref<uint64_t>(w_.raw); }
  std::atomic_ref<uint8_t>  st()   { return std::atomic_ref<uint8_t>(w_.f.state); }
  std::atomic_ref<uint16_t> tl()   { return std::atomic_ref<uint16_t>(w_.f.tail); }
  std::atomic_ref<uint32_t> ver()  { return std::atomic_ref<uint32_t>(w_.f.version); }

  static QNode* node_of(uint16_t id) { return id ? &g_qnodes[id - 1] : nullptr; }

  void enable_opportunistic_reads() {
    if (OPPORTUNISTIC_READS) st().store(G_LOCKED_OPT, rel);
  }
  void disable_opportunistic_reads() { st().store(G_LOCKED, rel); }

  // Phase 1B / 2B: spin until the lock word is free, then take it in the state
  // that lets readers keep going while we finish the queue bookkeeping.
  void acquire_global() {
    for (;;) {
      while (st().load(acq) != G_UNLOCKED) cpu_pause();
      uint8_t e = G_UNLOCKED;
      if (st().compare_exchange_weak(
              e, OPPORTUNISTIC_READS ? G_LOCKED_OPT : G_LOCKED, acq, rlx))
        return;
    }
  }

  // Phases 1B and 1C together, for the traditional (no function pointer) path.
  void acquire_global_and_handoff(QNode& q, uint16_t me) {
    acquire_global();
    uint16_t exp_tail = me;
    if (tl().load(acq) == me &&
        tl().compare_exchange_strong(exp_tail, 0, std::memory_order_acq_rel, rlx)) {
      disable_opportunistic_reads();
      return;
    }
    uint16_t nx;
    while ((nx = q.next.load(acq)) == 0) cpu_pause();
    g_qnodes[nx - 1].wait.store(false, rel);
    disable_opportunistic_reads();
  }

  // Listing 2, line 88. True means "do not combine": no successor, the waiter
  // uses traditional queueing, or it has no successor of its own yet. The last
  // clause is what guarantees the node we stop on was never executed by us.
  static bool check_batching_condition(QNode* qnext) {
    return qnext == nullptr || qnext->fn_ptr == nullptr ||
           qnext->next.load(acq) == 0;
  }

  // Executing a batched critical section must bump the version: a reader that
  // validates while opportunistic reads are re-enabled would otherwise miss a
  // write made earlier in the same batch. (Listing 2 leaves the helper
  // undefined; this is the only version consistent with Section 3.3.)
  void exec_cs(QNode& q) {
    q.tree_result = q.fn_ptr(q.input_ptr);
    ver().fetch_add(1, rel);
  }

  bool exec_cs_and_release(QNode& q) {  // Listing 2, lines 82-84
    bool r = q.fn_ptr(q.input_ptr);
    q.tree_result = r;
    ver().fetch_add(1, rel);
    st().store(G_UNLOCKED, rel);
    return r;
  }
};

using OptiQLLock = OpalLock<true, false>;
using OpalHybrid = OpalLock<true, true>;
using OpalNoR    = OpalLock<false, true>;

} // namespace lk
