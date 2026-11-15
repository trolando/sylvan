#include <sylvan_int.h>
#include "test_assert.h"

/* Assignment integers are in lexicographic order: the first variable is the
 * most significant bit. Gaps in the levels also exercise nonconsecutive sets. */
static const BDDVAR levels[] = {0, 2, 5, 7, 11};

static BDDSET set_from_mask(unsigned mask)
{
    BDDVAR selected[5];
    size_t count = 0;
    for (unsigned i=0; i<5; i++) {
        if (mask & (1u << (4-i))) selected[count++] = levels[i];
    }
    return sylvan_set_fromarray(selected, count);
}

static BDD truth_bdd(unsigned table, const BDDVAR *vars, unsigned count)
{
    if (count == 0) return table & 1 ? sylvan_true : sylvan_false;
    unsigned shift = 1u << (count-1);
    BDD low = bdd_refs_push(truth_bdd(table, vars+1, count-1));
    BDD high = bdd_refs_push(truth_bdd(table >> shift, vars+1, count-1));
    BDD result = sylvan_makenode(vars[0], low, high);
    bdd_refs_pop(2);
    return result;
}

/* Expand a three-variable truth table to all five assignment variables. */
static uint32_t truth_assignments(unsigned table, unsigned support)
{
    uint32_t result = 0;
    for (unsigned assignment=0; assignment<32; assignment++) {
        unsigned index = 0;
        for (unsigned bit=16; bit; bit >>= 1) {
            if (support & bit) index = 2*index + ((assignment & bit) != 0);
        }
        if ((table >> index) & 1) result |= UINT32_C(1) << assignment;
    }
    return result;
}

/* An independent evaluator: follow complemented edges, without BDD operations
 * such as quantification, conjunction or model counting in the oracle. */
static int evaluate(BDD dd, unsigned assignment)
{
    while (!sylvan_isconst(dd)) {
        if (dd == sylvan_invalid || mtbdd_isleaf(dd)) return -1;
        unsigned i;
        for (i=0; i<5 && levels[i] != sylvan_var(dd); i++) {}
        if (i == 5) return -1;
        dd = assignment & (1u << (4-i)) ? sylvan_high(dd) : sylvan_low(dd);
    }
    return dd == sylvan_true;
}

static int check_assignments(BDD dd, uint32_t expected)
{
    for (unsigned assignment=0; assignment<32; assignment++) {
        int value = (expected >> assignment) & 1;
        test_assert(evaluate(dd, assignment) == value);
        test_assert(evaluate(sylvan_not(dd), assignment) == !value);
    }
    return 0;
}

static int test_abstraction(void)
{
    BDD a = sylvan_false, unique = sylvan_false, representatives = sylvan_false;
    BDDSET selected = sylvan_true;
    sylvan_protect(&a);
    sylvan_protect(&selected);
    sylvan_protect(&unique);
    sylvan_protect(&representatives);

    /* Every Boolean function of three variables, every subset of five
     * variables, and every assignment. The first and last variables are absent
     * from the input; other variables may disappear through BDD reduction. */
    for (unsigned table=0; table<256; table++) {
        a = truth_bdd(table, levels+1, 3);
        uint32_t satisfying = truth_assignments(table, 14);
        for (unsigned mask=0; mask<32; mask++) {
            selected = set_from_mask(mask);
            unique = sylvan_unique(a, selected);
            representatives = sylvan_pick_representatives(a, selected);
            uint32_t parity = 0, first = 0;
            for (unsigned free_values=0; free_values<32; free_values++) {
                if (free_values & mask) continue;
                unsigned count = 0;
                for (unsigned assignment=0; assignment<32; assignment++) {
                    if ((assignment & ~mask) != free_values) continue;
                    if ((satisfying >> assignment) & 1) {
                        if (count == 0) first |= UINT32_C(1) << assignment;
                        count++;
                    }
                }
                if (count & 1) {
                    for (unsigned assignment=0; assignment<32; assignment++) {
                        if ((assignment & ~mask) == free_values) parity |= UINT32_C(1) << assignment;
                    }
                }
            }
            test_assert(check_assignments(unique, parity) == 0);
            test_assert(check_assignments(representatives, first) == 0);
            test_assert(sylvan_unique(a, selected) == unique);
            test_assert(sylvan_pick_representatives(a, selected) == representatives);
            test_assert(sylvan_pick_representatives(representatives, selected) == representatives);
        }
        if (table % 32 == 0) sylvan_gc();
    }
    test_assert(sylvan_unique(sylvan_invalid, selected) == sylvan_invalid);
    test_assert(sylvan_unique(a, sylvan_invalid) == sylvan_invalid);
    test_assert(sylvan_pick_representatives(sylvan_invalid, selected) == sylvan_invalid);
    test_assert(sylvan_pick_representatives(a, sylvan_invalid) == sylvan_invalid);
    sylvan_unprotect(&representatives);
    sylvan_unprotect(&unique);
    sylvan_unprotect(&selected);
    sylvan_unprotect(&a);
    return 0;
}

