/* report.c - turn the analysed graph into text: names, source lines, report, fix, dot. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "analyze.h"

static const char *RED = "", *YEL = "", *GRN = "", *RST = "";

void report_init(int use_color) {
    if (!use_color) return;
    RED = "\033[31m"; YEL = "\033[33m"; GRN = "\033[32m"; RST = "\033[0m";
}

/* ---------- external tools: addr2line and nm ---------- */

/* Split "module+0xoff" at the LAST '+'. Reject odd characters because the
 * module goes into a shell command line (popen). */
static int split_site(const char *s, char *mod, size_t n, char *off, size_t m) {
    const char *plus = strrchr(s, '+');
    if (!plus || (size_t)(plus - s) >= n || strlen(plus + 1) >= m) return 0;
    memcpy(mod, s, plus - s);
    mod[plus - s] = 0;
    strcpy(off, plus + 1);
    if (mod[0] != '/' || strncmp(off, "0x", 2) != 0) return 0;
    for (const char *p = mod; *p; p++)
        if (!isalnum((unsigned char)*p) && !strchr("/._-+", *p)) return 0;
    for (const char *p = off + 2; *p; p++)
        if (!isxdigit((unsigned char)*p)) return 0;
    return 1;
}

static void chomp(char *s) { s[strcspn(s, "\n")] = 0; }

/* "module+0xoff" -> "file.c:42 (function)". Falls back to the raw text when
 * addr2line cannot tell (stripped binary, missing file, hand-written log).
 * Results are cached: the same call site shows up in many edges. */
static void resolve_site(const char *site, char *out, size_t n) {
    static struct { char site[SITE_LEN]; char text[SITE_LEN + 64]; } cache[256];
    static int ncache;
    for (int i = 0; i < ncache; i++)
        if (strcmp(cache[i].site, site) == 0) { snprintf(out, n, "%s", cache[i].text); return; }

    char mod[SITE_LEN], off[32], cmd[SITE_LEN + 96], func[256] = "", where[256] = "";
    snprintf(out, n, "%s", site);
    if (split_site(site, mod, sizeof mod, off, sizeof off)) {
        /* -i lists inlined frames innermost first, so the first pair is the real line */
        snprintf(cmd, sizeof cmd, "addr2line -e '%s' -f -C -i %s 2>/dev/null", mod, off);
        FILE *p = popen(cmd, "r");
        if (p) {
            if (fgets(func, sizeof func, p) && fgets(where, sizeof where, p)) {
                chomp(func); chomp(where);
                const char *slash = strrchr(where, '/');
                if (strncmp(where, "??", 2) != 0 && strcmp(func, "??") != 0)
                    snprintf(out, n, "%s (%s)", slash ? slash + 1 : where, func);
            }
            pclose(p);
        }
    }
    if (ncache < 256) {
        snprintf(cache[ncache].site, SITE_LEN, "%s", site);
        snprintf(cache[ncache].text, sizeof cache[ncache].text, "%s", out);
        ncache++;
    }
}

/* Look the lock's address up among the module's data symbols (nm -S gives
 * address and size). Returns 1 and writes "var" or "var+0xN" on success. */
static int lookup_symbol(const char *lk, char *out, size_t n) {
    char mod[SITE_LEN], offs[32], cmd[SITE_LEN + 64], line[512];
    if (!split_site(lk, mod, sizeof mod, offs, sizeof offs)) return 0;
    unsigned long off = strtoul(offs, NULL, 16);
    snprintf(cmd, sizeof cmd, "nm -S --defined-only '%s' 2>/dev/null", mod);
    FILE *p = popen(cmd, "r");
    if (!p) return 0;
    int found = 0;
    while (!found && fgets(line, sizeof line, p)) {
        unsigned long val, size; char type, name[128];
        if (sscanf(line, "%lx %lx %c %127s", &val, &size, &type, name) != 4) continue;
        if (!strchr("bBdD", type) || off < val || off >= val + size) continue;
        if (off == val) snprintf(out, n, "%s", name);
        else snprintf(out, n, "%s+0x%lx", name, off - val);
        found = 1;
    }
    pclose(p);
    return found;
}

/* Variable name if we can find one, else L1, L2... in first-seen order.
 * A reused address gets "#2", "#3" so the two locks stay distinguishable. */
void name_locks(void) {
    int unnamed = 0;
    for (int i = 0; i < g.nnodes; i++) {
        struct node *nd = &g.nodes[i];
        char base[64];   /* smaller than name[] so the "#N" suffix always fits */
        if (!lookup_symbol(nd->lk, base, sizeof base)) snprintf(base, sizeof base, "L%d", ++unnamed);
        if (nd->gen > 0) snprintf(nd->name, sizeof nd->name, "%s#%d", base, nd->gen + 1);
        else snprintf(nd->name, sizeof nd->name, "%s", base);
    }
}

