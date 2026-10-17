/* gate.c - opposite lock order that is PROTECTED by a gate lock (no deadlock).
 *
 * Like bank.c, transfer A->B locks A then B and transfer B->A locks B then A.
 * But both first take the global mutex G, so only one transfer runs at a time
 * and the circular wait can never form. PDP must report this as FILTERED
 * (gate lock G), not as a real deadlock.
 *
 * Common PDP test interface:
 *   ./gate [seq]     one transfer after the other (default).
 *   ./gate race [N]  concurrent, N iterations each (default 1000).
 *   ./gate slow      concurrent, 100 ms sleep between the inner locks.
 * Never hangs in any mode. Exit 0 on completion.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t gate_G = PTHREAD_MUTEX_INITIALIZER;   /* the gate */
static pthread_mutex_t acct_a = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t acct_b = PTHREAD_MUTEX_INITIALIZER;
static int slow_mode, iters = 1;

/* Start barrier (race and slow modes only). WHY: without it thread 1 may run
 * its whole loop before thread 2 even exists, so the experiment would measure
 * thread-creation stagger instead of lock interleaving. The barrier makes all
 * workers begin contending together. pthread_barrier_wait is NOT intercepted
 * by PDP, so it adds no log events and no lock-order edges. */
static pthread_barrier_t start_barrier;
static int use_barrier;
static void wait_for_start(void) { if (use_barrier) pthread_barrier_wait(&start_barrier); }

static void pause_between_locks(void) { if (slow_mode) usleep(100 * 1000); }

static void *transfer_ab(void *arg) {
    (void)arg;
    wait_for_start();
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&gate_G);        /* GATE: held before any account */
        pthread_mutex_lock(&acct_a);
        pause_between_locks();
        pthread_mutex_lock(&acct_b);        /* A then B, but under G */
        pthread_mutex_unlock(&acct_b);
        pthread_mutex_unlock(&acct_a);
        pthread_mutex_unlock(&gate_G);
    }
    return NULL;
}

static void *transfer_ba(void *arg) {
    (void)arg;
    wait_for_start();
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&gate_G);        /* GATE: same gate */
        pthread_mutex_lock(&acct_b);
        pause_between_locks();
        pthread_mutex_lock(&acct_a);        /* B then A, but under G */
        pthread_mutex_unlock(&acct_a);
        pthread_mutex_unlock(&acct_b);
        pthread_mutex_unlock(&gate_G);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "seq";
    int concurrent = strcmp(mode, "race") == 0 || strcmp(mode, "slow") == 0;
    slow_mode = strcmp(mode, "slow") == 0;
    use_barrier = concurrent;
    if (strcmp(mode, "race") == 0) iters = argc > 2 ? atoi(argv[2]) : 1000;
    if (concurrent) pthread_barrier_init(&start_barrier, NULL, 2);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, transfer_ab, NULL);
    if (!concurrent) pthread_join(t1, NULL);
    pthread_create(&t2, NULL, transfer_ba, NULL);
    if (concurrent) pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("done\n");
    return 0;
}
