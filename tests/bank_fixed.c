/* bank_fixed.c - bank.c with the fix PDP suggested applied.
 *
 * PDP's report for bank said "acquire locks in this order: acct_b, acct_a".
 * So BOTH tellers now lock acct_b first, then acct_a, whatever the direction
 * of the money. One global lock order means no circular wait is possible.
 * Everything else (names, modes, barrier) is unchanged: same program, fixed.
 * Expected: PDP finds no cycle and slow mode finishes.
 *
 * Common PDP test interface (same for every program in tests/):
 *   ./bank_fixed [seq]     the tellers run one after the other (create+join each).
 *                    They can never block each other, so the program always
 *                    finishes, and PDP now finds no cycle in the order it saw.
 *                    order it observed. This is the default.
 *   ./bank_fixed race [N]  both tellers run concurrently, N iterations each (default
 *                    1000), no artificial delay. Never hangs.
 *   ./bank_fixed slow      concurrent, with a 100 ms sleep between the two locks:
 *                    the second lock waits only for the first holder: finishes.
 * Exit status is 0 when the program completes.
 *
 * Why two teller functions (kept from bank.c) instead of one transfer(from, to)? With a single
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

/* transfer(A, B): money still moves A -> B, but locks are taken B, then A. */
static void *teller1(void *arg) {
    (void)arg;
    wait_for_start();
    for (int i = 0; i < iters; i++) {
        /* CHANGED: lock acct_b first (global order b, a). WHY: teller2 also
         * takes b first, so neither can hold one lock while wanting the
         * other's: this breaks the circular wait. */
        pthread_mutex_lock(&acct_b.lock);
        pause_between_locks();
        pthread_mutex_lock(&acct_a.lock);
        acct_a.balance -= 10; acct_b.balance += 10;
        pthread_mutex_unlock(&acct_a.lock);
        pthread_mutex_unlock(&acct_b.lock);
    }
    return NULL;
}

/* transfer(B, A): locks B, then A - unchanged, it already followed the order. */
static void *teller2(void *arg) {
    (void)arg;
    wait_for_start();
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&acct_b.lock);
        pause_between_locks();
        pthread_mutex_lock(&acct_a.lock);   
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