static int test_witnesses(void)
{
    BDD a = sylvan_false, b = sylvan_false, witness = sylvan_false;
    sylvan_protect(&a);
    sylvan_protect(&b);
    sylvan_protect(&witness);
    const BDDVAR left_vars[] = {levels[0], levels[2], levels[4]};
    for (unsigned left=0; left<256; left++) {
        a = truth_bdd(left, left_vars, 3);
        for (unsigned right=0; right<256; right++) {
            b = truth_bdd(right, levels+1, 3);
            uint32_t intersection = truth_assignments(left, 21) & truth_assignments(right, 14);
            witness = sylvan_intersection_witness(a, b);
            test_assert(witness != sylvan_invalid);
            test_assert((witness == sylvan_false) == (intersection == 0));
            unsigned found = 0;
            for (unsigned assignment=0; assignment<32; assignment++) {
                int value = evaluate(witness, assignment);
                test_assert(value == 0 || value == 1);
                test_assert(!value || ((intersection >> assignment) & 1));
                if (!found && ((intersection >> assignment) & 1)) {
                    /* The low-first search includes the first intersection
                     * assignment, even when it returns several assignments. */
                    test_assert(value);
                    found = 1;
                }
            }
            test_assert(sylvan_intersection_witness(b, a) == witness);
        }
        if (left % 32 == 0) sylvan_gc();
    }
    test_assert(sylvan_intersection_witness(a, sylvan_not(a)) == sylvan_false);
    test_assert(sylvan_intersection_witness(sylvan_true, a) == a);
    test_assert(sylvan_intersection_witness(sylvan_invalid, a) == sylvan_invalid);
    test_assert(sylvan_intersection_witness(a, sylvan_invalid) == sylvan_invalid);
    sylvan_unprotect(&witness);
    sylvan_unprotect(&b);
    sylvan_unprotect(&a);
    return 0;
}

static unsigned popcount(unsigned value)
{
    unsigned count = 0;
    for (; value; value >>= 1) count += value & 1;
    return count;
}

