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
 * The adjacent variable swap.
 *
 * What every test here checks, one way or another, is that a swap changes the
 * variable order and nothing else. The oracles are:
 *
 * - the full truth table of every root, sampled over all 2^8 assignments
 *   *indexed by variable identity*, which is invariant under reordering;
 * - the root handles themselves, which a swap must never change, because it
 *   rewrites nodes in place and every parent edge in the forest keeps
 *   pointing at the same index;
 * - mtbdd_is_valid, which checks that a node's children have a strictly
 *   larger rank, and therefore fails immediately if the swap left a node out
 *   of order;
 * - canonicity, checked by rebuilding a function from its truth table under
 *   the new order and comparing handles: an equal handle means the swap
 *   produced exactly the nodes the unique table would have produced anyway,
 *   which is a stronger statement than an equal node count.
 *
 * Usage: test_varswap [workers] [stress rounds]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lace.h>
#include <sylvan/internal.h>

#include "test_assert.h"

#define TEST_VARS 8
#define TEST_ASSIGNMENTS (1u << TEST_VARS)
#define TEST_ROOTS 8

/* Swaps per stress round, and the default number of rounds. */
#define TEST_STRESS_SWAPS 64
static int test_stress_rounds = 5;

static BDDSET test_variables;
static BDD test_var[TEST_VARS];
static MTBDD test_root[TEST_ROOTS];
static MTBDD test_value[TEST_ROOTS][TEST_ASSIGNMENTS];

/** Deterministic small PRNG, so a failure is reproducible. */
static uint64_t test_random_state = UINT64_C(0x243f6a8885a308d3);

static uint64_t
test_random(void)
{
    test_random_state ^= test_random_state << 13;
    test_random_state ^= test_random_state >> 7;
    test_random_state ^= test_random_state << 17;
    return test_random_state;
}

/**
 * Turn an assignment, whose bit <i> is the value of variable *identity* <i>,
 * into the value array evaluation wants, which is indexed by position in the
 * current variable order.
 */
static void
test_values_for(unsigned assignment, uint8_t *values)
{
    uint32_t order[TEST_VARS];
    bdd_set_to_array(test_variables, order);
    for (int i = 0; i < TEST_VARS; i++) {
        values[i] = (uint8_t)((assignment >> order[i]) & 1u);
    }
}

/** Record the value of every root at every assignment. */
static int
test_record(void)
{
    for (int r = 0; r < TEST_ROOTS; r++) {
        for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
            uint8_t values[TEST_VARS];
            test_values_for(a, values);
            test_assert(mtbdd_eval(&test_value[r][a], test_root[r],
                                   test_variables, values, TEST_VARS)
                        == SYLVAN_OK);
        }
    }
    return 0;
}

/** Every root still denotes the same function and is still well formed. */
static int
test_check_roots_CALL(lace_worker *lace)
{
    for (int r = 0; r < TEST_ROOTS; r++) {
        test_assert(mtbdd_is_valid_CALL(lace, test_root[r]));
        for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
            uint8_t values[TEST_VARS];
            MTBDD evaluated = mtbdd_invalid;
            test_values_for(a, values);
            test_assert(mtbdd_eval(&evaluated, test_root[r], test_variables,
                                   values, TEST_VARS) == SYLVAN_OK);
            test_assert(evaluated == test_value[r][a]);
        }
    }
    return 0;
}

/**
 * Rebuild a Boolean root from its recorded truth table under the order that
 * holds right now, and check that it comes out as the very same handle. This
 * is the exactness oracle: the swap must leave the canonical representation.
 */
static int
test_check_canonical_CALL(lace_worker *lace, int root)
{
    BDD rebuilt = bdd_false;
    mtbdd_protect(&rebuilt);

    int failed = 0;
    for (unsigned a = 0; a < TEST_ASSIGNMENTS && !failed; a++) {
        if (test_value[root][a] != bdd_true) continue;
        uint8_t values[TEST_VARS];
        test_values_for(a, values);
        if (bdd_or_cube_CALL(lace, &rebuilt, rebuilt, test_variables, values)
            != SYLVAN_OK) {
            failed = 1;
        }
    }
    const BDD result = rebuilt;
    mtbdd_unprotect(&rebuilt);

    test_assert(!failed);
    test_assert(result == test_root[root]);
    return 0;
}

/**
 * Guards. A backend without the reorder primitives, an unregistered rank, and
 * a session that is not open are all refused rather than half-attempted.
 */
static int
test_guards(void)
{
    sylvan_varswap_result result;

    /* One past the last usable rank: there is no rank TEST_VARS-1 plus one. */
    test_assert(sylvan_test_varswap(TEST_VARS - 1, &result)
                == SYLVAN_ERR_INVALID);
    test_assert(sylvan_test_varswap(1u << 20, &result) == SYLVAN_ERR_INVALID);

    /* sylvan_varswap outside a session does nothing. */
    test_assert(sylvan_varswap(0, &result) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_reorder_end() == SYLVAN_ERR_INVALID);
    return 0;
}

