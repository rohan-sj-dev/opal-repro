/*
 * C11 model of include/opal.h for the GenMC model checker
 * (https://github.com/MPI-SWS/genmc).
 *
 * Every atomic access and memory order below is copied from opal.h. The one
 * structural change: GenMC does not support mixed-size accesses to the same
 * location, so the 8-byte lock word is split in two.
 *
 *   sv   : state (low byte) + version (high 32 bits), one 64-bit atomic.
 *          Readers still observe state and version in a single load, which
 *          is the property opal.h relies on.
 *   tail : the MCS tail, a separate 16-bit atomic.
 *
 * A byte CAS on `state` becomes a CAS on `sv`. In opal.h only the lock holder
 * changes the version, and a state CAS only succeeds from G_UNLOCKED, so the
 * two agree except that the word CAS may fail spuriously when the version
 * moved. That only *adds* executions, so a property verified here also holds
 * for the byte-CAS code.
 *
 * Knobs (compile-time):
 *   OPP        1 = opportunistic reads enabled (Opal), 0 = Opal-NOR
 *   BATCH      1 = function-pointer combining enabled, 0 = OptiQL-style
 *   FIX_FENCES 1 = add the fences a seqlock needs under C11 (see README)
 */
#ifndef OPAL_MODEL_H
#define OPAL_MODEL_H

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifndef OPP
#define OPP 1
#endif
#ifndef BATCH
#define BATCH 1
#endif
#ifndef FIX_FENCES
#define FIX_FENCES 0
#endif
#ifndef MAX_THREADS
#define MAX_THREADS 5
#endif

#define G_UNLOCKED   0u
#define G_LOCKED     1u
#define G_LOCKED_OPT 3u
#define VER_ONE      (1ull << 32)
#define STATE_MASK   0xFFull

#define acq memory_order_acquire
#define rel memory_order_release
#define rlx memory_order_relaxed
#define acqrel memory_order_acq_rel

/* Small enough that GenMC explores every interleaving; opal.h uses 16384. */
#ifndef WAITERS_TO_BATCH
#define WAITERS_TO_BATCH 8
#endif

typedef bool (*opfn_t)(void *);

struct qnode {
    atomic_bool wait;
    atomic_bool processed;
    atomic_ushort next;   /* qnode id + 1; 0 == NULL */
    opfn_t fn_ptr;        /* plain field, as in opal.h */
    void *input_ptr;      /* plain field, as in opal.h */
    bool tree_result;     /* plain field, as in opal.h */
};

struct opal_lock {
    atomic_ullong sv;
    atomic_ushort tail;
};

static struct qnode g_qnodes[MAX_THREADS];

static inline unsigned st_of(unsigned long long w) { return (unsigned)(w & STATE_MASK); }

/* compare_exchange_strong(state, exp -> des, acq, rlx) on the state byte. */
static inline bool cas_state(struct opal_lock *L, unsigned exp, unsigned des)
{
    unsigned long long w = atomic_load_explicit(&L->sv, rlx);
    if (st_of(w) != exp)
        return false;
    unsigned long long nw = (w & ~STATE_MASK) | des;
    return atomic_compare_exchange_strong_explicit(&L->sv, &w, nw, acq, rlx);
}

/* st().store(s, rel). Only the lock holder writes sv while the lock is held. */
static inline void store_state(struct opal_lock *L, unsigned s)
{
    unsigned long long w = atomic_load_explicit(&L->sv, rlx);
    atomic_store_explicit(&L->sv, (w & ~STATE_MASK) | s, rel);
}

static inline unsigned load_state(struct opal_lock *L, memory_order mo)
{
    return st_of(atomic_load_explicit(&L->sv, mo));
}

/* ver().fetch_add(1, rel) */
static inline void bump_version(struct opal_lock *L)
{
    atomic_fetch_add_explicit(&L->sv, VER_ONE, rel);
}

static inline void enable_opportunistic_reads(struct opal_lock *L)
{
    if (OPP)
        store_state(L, G_LOCKED_OPT);
}

static inline void disable_opportunistic_reads(struct opal_lock *L)
{
    store_state(L, G_LOCKED);
}

/* Fence a C11 seqlock writer needs between taking the lock and writing the
 * protected data. opal.h has none: x86-TSO never reorders store-store, so the
 * code works on x86 without it. */
static inline void writer_entry_fence(void)
{
    if (FIX_FENCES)
        atomic_thread_fence(rel);
}

/* ---------------------------------------------------------- read side */

static inline bool read_lock(struct opal_lock *L, unsigned long long *version)
{
    unsigned long long w = atomic_load_explicit(&L->sv, acq);
    *version = w >> 32;
    unsigned s = st_of(w);
    return s == G_UNLOCKED || s == G_LOCKED_OPT;
}

static inline bool read_unlock(struct opal_lock *L, unsigned long long version)
{
    if (FIX_FENCES)
        atomic_thread_fence(acq);  /* order the data loads before validation */
    unsigned long long w = atomic_load_explicit(&L->sv, acq);
    return st_of(w) != G_LOCKED && (w >> 32) == version;
}

/* ------------------------------------------- Phase 1B / 2B: global TAS */

