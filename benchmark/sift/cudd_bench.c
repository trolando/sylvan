/*
 * CUDD side of the sifting evaluation.
 *
 * The same three cases as sylvan_bench.c, built from the same shared BLIF
 * reader, gate for gate in the same topological order, under the same starting
 * variable order. The reordering call is the plain one:
 *
 *     Cudd_ReduceHeap(dd, CUDD_REORDER_SIFT, 0)
 *
 * so the algorithm class matches sylvan_sift: Rudell sifting, no grouping, no
 * convergence, no symmetry check.
 *
 * Knob alignment (see the report): maxGrowth is set to the same 1.2 both tools
 * default to, and siftMaxVar/siftMaxSwap are raised to INT_MAX so that CUDD is
 * unlimited exactly as Sylvan's max_vars = max_swaps = 0 is. Dynamic
 * reordering is disabled explicitly, because Sylvan never reorders on its own
 * and the initial order has to be the file's. --max-swaps and --max-vars cap
 * the search the way sylvan_bench's flags of the same name do, for the
 * experiments that sweep the caps on both tools.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>

#include "cudd.h"
/* For dd->ddTotalNumberSwapping, which is the only counter that actually counts
   adjacent swaps: the public Cudd_ReadSwapSteps reads dd->swapSteps, which
   accumulates node-count deltas rather than swaps and is a different quantity.
   Cudd_ReduceHeap zeroes ddTotalNumberSwapping on entry, so reading it after the
   call gives exactly the swaps that call made -- search legs and return legs
   alike, which is what sylvan_sift_result.swaps counts too. */
#include "cuddInt.h"
#include "blif.h"

static const char *opt_mode   = "bryant";
static const char *opt_file   = NULL;
static const char *opt_case   = NULL;
static int      opt_pairs     = 6;
static int      opt_queens    = 8;
static int      opt_sift      = 1;
static int      opt_verify    = 0;
static int      opt_ops       = 0;
static int      opt_order_out = 0;
static double   opt_growth    = 1.2;
static size_t   opt_memory    = 8ull << 30;
static unsigned opt_seed      = 12345;
static int      opt_unlimited = 1;   /* --cudd-defaults turns this off */
static long     opt_maxswaps  = -1;  /* --max-swaps; -1 leaves the cap as is */
static long     opt_maxvars   = -1;  /* --max-vars; 0 is unlimited, like Sylvan */
static int      opt_memstat   = 0;   /* resident memory from /proc */
static const char *opt_varorder = "file";
static unsigned opt_orderseed = 1;
static const char *opt_trace  = NULL;  /* --trace: one line per swap */
static int     *vperm;               /* circuit input i -> DD variable */

static double
now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* --memstat: resident memory as the kernel reports it, read from /proc after
   the build and after the sift. Linux only; 0 wherever a file or key is
   missing. */
typedef struct { long rss_kb, thp_kb; } memstat_t;

