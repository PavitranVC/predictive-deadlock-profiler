/* bank.c - the PDP demo program: the classic "transfer money" deadlock.
 *
 * Two accounts, each protected by its own mutex. Teller 1 moves money A -> B and
 * therefore locks A then B. Teller 2 moves money B -> A and locks B then A.
 * Opposite lock order = a deadlock WAITING to happen.
 *
 *   ./bank        the tellers run one after the other (we join in between).
 *                 They can never block each other, so the program always finishes,
 *                 yet PDP predicts the deadlock from the lock order it observed.
 *   ./bank slow   the tellers run at the same time and sleep between their two
 *                 locks. Both then hold one lock and wait for the other: HANG.
 *                 (Stop it with Ctrl-C.)
 *
 * Why two teller functions instead of one transfer(from, to)? With a single
 * function both threads would lock the second account from the SAME source line.
 * Writing them out gives the report two distinct lines to point at.
 */
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The lock is the first member, so nm names it by the variable (acct_a). */
struct account { pthread_mutex_t lock; int balance; };
struct account acct_a = { PTHREAD_MUTEX_INITIALIZER, 100 };
struct account acct_b = { PTHREAD_MUTEX_INITIALIZER, 100 };

static int slow_mode;       /* 1 = sleep between the two locks to force the hang */

/* The sleep only matters in slow mode: it gives the other teller time to grab
 * its first lock, which guarantees the circular wait. */
static void pause_between_locks(void) { if (slow_mode) usleep(200 * 1000); }

/* transfer(A, B): lock A, then B. */
static void *teller1(void *arg) {
    (void)arg;
    pthread_mutex_lock(&acct_a.lock);
    pause_between_locks();
    pthread_mutex_lock(&acct_b.lock);   /* SITE_T1: waits for B while holding A */
    acct_a.balance -= 10; acct_b.balance += 10;
    pthread_mutex_unlock(&acct_b.lock);
    pthread_mutex_unlock(&acct_a.lock);
    return NULL;
}

/* transfer(B, A): lock B, then A - the opposite order. */
static void *teller2(void *arg) {
    (void)arg;
    pthread_mutex_lock(&acct_b.lock);
    pause_between_locks();
    pthread_mutex_lock(&acct_a.lock);   /* SITE_T2: waits for A while holding B */
    acct_b.balance -= 5; acct_a.balance += 5;
    pthread_mutex_unlock(&acct_a.lock);
    pthread_mutex_unlock(&acct_b.lock);
    return NULL;
}

int main(int argc, char **argv) {
    slow_mode = argc > 1 && strcmp(argv[1], "slow") == 0;
    pthread_t t1, t2;
    pthread_create(&t1, NULL, teller1, NULL);
    if (!slow_mode) pthread_join(t1, NULL);     /* safe mode: finish teller 1 first */
    pthread_create(&t2, NULL, teller2, NULL);
    if (slow_mode) pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    printf("done: A=%d B=%d\n", acct_a.balance, acct_b.balance);
    return 0;
}
