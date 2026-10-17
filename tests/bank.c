/* bank.c - the PDP demo program: the classic "transfer money" deadlock.
 *
 * Two accounts, each protected by its own mutex. Teller 1 moves money A -> B and
 * therefore locks A then B. Teller 2 moves money B -> A and locks B then A.
 * Opposite lock order = a deadlock WAITING to happen.
 *
 * Common PDP test interface (same for every program in tests/):
 *   ./bank [seq]     the tellers run one after the other (create+join each).
 *                    They can never block each other, so the program always
 *                    finishes, yet PDP predicts the deadlock from the lock
 *                    order it observed. This is the default.
 *   ./bank race [N]  both tellers run concurrently, N iterations each (default
 *                    1000), no artificial delay. May or may not hang.
 *   ./bank slow      concurrent, with a 100 ms sleep between the two locks:
 *                    both hold one lock and wait for the other: HANG.
 *                    (Stop it with Ctrl-C.)
 * Exit status is 0 when the program completes.
 *
 * Why two teller functions instead of one transfer(from, to)? With a single
 * function both threads would lock the second account from the SAME source line.
 * Writing them out gives the report two distinct lines to point at.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The lock is the first member, so nm names it by the variable (acct_a). */
struct account { pthread_mutex_t lock; int balance; };
struct account acct_a = { PTHREAD_MUTEX_INITIALIZER, 100 };
struct account acct_b = { PTHREAD_MUTEX_INITIALIZER, 100 };

static int slow_mode;       /* 1 = sleep between the two locks to force the hang */
static int iters = 1;       /* loop count per teller (race mode raises it) */

/* The sleep only matters in slow mode: it gives the other teller time to grab
 * its first lock, which guarantees the circular wait. */
/* Start barrier (race and slow modes only). WHY: without it thread 1 may run
 * its whole loop before thread 2 even exists, so the experiment would measure
 * thread-creation stagger instead of lock interleaving. The barrier makes all
 * workers begin contending together. pthread_barrier_wait is NOT intercepted
 * by PDP, so it adds no log events and no lock-order edges. */
static pthread_barrier_t start_barrier;
static int use_barrier;
static void wait_for_start(void) { if (use_barrier) pthread_barrier_wait(&start_barrier); }

static void pause_between_locks(void) { if (slow_mode) usleep(100 * 1000); }

/* transfer(A, B): lock A, then B. */
static void *teller1(void *arg) {
    (void)arg;
    wait_for_start();
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&acct_a.lock);
        pause_between_locks();
        pthread_mutex_lock(&acct_b.lock);   /* SITE_T1: waits for B while holding A */
        acct_a.balance -= 10; acct_b.balance += 10;
        pthread_mutex_unlock(&acct_b.lock);
        pthread_mutex_unlock(&acct_a.lock);
    }
    return NULL;
}

/* transfer(B, A): lock B, then A - the opposite order. */
static void *teller2(void *arg) {
    (void)arg;
    wait_for_start();
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&acct_b.lock);
        pause_between_locks();
        pthread_mutex_lock(&acct_a.lock);   /* SITE_T2: waits for A while holding B */
        acct_b.balance -= 5; acct_a.balance += 5;
        pthread_mutex_unlock(&acct_a.lock);
        pthread_mutex_unlock(&acct_b.lock);
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
    pthread_create(&t1, NULL, teller1, NULL);
    if (!concurrent) pthread_join(t1, NULL);    /* seq: finish teller 1 first */
    pthread_create(&t2, NULL, teller2, NULL);
    if (concurrent) pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("done: A=%d B=%d\n", acct_a.balance, acct_b.balance);
    return 0;
}
