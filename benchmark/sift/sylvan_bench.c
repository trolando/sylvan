/*
 * Sylvan side of the sifting evaluation.
 *
 * Three cases, all of which build a forest under a known starting order, sift
 * it once with sylvan_sift_report, and report what changed:
 *
 *   --mode bryant --pairs N   f = x0.y0 + ... + x(N-1).y(N-1), built under the
 *                             split order x0..x(N-1) y0..y(N-1), whose optimum
 *                             (the paired order) is known to be about 2N nodes.
 *   --mode blif   --file F    every .outputs signal of a BLIF circuit, built
 *                             under the file's own variable order.
 *   --mode queens --n N       the N-queens board, whose natural order is
 *                             already decent.
 *
 * Everything measured is printed as one "RESULT {...}" JSON line on stdout, so
 * a run is a row and a batch is a .jsonl file.
 *
 * --auto measures the recommendation path instead of the explicit call: the
 * same configuration is installed with sylvan_gc_set_auto_sift and sylvan_gc()
 * is called where the sift would run, so a sift happens only if the growth
 * recommendation asks for one. "sifts" says whether it did. sylvan_gc() has
 * no result to report, so sift_initial, sift_final, swaps, refusals and
 * vars_sifted are 0 in that mode, and sift_s includes the collection that
 * precedes the decision; nodes_before/nodes_after are measured as always.
 *
 * SOUNDNESS. For --mode blif, every output is evaluated against the same set
 * of random input vectors before and after the sift, and its satisfying-
 * assignment count is taken before and after. Both are invariant under
 * reordering: the vectors index variables by identity, and a model count does
 * not depend on the order at all. mtbdd_is_valid additionally checks that
 * every node still has strictly increasing rank down every edge.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lace.h>
#include <sylvan/internal.h>

#include "blif.h"

/* The nodes table the library was built with; CMake defines it. */
#ifndef SIFT_BENCH_BACKEND
#define SIFT_BENCH_BACKEND "unknown"
#endif

/* ------------------------------------------------------------- parameters */

static const char *opt_mode   = "bryant";
static const char *opt_file   = NULL;
static const char *opt_case   = NULL;
static int      opt_pairs     = 6;
static int      opt_queens    = 8;
static int      opt_workers   = 1;
static int      opt_sift      = 1;
static int      opt_verify    = 0;     /* random vectors, 0 = skip           */
static int      opt_ops       = 0;     /* run the downstream op phase        */
static int      opt_order_out = 0;
static uint32_t opt_threshold = 0;     /* library default                    */
static uint32_t opt_maxswaps  = 0;     /* 0 = unlimited                      */
static uint32_t opt_maxvars   = 0;     /* 0 = unlimited                      */
static double   opt_growth    = 1.2;
static double   opt_timelimit = 0.0;   /* seconds, 0 = unlimited             */
static int      opt_fulllegs  = 0;     /* walk every leg to the end          */
static int      opt_auto      = 0;     /* sift through sylvan_gc's auto sift */
static int      opt_stats     = 0;     /* counters of the build and the sift */
static int      opt_batched   = 1;     /* batched index surgery in swaps     */
static uint64_t opt_memory    = 8ull << 30;
static unsigned opt_seed      = 12345;
static const char *opt_varorder = "file";
static unsigned opt_orderseed = 1;

static double
now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + 1e-9 * (double)t.tv_nsec;
}

/* --------------------------------------------------------------- the case */

static blif_t   circuit;
static int     *vperm;      /* circuit input i becomes DD variable vperm[i]   */
static int      n_var;
static int      n_root;
static BDD     *var;        /* var[i]: the variable with identity i          */
static BDD     *root;       /* the diagrams we keep                          */
static BDDSET   varset;

/* Soundness bookkeeping. */
static uint8_t *vectors;    /* opt_verify * n_var, indexed by identity       */
static uint8_t *eval_before;
static uint64_t *sat_before;

