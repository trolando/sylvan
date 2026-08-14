/*
 * Copyright 2026 Tom van Dijk, Formal Methods and Tools, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * Variable identity versus variable rank.
 *
 * Every test here installs a variable order that is NOT the identity order,
 * before a single node exists, and then checks that the BDD/MTBDD operations
 * follow the rank order rather than the identity order. Any operation that
 * still compares raw variable fields to decide "which variable is on top"
 * either builds a node whose children have a lower rank than itself (caught
 * by mtbdd_is_valid, which checks rank order) or computes the wrong function
 * (caught by the exhaustive truth-table checks).
 *
 * ZDD and ListDD are deliberately not initialised: they order by identity and
 * are excluded from the rank model.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lace.h>
#include <sylvan/internal.h>

#include "test_assert.h"

/**
 * A deliberately unstructured permutation: no fixed point, not a rotation,
 * and it inverts several pairs so that identity order and rank order
 * disagree in both directions.
 */
#define TEST_VARS 8
#define REL_PAIRS 4
#define REL_BASE TEST_VARS
#define ALL_VARS (TEST_VARS + 2 * REL_PAIRS)
static const uint32_t test_order[ALL_VARS] = {
    5, 2, 7, 0, 3, 6, 1, 4,      /* the Boolean domain, scrambled */
    12, 13, 8, 9, 14, 15, 10, 11 /* four s/t pairs, kept rank-adjacent */
};
static uint32_t test_rank[ALL_VARS];

static BDD test_variable[ALL_VARS];   /* indexed by identity */

/** Truth tables over TEST_VARS variables; bit a set iff assignment a holds. */
typedef uint64_t truth[4];

static int
truth_get(const truth table, unsigned assignment)
{
    return (table[assignment >> 6] >> (assignment & 63)) & 1;
}

static void
truth_set(truth table, unsigned assignment)
{
    table[assignment >> 6] |= UINT64_C(1) << (assignment & 63);
}

static size_t
truth_count(const truth table)
{
    size_t count = 0;
    for (int i = 0; i < 4; i++) count += popcnt_uint64(table[i]);
    return count;
}

/** Deterministic small PRNG, so a failure is reproducible. */
static uint64_t test_random_state = UINT64_C(0x9e3779b97f4a7c15);

static uint64_t
test_random(void)
{
    test_random_state ^= test_random_state << 13;
    test_random_state ^= test_random_state >> 7;
    test_random_state ^= test_random_state << 17;
    return test_random_state;
}

/**
 * Build the BDD of a truth table, one minterm at a time, using the variable
 * set (whose spine is in rank order) and cubes packed in that same order.
 */
TASK(int, truth_to_bdd, BDD*, result, const uint64_t*, table, BDDSET, variables)
int
truth_to_bdd_CALL(lace_worker *lace, BDD *result, const uint64_t *table,
                  BDDSET variables)
{
    BDD computed = bdd_false;
    mtbdd_refs_pushptr(&computed);
    for (unsigned assignment = 0; assignment < (1u << TEST_VARS); assignment++) {
        if (!truth_get(table, assignment)) continue;
        uint8_t cube[TEST_VARS];
        for (int position = 0; position < TEST_VARS; position++) {
            cube[position] = (assignment >> test_order[position]) & 1;
        }
        const int status = bdd_or_cube_CALL(
            lace, &computed, computed, variables, cube);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(1);
            return status;
        }
    }
    *result = computed;
    mtbdd_refs_popptr(1);
    return SYLVAN_OK;
}

/** Check that <dd> is exactly the function described by <table>. */
TASK(int, check_bdd, BDD, dd, const uint64_t*, table, BDDSET, variables)
int
check_bdd_CALL(lace_worker *lace, BDD dd, const uint64_t *table,
               BDDSET variables)
{
    test_assert(mtbdd_is_valid_CALL(lace, dd));

    for (unsigned assignment = 0; assignment < (1u << TEST_VARS); assignment++) {
        uint8_t values[TEST_VARS];
        for (int position = 0; position < TEST_VARS; position++) {
            values[position] = (assignment >> test_order[position]) & 1;
        }
        BDD evaluated = mtbdd_invalid;
        test_assert(bdd_eval(&evaluated, dd, variables, values, TEST_VARS)
                    == SYLVAN_OK);
        test_assert(evaluated == (truth_get(table, assignment)
                                  ? bdd_true : bdd_false));
    }

    uint64_t satisfying = 0;
    test_assert(bdd_sat_count_u64_CALL(lace, &satisfying, dd, variables)
                == SYLVAN_OK);
    test_assert(satisfying == truth_count(table));
    return 0;
}

/**
 * The permutation hook itself, plus the invariants every other test relies on.
 */
TASK(int, test_permutation)
int
test_permutation_CALL(lace_worker *lace)
{
    (void)lace;

    test_assert(sylvan_ranks_registered() == ALL_VARS);

    for (uint32_t identity = 0; identity < ALL_VARS; identity++) {
        test_assert(sylvan_rank_of(identity) == test_rank[identity]);
        test_assert(sylvan_identity_at(test_rank[identity]) == identity);
    }

    /* Unregistered identities keep rank == identity, which is what makes the
       0xffffffff "no variable" sentinel order last. */
    test_assert(sylvan_rank_of(ALL_VARS) == ALL_VARS);
    test_assert(sylvan_rank_of(UINT32_MAX) == UINT32_MAX);
    test_assert(sylvan_identity_at(ALL_VARS) == ALL_VARS);

    for (uint32_t a = 0; a < ALL_VARS; a++) {
        for (uint32_t b = 0; b < ALL_VARS; b++) {
            test_assert(sylvan_var_before(a, b)
                        == (test_rank[a] < test_rank[b]));
            test_assert(sylvan_var_min(a, b)
                        == (test_rank[a] < test_rank[b] ? a : b));
        }
        test_assert(sylvan_var_before(a, UINT32_MAX));
        test_assert(!sylvan_var_before(UINT32_MAX, a));
    }

    /* Rejected permutations leave the order untouched. */
    uint32_t rejected[ALL_VARS];
    memcpy(rejected, test_order, sizeof(rejected));
    rejected[0] = rejected[1];
    test_assert(sylvan_ranks_permute_unsafe(rejected, ALL_VARS)
                == SYLVAN_ERR_INVALID);
    test_assert(sylvan_ranks_permute_unsafe(test_order, ALL_VARS - 1)
                == SYLVAN_ERR_INVALID);
    test_assert(sylvan_ranks_permute_unsafe(NULL, ALL_VARS)
                == SYLVAN_ERR_INVALID);
    test_assert(sylvan_ranks_reserve(SYLVAN_RANKS_LIMIT + 1)
                == SYLVAN_ERR_INVALID);
    for (uint32_t identity = 0; identity < ALL_VARS; identity++) {
        test_assert(sylvan_rank_of(identity) == test_rank[identity]);
    }
    return 0;
}