static int test_cardinality(void)
{
    BDDSET variables = sylvan_true;
    BDD result = sylvan_false;
    sylvan_protect(&variables);
    sylvan_protect(&result);
    for (unsigned mask=0; mask<32; mask++) {
        variables = set_from_mask(mask);
        size_t size = popcount(mask);
        /* Reverse the bounds on the second pass without clearing the cache. */
        for (unsigned pass=0; pass<2; pass++) {
            for (size_t i=0; i<=size; i++) {
                size_t minimum = pass ? size-i : i;
                for (size_t j=minimum; j<=size; j++) {
                    size_t maximum = pass ? size+minimum-j : j;
                    result = sylvan_cardinality(variables, minimum, maximum);
                    uint32_t expected = 0;
                    for (unsigned assignment=0; assignment<32; assignment++) {
                        size_t count = popcount(assignment & mask);
                        if (minimum <= count && count <= maximum) expected |= UINT32_C(1) << assignment;
                    }
                    test_assert(check_assignments(result, expected) == 0);
                }
            }
        }
        test_assert(sylvan_cardinality(variables, 0, size) == sylvan_true);
        test_assert(sylvan_cardinality(variables, size, size) == variables);
        test_assert(sylvan_cardinality(variables, 1, 0) == sylvan_invalid);
        test_assert(sylvan_cardinality(variables, 0, size+1) == sylvan_invalid);
        test_assert(sylvan_cardinality(variables, SIZE_MAX, SIZE_MAX) == sylvan_invalid);
    }
    BDD x = bdd_refs_push(sylvan_ithvar(0));
    BDD y = bdd_refs_push(sylvan_ithvar(2));
    test_assert(sylvan_cardinality(sylvan_invalid, 0, 0) == sylvan_invalid);
    test_assert(sylvan_cardinality(sylvan_false, 0, 0) == sylvan_invalid);
    test_assert(sylvan_cardinality(sylvan_not(x), 0, 1) == sylvan_invalid);
    test_assert(sylvan_cardinality(sylvan_or(x, y), 0, 2) == sylvan_invalid);
    test_assert(sylvan_cardinality(mtbdd_int64(7), 0, 0) == sylvan_invalid);
    bdd_refs_pop(2);
    sylvan_unprotect(&result);
    sylvan_unprotect(&variables);
    return 0;
}

/* Port the source commits' characteristic cases, including SPAWN/SYNC and
 * live results across explicit collection. */
TASK_0(int, test_source_cases)
{
    BDD x = bdd_refs_push(sylvan_ithvar(0));
    BDD y = bdd_refs_push(sylvan_ithvar(2));
    BDD z = bdd_refs_push(sylvan_ithvar(5));
    BDD xy = bdd_refs_push(sylvan_and(x, y));
    BDD either = bdd_refs_push(sylvan_or(x, y));
    BDDSET selected = bdd_refs_push(sylvan_and(x, z));
    BDDSET all = bdd_refs_push(sylvan_and(xy, z));
    BDD xor_xy = bdd_refs_push(sylvan_xor(x, y));
    BDD expected = bdd_refs_push(sylvan_and(xor_xy, sylvan_not(z)));
    BDD right = bdd_refs_push(sylvan_or(sylvan_not(x), y));

    bdd_refs_spawn(SPAWN(sylvan_unique, xy, x));
    bdd_refs_spawn(SPAWN(sylvan_intersection_witness, either, right));
    bdd_refs_spawn(SPAWN(sylvan_pick_representatives, either, selected));
    bdd_refs_spawn(SPAWN(sylvan_cardinality, all, 1, 2));
    BDD exact = bdd_refs_push(CALL(sylvan_cardinality, all, 1, 1));
    CALL(sylvan_gc);
    BDD cardinality = bdd_refs_push(bdd_refs_sync(SYNC(sylvan_cardinality)));
    BDD representatives = bdd_refs_push(bdd_refs_sync(SYNC(sylvan_pick_representatives)));
    BDD witness = bdd_refs_push(bdd_refs_sync(SYNC(sylvan_intersection_witness)));
    BDD unique = bdd_refs_push(bdd_refs_sync(SYNC(sylvan_unique)));
    CALL(sylvan_gc);

    test_assert(unique == y);
    test_assert(sylvan_unique(xy, xy) == sylvan_true);
    test_assert(sylvan_unique(xor_xy, xy) == sylvan_false);
    /* Three satisfying extensions are odd, but are not exactly one. */
    test_assert(sylvan_unique(either, xy) == sylvan_true);
    test_assert(sylvan_unique(x, z) == sylvan_false);
    test_assert(sylvan_unique(y, x) == sylvan_false);
    test_assert(representatives == expected);
    test_assert(sylvan_exists(representatives, selected) == sylvan_exists(either, selected));
    test_assert(sylvan_pick_representatives(sylvan_true, selected) == sylvan_and(sylvan_not(x), sylvan_not(z)));
    test_assert(witness != sylvan_false && sylvan_subset(witness, y));
    test_assert(cardinality == sylvan_cardinality(all, 1, 2));
    test_assert(exact == sylvan_cardinality(all, 1, 1));
    const uint64_t counts[] = {1, 3, 3, 1};
    for (size_t minimum=0; minimum<=3; minimum++) {
        for (size_t maximum=minimum; maximum<=3; maximum++) {
            BDD result = bdd_refs_push(sylvan_cardinality(all, minimum, maximum));
            uint64_t expected_count = 0, count = 0;
            for (size_t i=minimum; i<=maximum; i++) expected_count += counts[i];
            test_assert(sylvan_satcount_u64(result, all, &count) && count == expected_count);
            bdd_refs_pop(1);
        }
    }
    bdd_refs_pop(15);
    return 0;
}