static int
alloc_case(int vars, int roots)
{
    n_var = vars;
    n_root = roots;
    var  = (BDD*)malloc((size_t)vars * sizeof(BDD));
    root = (BDD*)malloc((size_t)roots * sizeof(BDD));
    if (var == NULL || root == NULL) return -1;
    /* A protected slot is walked by every collection, so it must hold a valid
       handle from the moment it is protected -- never mtbdd_invalid. */
    for (int i = 0; i < vars; i++) { var[i] = bdd_false; mtbdd_protect(&var[i]); }
    for (int i = 0; i < roots; i++) { root[i] = bdd_false; mtbdd_protect(&root[i]); }
    varset = bdd_set_empty();
    mtbdd_protect(&varset);
    for (int i = 0; i < vars; i++) {
        if (bdd_var_at_level(&var[i], (uint32_t)i) != SYLVAN_OK) return -1;
    }
    return 0;
}

/*
 * The variable set is a conjunction of all n variables, so it is n nodes of
 * pure harness that CUDD's side has no counterpart for. It is dropped before
 * the sift and rebuilt after, so that what sylvan_sift measures and moves is
 * the output cone and the projection functions -- exactly what CUDD sifts.
 * Rebuilding is by identity, so it is correct whatever the new order is.
 */
static int
varset_make(void)
{
    uint32_t *ids = (uint32_t*)malloc((size_t)n_var * sizeof(uint32_t));
    if (ids == NULL) return -1;
    for (int i = 0; i < n_var; i++) ids[i] = (uint32_t)i;
    const int rc = bdd_set_from_array(&varset, ids, (size_t)n_var);
    free(ids);
    return rc == SYLVAN_OK ? 0 : -1;
}

/* ------------------------------------------------------------------ build */

static int
build_bryant_CALL(lace_worker *lace)
{
    /* Identity i is x_i for i < pairs and y_(i-pairs) after, so rank == identity
       is exactly the split order. */
    BDD acc = bdd_false, term = bdd_false;
    mtbdd_protect(&acc);
    mtbdd_protect(&term);
    for (int i = 0; i < opt_pairs; i++) {
        if (bdd_and_CALL(lace, &term, var[i], var[i + opt_pairs]) != SYLVAN_OK) return -1;
        if (bdd_or_CALL(lace, &acc, acc, term) != SYLVAN_OK) return -1;
    }
    root[0] = acc;
    mtbdd_unprotect(&term);
    mtbdd_unprotect(&acc);
    return 0;
}

/* One .names block: the sum of its cubes, negated when the output column is 0. */
static int
build_gate_CALL(lace_worker *lace, const blif_gate *g, BDD *cache, BDD *out)
{
    BDD sum = bdd_false, cube = bdd_true, lit = bdd_false;
    mtbdd_protect(&sum);
    mtbdd_protect(&cube);
    mtbdd_protect(&lit);
    int rc = -1;

    for (int c = 0; c < g->n_cube; c++) {
        cube = bdd_true;
        for (int i = 0; i < g->n_fanin; i++) {
            const char ch = g->cube[c][i];
            if (ch == '-') continue;
            const blif_signal *s = &circuit.sig[g->fanin[i]];
            lit = s->kind == BLIF_VAR ? var[vperm[s->var]] : cache[s->gate];
            if (ch == '0') lit = bdd_not(lit);
            if (bdd_and_CALL(lace, &cube, cube, lit) != SYLVAN_OK) goto done;
        }
        if (bdd_or_CALL(lace, &sum, sum, cube) != SYLVAN_OK) goto done;
    }
    *out = g->polarity ? sum : bdd_not(sum);
    rc = 0;
done:
    mtbdd_unprotect(&lit);
    mtbdd_unprotect(&cube);
    mtbdd_unprotect(&sum);
    return rc;
}