/**
 * bdd_var_at_level() is indexed by order position, and the node it returns
 * carries the identity ranked there.
 */
TASK(int, test_var_at_level)
int
test_var_at_level_CALL(lace_worker *lace)
{
    (void)lace;

    for (uint32_t level = 0; level < ALL_VARS; level++) {
        BDD variable = mtbdd_invalid;
        mtbdd_refs_pushptr(&variable);
        test_assert(bdd_var_at_level(&variable, level) == SYLVAN_OK);
        test_assert(mtbdd_node_variable(variable) == test_order[level]);
        test_assert(variable == test_variable[test_order[level]]);
        mtbdd_refs_popptr(1);
    }

    /* A level beyond the registered range creates that identity in order. */
    BDD fresh = mtbdd_invalid;
    mtbdd_refs_pushptr(&fresh);
    test_assert(bdd_var_at_level(&fresh, ALL_VARS + 3) == SYLVAN_OK);
    test_assert(mtbdd_node_variable(fresh) == ALL_VARS + 3);
    test_assert(sylvan_rank_of(ALL_VARS + 3) == ALL_VARS + 3);
    mtbdd_refs_popptr(1);
    return 0;
}

/**
 * Node construction puts the lower-ranked variable on top, both directly
 * through mtbdd_make_node and through the operations that pick a top
 * variable from two operands.
 */
TASK(int, test_make_node)
int
test_make_node_CALL(lace_worker *lace)
{
    for (uint32_t a = 0; a < TEST_VARS; a++) {
        for (uint32_t b = 0; b < TEST_VARS; b++) {
            if (a == b) continue;
            const uint32_t top = test_rank[a] < test_rank[b] ? a : b;

            BDD conjunction = mtbdd_invalid;
            BDD disjunction = mtbdd_invalid;
            BDD difference = mtbdd_invalid;
            BDD chosen = mtbdd_invalid;
            mtbdd_refs_pushptr(&conjunction);
            mtbdd_refs_pushptr(&disjunction);
            mtbdd_refs_pushptr(&difference);
            mtbdd_refs_pushptr(&chosen);

            test_assert(bdd_and_CALL(lace, &conjunction,
                                     test_variable[a], test_variable[b])
                        == SYLVAN_OK);
            test_assert(bdd_or_CALL(lace, &disjunction,
                                    test_variable[a], test_variable[b])
                        == SYLVAN_OK);
            test_assert(bdd_xor_CALL(lace, &difference,
                                     test_variable[a], test_variable[b])
                        == SYLVAN_OK);
            test_assert(bdd_ite_CALL(lace, &chosen, test_variable[a],
                                     test_variable[b], bdd_false)
                        == SYLVAN_OK);

            test_assert(mtbdd_node_variable(conjunction) == top);
            test_assert(mtbdd_node_variable(disjunction) == top);
            test_assert(mtbdd_node_variable(difference) == top);
            test_assert(mtbdd_node_variable(chosen) == top);
            test_assert(mtbdd_is_valid_CALL(lace, conjunction));
            test_assert(mtbdd_is_valid_CALL(lace, disjunction));
            test_assert(mtbdd_is_valid_CALL(lace, difference));
            test_assert(mtbdd_is_valid_CALL(lace, chosen));

            mtbdd_refs_popptr(4);
        }
    }

    /* Reduction still applies whichever variable is on top. */
    BDD reduced = mtbdd_make_node(test_order[TEST_VARS - 1], bdd_true, bdd_true);
    test_assert(reduced == bdd_true);
    return 0;
}

/**
 * Variable sets are spines ordered by rank, whatever order the caller passes.
 */
TASK(int, test_variable_sets)
int
test_variable_sets_CALL(lace_worker *lace)
{
    uint32_t identities[TEST_VARS];
    for (uint32_t i = 0; i < TEST_VARS; i++) identities[i] = i;

    BDDSET ascending = mtbdd_invalid;
    BDDSET descending = mtbdd_invalid;
    BDDSET shuffled = mtbdd_invalid;
    BDDSET incremental = mtbdd_invalid;
    mtbdd_refs_pushptr(&ascending);
    mtbdd_refs_pushptr(&descending);
    mtbdd_refs_pushptr(&shuffled);
    mtbdd_refs_pushptr(&incremental);

    test_assert(bdd_set_from_array(&ascending, identities, TEST_VARS)
                == SYLVAN_OK);

    uint32_t reversed[TEST_VARS];
    for (uint32_t i = 0; i < TEST_VARS; i++) reversed[i] = TEST_VARS - 1 - i;
    test_assert(bdd_set_from_array(&descending, reversed, TEST_VARS)
                == SYLVAN_OK);

    test_assert(bdd_set_from_array(&shuffled, test_order, TEST_VARS)
                == SYLVAN_OK);

    /* Insertion order must not matter: all three are the same set. */
    test_assert(ascending == descending);
    test_assert(ascending == shuffled);

    /* The spine comes out in rank order, both by traversal and by export. */
    BDDSET walk = ascending;
    for (uint32_t level = 0; level < TEST_VARS; level++) {
        test_assert(!bdd_set_is_empty(walk));
        test_assert(bdd_set_first(walk) == test_order[level]);
        walk = bdd_set_next(walk);
    }
    test_assert(bdd_set_is_empty(walk));

    uint32_t exported[TEST_VARS];
    bdd_set_to_array(ascending, exported);
    test_assert(memcmp(exported, test_order, sizeof(exported)) == 0);
    test_assert(bdd_set_count(ascending) == TEST_VARS);

    /* Incremental construction agrees with bulk construction. */
    incremental = bdd_set_empty();
    for (uint32_t i = 0; i < TEST_VARS; i++) {
        const uint32_t identity = test_order[(i * 5 + 3) % TEST_VARS];
        test_assert(bdd_set_add(&incremental, incremental, identity)
                    == SYLVAN_OK);
    }
    test_assert(incremental == ascending);

    /* Removing from anywhere in the spine keeps the rest in rank order. */
    for (uint32_t level = 0; level < TEST_VARS; level++) {
        BDDSET without = mtbdd_invalid;
        mtbdd_refs_pushptr(&without);
        test_assert(bdd_set_remove(&without, ascending, test_order[level])
                    == SYLVAN_OK);
        test_assert(bdd_set_count(without) == TEST_VARS - 1);
        test_assert(!bdd_set_contains(without, test_order[level]));
        BDDSET rest = without;
        for (uint32_t other = 0; other < TEST_VARS; other++) {
            if (other == level) continue;
            test_assert(bdd_set_first(rest) == test_order[other]);
            rest = bdd_set_next(rest);
        }
        test_assert(bdd_set_is_empty(rest));
        mtbdd_refs_popptr(1);
    }

    for (uint32_t identity = 0; identity < TEST_VARS; identity++) {
        test_assert(bdd_set_contains(ascending, identity));
    }
    test_assert(!bdd_set_contains(ascending, TEST_VARS));

    /* Union and difference walk two rank-ordered spines at once. */
    BDDSET even = mtbdd_invalid;
    BDDSET odd = mtbdd_invalid;
    BDDSET united = mtbdd_invalid;
    BDDSET remainder = mtbdd_invalid;
    mtbdd_refs_pushptr(&even);
    mtbdd_refs_pushptr(&odd);
    mtbdd_refs_pushptr(&united);
    mtbdd_refs_pushptr(&remainder);

    even = bdd_set_empty();
    odd = bdd_set_empty();
    for (uint32_t identity = 0; identity < TEST_VARS; identity++) {
        BDDSET *target = (identity & 1) ? &odd : &even;
        test_assert(bdd_set_add(target, *target, identity) == SYLVAN_OK);
    }
    test_assert(bdd_set_union_CALL(lace, &united, even, odd) == SYLVAN_OK);
    test_assert(united == ascending);
    test_assert(bdd_set_difference_CALL(lace, &remainder, ascending, even)
                == SYLVAN_OK);
    test_assert(remainder == odd);

    mtbdd_refs_popptr(8);
    return 0;
}

