/**
 * Tests for sylvan_reach and lddmc_reach: compare with a fixpoint of relnext
 * (BDD) or relprod (LDD) on random relations. The tables are small, so that
 * garbage collection also runs during the operations.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "sylvan.h"
#include "test_assert.h"

#include <lace.h>

static uint64_t seed = 1;

/* statistics, to see that the tests are not trivial */
static int bdd_grew = 0, ldd_grew = 0, ldd_merged_grew = 0;
static _Atomic(int) gc_count = 0;
/* Deltas around calls exclude construction and the relnext/relprod oracle. */
static int bdd_reach_gc = 0, ldd_reach_gc = 0, ldd_extend_gc = 0;

VOID_TASK_0(count_gc)
{
    gc_count++;
}

static uint64_t
xorshift_rand(void)
{
    uint64_t x = seed;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    seed = x;
    return x * 2685821657736338717LL;
}

/* random number in [low, high) */
static int
rng(int low, int high)
{
    return low + (int)(xorshift_rand() % (uint64_t)(high - low));
}

/**
 * BDD: the fixpoint of S = S \or relnext(S, R, vars)
 */
static BDD
bdd_bfs(BDD s, BDD r, BDD vars)
{
    BDD prev = sylvan_false, next = sylvan_false;
    sylvan_protect(&s);
    sylvan_protect(&prev);
    sylvan_protect(&next);
    while (s != prev) {
        prev = s;
        next = sylvan_relnext(s, r, vars);
        s = sylvan_or(s, next);
    }
    sylvan_unprotect(&s);
    sylvan_unprotect(&prev);
    sylvan_unprotect(&next);
    return s;
}

/**
 * Random relation on n state variables (s=2i, t=2i+1) and two action label
 * variables (2n, 2n+1), with vars a random subset of the state variables
 * (sometimes only s or only t of a pair), or all variables. For a state
 * variable that is not in vars, the relation sometimes depends on s or t
 * anyway; relnext (and reach) quantify those.
 */
static int
test_bdd_reach_once(void)
{
    int n = rng(2, 9);
    int all = rng(0, 4) == 0;

    BDD vars = sylvan_set_empty(), r = sylvan_false, s = sylvan_false, cube = sylvan_false;
    BDD reach = sylvan_false, bfs = sylvan_false;
    sylvan_protect(&vars);
    sylvan_protect(&r);
    sylvan_protect(&s);
    sylvan_protect(&cube);
    sylvan_protect(&reach);
    sylvan_protect(&bfs);

    /* the variables of the relation */
    int in_rel[16];
    for (int i=n-1; i>=0; i--) {
        in_rel[i] = all || rng(0, 4) != 0;
        if (in_rel[i]) {
            int which = rng(0, 4); // 0: only s, 1: only t, else both
            if (which != 0) vars = sylvan_set_add(vars, 2*i+1);
            if (which != 1) vars = sylvan_set_add(vars, 2*i);
        }
    }

    /* the relation: a union of cubes over the relation and label variables */
    int labels = all ? 0 : 2;
    uint32_t cube_vars[40];
    uint8_t cube_vals[40];
    int k = 0;
    for (int i=0; i<n; i++) {
        if (in_rel[i]) {
            cube_vars[k++] = 2*i;
            cube_vars[k++] = 2*i+1;
        } else {
            int which = rng(0, 3); // 0: s, 1: t, 2: neither (quantified by relnext)
            if (which < 2) cube_vars[k++] = 2*i + (uint32_t)which;
        }
    }
    for (int i=0; i<labels; i++) cube_vars[k++] = 2*n+i;
    BDD cube_set = sylvan_set_fromarray(cube_vars, k);
    sylvan_protect(&cube_set);

    int count = rng(1, 3*n);
    for (int c=0; c<count; c++) {
        for (int j=0; j<k; j++) cube_vals[j] = (uint8_t)rng(0, 3); // 0, 1 or 2 (any)
        cube = sylvan_cube(cube_set, cube_vals);
        r = sylvan_or(r, cube);
    }

    /* the initial states: a union of cubes over all state variables */
    uint32_t state_vars[16];
    for (int i=0; i<n; i++) state_vars[i] = 2*i;
    BDD state_set = sylvan_set_fromarray(state_vars, n);
    sylvan_protect(&state_set);
    int scount = rng(1, 4);
    for (int c=0; c<scount; c++) {
        for (int j=0; j<n; j++) cube_vals[j] = (uint8_t)(rng(0, 4) == 0 ? 2 : rng(0, 2));
        cube = sylvan_cube(state_set, cube_vals);
        s = sylvan_or(s, cube);
    }

    BDD v = all ? sylvan_false : vars;
    int before = gc_count;
    reach = sylvan_reach(s, r, v);
    bdd_reach_gc += gc_count - before;
    bfs = bdd_bfs(s, r, v);

    int ok = reach == bfs;
    if (reach != s) bdd_grew++;
    if (!ok) {
        fprintf(stderr, "sylvan_reach differs from bfs (n=%d, all=%d)\n", n, all);
        fprintf(stderr, "  states: initial %.0f, reach %.0f, bfs %.0f\n", sylvan_satcount(s, state_set),
                sylvan_satcount(reach, state_set), sylvan_satcount(bfs, state_set));
        fprintf(stderr, "  in_rel:");
        for (int i=0; i<n; i++) fprintf(stderr, " %d", in_rel[i]);
        fprintf(stderr, "\n");
    }

    sylvan_unprotect(&vars);
    sylvan_unprotect(&r);
    sylvan_unprotect(&s);
    sylvan_unprotect(&cube);
    sylvan_unprotect(&reach);
    sylvan_unprotect(&bfs);
    sylvan_unprotect(&cube_set);
    sylvan_unprotect(&state_set);

    test_assert(ok);
    return 0;
}