static int
build_blif_CALL(lace_worker *lace)
{
    BDD *cache = (BDD*)malloc((size_t)circuit.n_gate * sizeof(BDD));
    if (cache == NULL) return -1;
    for (int i = 0; i < circuit.n_gate; i++) {
        cache[i] = bdd_false;
        mtbdd_protect(&cache[i]);
    }
    int rc = -1;
    for (int t = 0; t < circuit.n_topo; t++) {
        const int g = circuit.topo[t];
        if (build_gate_CALL(lace, &circuit.gate[g], cache, &cache[g]) != 0) goto done;
    }
    for (int o = 0; o < circuit.n_out; o++) {
        const blif_signal *s = &circuit.sig[circuit.out_sig[o]];
        root[o] = s->kind == BLIF_VAR ? var[vperm[s->var]] : cache[s->gate];
    }
    rc = 0;
done:
    /* Drop every intermediate. What sifting sees must be the output cone and
       nothing else, exactly as on the CUDD side. */
    for (int i = 0; i < circuit.n_gate; i++) mtbdd_unprotect(&cache[i]);
    free(cache);
    return rc;
}

/* The classic N-queens encoding: variable N*i+j is "a queen on row i col j". */
static int
build_queens_CALL(lace_worker *lace)
{
    const int N = opt_queens;
    BDD board = bdd_true, row = bdd_false, cell = bdd_false;
    mtbdd_protect(&board);
    mtbdd_protect(&row);
    mtbdd_protect(&cell);
    int rc = -1;

    for (int i = 0; i < N; i++) {
        /* At least one queen in row i. */
        row = bdd_false;
        for (int j = 0; j < N; j++) {
            if (bdd_or_CALL(lace, &row, row, var[N * i + j]) != SYLVAN_OK) goto done;
        }
        if (bdd_and_CALL(lace, &board, board, row) != SYLVAN_OK) goto done;
    }
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            for (int k = 0; k < N; k++) {
                for (int l = 0; l < N; l++) {
                    if (i == k && j == l) continue;
                    const int attacks = (i == k) || (j == l) ||
                                        (i - j == k - l) || (i + j == k + l);
                    if (!attacks) continue;
                    /* not (queen at (i,j) and queen at (k,l)) */
                    if (bdd_and_CALL(lace, &cell, var[N * i + j], var[N * k + l])
                        != SYLVAN_OK) goto done;
                    if (bdd_and_CALL(lace, &board, board, bdd_not(cell))
                        != SYLVAN_OK) goto done;
                }
            }
        }
    }
    root[0] = board;
    rc = 0;
done:
    mtbdd_unprotect(&cell);
    mtbdd_unprotect(&row);
    mtbdd_unprotect(&board);
    return rc;
}

/* -------------------------------------------------------------- soundness */

/* values[] is indexed by position in the current order; vectors[] by identity. */
static void
values_for(const uint32_t *order, const uint8_t *vec, uint8_t *values)
{
    for (int i = 0; i < n_var; i++) values[i] = vec[order[i]];
}

static int
evaluate_CALL(lace_worker *lace, uint8_t *out)
{
    (void)lace; /* bdd_eval runs on the calling thread and takes no worker */
    uint32_t *order = (uint32_t*)malloc((size_t)n_var * sizeof(uint32_t));
    uint8_t  *values = (uint8_t*)malloc((size_t)n_var);
    if (order == NULL || values == NULL) { free(order); free(values); return -1; }
    bdd_set_to_array(varset, order);

    for (int v = 0; v < opt_verify; v++) {
        values_for(order, vectors + (size_t)v * n_var, values);
        for (int r = 0; r < n_root; r++) {
            BDD res = bdd_false;
            if (bdd_eval(&res, root[r], varset, values, (size_t)n_var)
                != SYLVAN_OK) { free(order); free(values); return -1; }
            out[(size_t)v * n_root + r] = (res == bdd_true) ? 1 : 0;
        }
    }
    free(order);
    free(values);
    return 0;
}