static inline void acquire_global(struct opal_lock *L)
{
    for (;;) {
        while (load_state(L, acq) != G_UNLOCKED)
            ;
        if (cas_state(L, G_UNLOCKED, OPP ? G_LOCKED_OPT : G_LOCKED))
            return;
    }
}

/* ------------------------------------- write lock, traditional queueing */

static inline void write_lock(struct opal_lock *L, int tid)
{
    if (cas_state(L, G_UNLOCKED, G_LOCKED)) {              /* fastpath */
        writer_entry_fence();
        return;
    }
    struct qnode *q = &g_qnodes[tid];
    const unsigned short me = (unsigned short)(tid + 1);
    atomic_store_explicit(&q->wait, true, rlx);
    atomic_store_explicit(&q->next, 0, rlx);
    q->fn_ptr = NULL;

    unsigned short qprev = atomic_exchange_explicit(&L->tail, me, acqrel);
    if (qprev != 0) {
        atomic_store_explicit(&g_qnodes[qprev - 1].next, me, rel);
        while (atomic_load_explicit(&q->wait, acq))
            ;
    }
    acquire_global(L);

    unsigned short exp_tail = me;
    if (atomic_load_explicit(&L->tail, acq) == me &&
        atomic_compare_exchange_strong_explicit(&L->tail, &exp_tail, 0, acqrel, rlx)) {
        disable_opportunistic_reads(L);
        writer_entry_fence();
        return;
    }
    unsigned short nx;
    while ((nx = atomic_load_explicit(&q->next, acq)) == 0)
        ;
    atomic_store_explicit(&g_qnodes[nx - 1].wait, false, rel);
    disable_opportunistic_reads(L);
    writer_entry_fence();
}

static inline void write_unlock(struct opal_lock *L)
{
    bump_version(L);
    store_state(L, G_UNLOCKED);
}

/* ------------------------------------- write lock with function pointer */

static inline struct qnode *node_of(unsigned short id)
{
    return id ? &g_qnodes[id - 1] : NULL;
}

static inline bool check_batching_condition(struct qnode *qnext)
{
    return qnext == NULL || qnext->fn_ptr == NULL ||
           atomic_load_explicit(&qnext->next, acq) == 0;
}

static inline void exec_cs(struct opal_lock *L, struct qnode *q)
{
    writer_entry_fence();
    q->tree_result = q->fn_ptr(q->input_ptr);
    bump_version(L);
}

static inline bool exec_cs_and_release(struct opal_lock *L, struct qnode *q)
{
    writer_entry_fence();
    bool r = q->fn_ptr(q->input_ptr);
    q->tree_result = r;
    bump_version(L);
    store_state(L, G_UNLOCKED);
    return r;
}

static inline bool execute_op(struct opal_lock *L, int tid, opfn_t fn, void *input)
{
    struct qnode *q = &g_qnodes[tid];
    q->fn_ptr = fn;
    q->input_ptr = input;

    if (cas_state(L, G_UNLOCKED, G_LOCKED))                /* fastpath */
        return exec_cs_and_release(L, q);

    const unsigned short me = (unsigned short)(tid + 1);   /* slowpath */
    atomic_store_explicit(&q->wait, true, rlx);
    atomic_store_explicit(&q->processed, false, rlx);
    atomic_store_explicit(&q->next, 0, rlx);

    /* Phase 2A */
    unsigned short qprev = atomic_exchange_explicit(&L->tail, me, acqrel);
    if (qprev != 0) {
        atomic_store_explicit(&g_qnodes[qprev - 1].next, me, rel);
        while (atomic_load_explicit(&q->wait, acq))
            ;
        if (atomic_load_explicit(&q->processed, acq))
            return q->tree_result;
    }

    /* Phase 2B */
    acquire_global(L);

    /* Phase 2C */
    unsigned short exp_tail = me;
    if (atomic_load_explicit(&L->tail, acq) == me &&
        atomic_compare_exchange_strong_explicit(&L->tail, &exp_tail, 0, acqrel, rlx)) {
        disable_opportunistic_reads(L);
        return exec_cs_and_release(L, q);
    }
    unsigned short nx;
    while ((nx = atomic_load_explicit(&q->next, acq)) == 0)
        ;
    struct qnode *qnext = &g_qnodes[nx - 1];

    if (BATCH && !check_batching_condition(qnext)) {
        /* Phase 2D */
        unsigned counter = 0;
        for (;;) {
            struct qnode *qcurr = qnext;
            qnext = node_of(atomic_load_explicit(&qcurr->next, acq));
            ++counter;
            disable_opportunistic_reads(L);
            exec_cs(L, qcurr);
            enable_opportunistic_reads(L);
            atomic_store_explicit(&qcurr->processed, true, rel);
            atomic_store_explicit(&qcurr->wait, false, rel);
            if (check_batching_condition(qnext) || counter >= WAITERS_TO_BATCH)
                break;
        }
    }
    atomic_store_explicit(&qnext->wait, false, rel);
    disable_opportunistic_reads(L);
    return exec_cs_and_release(L, q);
}

#endif /* OPAL_MODEL_H */