/**
 * The support of a diagram is a set, so it too must come out in rank order.
 */
TASK(int, test_support)
int
test_support_CALL(lace_worker *lace)
{
    const uint32_t used[3] = {test_order[6], test_order[1], test_order[4]};

    BDD dd = mtbdd_invalid;
    BDD conjunction = mtbdd_invalid;
    BDDSET support = mtbdd_invalid;
    mtbdd_refs_pushptr(&dd);
    mtbdd_refs_pushptr(&conjunction);
    mtbdd_refs_pushptr(&support);

    test_assert(bdd_and_CALL(lace, &conjunction,
                             test_variable[used[0]], test_variable[used[1]])
                == SYLVAN_OK);
    test_assert(bdd_or_CALL(lace, &dd, conjunction, test_variable[used[2]])
                == SYLVAN_OK);
    test_assert(mtbdd_is_valid_CALL(lace, dd));

    test_assert(mtbdd_support_CALL(lace, &support, dd) == SYLVAN_OK);
    test_assert(bdd_set_count(support) == 3);
    test_assert(bdd_set_first(support) == test_order[1]);
    test_assert(bdd_set_first(bdd_set_next(support)) == test_order[4]);
    test_assert(bdd_set_first(bdd_set_next(bdd_set_next(support)))
                == test_order[6]);

    mtbdd_refs_popptr(3);
    return 0;
}

/**
 * Map spines are ordered by the rank of their keys, and composition walks a
 * diagram and a map together.
 */
TASK(int, test_maps)
int
test_maps_CALL(lace_worker *lace)
{
    MTBDDMAP map = mtbdd_invalid;
    BDD source = mtbdd_invalid;
    BDD composed = mtbdd_invalid;
    BDD expected = mtbdd_invalid;
    mtbdd_refs_pushptr(&map);
    mtbdd_refs_pushptr(&source);
    mtbdd_refs_pushptr(&composed);
    mtbdd_refs_pushptr(&expected);

    map = mtbdd_map_empty();
    for (uint32_t i = 0; i < TEST_VARS; i++) {
        const uint32_t key = test_order[(i * 3 + 1) % TEST_VARS];
        test_assert(mtbdd_map_set(&map, map, key, test_variable[key])
                    == SYLVAN_OK);
    }
    test_assert(mtbdd_map_count(map) == TEST_VARS);

    MTBDDMAP walk = map;
    for (uint32_t level = 0; level < TEST_VARS; level++) {
        test_assert(!mtbdd_map_is_empty(walk));
        test_assert(mtbdd_map_key(walk) == test_order[level]);
        walk = mtbdd_map_next(walk);
    }
    test_assert(mtbdd_map_is_empty(walk));

    for (uint32_t identity = 0; identity < TEST_VARS; identity++) {
        test_assert(mtbdd_map_contains(map, identity));
    }

    /* Replacing every variable by itself is the identity substitution. */
    test_assert(bdd_and_CALL(lace, &source,
                             test_variable[test_order[5]],
                             test_variable[test_order[0]]) == SYLVAN_OK);
    test_assert(bdd_compose_CALL(lace, &composed, source, map) == SYLVAN_OK);
    test_assert(composed == source);

    /* Swapping two variables through the map does what it says. */
    MTBDDMAP swap = mtbdd_map_empty();
    mtbdd_refs_pushptr(&swap);
    test_assert(mtbdd_map_set(&swap, swap, test_order[5],
                              test_variable[test_order[0]]) == SYLVAN_OK);
    test_assert(mtbdd_map_set(&swap, swap, test_order[0],
                              test_variable[test_order[5]]) == SYLVAN_OK);
    test_assert(bdd_compose_CALL(lace, &composed, source, swap) == SYLVAN_OK);
    test_assert(composed == source);
    test_assert(mtbdd_is_valid_CALL(lace, composed));

    test_assert(mtbdd_map_remove(&swap, swap, test_order[5]) == SYLVAN_OK);
    test_assert(mtbdd_map_count(swap) == 1);
    test_assert(mtbdd_map_key(swap) == test_order[0]);

    mtbdd_refs_popptr(5);
    return 0;
}

/**
 * Exhaustive semantic check over random functions: everything the operations
 * compute must match the truth tables, and every intermediate must be a
 * rank-ordered diagram.
 */
