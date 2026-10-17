/* analyze.c - read a PDP log, build the lock-order graph, find deadlock cycles.
 * Usage: pdp-analyze <logfile> [--dot report.dot] [--no-color]
 * Exit: 0 = nothing found, 2 = REAL or UNDECIDED cycle(s), 1 = error.
 * Assumes ONE process: after fork() the child shares the log and reuses addresses,
 * and a reused tid would merge two threads; both can give wrong cycles. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "analyze.h"

struct graph g;

enum op { OP_REQ, OP_ACQ, OP_REL, OP_FAIL, OP_TRY_OK, OP_TRY_FAIL, OP_DESTROY, OP_POST, OP_UNKNOWN };
static const char *op_names[] = { "REQ", "ACQ", "REL", "FAIL", "TRY_OK", "TRY_FAIL", "DESTROY", "POST" };

struct event {
    unsigned long seq, addr;
    long tid;
    enum op op;
    char kind;
    char site[SITE_LEN], lk[SITE_LEN];
    int unknown_held;
    unsigned long held[MAX_HELDSET];
    int nheld;
};

/* ---------- parsing ---------- */

/* held=- (none), held=? (unknown) or held=0xA,0xB */
static void parse_held(const char *p, struct event *ev) {
    if (*p == '?') { ev->unknown_held = 1; return; }
    while (*p && *p != ' ' && *p != '\n' && *p != '-') {
        char *end;
        unsigned long a = strtoul(p, &end, 16);
        if (end == p) break;
        if (ev->nheld < MAX_HELDSET) ev->held[ev->nheld++] = a;
        p = (*end == ',') ? end + 1 : end;
    }
}

static int parse_event(const char *line, struct event *ev) {
    char opname[16];
    int used = 0;
    memset(ev, 0, sizeof *ev);
    if (sscanf(line, "%lu %ld %15s %c %lx %255s%n", &ev->seq, &ev->tid, opname,
               &ev->kind, &ev->addr, ev->site, &used) < 6)
        return 0;
    ev->op = OP_UNKNOWN;
    for (int i = 0; i < (int)(sizeof op_names / sizeof *op_names); i++)
        if (strcmp(opname, op_names[i]) == 0) ev->op = (enum op)i;
    const char *h = strstr(line + used, " held=");
    if (!h) return 0;
    parse_held(h + 6, ev);
    strcpy(ev->lk, "?");                       /* hand-written logs may omit lk= */
    const char *lk = strstr(line + used, " lk=");
    if (lk) sscanf(lk + 4, "%255s", ev->lk);
    return 1;
}

static int by_seq(const void *a, const void *b) {
    const struct event *x = a, *y = b;
    return (x->seq > y->seq) - (x->seq < y->seq);
}

/* Lines may be slightly out of order, so read everything, then sort by seq. */
static struct event *read_log(const char *path, int *count) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return NULL; }
    struct event *evs = NULL;
    int n = 0, cap = 0;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') { if (strstr(line, "# pdp WARN")) g.log_warns++; continue; }
        if (n == cap) {
            cap = cap ? cap * 2 : 1024;
            evs = realloc(evs, cap * sizeof *evs);
            if (!evs) { perror("realloc"); fclose(f); return NULL; }
        }
        if (parse_event(line, &evs[n])) n++;
    }
    fclose(f);
    if (!evs) evs = malloc(1);                 /* empty log is valid; keep a non-NULL result */
    qsort(evs, n, sizeof *evs, by_seq);
    *count = n;
    return evs;
}

/* ---------- graph building ---------- */

static int find_latest(unsigned long addr) {
    for (int i = g.nnodes - 1; i >= 0; i--)
        if (g.nodes[i].addr == addr) return i;
    return -1;
}

/* WHY generations: after pthread_mutex_destroy the allocator may hand the same
 * address to a brand new mutex. Treating it as the old lock would invent edges
 * between two unrelated locks (a false cycle). So DESTROY marks the node dead and
 * the next use of that address creates a new node with gen+1. Returns -1 if full. */
static int node_for(unsigned long addr, char kind, const char *lk) {
    int i = find_latest(addr);
    if (i < 0 || g.nodes[i].dead) {
        if (g.nnodes == MAX_NODES) { g.dropped++; return -1; }
        struct node *n = &g.nodes[g.nnodes];
        n->addr = addr;
        n->gen = (i < 0) ? 0 : g.nodes[i].gen + 1;
        n->kind = kind;
        strcpy(n->lk, "?");
        i = g.nnodes++;
    }
    if (strcmp(g.nodes[i].lk, "?") == 0) snprintf(g.nodes[i].lk, SITE_LEN, "%s", lk);
    return i;
}

static int same_set(const int *a, int na, const int *b, int nb) {
    if (na != nb) return 0;
    for (int i = 0; i < na; i++) {
        int found = 0;
        for (int j = 0; j < nb; j++) if (a[i] == b[j]) found = 1;
        if (!found) return 0;
    }
    return 1;
}

