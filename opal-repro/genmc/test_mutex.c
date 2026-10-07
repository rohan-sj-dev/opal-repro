/* Mutual exclusion of the traditional (SMO) write path.
 * Each thread increments a plain int under write_lock(). Any overlap of two
 * critical sections is a data race on `counter`, which GenMC reports, and a
 * lost update fails the final assertion. */
#include "opal_model.h"

#ifndef N
#define N 3
#endif

static struct opal_lock L;
static int counter;

static void *worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    write_lock(&L, tid);
    counter++;
    write_unlock(&L);
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
    assert(load_state(&L, rlx) == G_UNLOCKED);
    return 0;
}