TASK(int, test_operations, BDDSET, variables)
int
test_operations_CALL(lace_worker *lace, BDDSET variables)
{
    for (int round = 0; round < 12; round++) {
        truth left = {0, 0, 0, 0};
        truth right = {0, 0, 0, 0};
        for (int i = 0; i < 4; i++) {
            left[i] = test_random();
            right[i] = test_random();
        }
        /* Also cover the sparse case, which exercises more skipped levels. */
        if (round % 3 == 0) {
            memset(left, 0, sizeof(left));
            memset(right, 0, sizeof(right));
            for (int i = 0; i < 5; i++) {
                truth_set(left, test_random() % (1u << TEST_VARS));
                truth_set(right, test_random() % (1u << TEST_VARS));
            }
        }

        BDD a = mtbdd_invalid;
        BDD b = mtbdd_invalid;
        BDD computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&a);
        mtbdd_refs_pushptr(&b);
        mtbdd_refs_pushptr(&computed);

        test_assert(truth_to_bdd_CALL(lace, &a, left, variables) == SYLVAN_OK);
        test_assert(truth_to_bdd_CALL(lace, &b, right, variables) == SYLVAN_OK);
        if (check_bdd_CALL(lace, a, left, variables)) return 1;
        if (check_bdd_CALL(lace, b, right, variables)) return 1;

        truth expected;

        for (int i = 0; i < 4; i++) expected[i] = left[i] & right[i];
        test_assert(bdd_and_CALL(lace, &computed, a, b) == SYLVAN_OK);
        if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

        for (int i = 0; i < 4; i++) expected[i] = left[i] | right[i];
        test_assert(bdd_or_CALL(lace, &computed, a, b) == SYLVAN_OK);
        if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

        for (int i = 0; i < 4; i++) expected[i] = left[i] ^ right[i];
        test_assert(bdd_xor_CALL(lace, &computed, a, b) == SYLVAN_OK);
        if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

        for (int i = 0; i < 4; i++) expected[i] = ~left[i] | right[i];
        test_assert(bdd_imp_CALL(lace, &computed, a, b) == SYLVAN_OK);
        if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

        /* ite(a, b, ~b) */
        for (int i = 0; i < 4; i++) {
            expected[i] = (left[i] & right[i]) | (~left[i] & ~right[i]);
        }
        test_assert(bdd_ite_CALL(lace, &computed, a, b, bdd_not(b))
                    == SYLVAN_OK);
        if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

        /* Quantify one variable at a time, at every order position. */
        for (uint32_t level = 0; level < TEST_VARS; level++) {
            const uint32_t identity = test_order[level];
            BDDSET single = mtbdd_invalid;
            mtbdd_refs_pushptr(&single);
            test_assert(bdd_set_from_array(&single, &identity, 1) == SYLVAN_OK);

            memset(expected, 0, sizeof(expected));
            for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
                 assignment++) {
                const unsigned flipped = assignment ^ (1u << identity);
                if (truth_get(left, assignment) || truth_get(left, flipped)) {
                    truth_set(expected, assignment);
                }
            }
            test_assert(bdd_exists_CALL(lace, &computed, a, single)
                        == SYLVAN_OK);
            if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

            memset(expected, 0, sizeof(expected));
            for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
                 assignment++) {
                const unsigned flipped = assignment ^ (1u << identity);
                if (truth_get(left, assignment) && truth_get(left, flipped)) {
                    truth_set(expected, assignment);
                }
            }
            test_assert(bdd_forall_CALL(lace, &computed, a, single)
                        == SYLVAN_OK);
            if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

            memset(expected, 0, sizeof(expected));
            for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
                 assignment++) {
                const unsigned flipped = assignment ^ (1u << identity);
                if (truth_get(left, assignment) != truth_get(left, flipped)) {
                    truth_set(expected, assignment);
                }
            }
            test_assert(bdd_unique_CALL(lace, &computed, a, single)
                        == SYLVAN_OK);
            if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

            mtbdd_refs_popptr(1);
        }

        /* Quantify a block of three variables that are far apart in rank. */
        {
            const uint32_t block[3] =
                {test_order[0], test_order[3], test_order[7]};
            BDDSET quantified = mtbdd_invalid;
            mtbdd_refs_pushptr(&quantified);
            test_assert(bdd_set_from_array(&quantified, block, 3) == SYLVAN_OK);

            truth conjunction;
            for (int i = 0; i < 4; i++) conjunction[i] = left[i] & right[i];
            memset(expected, 0, sizeof(expected));
            for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
                 assignment++) {
                for (int mask = 0; mask < 8; mask++) {
                    unsigned probe = assignment;
                    for (int i = 0; i < 3; i++) {
                        probe = (probe & ~(1u << block[i])) |
                                (((unsigned)(mask >> i) & 1u) << block[i]);
                    }
                    if (truth_get(conjunction, probe)) {
                        truth_set(expected, assignment);
                        break;
                    }
                }
            }
            test_assert(bdd_and_exists_CALL(lace, &computed, a, b, quantified)
                        == SYLVAN_OK);
            if (check_bdd_CALL(lace, computed, expected, variables)) return 1;
            mtbdd_refs_popptr(1);
        }

        /* Picking a cube must land inside the function. */
        if (truth_count(left) != 0) {
            BDD cube = mtbdd_invalid;
            BDD intersection = mtbdd_invalid;
            mtbdd_refs_pushptr(&cube);
            mtbdd_refs_pushptr(&intersection);
            test_assert(bdd_pick_cube_CALL(lace, &cube, a, variables)
                        == SYLVAN_OK);
            test_assert(cube != bdd_false);
            test_assert(mtbdd_is_valid_CALL(lace, cube));
            test_assert(bdd_and_CALL(lace, &intersection, cube, a)
                        == SYLVAN_OK);
            test_assert(intersection == cube);
            mtbdd_refs_popptr(2);
        }

        mtbdd_refs_popptr(3);
    }

    return 0;
}

/**
 * Iteration is driven by the variable set, so it enumerates in rank order.
 */
TASK(int, test_iteration, BDDSET, variables)
int
test_iteration_CALL(lace_worker *lace, BDDSET variables)
{
    truth table = {0, 0, 0, 0};
    for (int i = 0; i < 9; i++) {
        truth_set(table, test_random() % (1u << TEST_VARS));
    }

    BDD dd = mtbdd_invalid;
    mtbdd_refs_pushptr(&dd);
    test_assert(truth_to_bdd_CALL(lace, &dd, table, variables) == SYLVAN_OK);

    sylvan_iterator *iterator = NULL;
    test_assert(bdd_iterator_create(&iterator, dd, variables,
                                    SYLVAN_ITERATOR_MINTERMS) == SYLVAN_OK);
    truth seen = {0, 0, 0, 0};
    for (;;) {
        uint8_t values[TEST_VARS];
        int has_item = 0;
        test_assert(bdd_iterator_next(iterator, values, TEST_VARS, &has_item)
                    == SYLVAN_OK);
        if (!has_item) break;
        unsigned assignment = 0;
        for (int position = 0; position < TEST_VARS; position++) {
            test_assert(values[position] <= 1);
            assignment |= (unsigned)values[position] << test_order[position];
        }
        test_assert(truth_get(table, assignment));
        test_assert(!truth_get(seen, assignment));
        truth_set(seen, assignment);
    }
    sylvan_iterator_destroy(iterator);
    test_assert(truth_count(seen) == truth_count(table));

    mtbdd_refs_popptr(1);
    return 0;
}

/**
 * The remaining BDD operations, each against an independently computed
 * reference over the same truth tables.
 */
