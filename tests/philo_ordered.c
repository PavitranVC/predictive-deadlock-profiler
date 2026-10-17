/* philo_ordered.c - dining philosophers, ordered version: NO cycle.
 *
 * Philosopher i needs forks i and (i+1)%5, but always locks the LOWER-numbered
 * one first (a global lock order). Philosopher 4 therefore locks fork 0 before
 * fork 4, so the edge fork4 -> fork0 never exists and no cycle can form.
 *
 * Common PDP test interface:
 *   ./philo_ordered [seq]     philosophers one after another (default, no hang).
 *   ./philo_ordered race [N]  all concurrent, N rounds each (default 1000).
 *   ./philo_ordered slow      concurrent, 100 ms sleep between the two forks.
 * Exit 0 on completion.
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NPHIL 5
/* Five named mutexes so reports show fork0 -> fork1 ...; the pointer array lets
 * the code index them. (Named fork_ because fork is a libc function.) */
static pthread_mutex_t fork0 = PTHREAD_MUTEX_INITIALIZER, fork1 = PTHREAD_MUTEX_INITIALIZER,
    fork2 = PTHREAD_MUTEX_INITIALIZER, fork3 = PTHREAD_MUTEX_INITIALIZER,
    fork4 = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t *fork_[NPHIL] = { &fork0, &fork1, &fork2, &fork3, &fork4 };
static int slow_mode, iters = 1;

/* Start barrier (race and slow modes only). WHY: without it thread 1 may run
 * its whole loop before thread 2 even exists, so the experiment would measure
 * thread-creation stagger instead of lock interleaving. The barrier makes all
 * workers begin contending together. pthread_barrier_wait is NOT intercepted
 * by PDP, so it adds no log events and no lock-order edges. */
static pthread_barrier_t start_barrier;
static int use_barrier;
static void wait_for_start(void) { if (use_barrier) pthread_barrier_wait(&start_barrier); }

static void pause_between_forks(void) { if (slow_mode) usleep(100 * 1000); }

static void *philosopher(void *arg) {
    wait_for_start();
    int a = (int)(intptr_t)arg, b = (a + 1) % NPHIL;
    int first = a < b ? a : b, second = a < b ? b : a;   /* ORDERING FIX */
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(fork_[first]);
        pause_between_forks();
        pthread_mutex_lock(fork_[second]);  /* safe: always the higher fork second */
        pthread_mutex_unlock(fork_[second]);
        pthread_mutex_unlock(fork_[first]);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "seq";
    int concurrent = strcmp(mode, "race") == 0 || strcmp(mode, "slow") == 0;
    slow_mode = strcmp(mode, "slow") == 0;
    use_barrier = concurrent;
    if (strcmp(mode, "race") == 0) iters = argc > 2 ? atoi(argv[2]) : 1000;
    if (concurrent) pthread_barrier_init(&start_barrier, NULL, NPHIL);
    pthread_t t[NPHIL];
    for (intptr_t i = 0; i < NPHIL; i++) {
        pthread_create(&t[i], NULL, philosopher, (void *)(intptr_t)i);
        if (!concurrent) pthread_join(t[i], NULL);
    }
    if (concurrent)
        for (int i = 0; i < NPHIL; i++) pthread_join(t[i], NULL);
    printf("done\n");
    return 0;
}