/**
 * Two variables that do not interact cost nothing but the metadata: no node
 * is rewritten, none is created, none dies.
 */
static int
test_non_interacting_CALL(lace_worker *lace)
{
    BDD skipping = mtbdd_invalid;
    mtbdd_protect(&skipping);
    test_assert(bdd_and_CALL(lace, &skipping, test_var[0], test_var[2])
                == SYLVAN_OK);

    const uint32_t before_upper = sylvan_identity_at(0);
    const uint32_t before_lower = sylvan_identity_at(1);
    const BDD handle = skipping;

    sylvan_varswap_result result;
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_OK);

    test_assert(result.rewritten == 0);
    test_assert(result.map_rewritten == 0);
    test_assert(result.created == 0);
    test_assert(result.died == 0);
    test_assert(result.delta == 0);
    test_assert(skipping == handle);
    test_assert(sylvan_identity_at(0) == before_lower);
    test_assert(sylvan_identity_at(1) == before_upper);
    test_assert(mtbdd_is_valid_CALL(lace, skipping));

    /* Put the order back. */
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_OK);
    test_assert(result.rewritten == 0);
    test_assert(sylvan_identity_at(0) == before_upper);

    mtbdd_unprotect(&skipping);
    return 0;
}

/** Look a key up in a map by walking its spine. */
static MTBDD
test_map_lookup(MTBDDMAP map, uint32_t key)
{
    while (!mtbdd_map_is_empty(map)) {
        if (mtbdd_map_key(map) == key) return mtbdd_map_value(map);
        map = mtbdd_map_next(map);
    }
    return mtbdd_invalid;
}

/**
 * MTBDDMAP nodes.
 *
 * A map spine is ordered by the ranks of its keys, so a swap has to fix
 * exactly one thing: a spine node keyed by the upper variable whose next node
 * is keyed by the lower one. A spine that holds only one of the two keys is
 * still correctly ordered afterwards and must cost nothing.
 *
 * This runs before the big roots exist, so that the counters in the result
 * describe the maps and nothing else.
 */
