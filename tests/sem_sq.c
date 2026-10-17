/* sem_sq.c - textbook semaphore deadlock (two binary semaphores S and Q).
 *
 *   P0: wait(S); wait(Q); ... post(Q); post(S);
 *   P1: wait(Q); wait(S); ... post(S); post(Q);
 *
 * Common PDP test interface:
 *   ./sem_sq [seq]     P0 then P1, create+join each: never hangs (default).
 *   ./sem_sq race [N]  concurrent, N iterations each (default 1000), no delay.
 *   ./sem_sq slow      concurrent, 100 ms sleep between the two waits: hangs.
 * Exit 0 on completion.
 */
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Binary semaphores (initial value 1) used as locks. */
static sem_t S, Q;
static int slow_mode, iters = 1;

/* Start barrier (race and slow modes only). WHY: without it thread 1 may run
 * its whole loop before thread 2 even exists, so the experiment would measure
 * thread-creation stagger instead of lock interleaving. The barrier makes all
 * workers begin contending together. pthread_barrier_wait is NOT intercepted
 * by PDP, so it adds no log events and no lock-order edges. */
static pthread_barrier_t start_barrier;
static int use_barrier;
static void wait_for_start(void) { if (use_barrier) pthread_barrier_wait(&start_barrier); }

static void pause_between_waits(void) { if (slow_mode) usleep(100 * 1000); }

static void *p0(void *arg) {
    (void)arg;
    wait_for_start();
    for (int i = 0; i < iters; i++) {
        sem_wait(&S);
        pause_between_waits();
        sem_wait(&Q);                   /* DEADLOCK-RELEVANT: holds S, wants Q */
        sem_post(&Q);
        sem_post(&S);
    }
    return NULL;
}

static void *p1(void *arg) {
    (void)arg;
    wait_for_start();
    for (int i = 0; i < iters; i++) {
        sem_wait(&Q);
        pause_between_waits();
        sem_wait(&S);                   /* DEADLOCK-RELEVANT: holds Q, wants S */
        sem_post(&S);
        sem_post(&Q);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "seq";
    int concurrent = strcmp(mode, "race") == 0 || strcmp(mode, "slow") == 0;
    slow_mode = strcmp(mode, "slow") == 0;
    use_barrier = concurrent;
    if (strcmp(mode, "race") == 0) iters = argc > 2 ? atoi(argv[2]) : 1000;
    sem_init(&S, 0, 1);
    sem_init(&Q, 0, 1);
    if (concurrent) pthread_barrier_init(&start_barrier, NULL, 2);
    pthread_t a, b;
    pthread_create(&a, NULL, p0, NULL);
    if (!concurrent) pthread_join(a, NULL);
    pthread_create(&b, NULL, p1, NULL);
    if (concurrent) pthread_join(a, NULL);
    pthread_join(b, NULL);
    printf("done\n");
    return 0;
}