/* Identical observations (same threads, site, held set) just bump a counter;
 * the counter is the "frequency" used later to pick the cheapest edge to drop. */
static void add_edge(int from, int to, const struct event *ev, const int *held, int nheld) {
    for (int i = 0; i < g.nedges; i++) {
        struct edge *e = &g.edges[i];
        if (e->from == from && e->to == to && e->tid == ev->tid &&
            strcmp(e->site, ev->site) == 0 && same_set(e->held, e->nheld, held, nheld)) {
            e->count++;
            return;
        }
    }
    if (g.nedges == MAX_EDGES) { g.dropped++; return; }
    struct edge *e = &g.edges[g.nedges++];
    e->from = from; e->to = to; e->tid = ev->tid; e->count = 1;
    snprintf(e->site, SITE_LEN, "%s", ev->site);
    memcpy(e->held, held, nheld * sizeof *held);
    e->nheld = nheld;
}

/* Edges come from REQ, not ACQ: a run that hangs never reaches ACQ for the
 * blocked lock, but its REQ is logged. TRY_* events add no edge because a
 * trylock never blocks, so it cannot be a link in a wait cycle; a lock obtained
 * by TRY_OK still shows up in later held sets (libpdp tracks it). */
static void handle_request(const struct event *ev) {
    int to = node_for(ev->addr, ev->kind, ev->lk);
    if (to < 0) return;
    if (ev->unknown_held) { g.unknown_held++; return; }   /* missing data, not "holds nothing" */
    int held[MAX_HELDSET], n = 0;
    for (int i = 0; i < ev->nheld; i++) {
        int h = node_for(ev->held[i], '?', "?");
        /* WHY return: the thread already holds `to`, so this is a recursive re-lock
         * (or a self-deadlock, a different bug). It cannot wait for another thread,
         * so it is no link in a wait cycle; edges from the other held locks would be false. */
        if (h == to) return;
        if (h >= 0) held[n++] = h;
    }
    for (int i = 0; i < n; i++) add_edge(held[i], to, ev, held, n);
}

static void build_graph(const struct event *evs, int n) {
    for (int i = 0; i < n; i++) {
        const struct event *ev = &evs[i];
        if (ev->op == OP_DESTROY) {
            int k = find_latest(ev->addr);
            if (k >= 0) g.nodes[k].dead = 1;
        } else if (ev->op == OP_REQ) {
            handle_request(ev);
        } else if (ev->op != OP_UNKNOWN) {
            node_for(ev->addr, ev->kind, ev->lk);   /* learn the lock's lk= for naming */
        }
    }
}

/* ---------- cycle detection: bounded elementary-cycle enumeration ---------- */

/* WHY per start node: one DFS (e.g. three colours) reports only one cycle per back
 * edge and misses cycles that share nodes with an earlier one. That is dangerous
 * because a found cycle can be FILTERED (same thread, gate lock) while a different
 * cycle through the same locks is a real deadlock. So for each start node s we search
 * for ALL paths that return to s, using only nodes with index > s. Every elementary
 * cycle has one smallest node, so it is found exactly once, from that node, and
 * no rotation/duplicate check is needed. */

static char adj[MAX_NODES][MAX_NODES];   /* adj[a][b] = some edge a -> b exists */
static char onpath[MAX_NODES];
static int  path[MAX_CYC_LEN], start;
static long cyc_steps;
static int  stop;                        /* a limit was hit: give up the whole enumeration */

static void record_cycle(int len) {
    if (g.ncycles == MAX_CYCLES) { g.cyc_capped = 1; stop = 1; return; }
    struct cycle *c = &g.cycles[g.ncycles++];
    memset(c, 0, sizeof *c);
    c->len = len; c->gate = -1;
    memcpy(c->nodes, path, len * sizeof(int));
}

/* path[0..depth] is the current path and path[depth] == u. */
static void extend(int u, int depth) {
    for (int v = start; v < g.nnodes && !stop; v++) {
        if (!adj[u][v]) continue;
        if (v == start) { record_cycle(depth + 1); continue; }   /* edge back to s closes a cycle */
        if (onpath[v]) continue;                                 /* keep the path elementary */
        if (depth + 1 >= MAX_CYC_LEN) { g.cyc_capped = 1; continue; }
        if (++cyc_steps > MAX_CYC_STEPS) { g.cyc_capped = 1; stop = 1; return; }
        onpath[v] = 1; path[depth + 1] = v;
        extend(v, depth + 1);
        onpath[v] = 0;                                           /* backtrack */
    }
}

static void find_cycles(void) {
    for (int i = 0; i < g.nedges; i++) adj[g.edges[i].from][g.edges[i].to] = 1;
    for (start = 0; start < g.nnodes && !stop; start++) {
        onpath[start] = 1; path[0] = start;
        extend(start, 0);
        onpath[start] = 0;
    }
}