/* ---------- report ---------- */

static const char *nm(int id) { return g.nodes[id].name; }

static void print_set(const struct edge *e) {
    printf("{");
    for (int i = 0; i < e->nheld; i++) printf("%s%s", i ? ", " : "", nm(e->held[i]));
    printf("}");
}

static void print_cycle_line(const struct cycle *c) {
    for (int i = 0; i <= c->len; i++) printf("%s%s", i ? " -> " : "", nm(c->nodes[i % c->len]));
}

static void print_edges(const struct cycle *c) {
    for (int hop = 0; hop < c->len; hop++) {
        const struct edge *e = &g.edges[c->pick[hop]];
        char where[SITE_LEN + 64];
        resolve_site(e->site, where, sizeof where);
        printf("      %s -> %s  tid %ld  at %s  holding ", nm(e->from), nm(e->to), e->tid, where);
        print_set(e);
        printf("\n");
    }
}

/* 0 = REAL, 1 = UNDECIDED (search capped), 2 = FILTERED */
static int verdict(const struct cycle *c) { return c->real ? 0 : c->capped ? 1 : 2; }

static void print_group(int want, const char *colour, const char *title) {
    int k = 0;
    for (int i = 0; i < g.ncycles; i++) {
        const struct cycle *c = &g.cycles[i];
        if (verdict(c) != want) continue;
        if (k++ == 0) printf("%s%s%s\n", colour, title, RST);
        printf("  %s[%d] ", colour, k);
        print_cycle_line(c);
        if (want == 2 && c->gate >= 0)
            printf("  -- gate lock %s held by both tid %ld and tid %ld", nm(c->gate),
                   g.edges[c->pick[c->gate_hop[0]]].tid, g.edges[c->pick[c->gate_hop[1]]].tid);
        else if (want == 2)
            printf("  -- same-thread cycle or too few distinct threads (one thread cannot deadlock itself)");
        printf("%s\n", RST);
        print_edges(c);
    }
}

void print_report(void) {
    printf("PDP analysis: %d locks, %d edges\n", g.nnodes, g.nedges);
    if (g.unknown_held) printf("warning: skipped %d REQ lines with held=? (unknown held set)\n", g.unknown_held);
    if (g.log_warns) printf("warning: log contains %d '# pdp WARN' lines (data was lost)\n", g.log_warns);
    if (g.dropped) printf("warning: %d items dropped (analyzer tables full)\n", g.dropped);
    if (g.cyc_capped)
        printf("warning: cycle search hit a limit (length %d, %d cycles or %d steps): some cycles may be missing\n",
               MAX_CYC_LEN, MAX_CYCLES, MAX_CYC_STEPS);
    print_group(0, RED, "REAL potential deadlocks:");
    print_group(1, RED, "UNDECIDED (search capped; cannot rule out a deadlock):");
    print_group(2, YEL, "FILTERED cycles (not deadlocks):");
    int found = 0;
    for (int i = 0; i < g.ncycles; i++) found += verdict(&g.cycles[i]) < 2;
    if (found == 0 && !g.cyc_capped) printf("%sNo deadlock potential found.%s\n", GRN, RST);
}

/* ---------- fix suggestion F(R) ---------- */

static char removed[MAX_NODES][MAX_NODES];   /* removed[a][b] = pair a->b dropped */

static int pair_count(int a, int b) {
    int total = 0;
    for (int i = 0; i < g.nedges; i++)
        if (g.edges[i].from == a && g.edges[i].to == b) total += g.edges[i].count;
    return total;
}

/* Every distinct call site that produced the pair: changing one is not enough. */
static void print_sites(int a, int b) {
    for (int i = 0; i < g.nedges; i++) {
        const struct edge *e = &g.edges[i];
        if (e->from != a || e->to != b) continue;
        int seen = 0;
        for (int j = 0; j < i; j++)
            if (g.edges[j].from == a && g.edges[j].to == b && strcmp(g.edges[j].site, e->site) == 0) seen = 1;
        if (seen) continue;
        char where[SITE_LEN + 64];
        resolve_site(e->site, where, sizeof where);
        printf("      %s (tid %ld)\n", where, e->tid);
    }
}

/* Break every real cycle by dropping its least frequent pair: the rarer
 * ordering is the cheapest one to change in the code. A pair dropped for an
 * earlier cycle may already break this one. */