TASK(int, test_more_operations, BDDSET, variables)
int
test_more_operations_CALL(lace_worker *lace, BDDSET variables)
{
    /* Two variable subsets that are scattered through the rank order. */
    const uint32_t kept[3] = {test_order[1], test_order[3], test_order[6]};
    const uint32_t quantified[2] = {test_order[0], test_order[5]};
    const unsigned kept_mask =
        (1u << kept[0]) | (1u << kept[1]) | (1u << kept[2]);

    BDDSET keep_set = mtbdd_invalid;
    BDDSET quantify_set = mtbdd_invalid;
    mtbdd_refs_pushptr(&keep_set);
    mtbdd_refs_pushptr(&quantify_set);
    test_assert(bdd_set_from_array(&keep_set, kept, 3) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&quantify_set, quantified, 2) == SYLVAN_OK);

    for (int round = 0; round < 6; round++) {
        truth left;
        truth right;
        for (int i = 0; i < 4; i++) {
            left[i] = test_random();
            right[i] = test_random();
        }

        BDD a = mtbdd_invalid;
        BDD b = mtbdd_invalid;
        BDD computed = mtbdd_invalid;
        BDD other = mtbdd_invalid;
        mtbdd_refs_pushptr(&a);
        mtbdd_refs_pushptr(&b);
        mtbdd_refs_pushptr(&computed);
        mtbdd_refs_pushptr(&other);

        test_assert(truth_to_bdd_CALL(lace, &a, left, variables) == SYLVAN_OK);
        test_assert(truth_to_bdd_CALL(lace, &b, right, variables) == SYLVAN_OK);

        truth expected;

        /* constrain and simplify agree with <a> wherever <b> holds. */
        test_assert(bdd_constrain_CALL(lace, &computed, a, b) == SYLVAN_OK);
        test_assert(mtbdd_is_valid_CALL(lace, computed));
        for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
             assignment++) {
            if (!truth_get(right, assignment)) continue;
            uint8_t values[TEST_VARS];
            for (int position = 0; position < TEST_VARS; position++) {
                values[position] = (assignment >> test_order[position]) & 1;
            }
            BDD evaluated = mtbdd_invalid;
            test_assert(bdd_eval(&evaluated, computed, variables, values,
                                 TEST_VARS) == SYLVAN_OK);
            test_assert(evaluated == (truth_get(left, assignment)
                                      ? bdd_true : bdd_false));
        }

        test_assert(bdd_simplify_CALL(lace, &computed, a, b) == SYLVAN_OK);
        test_assert(mtbdd_is_valid_CALL(lace, computed));
        test_assert(mtbdd_node_count(computed) <= mtbdd_node_count(a));
        for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
             assignment++) {
            if (!truth_get(right, assignment)) continue;
            uint8_t values[TEST_VARS];
            for (int position = 0; position < TEST_VARS; position++) {
                values[position] = (assignment >> test_order[position]) & 1;
            }
            BDD evaluated = mtbdd_invalid;
            test_assert(bdd_eval(&evaluated, computed, variables, values,
                                 TEST_VARS) == SYLVAN_OK);
            test_assert(evaluated == (truth_get(left, assignment)
                                      ? bdd_true : bdd_false));
        }

        /* project keeps <kept> and abstracts everything else. */
        memset(expected, 0, sizeof(expected));
        for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
             assignment++) {
            if (!truth_get(left, assignment)) continue;
            for (unsigned other_bits = 0; other_bits < (1u << TEST_VARS);
                 other_bits++) {
                if ((other_bits & kept_mask) != (assignment & kept_mask)) {
                    continue;
                }
                truth_set(expected, other_bits);
            }
        }
        test_assert(bdd_project_CALL(lace, &computed, a, keep_set)
                    == SYLVAN_OK);
        if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

        /* and_project is project of the conjunction. */
        memset(expected, 0, sizeof(expected));
        for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
             assignment++) {
            if (!(truth_get(left, assignment) && truth_get(right, assignment))) {
                continue;
            }
            for (unsigned other_bits = 0; other_bits < (1u << TEST_VARS);
                 other_bits++) {
                if ((other_bits & kept_mask) != (assignment & kept_mask)) {
                    continue;
                }
                truth_set(expected, other_bits);
            }
        }
        test_assert(bdd_and_project_CALL(lace, &computed, a, b, keep_set)
                    == SYLVAN_OK);
        if (check_bdd_CALL(lace, computed, expected, variables)) return 1;

        /* apply_abstract fuses a Boolean operation with a quantifier. */
        {
            const bdd_apply_operator applies[3] =
                {BDD_APPLY_AND, BDD_APPLY_XOR, BDD_APPLY_OR};
            const bdd_abstract_operator abstracts[3] =
                {BDD_ABSTRACT_EXISTS, BDD_ABSTRACT_FORALL,
                 BDD_ABSTRACT_UNIQUE};
            for (int i = 0; i < 3; i++) {
                truth applied;
                for (int w = 0; w < 4; w++) {
                    switch (applies[i]) {
                    case BDD_APPLY_AND: applied[w] = left[w] & right[w]; break;
                    case BDD_APPLY_XOR: applied[w] = left[w] ^ right[w]; break;
                    default:            applied[w] = left[w] | right[w]; break;
                    }
                }
                memset(expected, 0, sizeof(expected));
                for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
                     assignment++) {
                    int hits = 0;
                    int all = 1;
                    for (int mask = 0; mask < 4; mask++) {
                        unsigned probe = assignment;
                        for (int q = 0; q < 2; q++) {
                            probe = (probe & ~(1u << quantified[q])) |
                                    (((unsigned)(mask >> q) & 1u)
                                     << quantified[q]);
                        }
                        if (truth_get(applied, probe)) hits++;
                        else all = 0;
                    }
                    const int holds =
                        abstracts[i] == BDD_ABSTRACT_EXISTS ? hits != 0 :
                        abstracts[i] == BDD_ABSTRACT_FORALL ? all :
                        (hits & 1);
                    if (holds) truth_set(expected, assignment);
                }
                test_assert(bdd_apply_abstract_CALL(
                    lace, &computed, a, b, quantify_set, applies[i],
                    abstracts[i]) == SYLVAN_OK);
                if (check_bdd_CALL(lace, computed, expected, variables)) {
                    return 1;
                }
            }
        }

        /* pick_representatives is a subset with the same abstraction. */
        test_assert(bdd_pick_representatives_CALL(
            lace, &computed, a, quantify_set) == SYLVAN_OK);
        test_assert(mtbdd_is_valid_CALL(lace, computed));
        test_assert(bdd_subseteq_CALL(lace, computed, a));
        test_assert(bdd_exists_CALL(lace, &other, computed, quantify_set)
                    == SYLVAN_OK);
        {
            BDD reference = mtbdd_invalid;
            mtbdd_refs_pushptr(&reference);
            test_assert(bdd_exists_CALL(lace, &reference, a, quantify_set)
                        == SYLVAN_OK);
            test_assert(other == reference);
            mtbdd_refs_popptr(1);
        }

        /* pick_minterm returns exactly one assignment inside <a>. */
        if (truth_count(left) != 0) {
            test_assert(bdd_pick_minterm_CALL(lace, &computed, a, variables)
                        == SYLVAN_OK);
            test_assert(mtbdd_is_valid_CALL(lace, computed));
            test_assert(bdd_subseteq_CALL(lace, computed, a));
            uint64_t satisfying = 0;
            test_assert(bdd_sat_count_u64_CALL(
                lace, &satisfying, computed, variables) == SYLVAN_OK);
            test_assert(satisfying == 1);
        }

        /* Relational predicates. */
        {
            int shared = 0;
            int contained = 1;
            for (int w = 0; w < 4; w++) {
                if (left[w] & right[w]) shared = 1;
                if (left[w] & ~right[w]) contained = 0;
            }
            test_assert(bdd_disjoint_CALL(lace, a, b) == (shared ? 0 : 1));
            test_assert(bdd_subseteq_CALL(lace, a, b) == (contained ? 1 : 0));

            test_assert(bdd_intersection_witness_CALL(lace, &computed, a, b)
                        == SYLVAN_OK);
            test_assert(mtbdd_is_valid_CALL(lace, computed));
            if (!shared) {
                test_assert(computed == bdd_false);
            } else {
                test_assert(computed != bdd_false);
                test_assert(bdd_and_CALL(lace, &other, a, b) == SYLVAN_OK);
                test_assert(bdd_subseteq_CALL(lace, computed, other));
            }
        }

        /* Counting and probability read the variable set in rank order. */
        {
            const double count = (double)truth_count(left);
            test_assert(bdd_sat_count_double_CALL(lace, a, variables)
                        == count);

            double probabilities[TEST_VARS];
            for (int i = 0; i < TEST_VARS; i++) probabilities[i] = 0.5;
            double probability = -1.0;
            test_assert(bdd_probability_CALL(
                lace, &probability, a, variables, probabilities, TEST_VARS)
                == SYLVAN_OK);
            test_assert(probability == count / 256.0);

            /* A one-hot vector selects a single assignment. */
            const unsigned selected = 0xa5;
            for (int position = 0; position < TEST_VARS; position++) {
                probabilities[position] =
                    ((selected >> test_order[position]) & 1) ? 1.0 : 0.0;
            }
            test_assert(bdd_probability_CALL(
                lace, &probability, a, variables, probabilities, TEST_VARS)
                == SYLVAN_OK);
            test_assert(probability ==
                        (truth_get(left, selected) ? 1.0 : 0.0));

            /* The batch path builds its own sorted level array. */
            double batch[2 * TEST_VARS];
            double results[2] = {-1.0, -1.0};
            for (int position = 0; position < TEST_VARS; position++) {
                batch[position] = 0.5;
                batch[TEST_VARS + position] =
                    ((selected >> test_order[position]) & 1) ? 1.0 : 0.0;
            }
            const bdd_probability_batch_input input = {
                batch, TEST_VARS, 2, TEST_VARS};
            test_assert(bdd_probability_batch_CALL(
                lace, results, a, variables, &input) == SYLVAN_OK);
            test_assert(results[0] == count / 256.0);
            test_assert(results[1] == (truth_get(left, selected) ? 1.0 : 0.0));

            /* The gradient path walks the same structure. */
            double gradient[TEST_VARS];
            for (int i = 0; i < TEST_VARS; i++) probabilities[i] = 0.5;
            test_assert(bdd_probability_gradient_CALL(
                lace, &probability, gradient, a, variables, probabilities,
                TEST_VARS) == SYLVAN_OK);
            test_assert(probability == count / 256.0);
            for (int position = 0; position < TEST_VARS; position++) {
                const uint32_t identity = test_order[position];
                size_t high = 0;
                size_t low = 0;
                for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
                     assignment++) {
                    if (!truth_get(left, assignment)) continue;
                    if ((assignment >> identity) & 1) high++;
                    else low++;
                }
                const double derivative =
                    ((double)high - (double)low) / 128.0;
                test_assert(gradient[position] == derivative);
            }
        }

        mtbdd_refs_popptr(4);
    }

    /* Cardinality constraints are built straight from the variable set. */
    for (size_t minimum = 0; minimum <= 3; minimum++) {
        for (size_t maximum = minimum; maximum <= minimum + 2; maximum++) {
            truth expected = {0, 0, 0, 0};
            for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
                 assignment++) {
                const size_t ones = popcnt_uint64(assignment);
                if (ones >= minimum && ones <= maximum) {
                    truth_set(expected, assignment);
                }
            }
            BDD computed = mtbdd_invalid;
            mtbdd_refs_pushptr(&computed);
            test_assert(bdd_cardinality_CALL(
                lace, &computed, variables, minimum, maximum) == SYLVAN_OK);
            if (check_bdd_CALL(lace, computed, expected, variables)) return 1;
            mtbdd_refs_popptr(1);
        }
    }

    /* A cube with don't-cares, written in rank order. */
    {
        uint8_t cube[TEST_VARS] = {0, 1, 2, 1, 0, 2, 1, 0};
        truth expected = {0, 0, 0, 0};
        for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
             assignment++) {
            int matches = 1;
            for (int position = 0; position < TEST_VARS; position++) {
                if (cube[position] == 2) continue;
                if (((assignment >> test_order[position]) & 1)
                    != cube[position]) {
                    matches = 0;
                    break;
                }
            }
            if (matches) truth_set(expected, assignment);
        }
        BDD computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&computed);
        test_assert(bdd_cube_CALL(lace, &computed, variables, cube)
                    == SYLVAN_OK);
        if (check_bdd_CALL(lace, computed, expected, variables)) return 1;
        mtbdd_refs_popptr(1);
    }

    mtbdd_refs_popptr(2);
    return 0;
}

