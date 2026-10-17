/* analyze.h - data shared by analyze.c (parse + graph + cycles) and report.c (output). */
#ifndef ANALYZE_H
#define ANALYZE_H

#define MAX_NODES   512
#define MAX_EDGES   4096
#define MAX_CYCLES  64
#define MAX_CYC_LEN 32
#define MAX_COMBOS  200000      /* cap on edge-choice combinations tried per cycle */
#define MAX_HELDSET 64
#define SITE_LEN    256

/* A lock = address + generation (see node_for in analyze.c). */
struct node {
    unsigned long addr;
    int  gen;                   /* 0 for the first lock at this address, 1 after a DESTROY, ... */
    int  dead;                  /* 1 once DESTROY was seen */
    char kind;                  /* M or S */
    char lk[SITE_LEN];          /* "module+0xoff" of the lock object, or "?" */
    char name[96];              /* filled by name_locks() */
};

/* "thread tid, at site, asked for `to` while holding the locks in held[]". */
struct edge {
    int  from, to;
    long tid;
    char site[SITE_LEN];
    int  held[MAX_HELDSET];     /* node ids, includes `from` */
    int  nheld;
    int  count;                 /* how many times this exact situation was logged */
};

struct cycle {
    int len;
    int nodes[MAX_CYC_LEN];     /* hop i is nodes[i] -> nodes[(i+1) % len] */
    int real;                   /* 1 = REAL, 0 = FILTERED */
    int pick[MAX_CYC_LEN];      /* chosen edge index per hop */
    int gate;                   /* FILTERED: node id of the gate lock, or -1 = same-thread */
    int capped;                 /* search hit MAX_COMBOS */
};

struct graph {
    struct node  nodes[MAX_NODES];  int nnodes;
    struct edge  edges[MAX_EDGES];  int nedges;
    struct cycle cycles[MAX_CYCLES]; int ncycles;
    int unknown_held;           /* REQ lines skipped because held=? */
    int log_warns;              /* "# pdp WARN" lines in the log */
    int dropped;                /* things lost to the fixed-size tables above */
};
extern struct graph g;

/* report.c */
void report_init(int use_color);
void name_locks(void);
void print_report(void);
void print_fix(void);
int  write_dot(const char *path);

#endif
