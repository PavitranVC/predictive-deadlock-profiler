/* libpdp.c - LD_PRELOAD interceptor that logs lock events (see pdp.h for format). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "pdp.h"

/* One record per thread. Global (not TLS) so a Stage 4 watchdog can read all
 * of them to build a wait-for graph. */
struct tcb {
    pid_t tid;                  /* 0 = slot free */
    void *held[MAX_HELD];       /* locks currently held */
    int   nheld;
    void *waiting_on;           /* lock being waited for, or 0 */
};
struct tcb pdp_threads[MAX_THREADS];

static volatile int tbl_lock;               /* TAS spin lock guarding the table */
static __thread int in_hook;                /* recursion guard */
static __thread struct tcb *me;             /* this thread's slot */
static int log_fd = -1;
static volatile int inited;
static unsigned long seq_counter;

static int (*real_lock)(pthread_mutex_t *);
static int (*real_unlock)(pthread_mutex_t *);
static int (*real_trylock)(pthread_mutex_t *);
static int (*real_destroy)(pthread_mutex_t *);
static int (*real_sem_wait)(sem_t *);
static int (*real_sem_post)(sem_t *);

/* WHY a spin lock: a pthread mutex here would call our own hook and recurse.
 * __sync_lock_test_and_set is an atomic exchange (test-and-set). */
static void tbl_acquire(void) { while (__sync_lock_test_and_set(&tbl_lock, 1)) { } }
static void tbl_release(void) { __sync_lock_release(&tbl_lock); }

/* Find (or claim) this thread's table slot. Returns NULL if the table is full;
 * callers then just skip bookkeeping. Slots are never reused (simplification). */
static struct tcb *self(void) {
    if (me) return me;
    tbl_acquire();
    for (int i = 0; i < MAX_THREADS; i++) {
        if (pdp_threads[i].tid == 0) {
            pdp_threads[i].tid = (pid_t)syscall(SYS_gettid);
            me = &pdp_threads[i];
            break;
        }
    }
    tbl_release();
    return me;
}

/* Remove addr from one held list (order not preserved). Caller holds tbl lock. */
static void held_remove(struct tcb *t, void *addr) {
    for (int i = 0; i < t->nheld; i++)
        if (t->held[i] == addr) { t->held[i] = t->held[--t->nheld]; return; }
}

/* Add addr to this thread's held list (silently drops if MAX_HELD exceeded). */
static void held_add(struct tcb *t, void *addr) {
    tbl_acquire();
    if (t->nheld < MAX_HELD) t->held[t->nheld++] = addr;
    tbl_release();
}

/* Remove addr from this thread's held list under the table lock. */
static void held_drop(struct tcb *t, void *addr) {
    tbl_acquire(); held_remove(t, addr); tbl_release();
}

/* Turn a return address into "module+0xoffset" so addr2line works on PIE/ASLR. */
static void site_str(void *ret, char *out, size_t n) {
    Dl_info info;
    if (dladdr(ret, &info) && info.dli_fname)
        snprintf(out, n, "%s+0x%lx", info.dli_fname,
                 (unsigned long)((char *)ret - (char *)info.dli_fbase));
    else
        snprintf(out, n, "?+0x%lx", (unsigned long)ret);
}

/* Build one log line on the stack and emit it with a single write() (O_APPEND
 * makes that atomic; no stdio, so no stdio locks). The lock being
 * logged (addr) is excluded from held, so REL shows what remains. */
static void log_event(struct tcb *t, const char *op, char kind, void *addr, void *ret) {
    if (log_fd < 0) return;
    char site[256], held[MAX_HELD * 20 + 2], buf[PDP_LINE_MAX];
    void *snap[MAX_HELD]; int n = 0;
    pid_t tid = t ? t->tid : (pid_t)syscall(SYS_gettid);
    if (t) {                                    /* snapshot under the spin lock */
        tbl_acquire();
        for (int i = 0; i < t->nheld; i++) if (t->held[i] != addr) snap[n++] = t->held[i];
        tbl_release();
    }
    size_t off = 0;
    for (int i = 0; i < n; i++)
        off += snprintf(held + off, sizeof held - off, "%s%p", i ? "," : "", snap[i]);
    if (n == 0) strcpy(held, "-");
    site_str(ret, site, sizeof site);
    unsigned long seq = __sync_fetch_and_add(&seq_counter, 1);
    int len = snprintf(buf, sizeof buf, "%lu %d %s %c %p %s held=%s\n",
                       seq, (int)tid, op, kind, addr, site, held);
    if (len >= (int)sizeof buf) { len = sizeof buf - 1; buf[len - 1] = '\n'; }
    ssize_t w = write(log_fd, buf, len);
    (void)w;
}

