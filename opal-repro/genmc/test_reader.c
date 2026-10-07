/* Optimistic read validation (Listing 1, read_lock/read_unlock).
 * The writer stores the same value to x and y inside one critical section.
 * A reader whose read_unlock() succeeds must therefore never see x != y.
 *
 * x and y are relaxed atomics. In opal.h / btree.h the node contents are plain
 * memory, which C11 would already flag as a data race; making them atomic
 * asks the narrower question of whether the version check alone is enough.
 *
 * WRITER selects how the writer takes the lock:
 *   0  write_lock()            (SMO path)
 *   1  execute_op() with a second batched writer queued behind it, so the
 *      combiner path with opportunistic reads is exercised too */
#include "opal_model.h"

#ifndef WRITER
#define WRITER 0
#endif

static struct opal_lock L;
static atomic_int x, y;

static bool set_xy(void *p)
{
    int v = (int)(intptr_t)p;
    atomic_store_explicit(&x, v, rlx);
    atomic_store_explicit(&y, v, rlx);
    return true;
}

static void *writer(void *p)
{
    int tid = (int)(intptr_t)p;
#if WRITER == 0
    write_lock(&L, tid);
    set_xy((void *)(intptr_t)1);
    write_unlock(&L);
#else
    execute_op(&L, tid, set_xy, (void *)(intptr_t)(tid + 1));
#endif
    return NULL;
}

static void *reader(void *p)
{
    (void)p;
    unsigned long long v;
    if (!read_lock(&L, &v))
        return NULL;
    int a = atomic_load_explicit(&x, rlx);
    int b = atomic_load_explicit(&y, rlx);
    if (read_unlock(&L, v))
        assert(a == b);
    return NULL;
}

int main(void)
{
#if WRITER == 0
    pthread_t w, r;
    pthread_create(&w, NULL, writer, (void *)(intptr_t)0);
    pthread_create(&r, NULL, reader, NULL);
    pthread_join(w, NULL);
    pthread_join(r, NULL);
#else
    pthread_t w0, w1, r;
    pthread_create(&w0, NULL, writer, (void *)(intptr_t)0);
    pthread_create(&w1, NULL, writer, (void *)(intptr_t)1);
    pthread_create(&r, NULL, reader, NULL);
    pthread_join(w0, NULL);
    pthread_join(w1, NULL);
    pthread_join(r, NULL);
#endif
    return 0;
}