static int
test_maps_CALL(lace_worker *lace)
{
    const uint32_t upper = sylvan_identity_at(3);
    const uint32_t lower = sylvan_identity_at(4);
    const uint32_t above = sylvan_identity_at(1);
    const uint32_t below = sylvan_identity_at(6);

    MTBDDMAP both = mtbdd_map_empty();
    MTBDDMAP only_upper = mtbdd_map_empty();
    MTBDDMAP only_lower = mtbdd_map_empty();
    mtbdd_protect(&both);
    mtbdd_protect(&only_upper);
    mtbdd_protect(&only_lower);

    /* A spine with a neighbour on both sides, so that the rewrite has to keep
       the rest of the chain attached. Every value differs, so that the three
       spines share no node and the counters below are unambiguous. */
    test_assert(mtbdd_map_set(&both, both, below, test_var[7]) == SYLVAN_OK);
    test_assert(mtbdd_map_set(&both, both, lower, test_var[5]) == SYLVAN_OK);
    test_assert(mtbdd_map_set(&both, both, upper, test_var[6]) == SYLVAN_OK);
    test_assert(mtbdd_map_set(&both, both, above, test_var[2]) == SYLVAN_OK);

    test_assert(mtbdd_map_set(&only_upper, only_upper, below, test_var[1])
                == SYLVAN_OK);
    test_assert(mtbdd_map_set(&only_upper, only_upper, upper, test_var[0])
                == SYLVAN_OK);
    test_assert(mtbdd_map_set(&only_lower, only_lower, below, test_var[4])
                == SYLVAN_OK);
    test_assert(mtbdd_map_set(&only_lower, only_lower, lower, test_var[3])
                == SYLVAN_OK);

    const MTBDDMAP both_handle = both;
    const MTBDDMAP only_upper_handle = only_upper;
    const MTBDDMAP only_lower_handle = only_lower;

    sylvan_varswap_result result;
    test_assert(sylvan_test_varswap(3, &result) == SYLVAN_OK);

    /* Exactly one spine node is keyed by the upper variable and followed by
       the lower one: the one in <both>. */
    test_assert(result.map_rewritten == 1);
    test_assert(result.rewritten == 0);
    test_assert(result.created == 1);
    /* The inner node dies, and the death theorem says nothing below it does. */
    test_assert(result.died == 1);
    test_assert(result.deep_deaths == 0);
    test_assert(result.delta == 0);

    /* Handles are stable and every key still maps to the same value. */
    test_assert(both == both_handle);
    test_assert(only_upper == only_upper_handle);
    test_assert(only_lower == only_lower_handle);

    test_assert(mtbdd_map_count(both) == 4);
    test_assert(test_map_lookup(both, above) == test_var[2]);
    test_assert(test_map_lookup(both, upper) == test_var[6]);
    test_assert(test_map_lookup(both, lower) == test_var[5]);
    test_assert(test_map_lookup(both, below) == test_var[7]);
    test_assert(mtbdd_map_contains(both, upper));
    test_assert(mtbdd_map_contains(both, lower));

    test_assert(test_map_lookup(only_upper, upper) == test_var[0]);
    test_assert(test_map_lookup(only_upper, below) == test_var[1]);
    test_assert(test_map_lookup(only_lower, lower) == test_var[3]);
    test_assert(test_map_lookup(only_lower, below) == test_var[4]);

    /* The spine now really is ordered by the new ranks. */
    test_assert(mtbdd_map_key(mtbdd_map_next(both)) == lower);
    test_assert(mtbdd_map_key(mtbdd_map_next(mtbdd_map_next(both))) == upper);

    /* Swapping back restores everything, and rebuilding the map from scratch
       under the new order produces the very same handle. */
    test_assert(sylvan_test_varswap(3, &result) == SYLVAN_OK);
    test_assert(result.map_rewritten == 1);
    test_assert(both == both_handle);
    test_assert(mtbdd_map_key(mtbdd_map_next(both)) == upper);

    MTBDDMAP rebuilt = mtbdd_map_empty();
    mtbdd_protect(&rebuilt);
    test_assert(mtbdd_map_set(&rebuilt, rebuilt, below, test_var[7])
                == SYLVAN_OK);
    test_assert(mtbdd_map_set(&rebuilt, rebuilt, upper, test_var[6])
                == SYLVAN_OK);
    test_assert(mtbdd_map_set(&rebuilt, rebuilt, lower, test_var[5])
                == SYLVAN_OK);
    test_assert(mtbdd_map_set(&rebuilt, rebuilt, above, test_var[2])
                == SYLVAN_OK);
    test_assert(rebuilt == both);
    mtbdd_unprotect(&rebuilt);

    /* A swap of two ranks the maps do not both mention costs nothing. */
    test_assert(sylvan_test_varswap(5, &result) == SYLVAN_OK);
    test_assert(result.map_rewritten == 0);
    test_assert(result.created == 0);
    test_assert(result.died == 0);
    test_assert(sylvan_test_varswap(5, &result) == SYLVAN_OK);
    test_assert(result.map_rewritten == 0);

    mtbdd_unprotect(&both);
    mtbdd_unprotect(&only_upper);
    mtbdd_unprotect(&only_lower);
    (void)lace;
    return 0;
}

/** Build the roots the rest of the tests work on. */
static int
test_build_roots_CALL(lace_worker *lace)
{
    /* Parity: every adjacent pair of variables interacts. */
    BDD parity = bdd_false;
    mtbdd_protect(&parity);
    for (int i = 0; i < TEST_VARS; i++) {
        test_assert(bdd_xor_CALL(lace, &parity, parity, test_var[i])
                    == SYLVAN_OK);
    }
    test_root[0] = parity;
    mtbdd_unprotect(&parity);

    /* Pairwise conjunctions, which interact only inside a pair. */
    BDD pairs = bdd_false;
    BDD conjunction = bdd_false;
    mtbdd_protect(&pairs);
    mtbdd_protect(&conjunction);
    for (int i = 0; i + 1 < TEST_VARS; i += 2) {
        test_assert(bdd_and_CALL(lace, &conjunction,
                                 test_var[i], test_var[i + 1]) == SYLVAN_OK);
        test_assert(bdd_or_CALL(lace, &pairs, pairs, conjunction)
                    == SYLVAN_OK);
    }
    test_root[1] = pairs;
    mtbdd_unprotect(&conjunction);
    mtbdd_unprotect(&pairs);

    /* Only the even variables, so some adjacent pairs never interact. */
    BDD evens = bdd_true;
    mtbdd_protect(&evens);
    for (int i = 0; i < TEST_VARS; i += 2) {
        test_assert(bdd_and_CALL(lace, &evens, evens, test_var[i])
                    == SYLVAN_OK);
    }
    test_root[2] = evens;
    mtbdd_unprotect(&evens);

    /* Shares subgraphs with all three of the above. */
    BDD mixed = bdd_false;
    mtbdd_protect(&mixed);
    test_assert(bdd_ite_CALL(lace, &mixed, test_root[0],
                             test_root[1], test_root[2]) == SYLVAN_OK);
    test_root[3] = mixed;
    mtbdd_unprotect(&mixed);

    /* A complemented handle onto a node another root points at directly. */
    test_root[4] = bdd_not(test_root[1]);

    /* A genuine multi-terminal diagram over the same variables. */
    MTBDD inner = mtbdd_invalid;
    MTBDD outer = mtbdd_invalid;
    mtbdd_protect(&inner);
    mtbdd_protect(&outer);
    test_assert(mtbdd_ite_CALL(lace, &inner, test_root[2],
                               mtbdd_int64(-3), mtbdd_int64(11)) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &outer, test_root[1],
                               mtbdd_int64(7), inner) == SYLVAN_OK);
    test_root[5] = outer;
    mtbdd_unprotect(&outer);
    mtbdd_unprotect(&inner);

    /* Two handles onto nodes deep inside the other roots. A swap rewrites
       nodes in place, so these must keep denoting the same function even
       though the node they name has different children afterwards. */
    test_root[6] = mtbdd_node_high(test_root[0]);
    test_root[7] = mtbdd_node_low(test_root[3]);

    for (int r = 0; r < TEST_ROOTS; r++) {
        test_assert(mtbdd_is_valid_CALL(lace, test_root[r]));
    }
    return 0;
}