/**
 * Multi-terminal operations use the same top-variable selection.
 */
TASK(int, test_mtbdd_operations, BDDSET, variables)
int
test_mtbdd_operations_CALL(lace_worker *lace, BDDSET variables)
{
    const uint32_t summed[2] = {test_order[2], test_order[7]};
    BDDSET sum_set = mtbdd_invalid;
    mtbdd_refs_pushptr(&sum_set);
    test_assert(bdd_set_from_array(&sum_set, summed, 2) == SYLVAN_OK);

    for (int round = 0; round < 4; round++) {
        truth left;
        truth right;
        for (int i = 0; i < 4; i++) {
            left[i] = test_random();
            right[i] = test_random();
        }

        BDD a = mtbdd_invalid;
        BDD b = mtbdd_invalid;
        MTBDD f = mtbdd_invalid;
        MTBDD g = mtbdd_invalid;
        MTBDD computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&a);
        mtbdd_refs_pushptr(&b);
        mtbdd_refs_pushptr(&f);
        mtbdd_refs_pushptr(&g);
        mtbdd_refs_pushptr(&computed);

        test_assert(truth_to_bdd_CALL(lace, &a, left, variables) == SYLVAN_OK);
        test_assert(truth_to_bdd_CALL(lace, &b, right, variables) == SYLVAN_OK);

        /* f is 3 where <a> holds and 0 elsewhere; g is 5 or 0. */
        test_assert(mtbdd_ite_CALL(lace, &f, a, mtbdd_int64(3), mtbdd_int64(0))
                    == SYLVAN_OK);
        test_assert(mtbdd_ite_CALL(lace, &g, b, mtbdd_int64(5), mtbdd_int64(0))
                    == SYLVAN_OK);
        test_assert(mtbdd_is_valid_CALL(lace, f));
        test_assert(mtbdd_is_valid_CALL(lace, g));

        test_assert(mtbdd_apply_CALL(lace, &computed, f, g,
                                     mtbdd_op_plus_CALL) == SYLVAN_OK);
        test_assert(mtbdd_is_valid_CALL(lace, computed));
        for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
             assignment++) {
            uint8_t values[TEST_VARS];
            for (int position = 0; position < TEST_VARS; position++) {
                values[position] = (assignment >> test_order[position]) & 1;
            }
            MTBDD leaf = mtbdd_invalid;
            test_assert(mtbdd_eval(&leaf, computed, variables, values,
                                   TEST_VARS) == SYLVAN_OK);
            const int64_t expected =
                (truth_get(left, assignment) ? 3 : 0) +
                (truth_get(right, assignment) ? 5 : 0);
            test_assert(mtbdd_leaf_int64(leaf) == expected);
        }

        /* Summing out two variables that are far apart in rank. */
        test_assert(mtbdd_abstract_add_CALL(lace, &computed, f, sum_set)
                    == SYLVAN_OK);
        test_assert(mtbdd_is_valid_CALL(lace, computed));
        for (unsigned assignment = 0; assignment < (1u << TEST_VARS);
             assignment++) {
            uint8_t values[TEST_VARS];
            for (int position = 0; position < TEST_VARS; position++) {
                values[position] = (assignment >> test_order[position]) & 1;
            }
            MTBDD leaf = mtbdd_invalid;
            test_assert(mtbdd_eval(&leaf, computed, variables, values,
                                   TEST_VARS) == SYLVAN_OK);
            int64_t expected = 0;
            for (int mask = 0; mask < 4; mask++) {
                unsigned probe = assignment;
                for (int q = 0; q < 2; q++) {
                    probe = (probe & ~(1u << summed[q])) |
                            (((unsigned)(mask >> q) & 1u) << summed[q]);
                }
                if (truth_get(left, probe)) expected += 3;
            }
            test_assert(mtbdd_leaf_int64(leaf) == expected);
        }

        /* mtbdd_sat_count counts the non-zero assignments of f. */
        uint64_t nonzero = 0;
        test_assert(mtbdd_sat_count_u64_CALL(lace, &nonzero, f, variables)
                    == SYLVAN_OK);
        test_assert(nonzero == truth_count(left));

        mtbdd_refs_popptr(5);
    }

    mtbdd_refs_popptr(1);
    return 0;
}

