/* libpdp.c - LD_PRELOAD interceptor that logs lock events (see pdp.h for format). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <link.h>
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
    int   held_lost;            /* 1 = held[] overflowed, list is incomplete */
};
struct tcb pdp_threads[MAX_THREADS];

static volatile int tbl_lock;               /* TAS spin lock guarding the table */
static __thread int in_hook;                /* recursion guard */
static __thread struct tcb *me;             /* this thread's slot */
static int log_fd = -1;
static volatile int inited;
static pthread_key_t tkey;                  /* only used to get a destructor at thread exit */
static __thread int warned_threads;         /* warn once per thread when the table is full */
static char exe_path[512] = "?";            /* absolute path of the main program */
static void *exe_base;                      /* load address of the main program (PIE) */
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

/* Write one "# pdp WARN" line so the analyzer knows data was lost. */
static void warn_overflow(const char *what) {
    char buf[96];
    int len = snprintf(buf, sizeof buf, "# pdp WARN overflow tid=%d what=%s\n",
                       (int)syscall(SYS_gettid), what);
    if (log_fd >= 0 && len > 0) { ssize_t w = write(log_fd, buf, len); (void)w; }
}

/* Runs when a thread that owns a slot exits: free the slot so thread pools
 * and many short-lived threads do not exhaust the 64 entries. */
static void slot_free(void *p) {
    struct tcb *t = p;
    tbl_acquire();
    t->nheld = 0; t->held_lost = 0; t->waiting_on = 0;
    t->tid = 0;                                 /* last: marks the slot free */
    tbl_release();
    me = NULL; warned_threads = 0;
}

/* Find (or claim) this thread's table slot. Returns NULL if the table is full
 * (warns once); callers then skip bookkeeping and log held=? for this thread. */
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
    if (me) pthread_setspecific(tkey, me);      /* arms slot_free() for thread exit */
    else if (!warned_threads) { warned_threads = 1; warn_overflow("threads"); }
    return me;
}

/* Remove addr from one held list (order not preserved). Caller holds tbl lock. */
static void held_remove(struct tcb *t, void *addr) {
    for (int i = 0; i < t->nheld; i++)
        if (t->held[i] == addr) { t->held[i] = t->held[--t->nheld]; return; }
}

/* Caller holds the table lock. On overflow mark the list incomplete; returns 1
 * the first time so the caller can warn (after releasing the lock). */
static int held_push(struct tcb *t, void *addr) {
    if (t->nheld < MAX_HELD) { t->held[t->nheld++] = addr; return 0; }
    int first = !t->held_lost;
    t->held_lost = 1;
    return first;
}

/* Remove addr from this thread's held list under the table lock. */
static void held_drop(struct tcb *t, void *addr) {
    tbl_acquire(); held_remove(t, addr); tbl_release();
}

/* Turn a return address into "module+0xoffset" so addr2line works under ASLR. */
static void site_str(void *ret, char *out, size_t n) {
    Dl_info info;
    /* WHY ret-1: the return address is the instruction AFTER the call, which
     * can belong to the next source line. One byte back is inside the call. */
    char *call = (char *)ret - 1;
    if (dladdr(call, &info) && info.dli_fname) {
        /* glibc reports the main program by its argv[0] spelling (often relative);
         * use the absolute path from /proc/self/exe so the analyzer can open it. */
        const char *mod = info.dli_fbase == exe_base ? exe_path : info.dli_fname;
        snprintf(out, n, "%s+0x%lx", mod, (unsigned long)(call - (char *)info.dli_fbase));
    } else
        snprintf(out, n, "?+0x%lx", (unsigned long)call);
}

/* Build one log line on the stack and emit it with a single write() (O_APPEND
 * makes that atomic; no stdio, so no stdio locks). The lock being logged
 * (addr) is excluded from held, so REL shows what remains. err != 0 appends
 * " err=<code>" (FAIL events). */