/** One swap at every rank, with the full battery after each. */
static int
test_every_rank_CALL(lace_worker *lace)
{
    for (uint32_t rank = 0; rank + 1 < TEST_VARS; rank++) {
        const uint32_t upper = sylvan_identity_at(rank);
        const uint32_t lower = sylvan_identity_at(rank + 1);
        MTBDD handle[TEST_ROOTS];
        for (int r = 0; r < TEST_ROOTS; r++) handle[r] = test_root[r];

        sylvan_varswap_result result;
        test_assert(sylvan_test_varswap(rank, &result) == SYLVAN_OK);
        test_assert(result.deep_deaths == 0);

        test_assert(sylvan_identity_at(rank) == lower);
        test_assert(sylvan_identity_at(rank + 1) == upper);
        test_assert(sylvan_rank_of(lower) == rank);
        test_assert(sylvan_rank_of(upper) == rank + 1);
        for (int r = 0; r < TEST_ROOTS; r++) {
            test_assert(test_root[r] == handle[r]);
        }
        if (test_check_roots_CALL(lace)) return 1;

        /* And back, so that every rank is tested from the same order. */
        test_assert(sylvan_test_varswap(rank, &result) == SYLVAN_OK);
        test_assert(result.deep_deaths == 0);
        test_assert(sylvan_identity_at(rank) == upper);
        for (int r = 0; r < TEST_ROOTS; r++) {
            test_assert(test_root[r] == handle[r]);
        }
        if (test_check_roots_CALL(lace)) return 1;
    }
    return 0;
}

/**
 * Swapping the same pair twice is the identity: the same functions, the same
 * root handles, and the same number of nodes reachable from the roots.
 *
 * The *slots* need not be the same. A node that lost its last reference in
 * the first swap is released, and the second swap interns its replacement
 * wherever the allocator happens to hand out a slot.
 */
static int
test_double_swap_CALL(lace_worker *lace)
{
    for (uint32_t rank = 0; rank + 1 < TEST_VARS; rank++) {
        MTBDD handle[TEST_ROOTS];
        for (int r = 0; r < TEST_ROOTS; r++) handle[r] = test_root[r];
        const size_t before = mtbdd_shared_node_count(test_root, TEST_ROOTS);

        sylvan_varswap_result first, second;
        test_assert(sylvan_test_varswap(rank, &first) == SYLVAN_OK);
        test_assert(sylvan_test_varswap(rank, &second) == SYLVAN_OK);

        test_assert(mtbdd_shared_node_count(test_root, TEST_ROOTS) == before);
        test_assert(first.deep_deaths == 0 && second.deep_deaths == 0);
        for (int r = 0; r < TEST_ROOTS; r++) {
            test_assert(test_root[r] == handle[r]);
        }
        /* What the first swap built the second one took apart again. */
        test_assert(first.delta + second.delta == 0);
        if (test_check_roots_CALL(lace)) return 1;
    }
    return 0;
}

/**
 * Opening a session collects, and that is what makes <delta> a statement
 * about the forest rather than about the table.
 *
 * The same swap is measured twice from the same starting order: once on a
 * table that holds only the forest, and once on a table that also holds a
 * pile of garbage. Pre-session garbage is invisible to a session -- the
 * collection removed it before the reference counts were seeded -- so both
 * runs have to report the same numbers. Each session must also collect
 * exactly once: it is a precondition of the session, not a convention its
 * caller is trusted to follow, and not something a session does twice.
 */
