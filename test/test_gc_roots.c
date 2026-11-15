/**
 * Garbage collection with more protected roots than the task deque of a
 * worker holds.
 *
 * Garbage collection marks the roots in the tables of mtbdd_protect,
 * listdd_protect and zdd_protect. The ListDD and ZDD tables used to be
 * marked with one task per root, all on the deque of the worker that
 * collected, so that with more roots than the deque holds, Lace aborted (task
 * deque overflow). This test starts Lace with a small deque, makes many
 * distinct roots of every kind (so that each one has to be marked itself),
 * collects garbage, and checks that every root is still in the nodes table.
 */

#include <stdio.h>
#include <stdlib.h>

#include <lace.h>
#include <sylvan/sylvan.h>
#include <sylvan/internal.h>

#include "test_assert.h"

#define DEQUE 8192   // task deque entries per worker
#define N 40000      // roots of each kind, more than DEQUE
#define NVARS 17     // 2^17 > N, so that all minterms are distinct

static BDD mtbdd_roots[N];
static LISTDD listdd_roots[N];
static ZDD zdd_roots[N];
static BDDSET vars;

/* Make and protect all roots, then collect garbage and check that they are all still there */
TASK(int, test_many_roots)
int
test_many_roots_CALL(lace_worker *lace)
{
    uint32_t v[NVARS];
    for (int i=0; i<NVARS; i++) v[i] = (uint32_t)i;
    vars = bdd_true;
    mtbdd_protect(&vars);
    test_assert(bdd_set_from_array(&vars, v, NVARS) == SYLVAN_OK);

    // the nodes table is large enough that no garbage collection runs while building
    for (int i=0; i<N; i++) {
        uint8_t cube[NVARS];
        for (int b=0; b<NVARS; b++) cube[b] = (uint8_t)((i >> b) & 1);
        mtbdd_roots[i] = bdd_false;
        mtbdd_protect(&mtbdd_roots[i]);
        test_assert(bdd_cube_CALL(lace, &mtbdd_roots[i], vars, cube) == SYLVAN_OK);
        zdd_roots[i] = zdd_false;
        zdd_protect(&zdd_roots[i]);
        test_assert(zdd_cube(&zdd_roots[i], vars, cube) == SYLVAN_OK);
        uint32_t values[3] = { (uint32_t)(i % 64), (uint32_t)((i / 64) % 64), (uint32_t)(i / 4096) };
        listdd_roots[i] = listdd_empty;
        listdd_protect(&listdd_roots[i]);
        test_assert(listdd_add(&listdd_roots[i], listdd_empty, values, 3) == SYLVAN_OK);
    }

    for (int r=0; r<2; r++) {
        sylvan_gc_CALL(lace);
        for (int i=0; i<N; i++) {
            test_assert(nodes_is_marked(nodes, MTBDD_STRIPMARK(mtbdd_roots[i])));
            test_assert(nodes_is_marked(nodes, ZDD_GETINDEX(zdd_roots[i])));
            test_assert(nodes_is_marked(nodes, listdd_roots[i]));
        }
    }

    for (int i=0; i<N; i++) {
        mtbdd_unprotect(&mtbdd_roots[i]);
        zdd_unprotect(&zdd_roots[i]);
        listdd_unprotect(&listdd_roots[i]);
    }
    mtbdd_unprotect(&vars);
    return 0;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 2;
    lace_start(workers, DEQUE, 0);

    sylvan_set_sizes(1LL<<20, 1LL<<22, 1LL<<16, 1LL<<18);
    if (sylvan_init_package() != SYLVAN_OK || mtbdd_init() != SYLVAN_OK ||
        listdd_init() != SYLVAN_OK || zdd_init() != SYLVAN_OK) {
        fprintf(stderr, "Sylvan initialization failed.\n");
        return 1;
    }

    if (test_many_roots()) return 1;
    printf("Garbage collection with %d roots of each kind and a deque of %d tasks: ok.\n", N, DEQUE);

    sylvan_quit();
    lace_stop();

    return 0;
}