TASK_0(int, test_gc_results)
{
    CALL(sylvan_gc);
    BDDVAR vars[64];
    uint8_t values[64];
    for (unsigned i=0; i<64; i++) {
        vars[i] = i;
        values[i] = i >= 32;
    }
    BDDSET all = bdd_refs_push(sylvan_set_fromarray(vars, 64));
    size_t before = llmsset_get_size(nodes);
    /* Exact-32 needs more than the initial 1024 nodes, forcing collection
     * while cardinality's recursive results are still on the reference stacks. */
    BDD exact = bdd_refs_push(CALL(sylvan_cardinality, all, 32, 32));
    test_assert(llmsset_get_size(nodes) > before);
    BDD range = bdd_refs_push(CALL(sylvan_cardinality, all, 31, 33));
    BDD expected = bdd_refs_push(sylvan_cube(all, values));

    bdd_refs_spawn(SPAWN(sylvan_unique, exact, all));
    bdd_refs_spawn(SPAWN(sylvan_intersection_witness, exact, range));
    bdd_refs_spawn(SPAWN(sylvan_pick_representatives, exact, all));
    CALL(sylvan_gc);
    BDD representatives = bdd_refs_push(bdd_refs_sync(SYNC(sylvan_pick_representatives)));
    BDD witness = bdd_refs_push(bdd_refs_sync(SYNC(sylvan_intersection_witness)));
    BDD unique = bdd_refs_push(bdd_refs_sync(SYNC(sylvan_unique)));
    CALL(sylvan_gc);
    test_assert(representatives == expected);
    test_assert(witness != sylvan_false && sylvan_subset(witness, exact));
    test_assert(sylvan_subset(expected, witness));
    /* The central binomial coefficient (64 choose 32) is even. */
    test_assert(unique == sylvan_false);
    test_assert(sylvan_cardinality(all, 32, 32) == exact);
    bdd_refs_pop(7);
    return 0;
}

TASK_0(int, runtests)
{
    if (test_abstraction()) return 1;
    if (test_witnesses()) return 1;
    if (test_cardinality()) return 1;
    if (CALL(test_source_cases)) return 1;
    if (CALL(test_gc_results)) return 1;
#if SYLVAN_STATS
    sylvan_stats_t stats;
    sylvan_stats_snapshot(&stats);
    const Sylvan_Counters operations[] = {
        BDD_UNIQUE, BDD_INTERSECTION_WITNESS, BDD_PICK_REPRESENTATIVES, BDD_CARDINALITY
    };
    for (size_t i=0; i<sizeof(operations)/sizeof(operations[0]); i++) {
        test_assert(stats.counters[operations[i]] > 0);
        test_assert(stats.counters[operations[i]+1] > 0); /* cache puts */
        test_assert(stats.counters[operations[i]+2] > 0); /* cache hits */
    }
#endif
    return 0;
}

int main(int argc, char **argv)
{
    unsigned workers = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    lace_start(workers, 0);
    sylvan_set_sizes(1<<10, 1<<20, 1<<10, 1<<18);
    sylvan_init_package();
    sylvan_init_bdd();
    int result = RUN(runtests);
    sylvan_quit();
    lace_stop();
    return result;
}