static int
test_begin_collects_CALL(lace_worker *lace)
{
    sylvan_varswap_result clean, back, dirty, dirty_back;

    uint64_t collections = sylvan_gc_count();
    test_assert(sylvan_test_varswap(0, &clean) == SYLVAN_OK);
    test_assert(sylvan_gc_count() == collections + 1);
    test_assert(sylvan_test_varswap(0, &back) == SYLVAN_OK);
    test_assert(clean.delta + back.delta == 0);

    /* Garbage: a diagram built and then dropped. Nothing refers to it, so the
       session's collection has to remove it before it counts anything. */
    size_t before = 0, after = 0, total = 0;
    sylvan_table_usage(&before, &total);
    BDD junk = bdd_false;
    mtbdd_protect(&junk);
    for (int i = 0; i < 64; i++) {
        uint8_t values[TEST_VARS];
        test_values_for((unsigned)(test_random() % TEST_ASSIGNMENTS), values);
        test_assert(bdd_or_cube_CALL(lace, &junk, junk, test_variables, values)
                    == SYLVAN_OK);
    }
    junk = bdd_false;
    mtbdd_unprotect(&junk);
    sylvan_table_usage(&after, &total);
    test_assert(after > before);

    collections = sylvan_gc_count();
    test_assert(sylvan_test_varswap(0, &dirty) == SYLVAN_OK);
    test_assert(sylvan_gc_count() == collections + 1);
    test_assert(sylvan_test_varswap(0, &dirty_back) == SYLVAN_OK);

    test_assert(dirty.rewritten == clean.rewritten);
    test_assert(dirty.created == clean.created);
    test_assert(dirty.died == clean.died);
    test_assert(dirty.delta == clean.delta);
    test_assert(dirty_back.delta == back.delta);
    test_assert(dirty.deep_deaths == 0 && dirty_back.deep_deaths == 0);

    /* And the garbage is gone, rather than counted as something the swap
       killed. */
    sylvan_table_usage(&after, &total);
    test_assert(after <= before);
    return test_check_roots_CALL(lace);
}

/**
 * Sift one variable all the way down and all the way back up, checking after
 * every single swap. This is the motion a sifting driver makes.
 */
static int
test_sift_one_CALL(lace_worker *lace)
{
    const uint32_t moving = sylvan_identity_at(0);

    for (uint32_t rank = 0; rank + 1 < TEST_VARS; rank++) {
        sylvan_varswap_result result;
        test_assert(sylvan_test_varswap(rank, &result) == SYLVAN_OK);
        test_assert(result.deep_deaths == 0);
        test_assert(sylvan_rank_of(moving) == rank + 1);
        if (test_check_roots_CALL(lace)) return 1;
    }
    test_assert(sylvan_rank_of(moving) == TEST_VARS - 1);
    if (test_check_canonical_CALL(lace, 0)) return 1;
    if (test_check_canonical_CALL(lace, 3)) return 1;

    for (uint32_t rank = TEST_VARS - 1; rank > 0; rank--) {
        sylvan_varswap_result result;
        test_assert(sylvan_test_varswap(rank - 1, &result) == SYLVAN_OK);
        test_assert(result.deep_deaths == 0);
        test_assert(sylvan_rank_of(moving) == rank - 1);
        if (test_check_roots_CALL(lace)) return 1;
    }
    test_assert(sylvan_rank_of(moving) == 0);
    if (test_check_canonical_CALL(lace, 0)) return 1;
    if (test_check_canonical_CALL(lace, 1)) return 1;
    if (test_check_canonical_CALL(lace, 3)) return 1;
    return 0;
}

/**
 * Preflighting the sparse overflow map.
 *
 * With the promotion ceiling lowered to three, almost every node in the forest
 * moves into the overflow map, which no realistic forest would ever do and
 * which is exactly the point: the map is the structure a swap used to grow
 * while it was already rewriting records. The classification phase now
 * reserves for it, so a session that promotes thousands of times still has to
 * allocate nothing at all once classification is done.
 *
 * The sequence sifts one variable down and back up, so the order is where it
 * started afterwards.
 */
static int
test_overflow_preflight_CALL(lace_worker *lace)
{
    const uint64_t unpreflighted = nodes_late_allocations();
    uint32_t ranks[TEST_VARS - 1];
    sylvan_varswap_result down, up;

    sylvan_reorder_set_overflow_at(3);

    for (uint32_t i = 0; i + 1 < TEST_VARS; i++) ranks[i] = i;
    test_assert(sylvan_test_varswap_sequence(ranks, TEST_VARS - 1, &down)
                == SYLVAN_OK);
    test_assert(sylvan_reorder_overflow_promotions() > 0);

    for (uint32_t i = 0; i + 1 < TEST_VARS; i++) ranks[i] = TEST_VARS - 2 - i;
    test_assert(sylvan_test_varswap_sequence(ranks, TEST_VARS - 1, &up)
                == SYLVAN_OK);
    test_assert(sylvan_reorder_overflow_promotions() > 0);

    sylvan_reorder_set_overflow_at(UINT16_MAX);

    test_assert(nodes_late_allocations() == unpreflighted);
    test_assert(down.deep_deaths == 0 && up.deep_deaths == 0);
    test_assert(down.delta + up.delta == 0);
    for (int r = 0; r < TEST_ROOTS; r++) {
        test_assert(mtbdd_is_valid_CALL(lace, test_root[r]));
    }
    return test_check_roots_CALL(lace);
}