static void log_event(struct tcb *t, const char *op, char kind, void *addr, void *ret, int err) {
    if (log_fd < 0) return;
    /* worst case: held = 16 * ("0x"+16 hex+",") = 19 bytes each -> 20 per entry */
    char site[640], held[MAX_HELD * 20 + 2], buf[PDP_LINE_MAX];
    void *snap[MAX_HELD]; int n = 0, lost = 1;  /* no slot -> unknown */
    pid_t tid = t ? t->tid : (pid_t)syscall(SYS_gettid);
    if (t) {                                    /* snapshot under the spin lock */
        tbl_acquire();
        lost = t->held_lost;
        for (int i = 0; i < t->nheld; i++) if (t->held[i] != addr) snap[n++] = t->held[i];
        tbl_release();
    }
    size_t off = 0;
    for (int i = 0; i < n && !lost; i++) {
        int w = snprintf(held + off, sizeof held - off, "%s%p", i ? "," : "", snap[i]);
        if (w > 0 && (size_t)w < sizeof held - off) off += w;
    }
    if (lost) strcpy(held, "?");                /* never claim "-" when we do not know */
    else if (n == 0) strcpy(held, "-");
    site_str(ret, site, sizeof site);
    unsigned long seq = __sync_fetch_and_add(&seq_counter, 1);
    int len = snprintf(buf, sizeof buf, "%lu %d %s %c %p %s held=%s",
                       seq, (int)tid, op, kind, addr, site, held);
    if (err && len > 0 && len < (int)sizeof buf)
        len += snprintf(buf + len, sizeof buf - len, " err=%d", err);
    if (len < 0) return;
    if (len > (int)sizeof buf - 2) len = sizeof buf - 2;   /* truncated: keep room for \n */
    buf[len++] = '\n';
    ssize_t w = write(log_fd, buf, len);
    (void)w;
}

/* dl_iterate_phdr visits the main program first; remember its load address. */
static int first_object(struct dl_phdr_info *i, size_t sz, void *arg) {
    (void)sz; (void)arg;
    exe_base = (void *)i->dlpi_addr;
    return 1;                                   /* stop after the first */
}

/* Resolve real functions once. in_hook doubles as the "initializing" flag: if
 * dlsym/open ever reached a hooked function, that call sees in_hook=1, skips
 * logging and returns early here instead of recursing. The process is still
 * single-threaded in the constructor, so no other thread can race this. */
