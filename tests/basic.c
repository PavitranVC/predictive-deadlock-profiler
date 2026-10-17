/* basic.c - 2 threads, consistent lock order A then B (no deadlock). */
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>

static pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t B = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t C = PTHREAD_MUTEX_INITIALIZER;
static sem_t sem;
static int counter;

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&A);
    pthread_mutex_lock(&B);
    counter++;
    pthread_mutex_unlock(&B);
    pthread_mutex_unlock(&A);

    if (pthread_mutex_trylock(&C) == 0) {
        counter++;
        pthread_mutex_unlock(&C);
    }

    sem_wait(&sem);
    counter++;
    sem_post(&sem);
    return NULL;
}

int main(void) {
    pthread_t t1, t2;
    sem_init(&sem, 0, 1);
    pthread_create(&t1, NULL, worker, NULL);
    pthread_create(&t2, NULL, worker, NULL);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    sem_destroy(&sem);
    pthread_mutex_destroy(&A);
    printf("counter=%d\n", counter);
    return 0;
}