/* Resolve real functions once. Safe to call repeatedly: it is also the lazy
 * fallback if a hook fires before the constructor ran. */
static void pdp_init(void) {
    if (inited) return;
    in_hook = 1;                                /* our own setup must not be logged */
    real_lock     = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_unlock   = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_trylock  = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_destroy  = dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    real_sem_wait = dlsym(RTLD_NEXT, "sem_wait");
    real_sem_post = dlsym(RTLD_NEXT, "sem_post");
    const char *path = getenv(PDP_LOG_ENV);
    log_fd = open(path ? path : PDP_LOG_DEFAULT, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    char exe[512] = "?", hdr[640];
    ssize_t r = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (r > 0) exe[r] = 0;
    int len = snprintf(hdr, sizeof hdr, "# pdp v%d pid=%d exe=%s\n", PDP_VERSION, (int)getpid(), exe);
    if (log_fd >= 0) { ssize_t w = write(log_fd, hdr, len); (void)w; }
    inited = 1;
    in_hook = 0;
}
__attribute__((constructor)) static void pdp_ctor(void) { pdp_init(); }

/* Mark thread as waiting and log REQ. Shared by mutex lock and sem_wait. */
static void begin_wait(struct tcb *t, char kind, void *addr, void *ret) {
    /* STAGE 4 HOOK POINT: delay injection goes here (PDP_DELAY_SITE). Comment only. */
    if (t) t->waiting_on = addr;
    log_event(t, "REQ", kind, addr, ret);
}

/* Clear waiting_on; on success log ACQ (held set before adding) then record the lock. */
static void end_wait(struct tcb *t, char kind, void *addr, void *ret, int ok) {
    if (t) t->waiting_on = 0;
    if (!ok) return;
    log_event(t, "ACQ", kind, addr, ret);
    if (t) held_add(t, addr);
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    pdp_init();
    if (in_hook) return real_lock(m);
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    begin_wait(t, 'M', m, ret);
    int r = real_lock(m);
    end_wait(t, 'M', m, ret, r == 0);
    in_hook = 0;
    return r;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    pdp_init();
    if (in_hook) return real_unlock(m);
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    if (t) held_drop(t, m);                 /* drop first: another thread may grab it instantly */
    log_event(t, "REL", 'M', m, ret);
    int r = real_unlock(m);
    in_hook = 0;
    return r;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    pdp_init();
    if (in_hook) return real_trylock(m);
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    int r = real_trylock(m);                /* never blocks, so no waiting_on */
    log_event(t, r == 0 ? "TRY_OK" : "TRY_FAIL", 'M', m, ret);
    if (r == 0 && t) held_add(t, m);
    in_hook = 0;
    return r;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    pdp_init();
    if (in_hook) return real_destroy(m);
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    log_event(t, "DESTROY", 'M', m, ret);
    tbl_acquire();                          /* retire: no thread may still list this address */
    for (int i = 0; i < MAX_THREADS; i++)
        if (pdp_threads[i].tid) held_remove(&pdp_threads[i], m);
    tbl_release();
    int r = real_destroy(m);
    in_hook = 0;
    return r;
}

/* LIMITATION: semaphores are modelled as binary locks (wait=acquire, post=release);
 * counting semaphores and cross-thread posts are only approximated. */
int sem_wait(sem_t *s) {
    pdp_init();
    if (in_hook) return real_sem_wait(s);
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    begin_wait(t, 'S', s, ret);
    int r = real_sem_wait(s);
    end_wait(t, 'S', s, ret, r == 0);
    in_hook = 0;
    return r;
}

int sem_post(sem_t *s) {
    pdp_init();
    if (in_hook) return real_sem_post(s);
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    if (t) held_drop(t, s);                 /* no-op if the poster never waited on it */
    log_event(t, "POST", 'S', s, ret);
    int r = real_sem_post(s);
    in_hook = 0;
    return r;
}
