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
 * --recommend-factor F sets the growth factor that recommendation compares
 * against (sylvan_sift_set_recommend_factor; the library's 2.0 otherwise).
 *
 * SOUNDNESS. For --mode blif, every output is evaluated against the same set
 * of random input vectors before and after the sift, and its satisfying-
 * assignment count is taken before and after. Both are invariant under
 * reordering: the vectors index variables by identity, and a model count does
 * not depend on the order at all. mtbdd_is_valid additionally checks that
 * every node still has strictly increasing rank down every edge.
 *
 * The count is a double (bdd_sat_count_double), and exact as well
 * (bdd_sat_count_u64) where it fits in 64 bits; for a circuit, it does not
 * fit whenever the circuit has more than 64 inputs. See counts_agree for how
 * the two counts of a root are compared. Only the evaluation is compared
 * across tools, as eval_hash; the counts are a check of each tool against
 * itself.
 *
 * PROBABILITY. --probs FILE (with --mode blif) reads a probability for every
 * input (probs.h; tools/mef2blif.py writes the file for a fault tree) and
 * computes the probability of the first output, the top event, before and
 * after the sift: "prob_before" and "prob_after", which must agree to the
 * rounding of adding the same terms in another order. cudd_bench --probs
 * computes the same, so that the tools can be compared as well.
 */

#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lace.h>
#include <sylvan/internal.h>

#include "blif.h"
#include "probs.h"

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
static int      opt_grouppairs = 0;    /* sift (2k, 2k+1) as one block       */
static int      opt_auto      = 0;     /* sift through sylvan_gc's auto sift */
static int      opt_stats     = 0;     /* counters of the build and the sift */
static int      opt_batched   = 1;     /* batched index surgery in swaps     */
static double   opt_recfactor = -1.0;  /* recommendation factor, <0 = default */
static int      opt_memstat   = 0;     /* resident memory from /proc        */
static uint64_t opt_memory    = 8ull << 30;
static unsigned opt_seed      = 12345;
static const char *opt_varorder = "file";
static unsigned opt_orderseed = 1;
static const char *opt_trace  = NULL;  /* one line per swap of the sift     */
static const char *opt_probs  = NULL;  /* input probabilities, --probs      */

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

/* One root's model count over all n_var variables. */
typedef struct {
    double   approx;        /* bdd_sat_count_double; +inf past DBL_MAX       */
    int      exact_ok;      /* <exact> was taken: the count fits in 64 bits */
    uint64_t exact;         /* bdd_sat_count_u64                             */
} model_count;

static model_count *sat_before;

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

/*
 * Every root's model count: the double, and the exact count where the double
 * says it fits. The exact count is not merely attempted: on a count past
 * 2^64 bdd_sat_count_u64 fails with SYLVAN_ERR_OVERFLOW, and a failed
 * subresult is not cached, so the attempt walks every path through the nodes
 * whose counts overflow -- exponentially many on a circuit of a few hundred
 * inputs. Under 2^63 the double is close enough to the count that the count
 * is under 2^64, and then no subresult overflows either: each is at most the
 * whole.
 */
static int
satcounts_CALL(lace_worker *lace, model_count *out)
{
    for (int r = 0; r < n_root; r++) {
        out[r].approx = bdd_sat_count_double_CALL(lace, root[r], varset);
        if (isnan(out[r].approx)) {
            fprintf(stderr, "verify: bdd_sat_count_double of output %d is "
                    "NaN\n", r);
            return -1;
        }
        out[r].exact_ok = 0;
        out[r].exact = 0;
        if (!(out[r].approx < 9223372036854775808.0)) continue;   /* 2^63 */
        const int status =
            bdd_sat_count_u64_CALL(lace, &out[r].exact, root[r], varset);
        if (status != SYLVAN_OK) {
            fprintf(stderr, "verify: bdd_sat_count_u64 of output %d failed "
                    "(status %d)\n", r, status);
            return -1;
        }
        out[r].exact_ok = 1;
    }
    return 0;
}

/*
 * Whether two counts of the same function agree. Exactly, where both have the
 * exact count. Otherwise the doubles must agree to a relative 1e-9: before
 * and after the sift they are summed in different orders and may round
 * differently, by at most about n_var ulps (1e-13 for the largest circuits),
 * while an infinite count (past 2^1024) only matches another. That includes
 * a count so close to 2^63 that rounding put one side under it and the other
 * not. cudd_bench compares Cudd_CountMinterm by the same rule.
 */
static int
doubles_agree(double a, double b)
{
    if (a == b) return 1;
    if (!isfinite(a) || !isfinite(b)) return 0;
    const double diff = a > b ? a - b : b - a;
    const double scale = a > 0 ? a : -a;
    return diff <= 1e-9 * scale;
}

static int
counts_agree(const model_count *a, const model_count *b)
{
    if (a->exact_ok && b->exact_ok) return a->exact == b->exact;
    return doubles_agree(a->approx, b->approx);
}

/* A count as a JSON number: exact where it fits, else the double printed as
   an integer, and null for the infinite one, which JSON cannot spell. */
static void
print_count(const model_count *c)
{
    if (c->exact_ok) printf("%llu", (unsigned long long)c->exact);
    else if (isfinite(c->approx)) printf("%.0f", c->approx);
    else printf("null");
}

/* The same for a checksum of the op phase, into <buf>. */
static const char *
json_double(char *buf, size_t size, double x)
{
    if (isfinite(x)) snprintf(buf, size, "%.6g", x);
    else snprintf(buf, size, "null");
    return buf;
}

/* An ordinary operation-heavy phase over the same forest: every pair of
   outputs, conjoined and exclusive-ored. Pure BDD work whose cost tracks the
   size of the diagrams it walks, which is the point. The checksum is CUDD's
   driver's: the double model counts, count(and) + 2 count(xor), summed. */
static int
op_phase_CALL(lace_worker *lace, double *seconds, double *checksum)
{
    BDD a = bdd_false, b = bdd_false;
    mtbdd_protect(&a);
    mtbdd_protect(&b);
    double sum = 0.0;
    const double t0 = now();
    int rc = -1;

    for (int i = 0; i < n_root; i++) {
        for (int j = i + 1; j < n_root; j++) {
            if (bdd_and_CALL(lace, &a, root[i], root[j]) != SYLVAN_OK) goto done;
            if (bdd_xor_CALL(lace, &b, root[i], root[j]) != SYLVAN_OK) goto done;
            sum += bdd_sat_count_double_CALL(lace, a, varset);
            sum += 2.0 * bdd_sat_count_double_CALL(lace, b, varset);
        }
    }
    *seconds = now() - t0;
    *checksum = sum;
    rc = 0;
done:
    if (rc != 0) fprintf(stderr, "ops: an operation failed\n");
    mtbdd_unprotect(&b);
    mtbdd_unprotect(&a);
    return rc;
}

/* ------------------------------------------------------------ probability */

static double    *probs_in;     /* --probs, per circuit variable            */
static double    *prob_of_id;   /* the same, by variable identity           */
static probs_memo pmemo;

/* See probs.h. mtbdd_node_low and _high pass a complement on to the child, so
   a complemented edge is a function of its own and nothing is subtracted. */
static double
prob_rec(BDD f)
{
    if (f == bdd_true) return 1.0;
    if (f == bdd_false) return 0.0;
    double r;
    if (probs_memo_get(&pmemo, (uint64_t)f, &r)) return r;
    const double p = prob_of_id[mtbdd_node_variable(f)];
    r = (1.0 - p) * prob_rec(mtbdd_node_low(f)) + p * prob_rec(mtbdd_node_high(f));
    probs_memo_put(&pmemo, (uint64_t)f, r);
    return r;
}

/* The probability of the first output, or -1 when the memo ran out of
   memory. */
static double
prob_top(void)
{
    if (n_root < 1 || probs_memo_init(&pmemo) != 0) return -1.0;
    double r = prob_rec(root[0]);
    if (pmemo.failed) r = -1.0;
    probs_memo_free(&pmemo);
    return r;
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

/*
 * --trace: one line per swap the sift makes, in the order it makes them,
 *
 *     POS UPPER LOWER SCORE NODES LEG
 *
 * POS is the rank of the upper of the two variables swapped, UPPER and LOWER
 * their identities before the swap, SCORE the size the driver compares after
 * it and NODES the internal nodes of the forest after it -- both the session's
 * forest size, so equal here; cudd_bench --trace writes the same columns, and
 * there they differ -- and LEG u or d for a search leg up or down, b for the
 * return to the best position, r for reuniting a pair. See README.md.
 */
static void
trace_swap(void *context, uint32_t rank, uint32_t upper, uint32_t lower,
           uint64_t size, sylvan_sift_leg leg)
{
    static const char legs[] = "udbr";
    fprintf((FILE*)context, "%u %u %u %llu %llu %c\n", (unsigned)rank,
            (unsigned)upper, (unsigned)lower, (unsigned long long)size,
            (unsigned long long)size, legs[(unsigned)leg & 3u]);
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
    memstat_t mem_build = {0, 0}, mem_sift = {0, 0};
    if (opt_memstat) memstat_take(&mem_build);

    const size_t nodes_before = mtbdd_shared_node_count(root, (size_t)n_root);
    double prob_before = -1.0, prob_after = -1.0;
    if (opt_probs != NULL) {
        prob_of_id = (double*)malloc((size_t)n_var * sizeof(double));
        if (prob_of_id == NULL) { fprintf(stderr, "--probs: allocation failed\n"); return 1; }
        for (int i = 0; i < n_var; i++) prob_of_id[vperm[i]] = probs_in[i];
        prob_before = prob_top();
    }
    if (varset_make() != 0) { fprintf(stderr, "varset failed\n"); return 1; }

    /* Soundness inputs, taken before anything moves. */
    uint8_t *eval_after = NULL;
    model_count *sat_after = NULL;
    if (opt_verify > 0) {
        vectors = (uint8_t*)malloc((size_t)opt_verify * n_var);
        eval_before = (uint8_t*)malloc((size_t)opt_verify * n_root);
        eval_after  = (uint8_t*)malloc((size_t)opt_verify * n_root);
        sat_before  = (model_count*)malloc((size_t)n_root * sizeof(model_count));
        sat_after   = (model_count*)malloc((size_t)n_root * sizeof(model_count));
        if (vectors == NULL || eval_before == NULL || eval_after == NULL ||
            sat_before == NULL || sat_after == NULL) {
            fprintf(stderr, "verify: allocation failed\n");
            return 1;
        }
        srand(opt_seed);
        for (int v = 0; v < opt_verify; v++) {
            for (int i = 0; i < n_var; i++) {
                vectors[(size_t)v * n_var + i] = (uint8_t)(rand() & 1);
            }
        }
        if (evaluate_CALL(lace, eval_before) != 0) {
            fprintf(stderr, "verify: evaluation before the sift failed\n");
            return 1;
        }
        if (satcounts_CALL(lace, sat_before) != 0) return 1;
    }

    double ops_before_s = -1.0, ops_after_s = -1.0;
    double ops_before_sum = 0.0, ops_after_sum = 0.0;
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
    FILE *trace = NULL;
    if (opt_trace != NULL && opt_sift) {
        trace = fopen(opt_trace, "w");
        if (trace == NULL) { perror(opt_trace); return 1; }
        sylvan_sift_set_trace(trace_swap, trace);
    }
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
        config.group_pairs = (uint8_t)opt_grouppairs;

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
    if (trace != NULL) {
        sylvan_sift_set_trace(NULL, NULL);
        if (fclose(trace) != 0) { perror(opt_trace); return 1; }
    }
    if (opt_stats) stats_phase(opt_sift ? "sift" : "no-sift");
    if (opt_memstat) memstat_take(&mem_sift);
    /* From the order itself rather than the result, so that --auto and
       --no-sift rows carry it too. */
    const uint32_t split_pairs = sylvan_sift_split_pairs();

    const size_t nodes_after = mtbdd_shared_node_count(root, (size_t)n_root);
    if (opt_probs != NULL) prob_after = prob_top();
    if (varset_make() != 0) { fprintf(stderr, "varset rebuild failed\n"); return 1; }

    int sound = -1;
    int valid = 1;
    uint64_t eval_hash = 0;
    if (opt_verify > 0) {
        if (evaluate_CALL(lace, eval_after) != 0) {
            fprintf(stderr, "verify: evaluation after the sift failed\n");
            return 1;
        }
        eval_hash = fnv(eval_after, (size_t)opt_verify * n_root);
        if (satcounts_CALL(lace, sat_after) != 0) return 1;
        sound = 1;
        for (size_t i = 0; i < (size_t)opt_verify * n_root; i++) {
            if (eval_before[i] == eval_after[i]) continue;
            fprintf(stderr, "verify: output %d differs on vector %d after "
                    "the sift\n", (int)(i % (size_t)n_root),
                    (int)(i / (size_t)n_root));
            sound = 0;
            break;
        }
        for (int r = 0; r < n_root; r++) {
            if (counts_agree(&sat_before[r], &sat_after[r])) continue;
            fprintf(stderr, "verify: output %d has %.17g models before the "
                    "sift and %.17g after\n", r,
                    sat_before[r].approx, sat_after[r].approx);
            sound = 0;
        }
        for (int r = 0; r < n_root; r++) {
            if (mtbdd_is_valid_CALL(lace, root[r])) continue;
            fprintf(stderr, "verify: output %d is not a valid diagram after "
                    "the sift\n", r);
            valid = 0;
        }
    }
    if (opt_ops) {
        if (op_phase_CALL(lace, &ops_after_s, &ops_after_sum) != 0) return 1;
    }

    char ops_before_buf[32], ops_after_buf[32];
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
           ",\"ops_before_sum\":%s,\"ops_after_sum\":%s"
           ",\"backend\":\"%s\",\"full_legs\":%d,\"time_limit\":%.3f"
           ",\"auto\":%d,\"sifts\":%llu,\"batched\":%d"
           ",\"group_pairs\":%d,\"split_pairs\":%u",
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
           json_double(ops_before_buf, sizeof(ops_before_buf), ops_before_sum),
           json_double(ops_after_buf, sizeof(ops_after_buf), ops_after_sum),
           SIFT_BENCH_BACKEND, opt_fulllegs, opt_timelimit,
           opt_auto, (unsigned long long)sifts, opt_batched,
           opt_grouppairs, (unsigned)split_pairs);
    if (opt_verify > 0) {
        printf(", \"satsum\":[");
        for (int r = 0; r < n_root && r < 8; r++) {
            printf("%s", r ? "," : "");
            print_count(&sat_after[r]);
        }
        printf("]");
    }
    if (opt_order_out) print_order();
    if (opt_memstat) memstat_print(&mem_build, &mem_sift);
    if (opt_probs != NULL) probs_print(prob_before, prob_after);
    printf("}\n");
    fflush(stdout);

    free(vectors); free(eval_before); free(eval_after);
    free(sat_before); free(sat_after); free(prob_of_id);
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
        "          --group-pairs (sift each pair (2k, 2k+1) as one block)\n"
        "          --no-sift  --no-batch (per-node index surgery in swaps)\n"
        "          --recommend-factor F (growth the --auto recommendation\n"
        "                    asks for; library default 2.0, 0 disables)\n"
        "  checks: --verify VECTORS  --seed S  --ops  --order\n"
        "          --trace FILE  (one line per swap of the sift)\n"
        "          --probs FILE  (blif: the first output's probability before\n"
        "                    and after, from one per input; see probs.h)\n"
        "          --memstat  (VmRSS, VmHWM and AnonHugePages after the build\n"
        "                      and after the sift, from /proc; Linux)\n"
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
        else if (!strcmp(a, "--group-pairs"))    { opt_grouppairs = 1; }
        else if (!strcmp(a, "--auto"))           { opt_auto = 1; }
        else if (!strcmp(a, "--stats"))          { opt_stats = 1; }
        else if (!strcmp(a, "--no-batch"))       { opt_batched = 0; }
        else if (!strcmp(a, "--memstat"))        { opt_memstat = 1; }
        else if (!strcmp(a, "--recommend-factor") && v) { opt_recfactor = atof(v); i++; }
        else if (!strcmp(a, "--memory") && v)    { opt_memory = strtoull(v, NULL, 10); i++; }
        else if (!strcmp(a, "--seed") && v)      { opt_seed = (unsigned)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--var-order") && v) { opt_varorder = v; i++; }
        else if (!strcmp(a, "--order-seed") && v){ opt_orderseed = (unsigned)strtoul(v, NULL, 10); i++; }
        else if (!strcmp(a, "--trace") && v)     { opt_trace = v; i++; }
        else if (!strcmp(a, "--probs") && v)     { opt_probs = v; i++; }
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
        if (opt_probs != NULL) {
            probs_in = (double*)malloc((size_t)(circuit.n_var > 0 ? circuit.n_var : 1) * sizeof(double));
            if (probs_in == NULL) { fprintf(stderr, "--probs: out of memory\n"); return 1; }
            if (probs_read(&circuit, opt_probs, probs_in, err, sizeof(err)) != 0) {
                fprintf(stderr, "--probs: %s\n", err);
                return 1;
            }
        }
    } else if (opt_probs != NULL) {
        fprintf(stderr, "--probs needs --mode blif\n");
        return 2;
    }

    lace_start((unsigned)opt_workers, 0, 128u * 1024u * 1024u);
    sylvan_set_limits(opt_memory, 1, 4);
    if (sylvan_init_package() != SYLVAN_OK || mtbdd_init() != SYLVAN_OK) {
        fprintf(stderr, "sylvan init failed\n");
        return 1;
    }
    sylvan_reorder_set_batched(opt_batched);
    if (opt_recfactor >= 0.0) sylvan_sift_set_recommend_factor(opt_recfactor);

    const int rc = run();

    sylvan_quit();
    lace_stop();
    if (strcmp(opt_mode, "blif") == 0) blif_free(&circuit);
    free(probs_in);
    return rc;
}