/* ---------- filtering: is the cycle a real deadlock? ---------- */

static int cand[MAX_CYC_LEN][MAX_EDGES];   /* edges usable for each hop, collected once */
static int ncand[MAX_CYC_LEN];

struct search {
    struct cycle *c;
    int pick[MAX_CYC_LEN];
    long steps;
    int capped;
    int gate, gate_hop[2], gate_edge[2];   /* first blocking gate lock seen, or gate = -1 */
    int found;
};

static int in_held(const struct edge *e, int node) {
    for (int i = 0; i < e->nheld; i++) if (e->held[i] == node) return 1;
    return 0;
}

/* A lock held by both edges' threads, or -1. Both threads cannot hold it at the same
 * time, so they cannot be stuck in this cycle together. */
static int shared_lock(const struct edge *a, const struct edge *b) {
    for (int i = 0; i < a->nheld; i++)
        if (in_held(b, a->held[i])) return a->held[i];
    return -1;
}

/* Pick one concrete edge per hop. GoodLock rule: a combination is valid only if
 * the threads are pairwise distinct AND the held sets of every pair are disjoint.
 * (The held set includes the edge's own source lock; a lock held by two threads at
 * once is impossible anyway, so counting it is correct.) This subsumes the old
 * "one gate lock common to all edges" rule: it also catches a gate held by only
 * some of the threads. Checked pair by pair as we go so bad branches die early.
 * `steps` counts candidates examined (work done), so the cap really bounds time. */
static void choose(struct search *s, int hop) {
    if (hop == s->c->len) { s->found = 1; return; }
    for (int k = 0; k < ncand[hop]; k++) {
        if (s->found || s->capped) return;
        if (++s->steps > MAX_STEPS) { s->capped = 1; return; }
        int i = cand[hop][k], ok = 1;
        for (int j = 0; j < hop && ok; j++) {
            const struct edge *a = &g.edges[s->pick[j]], *b = &g.edges[i];
            if (a->tid == b->tid) { ok = 0; continue; }
            int x = shared_lock(a, b);
            if (x < 0) continue;
            ok = 0;
            if (s->gate < 0) {      /* remember the first blocker for the report */
                s->gate = x; s->gate_hop[0] = j; s->gate_hop[1] = hop;
                s->gate_edge[0] = s->pick[j]; s->gate_edge[1] = i;
            }
        }
        if (!ok) continue;
        s->pick[hop] = i;
        choose(s, hop + 1);
    }
}

static void classify(struct cycle *c) {
    static struct search s;
    memset(&s, 0, sizeof s);
    s.c = c; s.gate = -1;
    for (int hop = 0; hop < c->len; hop++) {
        int from = c->nodes[hop], to = c->nodes[(hop + 1) % c->len];
        ncand[hop] = 0;
        for (int i = 0; i < g.nedges; i++)
            if (g.edges[i].from == from && g.edges[i].to == to) cand[hop][ncand[hop]++] = i;
    }
    choose(&s, 0);
    if (s.found) { c->real = 1; memcpy(c->pick, s.pick, sizeof c->pick); return; }
    /* Not proven real. If we ran out of steps we do not know: UNDECIDED, which is
     * treated like a possible deadlock (a false alarm is better than a missed one). */
    c->capped = s.capped;
    for (int hop = 0; hop < c->len; hop++) c->pick[hop] = cand[hop][0];
    if (!s.capped && s.gate >= 0) {
        c->gate = s.gate;
        for (int k = 0; k < 2; k++) {
            c->gate_hop[k] = s.gate_hop[k];
            c->pick[s.gate_hop[k]] = s.gate_edge[k];
        }
    }
}

/* ---------- main ---------- */

int main(int argc, char **argv) {
    const char *log = NULL, *dot = NULL;
    int no_color = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-color") == 0) no_color = 1;
        else if (strcmp(argv[i], "--dot") == 0 && i + 1 < argc) dot = argv[++i];
        else if (argv[i][0] != '-' && !log) log = argv[i];
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); log = NULL; break; }
    }
    if (!log) { fprintf(stderr, "usage: pdp-analyze <logfile> [--dot report.dot] [--no-color]\n"); return 1; }

    int n = 0;
    struct event *evs = read_log(log, &n);
    if (!evs) return 1;
    build_graph(evs, n);
    free(evs);
    find_cycles();
    for (int i = 0; i < g.ncycles; i++) classify(&g.cycles[i]);

    report_init(!no_color && !getenv("NO_COLOR") && isatty(STDOUT_FILENO));
    name_locks();
    print_report();
    print_fix();
    if (dot && write_dot(dot) != 0) return 1;

    for (int i = 0; i < g.ncycles; i++) if (g.cycles[i].real || g.cycles[i].capped) return 2;
    return 0;
}