/**
 * Long random swap sequences inside a single session, with both slot release
 * policies. A session that runs many swaps is where the node lists, the slot
 * reuse and the reference counts are actually exercised against each other.
 *
 * Deferring every release to the end of the session is allowed to run out of
 * room -- that is the whole trade-off of the policy, and this table never
 * grows -- so those rounds accept SYLVAN_ERR_OOM. What they do not accept is
 * a forest that is anything other than intact afterwards, which is the point
 * of stopping a session halfway rather than rolling anything back.
 */
static int
test_stress_CALL(lace_worker *lace)
{
    uint64_t promotions = 0;

    for (int round = 0; round < test_stress_rounds; round++) {
        uint32_t ranks[TEST_STRESS_SWAPS];
        for (int i = 0; i < TEST_STRESS_SWAPS; i++) {
            ranks[i] = (uint32_t)(test_random() % (TEST_VARS - 1));
        }

        const int release_at_swap_end = round & 1;
        sylvan_reorder_set_release_at_swap_end(release_at_swap_end);

        /* Half the rounds promote almost every node into the sparse overflow
           map, which no realistic forest would ever do, so that both halves
           of the reference count representation are exercised. */
        sylvan_reorder_set_overflow_at((round & 2) ? 3u : UINT16_MAX);

        /* Opening a session collects, which is what keeps a long run going:
           without it the intermediate nodes the checks above left behind
           would fill this table, which never grows. The round used to call
           sylvan_gc() itself; now it checks that the session did, exactly
           once, rather than trusting a convention. */
        const uint64_t collections = sylvan_gc_count();
        const uint64_t unpreflighted = nodes_late_allocations();

        sylvan_varswap_result result;
        const int status = sylvan_test_varswap_sequence(
            ranks, TEST_STRESS_SWAPS, &result);

        test_assert(sylvan_gc_count() == collections + 1);
        /* Nothing in a swap allocates once its classification phase is done,
           whichever backend is underneath. */
        test_assert(nodes_late_allocations() == unpreflighted);

        if (release_at_swap_end) {
            test_assert(status == SYLVAN_OK);
            test_assert(result.swaps == TEST_STRESS_SWAPS);
        } else {
            test_assert(status == SYLVAN_OK || status == SYLVAN_ERR_OOM);
            if (status == SYLVAN_ERR_OOM) {
                printf("  round %d ran %llu of %d swaps before the deferred "
                       "garbage filled the table\n", round,
                       (unsigned long long)result.swaps, TEST_STRESS_SWAPS);
            }
        }
        test_assert(result.deep_deaths == 0);
        promotions += sylvan_reorder_overflow_promotions();

        for (int r = 0; r < TEST_ROOTS; r++) {
            test_assert(mtbdd_is_valid_CALL(lace, test_root[r]));
        }
        if (test_check_roots_CALL(lace)) return 1;
    }

    /* The rounds with the lowered threshold really did use the overflow map. */
    if (test_stress_rounds > 2) test_assert(promotions > 0);

    sylvan_reorder_set_release_at_swap_end(1);
    sylvan_reorder_set_overflow_at(UINT16_MAX);
    return 0;
}

/**
 * A swap that cannot reserve what it needs changes nothing at all.
 *
 * The table has to be filled with *live* nodes, not merely with nodes: a
 * session collects before it opens, so a table full of garbage is one a swap
 * will happily proceed on. The loop below therefore references everything it
 * builds and alternates filling with collecting until a collection no longer
 * buys room -- which is exactly the state in which the session's own
 * collection buys none either, so the reservation in the classification phase
 * is what fails. Automatic collection stays off while filling, so that a full
 * table is reported to this loop rather than collected underneath it.
 *
 * This is the last test that touches the forest.
 */