static void drop_edges(void) {
    for (int i = 0; i < g.ncycles; i++) {
        const struct cycle *c = &g.cycles[i];
        if (!c->real) continue;
        int best = -1, best_count = 0, already = 0;
        for (int hop = 0; hop < c->len; hop++) {
            const struct edge *e = &g.edges[c->pick[hop]];
            if (removed[e->from][e->to]) already = 1;
            int cnt = pair_count(e->from, e->to);
            if (best < 0 || cnt < best_count) { best = hop; best_count = cnt; }
        }
        if (already) continue;
        const struct edge *e = &g.edges[c->pick[best]];
        removed[e->from][e->to] = 1;
        printf("  drop %s -> %s (seen %dx): change the call site(s):\n",
               nm(e->from), nm(e->to), best_count);
        print_sites(e->from, e->to);
    }
}

/* Kahn's algorithm: repeatedly output a lock nobody still has to precede. */
static void print_order(void) {
    static char adj[MAX_NODES][MAX_NODES], used[MAX_NODES], done[MAX_NODES];
    for (int i = 0; i < g.nedges; i++) {
        int a = g.edges[i].from, b = g.edges[i].to;
        if (!removed[a][b]) adj[a][b] = 1;
        used[a] = used[b] = 1;
    }
    printf("  acquire locks in this order: ");
    int first = 1, progress = 1;
    while (progress) {
        progress = 0;
        for (int v = 0; v < g.nnodes; v++) {
            if (!used[v] || done[v]) continue;
            int ready = 1;
            for (int u = 0; u < g.nnodes; u++) if (used[u] && !done[u] && adj[u][v]) ready = 0;
            if (!ready) continue;
            printf("%s%s", first ? "" : ", ", nm(v));
            first = 0; done[v] = 1; progress = 1;
            break;                                 /* restart: smallest id first */
        }
    }
    printf("\n");
    int left = 0;                                  /* these sit in cycles that remain: no valid order */
    for (int v = 0; v < g.nnodes; v++)
        if (used[v] && !done[v]) { printf("%s%s", left++ ? ", " : "  still cyclic (filtered or unanalysed): ", nm(v)); }
    if (left) printf("\n");
}

void print_fix(void) {
    int real = 0;
    for (int i = 0; i < g.ncycles; i++) real += g.cycles[i].real;
    if (!real) return;
    printf("%sSuggested fix:%s\n", GRN, RST);
    drop_edges();
    print_order();
}

/* ---------- Graphviz ---------- */

/* 2 = pair lies on a real cycle, 1 = only on filtered cycles, 0 = neither */
static int pair_status(int a, int b) {
    int status = 0;
    for (int i = 0; i < g.ncycles; i++) {
        const struct cycle *c = &g.cycles[i];
        for (int hop = 0; hop < c->len; hop++)
            if (c->nodes[hop] == a && c->nodes[(hop + 1) % c->len] == b) {
                int s = verdict(c) < 2 ? 2 : 1;   /* undecided counts as dangerous */
                if (s > status) status = s;
            }
    }
    return status;
}

int write_dot(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    fprintf(f, "digraph lockorder {\n  node [shape=box];\n");
    for (int i = 0; i < g.nnodes; i++) fprintf(f, "  \"%s\";\n", nm(i));
    for (int i = 0; i < g.nedges; i++) {
        int a = g.edges[i].from, b = g.edges[i].to, dup = 0;
        for (int j = 0; j < i; j++) if (g.edges[j].from == a && g.edges[j].to == b) dup = 1;
        if (dup) continue;                       /* one drawn arrow per lock pair */
        int st = pair_status(a, b);
        fprintf(f, "  \"%s\" -> \"%s\" [label=\"%dx\"%s];\n", nm(a), nm(b), pair_count(a, b),
                st == 2 ? ", color=red, penwidth=2" : st == 1 ? ", color=orange, style=dashed" : "");
    }
    fprintf(f, "}\n");
    fclose(f);
    return 0;
}

/* --sites output for `pdp confirm`: one line per REAL or UNDECIDED cycle,
 *   CYCLE <n> <site1>,<site2>,... <name1>-><name2>->...-><name1>
 * The sites are the raw module+0xoff REQ sites of the chosen edges (what the
 * delay injector matches); the names are only for the confirm summary line. */
void print_cycle_sites(void) {
    int k = 0;
    for (int i = 0; i < g.ncycles; i++) {
        const struct cycle *c = &g.cycles[i];
        if (verdict(c) > 1) continue;
        printf("CYCLE %d ", ++k);
        for (int h = 0; h < c->len; h++) printf("%s%s", h ? "," : "", g.edges[c->pick[h]].site);
        printf(" ");
        for (int h = 0; h <= c->len; h++) printf("%s%s", h ? "->" : "", nm(c->nodes[h % c->len]));
        printf("\n");
    }
}
