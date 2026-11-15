/**
 * Garbage collection with more protected and referenced roots than the task
 * deque of a worker holds.
 *
 * Garbage collection marks the roots in the tables of mtbdd_protect,
 * mtbdd_ref, lddmc_protect, lddmc_ref and zdd_protect. It used to spawn one
 * task per root, all on the deque of the worker that collected, so that with
 * more roots than the deque holds, Lace aborted (task deque overflow). This
 * test starts Lace with a small deque, makes many distinct roots of every kind
 * (so that each one has to be marked itself), collects garbage, and checks
 * that every root is still in the nodes table.
 */

#include <stdio.h>
#include <stdlib.h>

#include "sylvan.h"
#include "sylvan_int.h"
#include "test_assert.h"

#include <lace.h>

#define DEQUE 8192   // task deque entries per worker
#define N 40000      // roots of each kind, more than DEQUE
#define NVARS 17     // 2^17 > 2N, so that all minterms are distinct

static BDD mtbdd_roots[N];   // protected
static BDD mtbdd_values[N];  // referenced with mtbdd_ref
static MDD ldd_roots[N];     // protected
static MDD ldd_values[N];    // referenced with lddmc_ref
static ZDD zdd_roots[N];     // protected

/* The minterm of the bits of i over the variables 0 .. NVARS-1 */
static BDD
bdd_minterm(int i)
{
    BDD res = sylvan_true;
    for (int v=NVARS-1; v>=0; v--) {
        res = sylvan_makenode((uint32_t)v, (i >> v) & 1 ? sylvan_false : res, (i >> v) & 1 ? res : sylvan_false);
    }
    return res;
}

/* An LDD with the single vector (i mod 64, i / 64 mod 64, i / 4096) */
static MDD
ldd_vector(int i)
{
    uint32_t values[3] = { (uint32_t)(i % 64), (uint32_t)((i / 64) % 64), (uint32_t)(i / 4096) };
    return lddmc_cube(values, 3);
}

static ZDD zdd_vars;

/* The ZDD of the single set whose elements are the bits of i over the variables 0 .. NVARS-1 */
static ZDD
zdd_minterm(int i)
{
    uint8_t values[NVARS];
    for (int v=0; v<NVARS; v++) values[v] = (uint8_t)((i >> v) & 1);
    return zdd_cube(zdd_vars, values, zdd_true);
}

/* Make and protect (or reference) all roots; nodes are made by Lace workers */
VOID_TASK_0(make_roots)
{
    uint32_t vars[NVARS];
    for (int v=0; v<NVARS; v++) vars[v] = (uint32_t)v;
    zdd_vars = zdd_false;
    zdd_protect(&zdd_vars);
    zdd_vars = zdd_set_from_array(vars, NVARS);

    // the nodes table is large enough that no garbage collection runs while building
    for (int i=0; i<N; i++) {
        mtbdd_roots[i] = sylvan_false;
        mtbdd_protect(&mtbdd_roots[i]);
        mtbdd_roots[i] = bdd_minterm(i);
        mtbdd_values[i] = mtbdd_ref(bdd_minterm(N+i));
        ldd_roots[i] = lddmc_false;
        lddmc_protect(&ldd_roots[i]);
        ldd_roots[i] = ldd_vector(i);
        ldd_values[i] = lddmc_ref(ldd_vector(N+i));
        zdd_roots[i] = zdd_false;
        zdd_protect(&zdd_roots[i]);
        zdd_roots[i] = zdd_minterm(i);
    }
}

static int
test_many_roots(void)
{
    RUN(make_roots);
    for (int r=0; r<2; r++) {
        sylvan_gc();
        for (int i=0; i<N; i++) {
            test_assert(mtbdd_test_isvalid(mtbdd_roots[i]));
            test_assert(mtbdd_test_isvalid(mtbdd_values[i]));
            test_assert(llmsset_is_marked(nodes, ldd_roots[i]));
            test_assert(llmsset_is_marked(nodes, ldd_values[i]));
            test_assert(llmsset_is_marked(nodes, ZDD_GETINDEX(zdd_roots[i])));
        }
    }

    for (int i=0; i<N; i++) {
        mtbdd_unprotect(&mtbdd_roots[i]);
        mtbdd_deref(mtbdd_values[i]);
        lddmc_unprotect(&ldd_roots[i]);
        lddmc_deref(ldd_values[i]);
        zdd_unprotect(&zdd_roots[i]);
    }
    zdd_unprotect(&zdd_vars);
    return 0;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 2;
    lace_start(workers, DEQUE);

    sylvan_set_sizes(1LL<<20, 1LL<<22, 1LL<<16, 1LL<<18);
    sylvan_init_package();
    sylvan_init_bdd();
    sylvan_init_ldd();
    sylvan_init_zdd();

    if (test_many_roots()) return 1;
    printf("Garbage collection with %d roots of each kind and a deque of %d tasks: ok.\n", N, DEQUE);

    sylvan_quit();
    lace_stop();

    return 0;
}