static int
test_out_of_memory_CALL(lace_worker *lace)
{
    /* Distinct integer leaves: one node per iteration, nothing intermediate
       to become garbage, and every one of them referenced. A collection then
       has literally nothing to reclaim, which is what the session's own
       collection has to find too.
       Collecting first is not decoration. A worker fills the region it owns
       before it claims another and can only claim a region no worker owns, so
       free slots inside a region another worker still holds are out of reach;
       a collection releases every claim, and after it this one worker can
       reach every free slot in the table. */
    int64_t leaf = 0;
    size_t pinned = 0;
    MTBDD *pins;
    {
        /* One protected slot per table bucket: the by-value reference API is
           gone, so each distinct leaf is pinned through its own address. */
        size_t f0 = 0, t0 = 0;
        sylvan_table_usage(&f0, &t0);
        pins = (MTBDD *)malloc(t0 * sizeof(MTBDD));
        test_assert(pins != NULL);
    }
    for (int sweep = 0; ; sweep++) {
        test_assert(sweep < 8);
        sylvan_gc_enable();
        sylvan_gc();
        sylvan_gc_disable();

        size_t filled = 0, total = 0;
        sylvan_table_usage(&filled, &total);
        if (filled >= total) break;
        while (filled < total) {
            pins[pinned] = mtbdd_int64(leaf++);
            mtbdd_protect(&pins[pinned]);
            pinned++;
            filled++;
        }
    }

    test_assert(nodes_reserve(nodes, 1) != SYLVAN_OK);

    /* Measured right after a collection that reclaimed nothing, so the
       session's own collection cannot reclaim anything either and every
       number below has to come back unchanged. */
    size_t nodes_before = 0, total = 0;
    sylvan_table_usage(&nodes_before, &total);
    uint32_t order_before[TEST_VARS];
    bdd_set_to_array(test_variables, order_before);
    MTBDD handle[TEST_ROOTS];
    for (int r = 0; r < TEST_ROOTS; r++) handle[r] = test_root[r];
    const uint64_t collections = sylvan_gc_count();
    const uint64_t promotions = sylvan_reorder_overflow_promotions();
    const uint64_t unpreflighted = nodes_late_allocations();

    /* Parity guarantees a swap that needs new nodes at every rank, whatever
       order the rounds above left behind. */
    sylvan_varswap_result result;
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_ERR_OOM);

    size_t nodes_after = 0;
    sylvan_table_usage(&nodes_after, &total);
    uint32_t order_after[TEST_VARS];
    bdd_set_to_array(test_variables, order_after);

    /* Nothing mutated: the same nodes, the same order, the same handles, the
       same functions. The one collection is the session's own, which runs
       before the classification phase reports the shortfall. */
    test_assert(nodes_after == nodes_before);
    test_assert(memcmp(order_before, order_after, sizeof(order_before)) == 0);
    test_assert(sylvan_gc_count() == collections + 1);
    test_assert(sylvan_reorder_overflow_promotions() == promotions);
    test_assert(nodes_late_allocations() == unpreflighted);
    test_assert(result.swaps == 0 && result.created == 0 && result.died == 0);
    for (int r = 0; r < TEST_ROOTS; r++) {
        test_assert(test_root[r] == handle[r]);
    }
    if (test_check_roots_CALL(lace)) return 1;
    return 0;
}

/**
 * Sort the order back to the identity with adjacent swaps, whatever the
 * random rounds left behind. Bubble sort, so TEST_VARS passes always suffice.
 */
static int
test_restore_identity_order(void)
{
    sylvan_varswap_result result;

    for (uint32_t pass = 0; pass < TEST_VARS; pass++) {
        for (uint32_t rank = 0; rank + 1 < TEST_VARS; rank++) {
            if (sylvan_identity_at(rank) < sylvan_identity_at(rank + 1)) continue;
            test_assert(sylvan_test_varswap(rank, &result) == SYLVAN_OK);
        }
    }
    return 0;
}

/**
 * ZDD may only be used while the order is the identity order, because it
 * reads a BDDSET domain spine expecting identity order while the BDD set
 * operations build that spine in rank order. So zdd_init() refuses once a
 * session has permuted the order -- and accepts again once a session has put
 * it back, which is why the question may not be answered from history.
 *
 * This is one direction of the exclusion. The other, a session refusing to
 * open once ZDD is initialised, is in test_foreign_state_guards below: it
 * cannot be checked here because initialising ZDD would stop every test that
 * still has to swap.
 */
static int
test_rank_identity_guard(void)
{
    sylvan_varswap_result result;

    if (test_restore_identity_order()) return 1;
    test_assert(sylvan_ranks_is_identity());

    /* One swap away from the identity, and ZDD refuses. */
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_OK);
    test_assert(!sylvan_ranks_is_identity());
    test_assert(zdd_init() == SYLVAN_ERR_INVALID);
    test_assert(!zdd_is_initialized());

    /* Swapping the same rank back restores the identity. A session that ends
       where it started must not lock ZDD out for the rest of the process. */
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_OK);
    test_assert(sylvan_ranks_is_identity());
    return 0;
}

/**
 * The three reasons a session refuses to open that cannot be undone: ZDD in
 * use, ListDD in use, and roots that only a consumer's own marking hook knows
 * about. They run last, and in an order that lets the ZDD refusal be
 * attributed to ZDD alone.
 */
static void
test_dummy_mark_hook(lace_worker *lace)
{
    (void)lace;
}