/**
 * Relational operations over four interleaved s/t pairs.
 *
 * The pairs keep their identity arithmetic (t == s|1) and are rank-adjacent
 * with s first, which is exactly the precondition these operations document.
 * The pairs themselves are in a scrambled order, and they sit below the
 * Boolean domain, so identity order and rank order still disagree.
 */
#define REL_STATES (1u << REL_PAIRS)

static uint32_t
rel_state_identity(int bit, int next)
{
    return (uint32_t)(REL_BASE + 2 * bit + (next ? 1 : 0));
}

/** Write the cube of state <value> over the current (or next) variables. */
static void
rel_cube(uint8_t *cube, unsigned value, int next, int constrain)
{
    for (int position = 0; position < 2 * REL_PAIRS; position++) {
        const uint32_t identity = test_order[REL_BASE + position];
        const int bit = (int)(identity - REL_BASE) / 2;
        const int is_next = (identity - REL_BASE) % 2;
        if (!constrain || is_next != (next ? 1 : 0)) {
            cube[position] = 2;
        } else {
            cube[position] = (value >> bit) & 1;
        }
    }
}

TASK(int, rel_states_to_bdd, BDD*, result, const uint16_t*, states, int, next,
     BDDSET, variables)
int
rel_states_to_bdd_CALL(lace_worker *lace, BDD *result, const uint16_t *states,
                       int next, BDDSET variables)
{
    BDD computed = bdd_false;
    mtbdd_refs_pushptr(&computed);
    for (unsigned value = 0; value < REL_STATES; value++) {
        if (!((*states >> value) & 1)) continue;
        uint8_t cube[2 * REL_PAIRS];
        rel_cube(cube, value, next, 1);
        const int status = bdd_or_cube_CALL(
            lace, &computed, computed, variables, cube);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(1);
            return status;
        }
    }
    *result = computed;
    mtbdd_refs_popptr(1);
    return SYLVAN_OK;
}

