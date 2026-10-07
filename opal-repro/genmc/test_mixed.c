/* Traditional and function-pointer writers on one lock (paper Section 3.3,
 * "Correct mechanism used for critical section execution"). A combiner must
 * never try to run a waiter that queued without a function pointer. Thread 0
 * takes the plain write lock; the others use execute_op(). */
#include "opal_model.h"

#ifndef N
#define N 4
#endif

static struct opal_lock L;
static int counter;
static int runs[N];

struct arg { int tid; };
static struct arg args[N];

static bool cs(void *p)
{
    struct arg *a = p;
    counter++;
    runs[a->tid]++;
    return true;
}

static void *plain(void *p)
{
    int tid = (int)(intptr_t)p;
    write_lock(&L, tid);
    counter++;
    runs[tid]++;
    write_unlock(&L);
    return NULL;
}

static void *batched(void *p)
{
    int tid = (int)(intptr_t)p;
    args[tid].tid = tid;
    bool r = execute_op(&L, tid, cs, &args[tid]);
    assert(r);
    return NULL;
}

int main(void)
{
    pthread_t t[N];
    pthread_create(&t[0], NULL, plain, (void *)(intptr_t)0);
    for (int i = 1; i < N; i++)
        pthread_create(&t[i], NULL, batched, (void *)(intptr_t)i);
    for (int i = 0; i < N; i++)
        pthread_join(t[i], NULL);
    assert(counter == N);
    for (int i = 0; i < N; i++)
        assert(runs[i] == 1);
    return 0;
}