/**
 * LDD: the fixpoint of S = S \union relprod(S, R_1, meta_1) ... relprod(S, R_k, meta_k)
 */
static MDD
ldd_bfs(MDD s, MDD *r, MDD *meta, int k)
{
    MDD prev = lddmc_false, next = lddmc_false;
    lddmc_protect(&s);
    lddmc_protect(&prev);
    lddmc_protect(&next);
    while (s != prev) {
        prev = s;
        for (int i=0; i<k; i++) {
            next = lddmc_relprod(prev, r[i], meta[i]);
            s = lddmc_union(s, next);
        }
    }
    lddmc_unprotect(&s);
    lddmc_unprotect(&prev);
    lddmc_unprotect(&next);
    return s;
}

/**
 * Random LDD relation on n variables with values in [0, d): a random meta
 * (0, 1+2, 3, 4 per variable, an optional -1 at the end, and optional
 * action labels at any position), and a union of random cubes, with copy nodes.
 */
static void
ldd_random_rel(int n, int d, MDD *rel, MDD *meta)
{
    uint32_t meta_vals[40];
    int m = 0;
    int last = n;
    if (rng(0, 3) == 0) last = rng(0, n+1); // -1 after variable last
    for (int i=0; i<last; i++) {
        if (rng(0, 3) == 0) meta_vals[m++] = 5; // action label before this variable
        int type = rng(0, 4);
        if (type == 0) meta_vals[m++] = 0;
        else if (type == 1) {
            meta_vals[m++] = 1;
            int labels = rng(0, 3); // including consecutive labels between read and write
            for (int j=0; j<labels; j++) meta_vals[m++] = 5;
            meta_vals[m++] = 2;
        }
        else meta_vals[m++] = (uint32_t)(type+1);
        if (rng(0, 3) == 0) meta_vals[m++] = 5; // also next to only-read/write and copies
    }
    int label = last == n && rng(0, 3) == 0;
    if (label) meta_vals[m++] = 5;
    if (last < n || rng(0, 2) == 0) meta_vals[m++] = (uint32_t)-1;
    *meta = lddmc_cube(meta_vals, (size_t)m);

    uint32_t vals[40];
    int copy[40];
    int count = rng(1, 2*n+2);
    for (int c=0; c<count; c++) {
        int k = 0;
        for (int j=0; j<m; j++) {
            uint32_t mv = meta_vals[j];
            if (mv == 0 || mv == (uint32_t)-1) continue;
            vals[k] = (uint32_t)rng(0, d);
            copy[k] = mv != 5 && rng(0, 4) == 0;
            if (copy[k]) vals[k] = 0;
            k++;
        }
        *rel = lddmc_union_cube_copy(*rel, vals, copy, (size_t)k);
    }
}

