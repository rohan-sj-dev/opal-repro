/* Function-pointer combining (Listing 2).
 * Every thread submits one critical section through execute_op(). Checked:
 *   - critical sections never overlap (plain counter, races are reported)
 *   - each one runs exactly once (per-thread run count)
 *   - each caller gets back its *own* critical section's result, including
 *     when a combiner ran it on the caller's behalf
 * Batching needs a holder plus at least three queued waiters, so N=4 is the
 * smallest instance that exercises Phase 2D. */
#include "opal_model.h"

#ifndef N
#define N 4
#endif

static struct opal_lock L;
static int counter;
static int runs[N];
static bool got[N];

struct arg { int tid; };
static struct arg args[N];

static bool cs(void *p)
{
    struct arg *a = p;
    counter++;
    runs[a->tid]++;
    return (a->tid & 1) == 0;   /* distinct results so a mix-up is visible */
}

static void *worker(void *p)
{
    int tid = (int)(intptr_t)p;
    args[tid].tid = tid;
    got[tid] = execute_op(&L, tid, cs, &args[tid]);
    return NULL;
}

int main(void)
{
    pthread_t t[N];
    for (int i = 0; i < N; i++)
        pthread_create(&t[i], NULL, worker, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++)
        pthread_join(t[i], NULL);
    assert(counter == N);
    for (int i = 0; i < N; i++) {
        assert(runs[i] == 1);
        assert(got[i] == ((i & 1) == 0));
    }
    assert(load_state(&L, rlx) == G_UNLOCKED);
    return 0;
}
