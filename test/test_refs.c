/**
 * Tests for the tables of protected variables (mtbdd_protect and friends),
 * used by many workers at once.
 *
 * 1. Many workers add and remove values at the same time, on a table that
 *    starts small, so that it resizes again and again while the other workers
 *    use it. Afterwards the table holds exactly the values that should be
 *    there. Once with a counter per worker, and once with only some workers
 *    having a counter, so that the others use the shared one at the same time.
 * 2. Workers protect BDDs at the same time; after garbage collection every
 *    protected BDD is still there.
 */

#include <stdio.h>
#include <stdlib.h>

#include <lace.h>
#include <sylvan/sylvan.h>
#include <sylvan/internal.h>

#include "refs.h"
#include "test_assert.h"

#define N 50000      // values per phase
#define ROUNDS 20

static refs_table_t tbl;

/* Add (up) or remove the values first, ..., first+count-1, in parallel */
TASK(void, protect_range, uint64_t, first, uint64_t, count, int, up)
void
protect_range_CALL(lace_worker *lace, uint64_t first, uint64_t count, int up)
{
    if (count > 128) {
        protect_range_SPAWN(lace, first, count/2, up);
        protect_range_CALL(lace, first+count/2, count-count/2, up);
        protect_range_SYNC(lace);
        return;
    }
    for (uint64_t i=0; i<count; i++) {
        if (up) protect_up(&tbl, first+i);
        else protect_down(&tbl, first+i);
    }
}

/* Add the values in [add, add+N) and remove those in [remove, remove+N), at the same time */
TASK(void, protect_both, uint64_t, add, uint64_t, remove)
void
protect_both_CALL(lace_worker *lace, uint64_t add, uint64_t remove)
{
    protect_range_SPAWN(lace, add, N, 1);
    protect_range_CALL(lace, remove, N, 0);
    protect_range_SYNC(lace);
}

/* Check that the table holds each value in [first, first+N) once, and nothing else */
static int
check_contents(uint64_t first)
{
    static char seen[N];
    for (int i=0; i<N; i++) seen[i] = 0;
    size_t count = 0;
    uint64_t *it = protect_iter(&tbl, 0, tbl.refs_size);
    while (it != NULL) {
        uint64_t v = protect_next(&tbl, &it, tbl.refs_size);
        test_assert(v >= first && v < first+N);
        test_assert(seen[v-first] == 0);
        seen[v-first] = 1;
        count++;
    }
    test_assert(count == N);
    test_assert(protect_count(&tbl) == N);
    return 0;
}

/* One round on a fresh table that starts small; workers with an id below nworkers have a counter */
static int
test_table_round(size_t nworkers)
{
    protect_create(&tbl, 128);
    size_t allocated = tbl.refs_nworkers;
    if (nworkers < allocated) tbl.refs_nworkers = nworkers;

    // add [1, N], while the table grows
    protect_range(1, N, 1);
    if (check_contents(1)) return 1;
    // add [N+1, 2N] and remove [1, N] at the same time
    protect_both(N+1, 1);
    if (check_contents(N+1)) return 1;
    protect_range(N+1, N, 0);
    test_assert(protect_count(&tbl) == 0);

    tbl.refs_nworkers = allocated;
    protect_free(&tbl);
    return 0;
}

static int
test_tables(void)
{
    for (int r=0; r<ROUNDS; r++) {
        // every worker has a counter
        if (test_table_round((size_t)-1)) return 1;
        // only worker 0 has a counter, the others use the shared one
        if (test_table_round(1)) return 1;
    }
    return 0;
}

#define SLOTS 4096
#define NVARS 12

static BDD slots[SLOTS];
static BDDSET vars;

/* The minterm of the bits of i over the variables 0 .. NVARS-1 */
static int
minterm(lace_worker *lace, BDD *result, int i)
{
    uint8_t cube[NVARS];
    for (int v=0; v<NVARS; v++) cube[v] = (uint8_t)((i >> v) & 1);
    return bdd_cube_CALL(lace, result, vars, cube);
}

/* Protect slots first, ..., first+count-1, in parallel */
TASK(void, protect_slots, int, first, int, count)
void
protect_slots_CALL(lace_worker *lace, int first, int count)
{
    if (count > 16) {
        protect_slots_SPAWN(lace, first, count/2);
        protect_slots_CALL(lace, first+count/2, count-count/2);
        protect_slots_SYNC(lace);
        return;
    }
    for (int i=first; i<first+count; i++) mtbdd_protect(&slots[i]);
}

/* Sylvan's operations keep their temporary results on the worker's stack, so this runs in a task */
TASK(int, test_gc_survival)
int
test_gc_survival_CALL(lace_worker *lace)
{
    uint32_t v[NVARS];
    for (int i=0; i<NVARS; i++) v[i] = (uint32_t)i;
    vars = bdd_true;
    mtbdd_protect(&vars);
    test_assert(bdd_set_from_array(&vars, v, NVARS) == SYLVAN_OK);

    // the nodes table is large enough that no garbage collection runs while building
    for (int i=0; i<SLOTS; i++) test_assert(minterm(lace, &slots[i], i) == SYLVAN_OK);
    size_t before = mtbdd_protected_count();
    protect_slots_CALL(lace, 0, SLOTS);
    test_assert(mtbdd_protected_count() == before + SLOTS);
    for (int r=0; r<3; r++) {
        sylvan_gc_CALL(lace);
        for (int i=0; i<SLOTS; i++) {
            test_assert(nodes_is_marked(nodes, MTBDD_STRIPMARK(slots[i])));
            BDD again;
            test_assert(minterm(lace, &again, i) == SYLVAN_OK);
            test_assert(again == slots[i]);
        }
    }
    for (int i=0; i<SLOTS; i++) mtbdd_unprotect(&slots[i]);
    test_assert(mtbdd_protected_count() == before);
    mtbdd_unprotect(&vars);
    return 0;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 4;
    lace_start(workers, 0, 0);

    sylvan_set_sizes(1LL<<18, 1LL<<20, 1LL<<14, 1LL<<18);
    if (sylvan_init_package() != SYLVAN_OK || mtbdd_init() != SYLVAN_OK) {
        fprintf(stderr, "Sylvan initialization failed.\n");
        return 1;
    }

    if (test_tables()) return 1;
    printf("Protection tables used by %d workers at once: ok.\n", workers);

    if (test_gc_survival()) return 1;
    printf("BDDs protected by %d workers at once survive garbage collection: ok.\n", workers);

    sylvan_quit();
    lace_stop();

    return 0;
}