static MDD
ldd_random_set(int n, int d, int max_count)
{
    MDD s = lddmc_false;
    lddmc_protect(&s);
    uint32_t vals[16];
    int count = rng(1, max_count+1);
    for (int c=0; c<count; c++) {
        for (int j=0; j<n; j++) vals[j] = (uint32_t)rng(0, d+1); // value d is never in a relation
        s = lddmc_union_cube(s, vals, (size_t)n);
    }
    lddmc_unprotect(&s);
    return s;
}

static int
test_ldd_reach_once(void)
{
    int n = rng(1, 7);
    int d = rng(2, 5);
    int k = rng(1, 4);

    MDD r[4], meta[4], ext[4];
    MDD s = lddmc_false, merged = lddmc_false, full = lddmc_false;
    MDD reach = lddmc_false, bfs = lddmc_false, a = lddmc_false, b = lddmc_false, t = lddmc_false;
    for (int i=0; i<k; i++) {
        r[i] = meta[i] = ext[i] = lddmc_false;
        lddmc_protect(&r[i]);
        lddmc_protect(&meta[i]);
        lddmc_protect(&ext[i]);
    }
    lddmc_protect(&s);
    lddmc_protect(&merged);
    lddmc_protect(&full);
    lddmc_protect(&reach);
    lddmc_protect(&bfs);
    lddmc_protect(&a);
    lddmc_protect(&b);
    lddmc_protect(&t);

    for (int i=0; i<k; i++) ldd_random_rel(n, d, &r[i], &meta[i]);
    s = ldd_random_set(n, d, 3);

    uint32_t full_vals[32];
    for (int i=0; i<n; i++) {
        full_vals[2*i] = 1;
        full_vals[2*i+1] = 2;
    }
    full = lddmc_cube(full_vals, (size_t)(2*n));

    int ok = 1;

    /* each relation on its own */
    for (int i=0; i<k && ok; i++) {
        int before = gc_count;
        reach = lddmc_reach(s, r[i], meta[i]);
        ldd_reach_gc += gc_count - before;
        bfs = ldd_bfs(s, &r[i], &meta[i], 1);
        if (reach != s) ldd_grew++;
        if (reach != bfs) {
            fprintf(stderr, "lddmc_reach differs from bfs (n=%d, d=%d)\n", n, d);
            ok = 0;
        }
    }

    /* the extended relations have the same successors */
    for (int i=0; i<k && ok; i++) {
        int before = gc_count;
        ext[i] = lddmc_extend_rel(r[i], meta[i], (uint32_t)n);
        ldd_extend_gc += gc_count - before;
        t = ldd_random_set(n, d, 32);
        a = lddmc_relprod(t, r[i], meta[i]);
        b = lddmc_relprod(t, ext[i], full);
        if (a != b) {
            fprintf(stderr, "lddmc_extend_rel changes the successors (n=%d, d=%d)\n", n, d);
            ok = 0;
        }
    }

    /* reach with the union of the extended relations */
    if (ok) {
        for (int i=0; i<k; i++) merged = lddmc_union(merged, ext[i]);
        int before = gc_count;
        reach = lddmc_reach(s, merged, full);
        ldd_reach_gc += gc_count - before;
        bfs = ldd_bfs(s, r, meta, k);
        if (reach != s) ldd_merged_grew++;
        if (reach != bfs) {
            fprintf(stderr, "lddmc_reach with merged relations differs from bfs (n=%d, d=%d, k=%d)\n", n, d, k);
            ok = 0;
        }
    }

    for (int i=0; i<k; i++) {
        lddmc_unprotect(&r[i]);
        lddmc_unprotect(&meta[i]);
        lddmc_unprotect(&ext[i]);
    }
    lddmc_unprotect(&s);
    lddmc_unprotect(&merged);
    lddmc_unprotect(&full);
    lddmc_unprotect(&reach);
    lddmc_unprotect(&bfs);
    lddmc_unprotect(&a);
    lddmc_unprotect(&b);
    lddmc_unprotect(&t);

    test_assert(ok);
    return 0;
}

/* The reported one-variable failure, followed by exhaustive copy choices for
 * a relation with labels beside read/write, only-read and only-write levels. */
