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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define N 5
static pthread_mutex_t fork_[N] = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_MUTEX_INITIALIZER };
static int slow_mode, iters = 1;

static void pause_between_forks(void) { if (slow_mode) usleep(100 * 1000); }

static void *philosopher(void *arg) {
    int a = (int)(long)arg, b = (a + 1) % N;
    int first = a < b ? a : b, second = a < b ? b : a;   /* ORDERING FIX */
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&fork_[first]);
        pause_between_forks();
        pthread_mutex_lock(&fork_[second]);  /* safe: always the higher fork second */
        pthread_mutex_unlock(&fork_[second]);
        pthread_mutex_unlock(&fork_[first]);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "seq";
    int concurrent = strcmp(mode, "race") == 0 || strcmp(mode, "slow") == 0;
    slow_mode = strcmp(mode, "slow") == 0;
    if (strcmp(mode, "race") == 0) iters = argc > 2 ? atoi(argv[2]) : 1000;
    pthread_t t[N];
    for (long i = 0; i < N; i++) {
        pthread_create(&t[i], NULL, philosopher, (void *)i);
        if (!concurrent) pthread_join(t[i], NULL);
    }
    if (concurrent)
        for (int i = 0; i < N; i++) pthread_join(t[i], NULL);
    printf("done\n");
    return 0;
}