static void pdp_init(void) {
    if (inited || in_hook) return;
    in_hook = 1;                                /* our own setup must not be logged */
    real_lock     = dlsym(RTLD_NEXT, "pthread_mutex_lock");
    real_unlock   = dlsym(RTLD_NEXT, "pthread_mutex_unlock");
    real_trylock  = dlsym(RTLD_NEXT, "pthread_mutex_trylock");
    real_destroy  = dlsym(RTLD_NEXT, "pthread_mutex_destroy");
    real_sem_wait = dlsym(RTLD_NEXT, "sem_wait");
    real_sem_post = dlsym(RTLD_NEXT, "sem_post");
    pthread_key_create(&tkey, slot_free);
    dl_iterate_phdr(first_object, NULL);
    ssize_t r = readlink("/proc/self/exe", exe_path, sizeof exe_path - 1);
    if (r > 0) exe_path[r] = 0;                 /* else keep "?" */
    const char *path = getenv(PDP_LOG_ENV);
    log_fd = open(path ? path : PDP_LOG_DEFAULT, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    char hdr[640];
    int len = snprintf(hdr, sizeof hdr, "# pdp v%d pid=%d exe=%s\n", PDP_VERSION, (int)getpid(), exe_path);
    if (log_fd >= 0 && len > 0) { ssize_t w = write(log_fd, hdr, len); (void)w; }
    inited = 1;
    in_hook = 0;
}
__attribute__((constructor)) static void pdp_ctor(void) { pdp_init(); }

/* Mark thread as waiting (under the table lock, so a watchdog sees a consistent
 * snapshot) and log REQ. Shared by mutex lock and sem_wait. */
static void begin_wait(struct tcb *t, char kind, void *addr, void *ret) {
    /* STAGE 4 HOOK POINT: delay injection goes here (PDP_DELAY_SITE). Comment only. */
    if (t) { tbl_acquire(); t->waiting_on = addr; tbl_release(); }
    log_event(t, "REQ", kind, addr, ret, 0);
}

/* The wait ended. In ONE critical section clear waiting_on and (on success) add
 * the lock to held, so a watchdog never sees the thread in neither or both
 * states. Then log ACQ, or FAIL (+ error code) so a REQ without ACQ is explained. */
static void end_wait(struct tcb *t, char kind, void *addr, void *ret, int err) {
    int ok = (err == 0 || err == EOWNERDEAD);   /* robust mutex: lock IS owned */
    int warn = 0;
    if (t) {
        tbl_acquire();
        t->waiting_on = 0;
        if (ok) warn = held_push(t, addr);
        tbl_release();
    }
    if (warn) warn_overflow("held");
    if (ok) log_event(t, "ACQ", kind, addr, ret, 0);   /* held excludes addr itself */
    else    log_event(t, "FAIL", kind, addr, ret, err);
}

/* Each hook: errno is saved first and restored last, because our logging
 * (write/dladdr) may change it and the application must not notice.
 * "real_x ? ... : 0" only happens during init (process still single-threaded,
 * nobody can be using locks yet), so doing nothing and returning success is fine. */
int pthread_mutex_lock(pthread_mutex_t *m) {
    pdp_init();
    if (in_hook) return real_lock ? real_lock(m) : 0;
    int saved = errno;
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    begin_wait(t, 'M', m, ret);
    int r = real_lock(m);
    end_wait(t, 'M', m, ret, r);
    in_hook = 0;
    errno = saved;
    return r;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    pdp_init();
    if (in_hook) return real_unlock ? real_unlock(m) : 0;
    int saved = errno;
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    if (t) held_drop(t, m);                 /* drop first: another thread may grab it instantly */
    log_event(t, "REL", 'M', m, ret, 0);
    int r = real_unlock(m);
    in_hook = 0;
    errno = saved;
    return r;
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    pdp_init();
    if (in_hook) return real_trylock ? real_trylock(m) : 0;
    int saved = errno;
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    int r = real_trylock(m);                /* never blocks, so no waiting_on */
    log_event(t, r == 0 ? "TRY_OK" : "TRY_FAIL", 'M', m, ret, 0);
    if (r == 0 && t) {
        tbl_acquire(); int warn = held_push(t, m); tbl_release();
        if (warn) warn_overflow("held");
    }
    in_hook = 0;
    errno = saved;
    return r;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    pdp_init();
    if (in_hook) return real_destroy ? real_destroy(m) : 0;
    int saved = errno;
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    log_event(t, "DESTROY", 'M', m, ret, 0);
    tbl_acquire();                          /* retire: no thread may still list this address */
    for (int i = 0; i < MAX_THREADS; i++)
        if (pdp_threads[i].tid) held_remove(&pdp_threads[i], m);
    tbl_release();
    int r = real_destroy(m);
    in_hook = 0;
    errno = saved;
    return r;
}

/* LIMITATION: semaphores are modelled as binary locks (wait=acquire, post=release);
 * counting semaphores and cross-thread posts are only approximated. */
int sem_wait(sem_t *s) {
    pdp_init();
    if (in_hook) return real_sem_wait ? real_sem_wait(s) : 0;
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    begin_wait(t, 'S', s, ret);
    int r = real_sem_wait(s);
    int e = errno;                          /* sem_wait reports failure via errno (e.g. EINTR) */
    end_wait(t, 'S', s, ret, r == 0 ? 0 : e);
    in_hook = 0;
    errno = e;                              /* on success errno is whatever it was; keep the real call's */
    return r;
}

int sem_post(sem_t *s) {
    pdp_init();
    if (in_hook) return real_sem_post ? real_sem_post(s) : 0;
    int saved = errno;
    void *ret = __builtin_return_address(0);
    in_hook = 1;
    struct tcb *t = self();
    if (t) held_drop(t, s);                 /* no-op if the poster never waited on it */
    log_event(t, "POST", 'S', s, ret, 0);
    int r = real_sem_post(s);
    int e = errno;                          /* sem_post failure (EOVERFLOW) sets errno */
    in_hook = 0;
    errno = r == 0 ? saved : e;
    return r;
}
