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
 * Long random swap sequences inside a single session, with both slot release
 * policies. A session that runs many swaps is where the node lists, the slot
 * reuse and the reference counts are actually exercised against each other.
 */
static int
test_stress_CALL(lace_worker *lace)
{
    for (int round = 0; round < test_stress_rounds; round++) {
        uint32_t ranks[TEST_STRESS_SWAPS];
        for (int i = 0; i < TEST_STRESS_SWAPS; i++) {
            ranks[i] = (uint32_t)(test_random() % (TEST_VARS - 1));
        }

        sylvan_reorder_set_release_at_swap_end(round & 1);

        sylvan_varswap_result result;
        test_assert(sylvan_test_varswap_sequence(
                        ranks, TEST_STRESS_SWAPS, &result) == SYLVAN_OK);
        test_assert(result.swaps == TEST_STRESS_SWAPS);
        test_assert(result.deep_deaths == 0);

        for (int r = 0; r < TEST_ROOTS; r++) {
            test_assert(mtbdd_is_valid_CALL(lace, test_root[r]));
        }
        if (test_check_roots_CALL(lace)) return 1;
    }

    sylvan_reorder_set_release_at_swap_end(1);
    return 0;
}

/**
 * A swap that cannot reserve the slots it needs changes nothing at all.
 *
 * The table is filled up first, with garbage collection disabled so that a
 * full table is reported rather than collected. This is the last test that
 * touches the forest.
 */
static int
test_out_of_memory_CALL(lace_worker *lace)
{
    sylvan_gc_disable();

    BDD filler = bdd_false;
    mtbdd_protect(&filler);
    for (int attempt = 0; attempt < 100000; attempt++) {
        if (nodes_reserve(nodes, 16) != SYLVAN_OK) break;
        filler = bdd_false;
        int failed = 0;
        for (int minterm = 0; minterm < 24 && !failed; minterm++) {
            uint8_t values[TEST_VARS];
            const unsigned assignment =
                (unsigned)(test_random() % TEST_ASSIGNMENTS);
            test_values_for(assignment, values);
            if (bdd_or_cube_CALL(lace, &filler, filler, test_variables, values)
                != SYLVAN_OK) {
                failed = 1;
            }
        }
        if (failed) break;
        mtbdd_ref(filler);
    }
    filler = bdd_false;
    mtbdd_unprotect(&filler);

    test_assert(nodes_reserve(nodes, 1) != SYLVAN_OK);

    /* Find a rank whose swap actually needs new nodes; parity guarantees one
       at every rank, but the order is whatever the stress test left behind. */
    size_t nodes_before = 0, total = 0;
    sylvan_table_usage(&nodes_before, &total);
    uint32_t order_before[TEST_VARS];
    bdd_set_to_array(test_variables, order_before);
    MTBDD handle[TEST_ROOTS];
    for (int r = 0; r < TEST_ROOTS; r++) handle[r] = test_root[r];

    sylvan_varswap_result result;
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_ERR_OOM);

    size_t nodes_after = 0;
    sylvan_table_usage(&nodes_after, &total);
    uint32_t order_after[TEST_VARS];
    bdd_set_to_array(test_variables, order_after);

    test_assert(nodes_after == nodes_before);
    test_assert(memcmp(order_before, order_after, sizeof(order_before)) == 0);
    for (int r = 0; r < TEST_ROOTS; r++) {
        test_assert(test_root[r] == handle[r]);
    }
    if (test_check_roots_CALL(lace)) return 1;
    return 0;
}

/**
 * Roots that only a consumer's own marking hook knows about cannot be
 * enumerated by a session, so the session refuses to open. So does one that
 * finds ZDD or ListDD nodes sharing the arena. Both of these change global
 * state that cannot be undone, so they run last.
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

    sylvan_gc_add_mark(test_dummy_mark_hook);
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_ERR_INVALID);

    zdd_init();
    test_assert(sylvan_test_varswap(0, &result) == SYLVAN_ERR_INVALID);

    listdd_init();
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

    /* Drive the overflow map with a threshold no forest would ever reach on
       its own, so that the sparse path is exercised by every test below. */
    sylvan_reorder_set_overflow_at(3);

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

    printf("Testing a variable sifted down and back up.\n");
    if (test_sift_one_CALL(lace)) return 1;

    printf("Testing %d rounds of %d random swaps.\n",
           test_stress_rounds, TEST_STRESS_SWAPS);
    if (test_stress_CALL(lace)) return 1;

    test_assert(sylvan_reorder_overflow_promotions() > 0);

    printf("Testing a swap that cannot reserve what it needs.\n");
    if (test_out_of_memory_CALL(lace)) return 1;

    printf("Testing the foreign root guards.\n");
    if (test_foreign_state_guards()) return 1;

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
    sylvan_init_package();
    mtbdd_init();

    const int result = runtests();

    sylvan_quit();
    lace_stop();
    return result;
}
