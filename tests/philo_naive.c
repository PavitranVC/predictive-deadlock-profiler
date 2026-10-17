/* philo_naive.c - dining philosophers, naive version: REAL 5-cycle.
 *
 * Philosopher i sits between fork i and fork (i+1)%5. Each locks the LEFT fork
 * (i) first and the RIGHT fork ((i+1)%5) second. Philosopher 4 locks fork 4
 * then fork 0, which closes the circle: fork0 -> fork1 -> ... -> fork4 -> fork0.
 *
 * Common PDP test interface:
 *   ./philo_naive [seq]     philosophers one after another (default, no hang).
 *   ./philo_naive race [N]  all concurrent, N rounds each (default 1000).
 *   ./philo_naive slow      concurrent, 100 ms sleep between the two forks.
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
    int left = (int)(long)arg, right = (left + 1) % N;
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&fork_[left]);
        pause_between_forks();
        pthread_mutex_lock(&fork_[right]);  /* DEADLOCK-RELEVANT: holds left, wants right */
        pthread_mutex_unlock(&fork_[right]);
        pthread_mutex_unlock(&fork_[left]);
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