/* Same hash as the CUDD driver computes over the same vectors, so that the two
   drivers can be shown to have built the same functions. */
static uint64_t
fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static int
satcounts_CALL(lace_worker *lace, uint64_t *out)
{
    for (int r = 0; r < n_root; r++) {
        if (bdd_sat_count_u64_CALL(lace, &out[r], root[r], varset) != SYLVAN_OK) {
            return -1;
        }
    }
    return 0;
}

/* An ordinary operation-heavy phase over the same forest: every pair of
   outputs, conjoined and exclusive-ored. Pure BDD work whose cost tracks the
   size of the diagrams it walks, which is the point. */
static int
op_phase_CALL(lace_worker *lace, double *seconds, uint64_t *checksum)
{
    BDD a = bdd_false, b = bdd_false;
    mtbdd_protect(&a);
    mtbdd_protect(&b);
    uint64_t sum = 0;
    const double t0 = now();
    int rc = -1;

    for (int i = 0; i < n_root; i++) {
        for (int j = i + 1; j < n_root; j++) {
            if (bdd_and_CALL(lace, &a, root[i], root[j]) != SYLVAN_OK) goto done;
            if (bdd_xor_CALL(lace, &b, root[i], root[j]) != SYLVAN_OK) goto done;
            uint64_t ca = 0, cb = 0;
            if (bdd_sat_count_u64_CALL(lace, &ca, a, varset) != SYLVAN_OK) goto done;
            if (bdd_sat_count_u64_CALL(lace, &cb, b, varset) != SYLVAN_OK) goto done;
            sum += ca ^ (cb << 1);
        }
    }
    *seconds = now() - t0;
    *checksum = sum;
    rc = 0;
done:
    mtbdd_unprotect(&b);
    mtbdd_unprotect(&a);
    return rc;
}

/* ------------------------------------------------------------------- main */

/*
 * --stats: the library's own counters for one phase, as sylvan_stats_report
 * prints them, on stderr between two marker lines the scripts look for. The
 * counters are reset where the phase starts, so "build" covers the build
 * alone and "sift" the sift alone, including the collection it opens with.
 * A build without -DSYLVAN_STATS=ON has no counters, and says so.
 */
static void
stats_phase(const char *phase)
{
    fprintf(stderr, "SIFT-STATS-BEGIN %s\n", phase);
#if SYLVAN_STATS
    sylvan_stats_report(stderr);
#else
    fprintf(stderr, "(SYLVAN_STATS is off in this build: no counters)\n");
#endif
    fprintf(stderr, "SIFT-STATS-END %s\n", phase);
    fflush(stderr);
}

static void
print_order(void)
{
    printf(", \"order_after\":[");
    for (int i = 0; i < n_var; i++) {
        printf("%s%u", i ? "," : "", (unsigned)sylvan_identity_at((uint32_t)i));
    }
    printf("]");
}

TASK(int, run)

