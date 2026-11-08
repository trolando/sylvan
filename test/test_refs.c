/**
 * Tests for the tables of protected variables (mtbdd_protect and friends) and
 * of external references (mtbdd_ref and friends), used by many workers at once.
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

#include "sylvan.h"
#include "sylvan_int.h"
#include "sylvan_refs.h"
#include "test_assert.h"

#include <lace.h>

#define N 50000      // values per phase
#define ROUNDS 10

static refs_table_t tbl;

/* Add (up) or remove the values first, ..., first+count-1, in parallel */
VOID_TASK_4(protect_range, uint64_t, first, uint64_t, count, int, up, int, refs)
{
    if (count > 128) {
        SPAWN(protect_range, first, count/2, up, refs);
        CALL(protect_range, first+count/2, count-count/2, up, refs);
        SYNC(protect_range);
        return;
    }
    for (uint64_t i=0; i<count; i++) {
        if (refs) {
            if (up) refs_up(&tbl, first+i);
            else refs_down(&tbl, first+i);
        } else {
            if (up) protect_up(&tbl, first+i);
            else protect_down(&tbl, first+i);
        }
    }
}

/* Add the values in [add, add+N) and remove those in [remove, remove+N), at the same time */
VOID_TASK_3(protect_both, uint64_t, add, uint64_t, remove, int, refs)
{
    SPAWN(protect_range, add, N, 1, refs);
    CALL(protect_range, remove, N, 0, refs);
    SYNC(protect_range);
}

/* Check that the table holds each value in [first, first+N) once, and nothing else */
static int
check_contents(uint64_t first, int refs)
{
    static char seen[N];
    for (int i=0; i<N; i++) seen[i] = 0;
    size_t count = 0;
    uint64_t *it = refs ? refs_iter(&tbl, 0, tbl.refs_size) : protect_iter(&tbl, 0, tbl.refs_size);
    while (it != NULL) {
        uint64_t v = refs ? refs_next(&tbl, &it, tbl.refs_size) : protect_next(&tbl, &it, tbl.refs_size);
        test_assert(v >= first && v < first+N);
        test_assert(seen[v-first] == 0);
        seen[v-first] = 1;
        count++;
    }
    test_assert(count == N);
    test_assert((refs ? refs_count(&tbl) : protect_count(&tbl)) == N);
    return 0;
}

/* One round on a fresh table that starts small; workers with an id below nworkers have a counter */
static int
test_table_round(int refs, size_t nworkers)
{
    if (refs) refs_create(&tbl, 128);
    else protect_create(&tbl, 128);
    size_t allocated = tbl.refs_nworkers;
    if (nworkers < allocated) tbl.refs_nworkers = nworkers;

    // add [1, N], while the table grows
    RUN(protect_range, 1, N, 1, refs);
    if (check_contents(1, refs)) return 1;
    // add [N+1, 2N] and remove [1, N] at the same time
    RUN(protect_both, N+1, 1, refs);
    if (check_contents(N+1, refs)) return 1;
    // for references: a second reference to every value, then remove one again
    if (refs) {
        RUN(protect_range, N+1, N, 1, refs);
        RUN(protect_range, N+1, N, 0, refs);
        if (check_contents(N+1, refs)) return 1;
    }
    RUN(protect_range, N+1, N, 0, refs);
    test_assert((refs ? refs_count(&tbl) : protect_count(&tbl)) == 0);

    tbl.refs_nworkers = allocated;
    if (refs) refs_free(&tbl);
    else protect_free(&tbl);
    return 0;
}

static int
test_tables(void)
{
    for (int r=0; r<ROUNDS; r++) {
        for (int refs=0; refs<2; refs++) {
            // every worker has a counter
            if (test_table_round(refs, (size_t)-1)) return 1;
            // only worker 0 has a counter, the others use the shared one
            if (test_table_round(refs, 1)) return 1;
        }
    }
    return 0;
}

#define SLOTS 4096
#define NVARS 12

static BDD slots[SLOTS];

/* The minterm of the bits of i over the variables 0 .. NVARS-1 */
static BDD
minterm(int i)
{
    BDD res = sylvan_true;
    for (int v=NVARS-1; v>=0; v--) {
        res = sylvan_makenode((uint32_t)v, (i >> v) & 1 ? sylvan_false : res, (i >> v) & 1 ? res : sylvan_false);
    }
    return res;
}

/* Build and protect slots first, ..., first+count-1, in parallel */
VOID_TASK_2(protect_slots, int, first, int, count)
{
    if (count > 16) {
        SPAWN(protect_slots, first, count/2);
        CALL(protect_slots, first+count/2, count-count/2);
        SYNC(protect_slots);
        return;
    }
    for (int i=first; i<first+count; i++) {
        slots[i] = sylvan_false;
        sylvan_protect(&slots[i]);
        slots[i] = minterm(i);
    }
}

static int
test_gc_survival(void)
{
    size_t before = mtbdd_count_protected();
    RUN(protect_slots, 0, SLOTS);
    test_assert(mtbdd_count_protected() == before + SLOTS);
    for (int r=0; r<3; r++) {
        sylvan_gc();
        for (int i=0; i<SLOTS; i++) {
            test_assert(mtbdd_test_isvalid(slots[i]));
            test_assert(slots[i] == minterm(i));
        }
    }
    for (int i=0; i<SLOTS; i++) sylvan_unprotect(&slots[i]);
    test_assert(mtbdd_count_protected() == before);
    return 0;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 4;
    lace_start(workers, 0);

    sylvan_set_sizes(1LL<<18, 1LL<<20, 1LL<<14, 1LL<<18); // no garbage collection while minterm builds
    sylvan_init_package();
    sylvan_init_bdd();

    if (test_tables()) return 1;
    printf("Protection and reference tables used by %d workers at once: ok.\n", workers);

    if (test_gc_survival()) return 1;
    printf("BDDs protected by %d workers at once survive garbage collection: ok.\n", workers);

    sylvan_quit();
    lace_stop();

    return 0;
}