static int
test_foreign_state_guards(void)
{
    sylvan_varswap_result result;

    /* test_rank_identity_guard left the identity order behind, and the swap
       test_out_of_memory could not carry out did not disturb it. */
    test_assert(sylvan_ranks_is_identity());
    test_assert(zdd_init() == SYLVAN_OK);
    test_assert(zdd_is_initialized());
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_reorder_begin() == SYLVAN_ERR_INVALID);

    test_assert(listdd_init() == SYLVAN_OK);
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_ERR_INVALID);

    sylvan_gc_add_mark(test_dummy_mark_hook);
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_ERR_INVALID);
    return 0;
}

TASK(int, runtests)

int
runtests_CALL(lace_worker *lace)
{
    /* Every session end recomputes the reference counts from the arena and
       compares them with the ones the swaps maintained. */
    sylvan_reorder_set_validate(1);

    for (uint32_t i = 0; i < TEST_VARS; i++) {
        test_var[i] = mtbdd_invalid;
        mtbdd_protect(&test_var[i]);
        test_assert(bdd_var_at_level(&test_var[i], i) == SYLVAN_OK);
        test_assert(mtbdd_node_variable(test_var[i]) == i);
    }
    if (nodes_reorder_caps(nodes) == 0) {
        sylvan_varswap_result result;
        printf("Nodes table has no reorder primitives; testing the guard.\n");
        test_assert(sylvan_test_varswap(0, &result) == SYLVAN_ERR_INVALID);
        test_assert(sylvan_reorder_begin() == SYLVAN_ERR_INVALID);
        return 0;
    }

    printf("Testing the guards.\n");
    if (test_guards()) return 1;

    /* These two count exactly what a swap did, so they run while the forest
       holds nothing but the variables and what they build themselves. A
       variable set, for instance, is a chain in which every adjacent pair of
       variables interacts. */
    printf("Testing a swap of two variables that do not interact.\n");
    if (test_non_interacting_CALL(lace)) return 1;

    printf("Testing map nodes.\n");
    if (test_maps_CALL(lace)) return 1;

    sylvan_gc();

    test_variables = mtbdd_invalid;
    mtbdd_protect(&test_variables);
    uint32_t identities[TEST_VARS];
    for (uint32_t i = 0; i < TEST_VARS; i++) identities[i] = i;
    test_assert(bdd_set_from_array(&test_variables, identities, TEST_VARS)
                == SYLVAN_OK);

    for (int r = 0; r < TEST_ROOTS; r++) {
        test_root[r] = mtbdd_invalid;
        mtbdd_protect(&test_root[r]);
    }
    if (test_build_roots_CALL(lace)) return 1;
    if (test_record()) return 1;

    printf("Testing a swap at every rank.\n");
    if (test_every_rank_CALL(lace)) return 1;

    printf("Testing that swapping twice is the identity.\n");
    if (test_double_swap_CALL(lace)) return 1;

    printf("Testing that opening a session collects.\n");
    if (test_begin_collects_CALL(lace)) return 1;

    printf("Testing a variable sifted down and back up.\n");
    if (test_sift_one_CALL(lace)) return 1;

    printf("Testing a session that fills the overflow map.\n");
    if (test_overflow_preflight_CALL(lace)) return 1;

    printf("Testing %d rounds of %d random swaps.\n",
           test_stress_rounds, TEST_STRESS_SWAPS);
    if (test_stress_CALL(lace)) return 1;

    printf("Testing that ZDD refuses a permuted order.\n");
    if (test_rank_identity_guard()) return 1;

    printf("Testing a swap that cannot reserve what it needs.\n");
    if (test_out_of_memory_CALL(lace)) return 1;

    printf("Testing the foreign root guards.\n");
    if (test_foreign_state_guards()) return 1;

    /* Not one allocation anywhere in this file happened on a path a swap had
       already promised would not allocate. */
    test_assert(nodes_late_allocations() == 0);

    for (int r = 0; r < TEST_ROOTS; r++) mtbdd_unprotect(&test_root[r]);
    mtbdd_unprotect(&test_variables);
    for (uint32_t i = 0; i < TEST_VARS; i++) mtbdd_unprotect(&test_var[i]);
    return 0;
}

int
main(int argc, char **argv)
{
    int workers = 4;
    if (argc > 1) workers = atoi(argv[1]);
    if (workers < 1) workers = 1;
    if (argc > 2) test_stress_rounds = atoi(argv[2]);
    if (test_stress_rounds < 0) test_stress_rounds = 0;

    lace_start((unsigned)workers, 0, 0);

    /* A table that never grows, so that the reservation test knows exactly
       how much it has to fill and a session's garbage has a fixed budget. */
    sylvan_set_sizes(1LL << 15, 1LL << 15, 1LL << 15, 1LL << 15);
    if (sylvan_init_package() != SYLVAN_OK ||
        mtbdd_init() != SYLVAN_OK) {
        fprintf(stderr, "Sylvan initialization failed.\n");
        return 1;
    }

    const int result = runtests();

    sylvan_quit();
    lace_stop();
    return result;
}