int
run_CALL(lace_worker *lace)
{
    const int is_blif = strcmp(opt_mode, "blif") == 0;
    int vars, roots;

    if (is_blif) {
        vars = circuit.n_var;
        roots = circuit.n_out;
    } else if (strcmp(opt_mode, "bryant") == 0) {
        vars = 2 * opt_pairs;
        roots = 1;
    } else {
        vars = opt_queens * opt_queens;
        roots = 1;
    }
    if (alloc_case(vars, roots) != 0) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    if (opt_stats) sylvan_stats_reset_CALL(lace);
    const double t_build = now();
    int rc;
    if (is_blif)                            rc = build_blif_CALL(lace);
    else if (strcmp(opt_mode, "bryant") == 0) rc = build_bryant_CALL(lace);
    else                                    rc = build_queens_CALL(lace);
    const double build_s = now() - t_build;
    if (rc != 0) { fprintf(stderr, "build failed\n"); return 1; }
    if (opt_stats) stats_phase("build");

    const size_t nodes_before = mtbdd_shared_node_count(root, (size_t)n_root);
    if (varset_make() != 0) { fprintf(stderr, "varset failed\n"); return 1; }

    /* Soundness inputs, taken before anything moves. */
    uint8_t *eval_after = NULL;
    uint64_t *sat_after = NULL;
    if (opt_verify > 0) {
        vectors = (uint8_t*)malloc((size_t)opt_verify * n_var);
        eval_before = (uint8_t*)malloc((size_t)opt_verify * n_root);
        eval_after  = (uint8_t*)malloc((size_t)opt_verify * n_root);
        sat_before  = (uint64_t*)malloc((size_t)n_root * sizeof(uint64_t));
        sat_after   = (uint64_t*)malloc((size_t)n_root * sizeof(uint64_t));
        if (vectors == NULL || eval_before == NULL || eval_after == NULL ||
            sat_before == NULL || sat_after == NULL) return 1;
        srand(opt_seed);
        for (int v = 0; v < opt_verify; v++) {
            for (int i = 0; i < n_var; i++) {
                vectors[(size_t)v * n_var + i] = (uint8_t)(rand() & 1);
            }
        }
        if (evaluate_CALL(lace, eval_before) != 0) return 1;
        if (satcounts_CALL(lace, sat_before) != 0) return 1;
    }

    double ops_before_s = -1.0, ops_after_s = -1.0;
    uint64_t ops_before_sum = 0, ops_after_sum = 0;
    if (opt_ops) {
        if (op_phase_CALL(lace, &ops_before_s, &ops_before_sum) != 0) return 1;
    }

    /* Out of the way, so the sift sees the output cone and nothing else. */
    varset = bdd_set_empty();

    sylvan_sift_result result;
    memset(&result, 0, sizeof(result));
    double sift_s = -1.0;
    int sift_status = SYLVAN_OK;
    if (opt_stats) sylvan_stats_reset_CALL(lace);
    const uint64_t sifts_before = sylvan_sift_count();
    if (opt_sift) {
        sylvan_sift_config config;
        memset(&config, 0, sizeof(config));
        config.max_swaps = opt_maxswaps;
        config.max_vars = opt_maxvars;
        config.max_growth = (float)opt_growth;
        config.nodes_threshold = opt_threshold;
        config.time_limit = opt_timelimit;
        config.full_legs = opt_fulllegs;

        if (opt_auto) {
            sylvan_gc_set_auto_sift(&config);
            const double t0 = now();
            sylvan_gc_CALL(lace);
            sift_s = now() - t0;
            sylvan_gc_set_auto_sift(NULL);
        } else {
            const double t0 = now();
            sift_status = sylvan_sift_report(&config, &result);
            sift_s = now() - t0;
        }
    }
    const uint64_t sifts = sylvan_sift_count() - sifts_before;
    if (opt_stats) stats_phase(opt_sift ? "sift" : "no-sift");

    const size_t nodes_after = mtbdd_shared_node_count(root, (size_t)n_root);
    if (varset_make() != 0) { fprintf(stderr, "varset rebuild failed\n"); return 1; }

    int sound = -1;
    int valid = 1;
    uint64_t eval_hash = 0;
    if (opt_verify > 0) {
        if (evaluate_CALL(lace, eval_after) != 0) return 1;
        eval_hash = fnv(eval_after, (size_t)opt_verify * n_root);
        if (satcounts_CALL(lace, sat_after) != 0) return 1;
        sound = 1;
        if (memcmp(eval_before, eval_after, (size_t)opt_verify * n_root) != 0) sound = 0;
        for (int r = 0; r < n_root; r++) {
            if (sat_before[r] != sat_after[r]) sound = 0;
        }
        for (int r = 0; r < n_root; r++) {
            if (!mtbdd_is_valid_CALL(lace, root[r])) valid = 0;
        }
    }
    if (opt_ops) {
        if (op_phase_CALL(lace, &ops_after_s, &ops_after_sum) != 0) return 1;
    }

    printf("RESULT {\"tool\":\"sylvan\",\"mode\":\"%s\",\"case\":\"%s\""
           ",\"workers\":%d,\"sift\":%d"
           ",\"threshold\":%u,\"max_swaps\":%u,\"max_vars\":%u,\"growth\":%.3f"
           ",\"var_order\":\"%s\",\"order_seed\":%u"
           ",\"vars\":%d,\"roots\":%d"
           ",\"build_s\":%.6f,\"nodes_before\":%zu,\"nodes_after\":%zu"
           ",\"sift_s\":%.6f,\"sift_status\":%d"
           ",\"sift_initial\":%llu,\"sift_final\":%llu"
           ",\"swaps\":%llu,\"refusals\":%llu,\"vars_sifted\":%u"
           ",\"sound\":%d,\"valid\":%d,\"eval_hash\":\"%llx\""
           ",\"ops_before_s\":%.6f,\"ops_after_s\":%.6f"
           ",\"ops_before_sum\":%llu,\"ops_after_sum\":%llu"
           ",\"backend\":\"%s\",\"full_legs\":%d,\"time_limit\":%.3f"
           ",\"auto\":%d,\"sifts\":%llu,\"batched\":%d",
           opt_mode, opt_case ? opt_case : "",
           opt_workers, opt_sift,
           opt_threshold, opt_maxswaps, opt_maxvars, opt_growth,
           opt_varorder, opt_orderseed,
           n_var, n_root,
           build_s, nodes_before, nodes_after,
           sift_s, sift_status,
           (unsigned long long)result.initial_size,
           (unsigned long long)result.final_size,
           (unsigned long long)result.swaps,
           (unsigned long long)result.refusals,
           (unsigned)result.vars_sifted,
           sound, valid, (unsigned long long)eval_hash,
           ops_before_s, ops_after_s,
           (unsigned long long)ops_before_sum,
           (unsigned long long)ops_after_sum,
           SIFT_BENCH_BACKEND, opt_fulllegs, opt_timelimit,
           opt_auto, (unsigned long long)sifts, opt_batched);
    if (opt_verify > 0) {
        printf(", \"satsum\":[");
        for (int r = 0; r < n_root && r < 8; r++) {
            printf("%s%llu", r ? "," : "", (unsigned long long)sat_after[r]);
        }
        printf("]");
    }
    if (opt_order_out) print_order();
    printf("}\n");
    fflush(stdout);

    free(vectors); free(eval_before); free(eval_after);
    free(sat_before); free(sat_after);
    for (int i = 0; i < n_root; i++) mtbdd_unprotect(&root[i]);
    for (int i = 0; i < n_var; i++) mtbdd_unprotect(&var[i]);
    mtbdd_unprotect(&varset);
    free(var); free(root);
    return 0;
}