static int
test_ldd_labels(void)
{
    MDD s = lddmc_false, r = lddmc_false, meta = lddmc_false;
    MDD full = lddmc_false, ext = lddmc_false, reach = lddmc_false, expected = lddmc_false;
    MDD a = lddmc_false, b = lddmc_false;
    lddmc_protect(&s);
    lddmc_protect(&r);
    lddmc_protect(&meta);
    lddmc_protect(&full);
    lddmc_protect(&ext);
    lddmc_protect(&reach);
    lddmc_protect(&expected);
    lddmc_protect(&a);
    lddmc_protect(&b);
    uint32_t m[] = {1, 5, 2, (uint32_t)-1}, f[] = {1, 2, (uint32_t)-1};
    uint32_t transition[] = {0, 0, 1}, zero[] = {0}, one[] = {1};
    meta = lddmc_cube(m, 4);
    full = lddmc_cube(f, 3);
    s = lddmc_cube(zero, 1);
    r = lddmc_cube(transition, 3);
    expected = lddmc_union_cube(s, one, 1);
    reach = lddmc_reach(s, r, meta);
    test_assert(reach == expected);
    ext = lddmc_extend_rel(r, meta, 1);
    reach = lddmc_reach(s, ext, full);
    test_assert(reach == expected);
    a = lddmc_relprod(s, r, meta);
    b = lddmc_relprod(s, ext, full);
    test_assert(a == b);

    uint32_t mixed[] = {5, 1, 5, 5, 2, 5, 3, 5, 4, 5, (uint32_t)-1};
    uint32_t all[] = {1, 2, 1, 2, 1, 2, (uint32_t)-1};
    meta = lddmc_cube(mixed, 11);
    full = lddmc_cube(all, 7);
    for (unsigned mask=0; mask<16; mask++) {
        r = s = lddmc_false;
        for (uint32_t c=0; c<3; c++) {
            uint32_t vals[] = {7+c, c, 9-c, c, (c+1)%3, 4+c, c, c, (c+2)%3, 8};
            int copy[] = {0, mask&1, 0, 0, mask&2, 0, mask&4, 0, mask&8, 0};
            r = lddmc_union_cube_copy(r, vals, copy, 10);
            uint32_t state[] = {c, c, c};
            s = lddmc_union_cube(s, state, 3);
        }
        reach = lddmc_reach(s, r, meta);
        expected = ldd_bfs(s, &r, &meta, 1);
        test_assert(reach == expected);
        ext = lddmc_extend_rel(r, meta, 3);
        a = lddmc_relprod(s, r, meta);
        b = lddmc_relprod(s, ext, full);
        test_assert(a == b);
        reach = lddmc_reach(s, ext, full);
        test_assert(reach == expected);
    }
    lddmc_unprotect(&s);
    lddmc_unprotect(&r);
    lddmc_unprotect(&meta);
    lddmc_unprotect(&full);
    lddmc_unprotect(&ext);
    lddmc_unprotect(&reach);
    lddmc_unprotect(&expected);
    lddmc_unprotect(&a);
    lddmc_unprotect(&b);
    return 0;
}

TASK_0(int, runtests)
{
    printf("Testing sylvan_reach.\n");
    for (int j=0; j<2000; j++) if (test_bdd_reach_once()) { fprintf(stderr, "BDD test %d failed\n", j); return 1; }
    printf("Testing lddmc_reach and lddmc_extend_rel.\n");
    test_assert(test_ldd_labels() == 0);
    for (int j=0; j<2000; j++) if (test_ldd_reach_once()) return 1;
    printf("New states found: BDD %d of 2000, LDD %d (single relations), %d of 2000 (merged); %d garbage collections.\n",
           bdd_grew, ldd_grew, ldd_merged_grew, (int)gc_count);
    printf("Collections inside operations: sylvan_reach %d, lddmc_reach %d, lddmc_extend_rel %d.\n",
           bdd_reach_gc, ldd_reach_gc, ldd_extend_gc);
    test_assert(bdd_reach_gc > 0);
    test_assert(ldd_reach_gc > 0);
    test_assert(ldd_extend_gc > 0);
    return 0;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 2;
    lace_start(workers, 0);

    // small tables, so that garbage collection runs often
    sylvan_set_sizes(1LL<<12, 1LL<<12, 1LL<<10, 1LL<<10);
    sylvan_init_package();
    sylvan_init_bdd();
    sylvan_init_ldd();
    sylvan_gc_hook_pregc(count_gc_CALL);

    int res = RUN(runtests);
    if (res == 0) printf("All reach tests passed.\n");

    sylvan_quit();
    lace_stop();

    return res;
}
