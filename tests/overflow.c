/* overflow.c - exceed MAX_HELD and MAX_THREADS on purpose, then check slot reuse. */
#include <pthread.h>
#include <unistd.h>

#define NT 70                       /* > MAX_THREADS (64) alive at once */
static pthread_mutex_t A = PTHREAD_MUTEX_INITIALIZER, B = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t deep[17];    /* > MAX_HELD (16) */
static volatile int ready, go;

static void *worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&A);
    pthread_mutex_lock(&B);
    pthread_mutex_unlock(&B);
    pthread_mutex_unlock(&A);
    __sync_fetch_and_add(&ready, 1);
    while (!go) usleep(1000);       /* stay alive so slots are not freed yet */
    return NULL;
}

int main(void) {
    pthread_t t[NT], last;
    for (int i = 0; i < 17; i++) pthread_mutex_lock(&deep[i]);      /* held overflow */
    for (int i = 16; i >= 0; i--) pthread_mutex_unlock(&deep[i]);
    for (int i = 0; i < NT; i++) pthread_create(&t[i], NULL, worker, NULL);
    while (ready < NT) usleep(1000);
    go = 1;
    for (int i = 0; i < NT; i++) pthread_join(t[i], NULL);
    pthread_create(&last, NULL, worker, NULL);                      /* slots freed -> tracked */
    pthread_join(last, NULL);
    return 0;
}