static void
usage(FILE *f)
{
    fprintf(f,
        "usage: sylvan_bench [options]\n"
        "  case:   --mode bryant|blif|queens  --pairs N  --file F.blif  --n N\n"
        "          --case NAME  --var-order file|reverse|random  --order-seed S\n"
        "  sift:   --threshold N (0)  --growth G (1.2)  --max-swaps N (0)\n"
        "          --max-vars N (0)  --time-limit S (0)  --full-legs  --auto\n"
        "          --no-sift  --no-batch (per-node index surgery in swaps)\n"
        "  checks: --verify VECTORS  --seed S  --ops  --order\n"
        "          --stats  (counters of the build and of the sift, on stderr;\n"
        "                    needs a -DSYLVAN_STATS=ON build)\n"
        "  run:    --workers N (1)  --memory BYTES (8 GiB)\n"
        "0 means unlimited for --max-swaps, --max-vars and --time-limit.\n");
}

int
main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if      (!strcmp(a, "--help"))           { usage(stdout); return 0; }
        else if (!strcmp(a, "--mode") && v)      { opt_mode = v; i++; }
        else if (!strcmp(a, "--file") && v)      { opt_file = v; i++; }
        else if (!strcmp(a, "--case") && v)      { opt_case = v; i++; }
        else if (!strcmp(a, "--pairs") && v)     { opt_pairs = atoi(v); i++; }
        else if (!strcmp(a, "--n") && v)         { opt_queens = atoi(v); i++; }
        else if (!strcmp(a, "--workers") && v)   { opt_workers = atoi(v); i++; }
        else if (!strcmp(a, "--no-sift"))        { opt_sift = 0; }
        else if (!strcmp(a, "--verify") && v)    { opt_verify = atoi(v); i++; }
        else if (!strcmp(a, "--ops"))            { opt_ops = 1; }
        else if (!strcmp(a, "--order"))          { opt_order_out = 1; }
        else if (!strcmp(a, "--threshold") && v) { opt_threshold = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--max-swaps") && v) { opt_maxswaps = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--max-vars") && v)  { opt_maxvars = (uint32_t)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--growth") && v)    { opt_growth = atof(v); i++; }
        else if (!strcmp(a, "--time-limit") && v){ opt_timelimit = atof(v); i++; }
        else if (!strcmp(a, "--full-legs"))      { opt_fulllegs = 1; }
        else if (!strcmp(a, "--auto"))           { opt_auto = 1; }
        else if (!strcmp(a, "--stats"))          { opt_stats = 1; }
        else if (!strcmp(a, "--no-batch"))       { opt_batched = 0; }
        else if (!strcmp(a, "--memory") && v)    { opt_memory = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--seed") && v)      { opt_seed = (unsigned)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--var-order") && v) { opt_varorder = v; i++; }
        else if (!strcmp(a, "--order-seed") && v){ opt_orderseed = (unsigned)strtoul(v, NULL, 10); i++; }
        else { fprintf(stderr, "unknown option %s\n", a); usage(stderr); return 2; }
    }
    if (strcmp(opt_mode, "blif") != 0 && strcmp(opt_mode, "bryant") != 0 &&
        strcmp(opt_mode, "queens") != 0) {
        fprintf(stderr, "unknown --mode %s\n", opt_mode);
        return 2;
    }

    if (strcmp(opt_mode, "blif") == 0) {
        if (opt_file == NULL) { fprintf(stderr, "--file required\n"); return 2; }
        char err[512];
        if (blif_read(&circuit, opt_file, err, sizeof(err)) != 0) {
            fprintf(stderr, "blif: %s\n", err);
            return 1;
        }
        fprintf(stderr, "circuit %s: %d vars (%d latch) %d outputs %d gates "
                "(%d reachable)\n", circuit.model ? circuit.model : "?",
                circuit.n_var, circuit.n_latch, circuit.n_out, circuit.n_gate,
                circuit.n_topo);
        vperm = (int*)malloc((size_t)circuit.n_var * sizeof(int));
        if (vperm == NULL ||
            blif_var_perm(circuit.n_var, opt_varorder, opt_orderseed, vperm) != 0) {
            fprintf(stderr, "bad --var-order %s\n", opt_varorder);
            return 2;
        }
    }

    lace_start((unsigned)opt_workers, 0, 128u * 1024u * 1024u);
    sylvan_set_limits(opt_memory, 1, 4);
    if (sylvan_init_package() != SYLVAN_OK || mtbdd_init() != SYLVAN_OK) {
        fprintf(stderr, "sylvan init failed\n");
        return 1;
    }
    sylvan_reorder_set_batched(opt_batched);

    const int rc = run();

    sylvan_quit();
    lace_stop();
    if (strcmp(opt_mode, "blif") == 0) blif_free(&circuit);
    return rc;
}
