/* recursive.c - CORRECT use of a PTHREAD_MUTEX_RECURSIVE mutex: nested relock.
 * The second lock by the owner succeeds at once, but PDP briefly shows the
 * thread as "waiting on a lock it already holds". The watchdog once reported
 * that as a deadlock, so this program loops enough to hit the window.
 *
 * Common PDP test interface:
 *   ./recursive [seq]     one thread, a small loop.
 *   ./recursive race [N]  two threads contending, N iterations each (default 1000000).
 *   ./recursive slow      two threads, 1000 iterations, 100 us pause while nested.
 * Always exits 0.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static pthread_mutex_t m;
static int iters = 1000, slow_mode;

static void *worker(void *arg) {
    (void)arg;
    for (int i = 0; i < iters; i++) {
        pthread_mutex_lock(&m);
        pthread_mutex_lock(&m);         /* relock by the owner: allowed, recursive */
        if (slow_mode) usleep(100);
        pthread_mutex_unlock(&m);
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "seq";
    int concurrent = strcmp(mode, "race") == 0 || strcmp(mode, "slow") == 0;
    slow_mode = strcmp(mode, "slow") == 0;
    if (strcmp(mode, "race") == 0) iters = argc > 2 ? atoi(argv[2]) : 1000000;
    pthread_mutexattr_t at;
    pthread_mutexattr_init(&at);
    pthread_mutexattr_settype(&at, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m, &at);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, worker, NULL);
    if (!concurrent) pthread_join(t1, NULL);
    else pthread_create(&t2, NULL, worker, NULL);
    if (concurrent) { pthread_join(t1, NULL); pthread_join(t2, NULL); }
    printf("done\n");
    return 0;
}