TASK(int, test_relational)
int
test_relational_CALL(lace_worker *lace)
{
    uint32_t identities[2 * REL_PAIRS];
    for (int i = 0; i < 2 * REL_PAIRS; i++) {
        identities[i] = (uint32_t)(REL_BASE + i);
    }
    BDDSET variables = mtbdd_invalid;
    mtbdd_refs_pushptr(&variables);
    test_assert(bdd_set_from_array(&variables, identities, 2 * REL_PAIRS)
                == SYLVAN_OK);

    /* The pairs must be rank-adjacent with the current-state variable on
       top, otherwise these operations have no defined behaviour. */
    for (int bit = 0; bit < REL_PAIRS; bit++) {
        const uint32_t current = rel_state_identity(bit, 0);
        const uint32_t next = rel_state_identity(bit, 1);
        test_assert(sylvan_rank_of(next) == sylvan_rank_of(current) + 1);
    }

    for (int round = 0; round < 6; round++) {
        uint16_t source = (uint16_t)test_random();
        uint16_t relation[REL_STATES];
        for (unsigned value = 0; value < REL_STATES; value++) {
            relation[value] = (uint16_t)test_random();
        }

        BDD set = mtbdd_invalid;
        BDD rel = mtbdd_invalid;
        BDD computed = mtbdd_invalid;
        BDD expected = mtbdd_invalid;
        mtbdd_refs_pushptr(&set);
        mtbdd_refs_pushptr(&rel);
        mtbdd_refs_pushptr(&computed);
        mtbdd_refs_pushptr(&expected);

        test_assert(rel_states_to_bdd_CALL(lace, &set, &source, 0, variables)
                    == SYLVAN_OK);

        /* Build the relation as a disjunction of (current, next) pairs. */
        rel = bdd_false;
        for (unsigned from = 0; from < REL_STATES; from++) {
            for (unsigned to = 0; to < REL_STATES; to++) {
                if (!((relation[from] >> to) & 1)) continue;
                uint8_t cube[2 * REL_PAIRS];
                for (int position = 0; position < 2 * REL_PAIRS; position++) {
                    const uint32_t identity = test_order[REL_BASE + position];
                    const int bit = (int)(identity - REL_BASE) / 2;
                    const int is_next = (identity - REL_BASE) % 2;
                    cube[position] = is_next ? ((to >> bit) & 1)
                                             : ((from >> bit) & 1);
                }
                test_assert(bdd_or_cube_CALL(lace, &rel, rel, variables, cube)
                            == SYLVAN_OK);
            }
        }
        test_assert(mtbdd_is_valid_CALL(lace, set));
        test_assert(mtbdd_is_valid_CALL(lace, rel));

        /* Image: the successors of the source set. */
        uint16_t image = 0;
        for (unsigned from = 0; from < REL_STATES; from++) {
            if (!((source >> from) & 1)) continue;
            image = (uint16_t)(image | relation[from]);
        }
        test_assert(bdd_rel_next_CALL(lace, &computed, set, rel, variables)
                    == SYLVAN_OK);
        test_assert(mtbdd_is_valid_CALL(lace, computed));
        test_assert(rel_states_to_bdd_CALL(lace, &expected, &image, 0,
                                           variables) == SYLVAN_OK);
        test_assert(computed == expected);

        /* Preimage: the predecessors of the source set. */
        uint16_t preimage = 0;
        for (unsigned from = 0; from < REL_STATES; from++) {
            if (relation[from] & source) {
                preimage = (uint16_t)(preimage | (1u << from));
            }
        }
        test_assert(bdd_rel_prev_CALL(lace, &computed, rel, set, variables)
                    == SYLVAN_OK);
        test_assert(mtbdd_is_valid_CALL(lace, computed));
        test_assert(rel_states_to_bdd_CALL(lace, &expected, &preimage, 0,
                                           variables) == SYLVAN_OK);
        test_assert(computed == expected);

        /* Transitive closure of the relation. */
        uint16_t closure[REL_STATES];
        for (unsigned from = 0; from < REL_STATES; from++) {
            closure[from] = relation[from];
        }
        for (unsigned middle = 0; middle < REL_STATES; middle++) {
            for (unsigned from = 0; from < REL_STATES; from++) {
                if ((closure[from] >> middle) & 1) {
                    closure[from] = (uint16_t)(closure[from] | closure[middle]);
                }
            }
        }
        test_assert(bdd_transitive_closure_CALL(lace, &computed, rel)
                    == SYLVAN_OK);
        test_assert(mtbdd_is_valid_CALL(lace, computed));
        expected = bdd_false;
        for (unsigned from = 0; from < REL_STATES; from++) {
            for (unsigned to = 0; to < REL_STATES; to++) {
                if (!((closure[from] >> to) & 1)) continue;
                uint8_t cube[2 * REL_PAIRS];
                for (int position = 0; position < 2 * REL_PAIRS; position++) {
                    const uint32_t identity = test_order[REL_BASE + position];
                    const int bit = (int)(identity - REL_BASE) / 2;
                    const int is_next = (identity - REL_BASE) % 2;
                    cube[position] = is_next ? ((to >> bit) & 1)
                                             : ((from >> bit) & 1);
                }
                test_assert(bdd_or_cube_CALL(lace, &expected, expected,
                                             variables, cube) == SYLVAN_OK);
            }
        }
        test_assert(computed == expected);

        mtbdd_refs_popptr(4);
    }

    mtbdd_refs_popptr(1);
    return 0;
}

TASK(int, runtests)
int
runtests_CALL(lace_worker *lace)
{
    for (uint32_t rank = 0; rank < ALL_VARS; rank++) {
        test_rank[test_order[rank]] = rank;
    }

    /* Register the identities and permute the order before any node exists. */
    test_assert(sylvan_ranks_reserve(ALL_VARS) == SYLVAN_OK);

    /* sylvan_ranks_is_identity() answers from the current order and not from
       whether anything was ever permuted, so an order permuted back to the
       identity is the identity again. */
    uint32_t straight[ALL_VARS];
    for (uint32_t rank = 0; rank < ALL_VARS; rank++) straight[rank] = rank;
    test_assert(sylvan_ranks_is_identity());
    test_assert(sylvan_ranks_permute_unsafe(straight, ALL_VARS) == SYLVAN_OK);
    test_assert(sylvan_ranks_is_identity());
    test_assert(sylvan_ranks_permute_unsafe(test_order, ALL_VARS) == SYLVAN_OK);
    test_assert(!sylvan_ranks_is_identity());
    test_assert(sylvan_ranks_permute_unsafe(straight, ALL_VARS) == SYLVAN_OK);
    test_assert(sylvan_ranks_is_identity());

    test_assert(sylvan_ranks_permute_unsafe(test_order, ALL_VARS)
                == SYLVAN_OK);

    for (uint32_t identity = 0; identity < ALL_VARS; identity++) {
        test_variable[identity] = mtbdd_invalid;
        mtbdd_protect(&test_variable[identity]);
        test_assert(bdd_var_at_level(&test_variable[identity],
                                     test_rank[identity]) == SYLVAN_OK);
        test_assert(mtbdd_node_variable(test_variable[identity]) == identity);
    }

    BDDSET variables = mtbdd_invalid;
    mtbdd_protect(&variables);
    uint32_t identities[TEST_VARS];
    for (uint32_t i = 0; i < TEST_VARS; i++) identities[i] = i;
    test_assert(bdd_set_from_array(&variables, identities, TEST_VARS)
                == SYLVAN_OK);

    printf("Testing the permutation hook.\n");
    if (test_permutation_CALL(lace)) return 1;
    printf("Testing bdd_var_at_level.\n");
    if (test_var_at_level_CALL(lace)) return 1;
    printf("Testing node construction.\n");
    if (test_make_node_CALL(lace)) return 1;
    printf("Testing variable sets.\n");
    if (test_variable_sets_CALL(lace)) return 1;
    printf("Testing support.\n");
    if (test_support_CALL(lace)) return 1;
    printf("Testing maps.\n");
    if (test_maps_CALL(lace)) return 1;
    printf("Testing operations.\n");
    if (test_operations_CALL(lace, variables)) return 1;
    printf("Testing iteration.\n");
    if (test_iteration_CALL(lace, variables)) return 1;

    mtbdd_unprotect(&variables);
    for (uint32_t identity = 0; identity < ALL_VARS; identity++) {
        mtbdd_unprotect(&test_variable[identity]);
    }
    return 0;
}

int
main(void)
{
    lace_start(4, 0, 0);

    sylvan_set_sizes(1LL << 20, 1LL << 22, 1LL << 16, 1LL << 18);
    sylvan_init_package();
    mtbdd_init();

    const int result = runtests();

    sylvan_quit();
    lace_stop();
    return result;
}
