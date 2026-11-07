/**
 * Tests for garbage collection.
 *
 * 1. Several workers start garbage collection often and at the same time, with
 *    both sylvan_gc() and sylvan_gc_collect(). Before 1.12.2, a worker that did
 *    so just after another worker's collection had finished could wait forever.
 *    CTest gives this test a timeout, so that a hang fails the test. Two workers
 *    suffice to show the hang; more workers than cores make every collection
 *    slow (all workers take part).
 * 2. sylvan_gc() shrinks the nodes table to fit the live nodes, which keep their
 *    place and can still be found, and the table grows again when needed.
 */

#include <stdio.h>
#include <stdlib.h>

#include "sylvan.h"
#include "sylvan_int.h"
#include "test_assert.h"

#include <lace.h>

TASK_1(int, gc_storm, int, depth)
{
    if (depth == 0) {
        for (int i=0; i<20; i++) {
            if (i & 1) CALL(sylvan_gc);
            else CALL(sylvan_gc_collect);
        }
        return 1;
    }
    SPAWN(gc_storm, depth-1);
    int a = CALL(gc_storm, depth-1);
    CALL(sylvan_gc);
    int b = SYNC(gc_storm);
    return a + b;
}

static uint64_t seed;

static uint64_t
next_random(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

#define NVARS 48

/**
 * A union of <count> random cubes over NVARS variables, the same for the same <s>.
 */
static BDD
random_bdd(uint64_t s, int count)
{
    seed = s;
    uint32_t vars[NVARS];
    for (int i=0; i<NVARS; i++) vars[i] = (uint32_t)i;
    BDD varset = sylvan_set_fromarray(vars, NVARS);
    sylvan_protect(&varset);
    BDD result = sylvan_false, cube = sylvan_false;
    sylvan_protect(&result);
    sylvan_protect(&cube);
    uint8_t values[NVARS];
    for (int c=0; c<count; c++) {
        for (int i=0; i<NVARS; i++) values[i] = (uint8_t)(next_random() % 3); // 0, 1 or 2 (any)
        cube = sylvan_cube(varset, values);
        result = sylvan_or(result, cube);
    }
    sylvan_unprotect(&varset);
    sylvan_unprotect(&result);
    sylvan_unprotect(&cube);
    return result;
}

static size_t
table_size(void)
{
    size_t filled, total;
    sylvan_table_usage(&filled, &total);
    return total;
}

static size_t
table_filled(void)
{
    size_t filled, total;
    sylvan_table_usage(&filled, &total);
    return filled;
}

/**
 * The highest bucket with a node; right after a collection, the highest live node.
 */
static size_t
highest_node(void)
{
    for (size_t i = llmsset_get_max_size(nodes); i-- > 2;) {
        if (llmsset_is_marked(nodes, i)) return i;
    }
    return 0;
}

TASK_0(int, test_shrink)
{
    BDD keep = random_bdd(42, 200), late = sylvan_false, big = sylvan_false;
    sylvan_protect(&keep);
    sylvan_protect(&late);
    sylvan_protect(&big);
    size_t keep_nodes = sylvan_nodecount(keep);

    size_t start = table_size();
    big = random_bdd(7, 1000);
    size_t grown = table_size();
    test_assert(grown > start);

    /* a BDD made while the table is large, so that some of its nodes are in
       high buckets, above the size the table shrinks to */
    late = random_bdd(43, 200);
    size_t late_nodes = sylvan_nodecount(late);

    /* drop the big BDD, then shrink */
    big = sylvan_false;
    sylvan_gc();
    size_t shrunk = table_size();
    size_t live = table_filled();
    size_t highest = highest_node();
    printf("Nodes table: %zu buckets, grown to %zu, shrunk to %zu for %zu live nodes; highest live node %zu.\n",
           start, grown, shrunk, live, highest);
    test_assert(shrunk < grown);
    test_assert(shrunk >= start);
    test_assert(shrunk >= 2 * live);
    test_assert(highest < llmsset_get_data_size(nodes));

    /* the BDDs are intact, and building them again finds the same nodes */
    test_assert(sylvan_nodecount(keep) == keep_nodes);
    test_assert(sylvan_nodecount(late) == late_nodes);
    BDD again = random_bdd(42, 200);
    test_assert(again == keep);
    again = random_bdd(43, 200);
    test_assert(again == late);

    /* the table grows again, and keep stays the same */
    big = random_bdd(9, 1000);
    test_assert(table_size() > shrunk);
    again = random_bdd(42, 200);
    test_assert(again == keep);
    test_assert(sylvan_nodecount(keep) == keep_nodes);

    /* shrink once more with the big BDD alive: the table must keep room for it */
    size_t before = table_size();
    sylvan_gc();
    test_assert(table_size() <= before);
    test_assert(table_size() >= 2 * table_filled() || table_size() == before);
    test_assert(random_bdd(9, 1000) == big);

    sylvan_unprotect(&keep);
    sylvan_unprotect(&late);
    sylvan_unprotect(&big);
    return 0;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 2;
    lace_start(workers, 0);

    sylvan_set_sizes(1LL<<12, 1LL<<22, 1LL<<10, 1LL<<18);
    sylvan_init_package();
    sylvan_init_bdd();

    for (int r=0; r<10; r++) {
        int n = RUN(gc_storm, 8);
        if (n != 256) {
            fprintf(stderr, "gc_storm returned %d instead of 256\n", n);
            return 1;
        }
    }
    printf("Garbage collection by many workers at once: no hang.\n");

    if (RUN(test_shrink)) return 1;
    printf("Shrinking the nodes table with sylvan_gc: ok.\n");

    sylvan_quit();
    lace_stop();

    return 0;
}
