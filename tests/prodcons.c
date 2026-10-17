/* prodcons.c - CORRECT producer/consumer: two semaphores used for signalling
 * (items = filled slots, spaces = free slots) plus a mutex around the buffer.
 * It never deadlocks. It exists to prove PDP's watchdog stays quiet: a consumer
 * keeps "holding" `items` in PDP's binary-semaphore model after its sem_wait,
 * so its next sem_wait(items) once looked like a one-thread deadlock.
 *
 * Common PDP test interface:
 *   ./prodcons [seq]     bounded run: producer fills CAP slots and is joined,
 *                        then the consumer drains them. Neither can block.
 *   ./prodcons race [N]  concurrent, N items (default 1000).
 *   ./prodcons slow      concurrent, 5 ms pause per item (consumer often waits).
 * Always exits 0.
 */
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CAP 4
static sem_t items, spaces;
static pthread_mutex_t buf_lock = PTHREAD_MUTEX_INITIALIZER;
static int buf[CAP], head, tail, count_n = CAP, slow_mode;

static void *producer(void *arg) {
    (void)arg;
    for (int i = 0; i < count_n; i++) {
        if (slow_mode) usleep(5000);
        sem_wait(&spaces);
        pthread_mutex_lock(&buf_lock);
        buf[tail] = i; tail = (tail + 1) % CAP;
        pthread_mutex_unlock(&buf_lock);
        sem_post(&items);
    }
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    for (int i = 0; i < count_n; i++) {
        sem_wait(&items);
        pthread_mutex_lock(&buf_lock);
        head = (head + 1) % CAP;
        pthread_mutex_unlock(&buf_lock);
        sem_post(&spaces);
    }
    return NULL;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "seq";
    int concurrent = strcmp(mode, "race") == 0 || strcmp(mode, "slow") == 0;
    slow_mode = strcmp(mode, "slow") == 0;
    if (slow_mode) count_n = 40;
    if (strcmp(mode, "race") == 0) count_n = argc > 2 ? atoi(argv[2]) : 1000;
    sem_init(&items, 0, 0);
    sem_init(&spaces, 0, CAP);
    pthread_t p, c;
    pthread_create(&p, NULL, producer, NULL);
    if (!concurrent) pthread_join(p, NULL);   /* seq: CAP items fit, so no blocking */
    pthread_create(&c, NULL, consumer, NULL);
    if (concurrent) pthread_join(p, NULL);
    pthread_join(c, NULL);
    printf("done\n");
    return 0;
}
