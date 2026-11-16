#include <sylvan_int.h>
#include <math.h>
#include "test_assert.h"

static BDDVAR levels[] = {1, 4, 9, 15, 23};
static unsigned collections;

VOID_TASK_0(count_collection)
{
    collections++;
}

#ifdef SYLVAN_TEST_PROBABILITY_GC
void test_probability_gc_arm(unsigned visits);
void test_probability_exhaust_ids(void);
#endif

static uint32_t random_state = UINT32_C(0x127bc931);

static uint32_t random_u32(void)
{
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

/* Bit i of an assignment gives the value of levels[i]. The truth table,
 * rather than a BDD traversal, is the independent oracle. */
static BDD from_table(uint32_t table, unsigned count, unsigned level, unsigned assignment)
{
    if (level == count) return (table >> assignment) & 1 ? sylvan_true : sylvan_false;
    BDD low = bdd_refs_push(from_table(table, count, level+1, assignment));
    BDD high = bdd_refs_push(from_table(table, count, level+1, assignment | (1u << level)));
    BDD result = sylvan_makenode(levels[level], low, high);
    bdd_refs_pop(2);
    return result;
}

static double enumerate(uint32_t table, const double *probabilities, unsigned count)
{
    long double result = 0.0L;
    for (unsigned assignment=0; assignment<(1u << count); assignment++) {
        if (!((table >> assignment) & 1)) continue;
        long double weight = 1.0L;
        for (unsigned i=0; i<count; i++) {
            long double p = probabilities[i];
            weight *= assignment & (1u << i) ? p : 1.0L-p;
        }
        result += weight;
    }
    return (double)result;
}

TASK_0(int, test_enumeration)
{
    BDDSET variables = bdd_refs_push(sylvan_set_fromarray(levels, 5));
    double probabilities[] = {0.17, 0.41, 0.73, 0.31, 0.91};
    double result;
    /* All functions of three variables, including reduced support and both
     * constants. The last two supplied variables never occur in these BDDs. */
    for (unsigned table=0; table<256; table++) {
        BDD bdd = bdd_refs_push(from_table(table, 3, 0, 0));
        test_assert(CALL(sylvan_probability, bdd, variables, probabilities, 5, &result));
        test_assert(fabs(result-enumerate(table, probabilities, 3)) < 1e-12);
        bdd_refs_pop(1);
    }

    BDDVAR extra_levels[] = {0, 1, 2, 4, 9, 12, 15, 23, 31};
    const unsigned positions[] = {1, 3, 4, 6, 7};
    BDDSET extra = bdd_refs_push(sylvan_set_fromarray(extra_levels, 9));
    for (unsigned iteration=0; iteration<128; iteration++) {
        uint32_t table = random_u32();
        BDD bdd = bdd_refs_push(from_table(table, 5, 0, 0));
        /* Mutate the same array between calls, without clearing the cache. */
        for (unsigned repeat=0; repeat<3; repeat++) {
            double packed[9];
            for (unsigned i=0; i<9; i++) packed[i] = (double)random_u32() / UINT32_MAX;
            for (unsigned i=0; i<5; i++) probabilities[i] = packed[positions[i]];
            double expected = enumerate(table, probabilities, 5);
            test_assert(CALL(sylvan_probability, bdd, variables, probabilities, 5, &result));
            test_assert(fabs(result-expected) < 1e-12);
            test_assert(CALL(sylvan_probability, bdd, extra, packed, 9, &result));
            test_assert(fabs(result-expected) < 1e-12);
            test_assert(CALL(sylvan_probability, sylvan_not(bdd), extra, packed, 9, &result));
            test_assert(fabs(result-(1.0-expected)) < 1e-12);
            /* Changing an irrelevant probability must not affect the result. */
            packed[0] = 1.0-packed[0];
            packed[8] = 1.0-packed[8];
            test_assert(CALL(sylvan_probability, bdd, extra, packed, 9, &result));
            test_assert(fabs(result-expected) < 1e-12);
        }
        /* Every deterministic distribution, including all-zero and all-one. */
        for (unsigned assignment=0; assignment<32; assignment++) {
            for (unsigned i=0; i<5; i++) probabilities[i] = (assignment >> i) & 1;
            test_assert(CALL(sylvan_probability, bdd, variables, probabilities, 5, &result));
            test_assert(result == ((table >> assignment) & 1));
        }
        if (iteration % 16 == 0) CALL(sylvan_gc);
        bdd_refs_pop(1);
    }
    bdd_refs_pop(2);
    return 0;
}

TASK_0(int, test_edges)
{
    double result = -17.0;
    test_assert(sylvan_probability(sylvan_false, sylvan_true, NULL, 0, &result) && result == 0.0);
    test_assert(sylvan_probability(sylvan_true, sylvan_true, NULL, 0, &result) && result == 1.0);
    BDD x = bdd_refs_push(sylvan_ithvar(1));
    BDD y = bdd_refs_push(sylvan_ithvar(4));
    BDD f = bdd_refs_push(sylvan_makenode(1, sylvan_true, y));
    BDDSET variables = bdd_refs_push(sylvan_set_fromarray(levels, 2));
    double probabilities[] = {1.0, 1e-30};
    /* Preserve a tiny high-branch value when p=1 (no subtractive cancellation). */
    test_assert(CALL(sylvan_probability, f, variables, probabilities, 2, &result));
    test_assert(result == probabilities[1]);
    /* The output can alias an input element: it is written only on success. */
    test_assert(CALL(sylvan_probability, f, variables, probabilities, 2, probabilities));
    test_assert(probabilities[0] == 1e-30);

    BDD leaf = bdd_refs_push(mtbdd_int64(1));
    BDD map = bdd_refs_push(mtbdd_map_add(mtbdd_map_empty(), 4, sylvan_true));
    BDD numeric = bdd_refs_push(sylvan_makenode(1, sylvan_true, leaf));
    BDD embedded_map = bdd_refs_push(sylvan_makenode(1, sylvan_false, map));
    const BDD invalid_bdds[] = {sylvan_invalid, leaf, map, numeric, embedded_map,
        UINT64_C(1), UINT64_C(1)<<45, (BDD)llmsset_get_data_size(nodes)};
    result = -17.0;
    probabilities[0] = 0.0; /* Invalid high paths still have to be checked. */
    for (size_t i=0; i<sizeof(invalid_bdds)/sizeof(invalid_bdds[0]); i++) {
        test_assert(!CALL(sylvan_probability, invalid_bdds[i], variables, probabilities, 2, &result));
        test_assert(result == -17.0);
    }
    BDD disjunction = bdd_refs_push(sylvan_or(x, y));
    const BDDSET invalid_sets[] = {sylvan_invalid, sylvan_false, sylvan_not(x),
        disjunction, leaf, map, UINT64_C(1), (BDD)llmsset_get_data_size(nodes)};
    for (size_t i=0; i<sizeof(invalid_sets)/sizeof(invalid_sets[0]); i++) {
        test_assert(!CALL(sylvan_probability, x, invalid_sets[i], probabilities, 2, &result));
        test_assert(result == -17.0);
    }
    test_assert(!CALL(sylvan_probability, f, x, probabilities, 1, &result));
    test_assert(!CALL(sylvan_probability, f, y, probabilities, 1, &result));
    test_assert(!CALL(sylvan_probability, f, sylvan_true, NULL, 0, &result));
    test_assert(!CALL(sylvan_probability, f, variables, probabilities, 1, &result));
    test_assert(!CALL(sylvan_probability, f, x, probabilities, 2, &result));
    test_assert(!CALL(sylvan_probability, f, variables, NULL, 2, &result));
    test_assert(!CALL(sylvan_probability, f, variables, probabilities, 2, NULL));
    test_assert(result == -17.0);
    const double bad[] = {-0.01, 1.01, NAN, INFINITY, -INFINITY};
    for (size_t i=0; i<sizeof(bad)/sizeof(bad[0]); i++) {
        probabilities[1] = bad[i];
        test_assert(!CALL(sylvan_probability, f, variables, probabilities, 2, &result));
        test_assert(!CALL(sylvan_probability, x, variables, probabilities, 2, &result));
        test_assert(!CALL(sylvan_probability, sylvan_true, variables, probabilities, 2, &result));
        test_assert(!CALL(sylvan_probability, sylvan_false, variables, probabilities, 2, &result));
        test_assert(result == -17.0);
    }
    bdd_refs_pop(9);
    return 0;
}

TASK_0(int, test_parallel)
{
    BDDSET variables = bdd_refs_push(sylvan_set_fromarray(levels, 5));
    uint32_t table = UINT32_C(0x19fc038a);
    BDD bdd = bdd_refs_push(from_table(table, 5, 0, 0));
    double a[5], b[5], first, second;
    for (unsigned repeat=0; repeat<128; repeat++) {
        for (unsigned i=0; i<5; i++) {
            a[i] = (double)random_u32() / UINT32_MAX;
            b[i] = (double)random_u32() / UINT32_MAX;
        }
        SPAWN(sylvan_probability, bdd, variables, a, 5, &first);
        int second_ok = CALL(sylvan_probability, bdd, variables, b, 5, &second);
        int first_ok = SYNC(sylvan_probability);
        test_assert(first_ok && second_ok);
        test_assert(fabs(first-enumerate(table, a, 5)) < 1e-12);
        test_assert(fabs(second-enumerate(table, b, 5)) < 1e-12);
    }
    bdd_refs_pop(2);
    return 0;
}

TASK_0(int, test_collection)
{
    /* Fill a small table with live nodes, then retain a BDD and its variable
     * set above the size to which an explicit collection shrinks the hash. */
    const size_t count = 12000;
    MTBDD *garbage = malloc(count * sizeof(*garbage));
    test_assert(garbage != NULL);
    unsigned before = collections;
    for (size_t i=0; i<count; i++) garbage[i] = mtbdd_ref(mtbdd_int64((int64_t)i+100000));
    test_assert(collections > before);
    uint32_t table = UINT32_C(0x19fc038a);
    BDD bdd = bdd_refs_push(from_table(table, 5, 0, 0));
    BDDSET variables = bdd_refs_push(sylvan_set_fromarray(levels, 5));
    for (size_t i=0; i<count; i++) mtbdd_deref(garbage[i]);
    free(garbage);
    CALL(sylvan_gc);
    test_assert(MTBDD_GETINDEX(bdd) >= llmsset_get_size(nodes));
    test_assert(MTBDD_GETINDEX(bdd) < llmsset_get_data_size(nodes));
    test_assert(MTBDD_GETINDEX(variables) >= llmsset_get_size(nodes));

    double p[] = {0.13, 0.29, 0.71, 0.83, 0.97};
    double result;
    for (unsigned repeat=0; repeat<4; repeat++) {
        SPAWN(sylvan_probability, bdd, variables, p, 5, &result);
        CALL(sylvan_gc);
        test_assert(SYNC(sylvan_probability));
        test_assert(fabs(result-enumerate(table, p, 5)) < 1e-12);
    }
#ifdef SYLVAN_TEST_PROBABILITY_GC
    /* Drop the caller roots immediately before CALL: the public operation
     * must protect both inputs throughout a collection inside its recursion.
     * Private test instrumentation collects at the third safepoint, after
     * child tasks have been spawned, also with just one worker. */
    bdd_refs_pop(2);
    before = collections;
    test_probability_gc_arm(3);
    test_assert(CALL(sylvan_probability, bdd, variables, p, 5, &result));
    test_assert(collections == before+1);
    test_assert(fabs(result-enumerate(table, p, 5)) < 1e-12);
#else
    bdd_refs_pop(2);
#endif
    return 0;
}

TASK_0(int, runtests)
{
    if (CALL(test_enumeration)) return 1;
    if (CALL(test_edges)) return 1;
    if (CALL(test_parallel)) return 1;
    if (CALL(test_collection)) return 1;
#ifdef SYLVAN_TEST_PROBABILITY_GC
    /* Issue the last unique id, then evaluate with caching disabled forever. */
    test_probability_exhaust_ids();
    BDD x = bdd_refs_push(sylvan_ithvar(1));
    double probability = 0.25, result;
    for (unsigned i=0; i<3; i++, probability += 0.25) {
        test_assert(CALL(sylvan_probability, x, x, &probability, 1, &result));
        test_assert(result == probability);
    }
    bdd_refs_pop(1);
#endif
#if SYLVAN_STATS
    sylvan_stats_t stats;
    sylvan_stats_snapshot(&stats);
    test_assert(stats.counters[BDD_PROBABILITY] > 0);
    test_assert(stats.counters[BDD_PROBABILITY_CACHEDPUT] > 0);
    test_assert(stats.counters[BDD_PROBABILITY_CACHED] > 0);
#endif
    return 0;
}

int main(int argc, char **argv)
{
    lace_start(argc > 1 ? (unsigned)atoi(argv[1]) : 1, 0);
    sylvan_set_sizes(1<<10, 1<<16, 1<<10, 1<<14);
    sylvan_init_package();
    sylvan_init_bdd();
    sylvan_gc_hook_pregc(count_collection_CALL);
    int result = RUN(runtests);
    sylvan_quit();
    lace_stop();
    return result;
}
