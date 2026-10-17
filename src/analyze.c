/* analyze.c - read a PDP log, build the lock-order graph, find deadlock cycles.
 * Usage: pdp-analyze <logfile> [--dot report.dot] [--no-color]
 * Exit: 0 = no real cycle, 2 = real cycle(s), 1 = error. */
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
        if (line[0] == '#') { if (strstr(line, "WARN")) g.log_warns++; continue; }
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
        if (h >= 0 && h != to) held[n++] = h;   /* h == to: recursive lock, no ordering info */
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

/* ---------- cycle detection: three-colour DFS ---------- */

enum { WHITE, GREY, BLACK };
static int colour[MAX_NODES], path[MAX_NODES];

/* Store the cycle rotated to start at its smallest node id, so the same cycle
 * found from a different starting point compares equal. */
static void record_cycle(const int *nodes, int len) {
    if (len > MAX_CYC_LEN || g.ncycles == MAX_CYCLES) { g.dropped++; return; }
    int min = 0;
    for (int i = 1; i < len; i++) if (nodes[i] < nodes[min]) min = i;
    struct cycle c;
    memset(&c, 0, sizeof c);
    c.len = len; c.gate = -1;
    for (int i = 0; i < len; i++) c.nodes[i] = nodes[(min + i) % len];
    for (int k = 0; k < g.ncycles; k++)
        if (g.cycles[k].len == len && memcmp(g.cycles[k].nodes, c.nodes, len * sizeof(int)) == 0) return;
    g.cycles[g.ncycles++] = c;
}

/* GREY = on the current DFS path. An edge to a GREY node closes a loop. */
static void dfs(int u, int depth) {
    colour[u] = GREY;
    path[depth] = u;
    for (int i = 0; i < g.nedges; i++) {
        if (g.edges[i].from != u) continue;
        int v = g.edges[i].to;
        if (colour[v] == GREY) {
            int start = depth;
            while (path[start] != v) start--;
            record_cycle(path + start, depth - start + 1);
        } else if (colour[v] == WHITE) {
            dfs(v, depth + 1);
        }
    }
    colour[u] = BLACK;
}

static void find_cycles(void) {
    for (int u = 0; u < g.nnodes; u++)
        if (colour[u] == WHITE) dfs(u, 0);
}

/* ---------- filtering: is the cycle a real deadlock? ---------- */

struct search {
    struct cycle *c;
    int pick[MAX_CYC_LEN];
    long tried;
    int gate;                   /* first gate lock seen, or -1 */
    int gate_pick[MAX_CYC_LEN];
    int found;
};

static int in_cycle(const struct cycle *c, int node) {
    for (int i = 0; i < c->len; i++) if (c->nodes[i] == node) return 1;
    return 0;
}

static int in_held(const struct edge *e, int node) {
    for (int i = 0; i < e->nheld; i++) if (e->held[i] == node) return 1;
    return 0;
}

/* A gate lock is held by EVERY chosen thread (so they cannot be in the cycle at
 * the same time) and is not itself part of the cycle. Returns node id or -1. */
static int find_gate(const struct search *s) {
    const struct edge *first = &g.edges[s->pick[0]];
    for (int i = 0; i < first->nheld; i++) {
        int x = first->held[i], all = !in_cycle(s->c, x);
        for (int hop = 1; hop < s->c->len && all; hop++)
            if (!in_held(&g.edges[s->pick[hop]], x)) all = 0;
        if (all) return x;
    }
    return -1;
}

/* Try every way to pick one concrete edge per hop. Threads must be pairwise
 * distinct (one thread cannot wait for itself), pruned as we go. */
static void choose(struct search *s, int hop) {
    struct cycle *c = s->c;
    if (s->found || s->tried >= MAX_COMBOS) return;
    if (hop == c->len) {
        s->tried++;
        int gate = find_gate(s);
        if (gate < 0) { s->found = 1; memcpy(c->pick, s->pick, sizeof s->pick); }
        else if (s->gate < 0) { s->gate = gate; memcpy(s->gate_pick, s->pick, sizeof s->pick); }
        return;
    }
    int from = c->nodes[hop], to = c->nodes[(hop + 1) % c->len];
    for (int i = 0; i < g.nedges; i++) {
        if (g.edges[i].from != from || g.edges[i].to != to) continue;
        int clash = 0;
        for (int j = 0; j < hop; j++) if (g.edges[s->pick[j]].tid == g.edges[i].tid) clash = 1;
        if (clash) continue;
        s->pick[hop] = i;
        choose(s, hop + 1);
    }
}

static void classify(struct cycle *c) {
    struct search s;
    memset(&s, 0, sizeof s);
    s.c = c; s.gate = -1;
    choose(&s, 0);
    c->capped = !s.found && s.tried >= MAX_COMBOS;
    c->real = s.found;
    if (s.found) return;
    c->gate = s.gate;
    if (s.gate >= 0) memcpy(c->pick, s.gate_pick, sizeof c->pick);
    else for (int hop = 0; hop < c->len; hop++) {      /* same-thread: show the first edge per hop */
        for (int i = 0; i < g.nedges; i++)
            if (g.edges[i].from == c->nodes[hop] && g.edges[i].to == c->nodes[(hop + 1) % c->len]) {
                c->pick[hop] = i; break;
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

    for (int i = 0; i < g.ncycles; i++) if (g.cycles[i].real) return 2;
    return 0;
}