static long
proc_kb(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) return 0;
    char line[256];
    long kb = 0;
    const size_t n = strlen(key);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, n) == 0 && line[n] == ':') {
            kb = strtol(line + n + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return kb;
}

static void
memstat_take(memstat_t *m)
{
    m->rss_kb = proc_kb("/proc/self/status", "VmRSS");
    if (m->rss_kb == 0) m->rss_kb = proc_kb("/proc/self/smaps_rollup", "Rss");
    m->thp_kb = proc_kb("/proc/self/smaps_rollup", "AnonHugePages");
}

static void
memstat_print(const memstat_t *build, const memstat_t *sift)
{
    printf(",\"rss_build_kb\":%ld,\"rss_sift_kb\":%ld,\"hwm_kb\":%ld"
           ",\"thp_build_kb\":%ld,\"thp_sift_kb\":%ld",
           build->rss_kb, sift->rss_kb, proc_kb("/proc/self/status", "VmHWM"),
           build->thp_kb, sift->thp_kb);
}

static blif_t   circuit;
static DdManager *dd;
static int      n_var;
static int      n_root;
static DdNode **root;

/* --------------------------------------------------------------- building */

static DdNode *
build_gate(const blif_gate *g, DdNode **cache)
{
    DdNode *sum = Cudd_ReadLogicZero(dd);
    Cudd_Ref(sum);

    for (int c = 0; c < g->n_cube; c++) {
        DdNode *cube = Cudd_ReadOne(dd);
        Cudd_Ref(cube);
        for (int i = 0; i < g->n_fanin; i++) {
            const char ch = g->cube[c][i];
            if (ch == '-') continue;
            const blif_signal *s = &circuit.sig[g->fanin[i]];
            DdNode *lit = s->kind == BLIF_VAR
                            ? Cudd_bddIthVar(dd, vperm[s->var])
                            : cache[s->gate];
            if (ch == '0') lit = Cudd_Not(lit);
            DdNode *t = Cudd_bddAnd(dd, cube, lit);
            if (t == NULL) return NULL;
            Cudd_Ref(t);
            Cudd_RecursiveDeref(dd, cube);
            cube = t;
        }
        DdNode *t = Cudd_bddOr(dd, sum, cube);
        if (t == NULL) return NULL;
        Cudd_Ref(t);
        Cudd_RecursiveDeref(dd, sum);
        Cudd_RecursiveDeref(dd, cube);
        sum = t;
    }
    /* The reference is held on the regular node, so complementing is free and
       RecursiveDeref on the complemented pointer releases the same node. */
    return g->polarity ? sum : Cudd_Not(sum);
}

static int
build_blif(void)
{
    DdNode **cache = (DdNode**)calloc((size_t)circuit.n_gate, sizeof(DdNode*));
    if (cache == NULL) return -1;
    for (int t = 0; t < circuit.n_topo; t++) {
        const int g = circuit.topo[t];
        cache[g] = build_gate(&circuit.gate[g], cache);
        if (cache[g] == NULL) { free(cache); return -1; }
    }
    for (int o = 0; o < circuit.n_out; o++) {
        const blif_signal *s = &circuit.sig[circuit.out_sig[o]];
        root[o] = s->kind == BLIF_VAR ? Cudd_bddIthVar(dd, vperm[s->var])
                                      : cache[s->gate];
        Cudd_Ref(root[o]);
    }
    /* Drop every intermediate, so what sifting sees is the output cone only. */
    for (int t = 0; t < circuit.n_topo; t++) {
        Cudd_RecursiveDeref(dd, cache[circuit.topo[t]]);
    }
    free(cache);
    return 0;
}

static int
build_bryant(void)
{
    DdNode *acc = Cudd_ReadLogicZero(dd);
    Cudd_Ref(acc);
    for (int i = 0; i < opt_pairs; i++) {
        DdNode *t = Cudd_bddAnd(dd, Cudd_bddIthVar(dd, i),
                                Cudd_bddIthVar(dd, i + opt_pairs));
        if (t == NULL) return -1;
        Cudd_Ref(t);
        DdNode *u = Cudd_bddOr(dd, acc, t);
        if (u == NULL) return -1;
        Cudd_Ref(u);
        Cudd_RecursiveDeref(dd, acc);
        Cudd_RecursiveDeref(dd, t);
        acc = u;
    }
    root[0] = acc;
    return 0;
}

static int
build_queens(void)
{
    const int N = opt_queens;
    DdNode *board = Cudd_ReadOne(dd);
    Cudd_Ref(board);

    for (int i = 0; i < N; i++) {
        DdNode *row = Cudd_ReadLogicZero(dd);
        Cudd_Ref(row);
        for (int j = 0; j < N; j++) {
            DdNode *t = Cudd_bddOr(dd, row, Cudd_bddIthVar(dd, N * i + j));
            if (t == NULL) return -1;
            Cudd_Ref(t);
            Cudd_RecursiveDeref(dd, row);
            row = t;
        }
        DdNode *t = Cudd_bddAnd(dd, board, row);
        if (t == NULL) return -1;
        Cudd_Ref(t);
        Cudd_RecursiveDeref(dd, board);
        Cudd_RecursiveDeref(dd, row);
        board = t;
    }
    for (int i = 0; i < N; i++) {
      for (int j = 0; j < N; j++) {
        for (int k = 0; k < N; k++) {
          for (int l = 0; l < N; l++) {
            if (i == k && j == l) continue;
            const int attacks = (i == k) || (j == l) ||
                                (i - j == k - l) || (i + j == k + l);
            if (!attacks) continue;
            DdNode *c = Cudd_bddAnd(dd, Cudd_bddIthVar(dd, N * i + j),
                                    Cudd_bddIthVar(dd, N * k + l));
            if (c == NULL) return -1;
            Cudd_Ref(c);
            DdNode *t = Cudd_bddAnd(dd, board, Cudd_Not(c));
            if (t == NULL) return -1;
            Cudd_Ref(t);
            Cudd_RecursiveDeref(dd, board);
            Cudd_RecursiveDeref(dd, c);
            board = t;
          }
        }
      }
    }
    root[0] = board;
    return 0;
}

/* -------------------------------------------------------------- soundness */

static uint8_t *vectors;

static void
evaluate(uint8_t *out)
{
    int *inputs = (int*)malloc((size_t)n_var * sizeof(int));
    for (int v = 0; v < opt_verify; v++) {
        /* Cudd_Eval indexes by variable index, which reordering does not
           change, so the same vector means the same assignment throughout. */
        for (int i = 0; i < n_var; i++) {
            inputs[i] = vectors[(size_t)v * n_var + i];
        }
        for (int r = 0; r < n_root; r++) {
            DdNode *res = Cudd_Eval(dd, root[r], inputs);
            out[(size_t)v * n_root + r] = (res == Cudd_ReadOne(dd)) ? 1 : 0;
        }
    }
    free(inputs);
}

static uint64_t
fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static void
op_phase(double *seconds, double *checksum)
{
    double sum = 0.0;
    const double t0 = now();
    for (int i = 0; i < n_root; i++) {
        for (int j = i + 1; j < n_root; j++) {
            DdNode *a = Cudd_bddAnd(dd, root[i], root[j]);
            Cudd_Ref(a);
            DdNode *b = Cudd_bddXor(dd, root[i], root[j]);
            Cudd_Ref(b);
            sum += Cudd_CountMinterm(dd, a, n_var);
            sum += 2.0 * Cudd_CountMinterm(dd, b, n_var);
            Cudd_RecursiveDeref(dd, a);
            Cudd_RecursiveDeref(dd, b);
        }
    }
    *seconds = now() - t0;
    *checksum = sum;
}

/* ------------------------------------------------------------------- main */

/* A cap given on the command line: 0 means unlimited, as Sylvan's max_swaps
   and max_vars do, which CUDD spells INT_MAX. */
static int
cudd_cap(long n)
{
    return (n <= 0 || n > INT_MAX) ? INT_MAX : (int)n;
}

static void
usage(FILE *f)
{
    fprintf(f,
        "usage: cudd_bench [options]\n"
        "  case:   --mode bryant|blif|queens  --pairs N  --file F.blif  --n N\n"
        "          --case NAME  --var-order file|reverse|random  --order-seed S\n"
        "  sift:   --growth G (1.2)  --max-swaps N  --max-vars N  --cudd-defaults\n"
        "          --no-sift\n"
        "  checks: --verify VECTORS  --seed S  --ops  --order\n"
        "          --memstat  (VmRSS, VmHWM and AnonHugePages after the build\n"
        "                      and after the sift, from /proc; Linux)\n"
        "  run:    --memory BYTES (8 GiB)\n"
        "siftMaxSwap and siftMaxVar are raised to INT_MAX (unlimited, like Sylvan's\n"
        "defaults) unless --cudd-defaults keeps CUDD's own caps; --max-swaps and\n"
        "--max-vars set either cap on top of that, 0 meaning unlimited.\n");
}

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if      (!strcmp(a, "--help"))        { usage(stdout); return 0; }
        else if (!strcmp(a, "--mode") && v)   { opt_mode = v; i++; }
        else if (!strcmp(a, "--file") && v)   { opt_file = v; i++; }
        else if (!strcmp(a, "--case") && v)   { opt_case = v; i++; }
        else if (!strcmp(a, "--pairs") && v)  { opt_pairs = atoi(v); i++; }
        else if (!strcmp(a, "--n") && v)      { opt_queens = atoi(v); i++; }
        else if (!strcmp(a, "--no-sift"))     { opt_sift = 0; }
        else if (!strcmp(a, "--verify") && v) { opt_verify = atoi(v); i++; }
        else if (!strcmp(a, "--ops"))         { opt_ops = 1; }
        else if (!strcmp(a, "--order"))       { opt_order_out = 1; }
        else if (!strcmp(a, "--growth") && v) { opt_growth = atof(v); i++; }
        else if (!strcmp(a, "--memory") && v) { opt_memory = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--seed") && v)   { opt_seed = (unsigned)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--cudd-defaults")) { opt_unlimited = 0; }
        else if (!strcmp(a, "--max-swaps") && v) { opt_maxswaps = strtol(v, NULL, 10); i++; }
        else if (!strcmp(a, "--max-vars") && v)  { opt_maxvars = strtol(v, NULL, 10); i++; }
        else if (!strcmp(a, "--memstat"))       { opt_memstat = 1; }
        else if (!strcmp(a, "--var-order") && v) { opt_varorder = v; i++; }
        else if (!strcmp(a, "--order-seed") && v){ opt_orderseed = (unsigned)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--trace") && v)  { opt_trace = v; i++; }
        else { fprintf(stderr, "unknown option %s\n", a); usage(stderr); return 2; }
    }

    /* --trace: CUDD has no swap hook, so cudd-swap-trace.patch adds one to
       cuddSwapInPlace that writes to the file this variable names (see
       README.md for the line format). Nothing else reorders before the sift,
       so every line it writes belongs to the sift. A file left over from an
       earlier run is removed first, so that an unpatched CUDD, which writes
       nothing, cannot pass someone else's trace off as this one. */
    if (opt_trace != NULL && opt_sift) {
        (void)remove(opt_trace);
        if (setenv("CUDD_SWAP_TRACE", opt_trace, 1) != 0) {
            perror("setenv");
            return 1;
        }
    }

    const int is_blif = strcmp(opt_mode, "blif") == 0;
    if (is_blif) {
        if (opt_file == NULL) { fprintf(stderr, "--file required\n"); return 2; }
        char err[512];
        if (blif_read(&circuit, opt_file, err, sizeof(err)) != 0) {
            fprintf(stderr, "blif: %s\n", err);
            return 1;
        }
        n_var = circuit.n_var;
        n_root = circuit.n_out;
        vperm = (int*)malloc((size_t)n_var * sizeof(int));
        if (vperm == NULL ||
            blif_var_perm(n_var, opt_varorder, opt_orderseed, vperm) != 0) {
            fprintf(stderr, "bad --var-order %s\n", opt_varorder);
            return 2;
        }
    } else if (strcmp(opt_mode, "bryant") == 0) {
        n_var = 2 * opt_pairs;
        n_root = 1;
    } else {
        n_var = opt_queens * opt_queens;
        n_root = 1;
    }

    dd = Cudd_Init((unsigned)n_var, 0, CUDD_UNIQUE_SLOTS, CUDD_CACHE_SLOTS,
                   opt_memory);
    if (dd == NULL) { fprintf(stderr, "Cudd_Init failed\n"); return 1; }
    Cudd_AutodynDisable(dd);
    Cudd_SetMaxGrowth(dd, opt_growth);
    if (opt_unlimited) {
        Cudd_SetSiftMaxVar(dd, INT_MAX);
        Cudd_SetSiftMaxSwap(dd, INT_MAX);
    }
    if (opt_maxswaps >= 0) Cudd_SetSiftMaxSwap(dd, cudd_cap(opt_maxswaps));
    if (opt_maxvars >= 0) Cudd_SetSiftMaxVar(dd, cudd_cap(opt_maxvars));
    /* The row's "unlimited" says whether both caps are in fact raised, so that
       a capped run is never taken for a like-for-like one. */
    opt_unlimited = Cudd_ReadSiftMaxSwap(dd) == INT_MAX &&
                    Cudd_ReadSiftMaxVar(dd) == INT_MAX;
    /* Touch every projection function so the starting order is 0..n-1. */
    for (int i = 0; i < n_var; i++) (void)Cudd_bddIthVar(dd, i);

    root = (DdNode**)calloc((size_t)n_root, sizeof(DdNode*));

    const double t_build = now();
    int rc = is_blif ? build_blif()
                     : (strcmp(opt_mode, "bryant") == 0 ? build_bryant()
                                                        : build_queens());
    const double build_s = now() - t_build;
    if (rc != 0) { fprintf(stderr, "build failed\n"); return 1; }
    memstat_t mem_build = {0, 0}, mem_sift = {0, 0};
    if (opt_memstat) memstat_take(&mem_build);

    const int nodes_before = Cudd_SharingSize(root, n_root);
    const long live_before = Cudd_ReadNodeCount(dd);

    uint8_t *eval_before = NULL, *eval_after = NULL;
    double *mint_before = NULL, *mint_after = NULL;
    if (opt_verify > 0) {
        vectors = (uint8_t*)malloc((size_t)opt_verify * n_var);
        eval_before = (uint8_t*)malloc((size_t)opt_verify * n_root);
        eval_after  = (uint8_t*)malloc((size_t)opt_verify * n_root);
        mint_before = (double*)malloc((size_t)n_root * sizeof(double));
        mint_after  = (double*)malloc((size_t)n_root * sizeof(double));
        srand(opt_seed);
        for (int v = 0; v < opt_verify; v++) {
            for (int i = 0; i < n_var; i++) {
                vectors[(size_t)v * n_var + i] = (uint8_t)(rand() & 1);
            }
        }
        evaluate(eval_before);
        for (int r = 0; r < n_root; r++) {
            mint_before[r] = Cudd_CountMinterm(dd, root[r], n_var);
        }
    }

    double ops_before_s = -1.0, ops_after_s = -1.0;
    double ops_before_sum = 0.0, ops_after_sum = 0.0;
    if (opt_ops) op_phase(&ops_before_s, &ops_before_sum);

    double sift_s = -1.0;
    int sift_ok = 1;
    double swaps = 0.0;
    if (opt_sift) {
        const double t0 = now();
        sift_ok = Cudd_ReduceHeap(dd, CUDD_REORDER_SIFT, 0);
        sift_s = now() - t0;
        swaps = (double)dd->ddTotalNumberSwapping;
    }
    if (opt_trace != NULL && opt_sift) {
        /* The patch opens its stream at the first swap and keeps it open for
           the life of the process. A sift without swaps leaves no file, so
           write the empty trace it stands for. */
        fflush(NULL);
        FILE *check = fopen(opt_trace, swaps > 0 ? "r" : "a");
        if (check != NULL) {
            fclose(check);
        } else if (swaps > 0) {
            fprintf(stderr, "cudd_bench: --trace: no %s was written; build CUDD "
                    "with benchmark/sift/cudd-swap-trace.patch\n", opt_trace);
        } else {
            perror(opt_trace);
        }
    }
    if (opt_memstat) memstat_take(&mem_sift);

    const int nodes_after = Cudd_SharingSize(root, n_root);
    const long live_after = Cudd_ReadNodeCount(dd);

    int sound = -1;
    int valid = 1;
    uint64_t eval_hash = 0;
    if (opt_verify > 0) {
        evaluate(eval_after);
        for (int r = 0; r < n_root; r++) {
            mint_after[r] = Cudd_CountMinterm(dd, root[r], n_var);
        }
        sound = 1;
        if (memcmp(eval_before, eval_after, (size_t)opt_verify * n_root) != 0) sound = 0;
        for (int r = 0; r < n_root; r++) {
            if (mint_before[r] != mint_after[r]) sound = 0;
        }
        valid = Cudd_DebugCheck(dd) == 0;
        eval_hash = fnv(eval_after, (size_t)opt_verify * n_root);
    }
    if (opt_ops) op_phase(&ops_after_s, &ops_after_sum);

    printf("RESULT {\"tool\":\"cudd\",\"mode\":\"%s\",\"case\":\"%s\""
           ",\"workers\":1,\"sift\":%d,\"unlimited\":%d,\"growth\":%.3f"
           ",\"var_order\":\"%s\",\"order_seed\":%u"
           ",\"vars\":%d,\"roots\":%d"
           ",\"build_s\":%.6f,\"nodes_before\":%d,\"nodes_after\":%d"
           ",\"live_before\":%ld,\"live_after\":%ld"
           ",\"sift_s\":%.6f,\"sift_ok\":%d,\"swaps\":%.0f"
           ",\"sound\":%d,\"valid\":%d,\"eval_hash\":\"%llx\""
           ",\"ops_before_s\":%.6f,\"ops_after_s\":%.6f"
           ",\"ops_before_sum\":%.6g,\"ops_after_sum\":%.6g",
           opt_mode, opt_case ? opt_case : "",
           opt_sift, opt_unlimited, opt_growth,
           opt_varorder, opt_orderseed,
           n_var, n_root,
           build_s, nodes_before, nodes_after,
           live_before, live_after,
           sift_s, sift_ok, swaps,
           sound, valid, (unsigned long long)eval_hash,
           ops_before_s, ops_after_s, ops_before_sum, ops_after_sum);
    if (opt_verify > 0) {
        printf(", \"mintsum\":[");
        for (int r = 0; r < n_root && r < 8; r++) {
            printf("%s%.0f", r ? "," : "", mint_after[r]);
        }
        printf("]");
    }
    if (opt_order_out) {
        printf(", \"order_after\":[");
        for (int i = 0; i < n_var; i++) {
            printf("%s%d", i ? "," : "", Cudd_ReadInvPerm(dd, i));
        }
        printf("]");
    }
    if (opt_memstat) memstat_print(&mem_build, &mem_sift);
    printf("}\n");
    fflush(stdout);

    for (int r = 0; r < n_root; r++) Cudd_RecursiveDeref(dd, root[r]);
    Cudd_Quit(dd);
    free(root); free(vectors);
    free(eval_before); free(eval_after); free(mint_before); free(mint_after);
    if (is_blif) blif_free(&circuit);
    return 0;
}
