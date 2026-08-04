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
static const uint32_t test_order[TEST_VARS] = {5, 2, 7, 0, 3, 6, 1, 4};
static uint32_t test_rank[TEST_VARS];

static BDD test_variable[TEST_VARS];   /* indexed by identity */

/**
 * The permutation hook itself, plus the invariants every other test relies on.
 */
TASK(int, test_permutation)
int
test_permutation_CALL(lace_worker *lace)
{
    (void)lace;

    test_assert(sylvan_ranks_registered() == TEST_VARS);

    for (uint32_t identity = 0; identity < TEST_VARS; identity++) {
        test_assert(sylvan_rank_of(identity) == test_rank[identity]);
        test_assert(sylvan_identity_at(test_rank[identity]) == identity);
    }

    /* Unregistered identities keep rank == identity, which is what makes the
       0xffffffff "no variable" sentinel order last. */
    test_assert(sylvan_rank_of(TEST_VARS) == TEST_VARS);
    test_assert(sylvan_rank_of(UINT32_MAX) == UINT32_MAX);
    test_assert(sylvan_identity_at(TEST_VARS) == TEST_VARS);

    for (uint32_t a = 0; a < TEST_VARS; a++) {
        for (uint32_t b = 0; b < TEST_VARS; b++) {
            test_assert(sylvan_var_before(a, b)
                        == (test_rank[a] < test_rank[b]));
            test_assert(sylvan_var_min(a, b)
                        == (test_rank[a] < test_rank[b] ? a : b));
        }
        test_assert(sylvan_var_before(a, UINT32_MAX));
        test_assert(!sylvan_var_before(UINT32_MAX, a));
    }

    /* Rejected permutations leave the order untouched. */
    uint32_t rejected[TEST_VARS];
    memcpy(rejected, test_order, sizeof(rejected));
    rejected[0] = rejected[1];
    test_assert(sylvan_ranks_permute_unsafe(rejected, TEST_VARS)
                == SYLVAN_ERR_INVALID);
    test_assert(sylvan_ranks_permute_unsafe(test_order, TEST_VARS - 1)
                == SYLVAN_ERR_INVALID);
    test_assert(sylvan_ranks_permute_unsafe(NULL, TEST_VARS)
                == SYLVAN_ERR_INVALID);
    test_assert(sylvan_ranks_reserve(SYLVAN_RANKS_LIMIT + 1)
                == SYLVAN_ERR_INVALID);
    for (uint32_t identity = 0; identity < TEST_VARS; identity++) {
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

    for (uint32_t level = 0; level < TEST_VARS; level++) {
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
    test_assert(bdd_var_at_level(&fresh, TEST_VARS + 3) == SYLVAN_OK);
    test_assert(mtbdd_node_variable(fresh) == TEST_VARS + 3);
    test_assert(sylvan_rank_of(TEST_VARS + 3) == TEST_VARS + 3);
    mtbdd_refs_popptr(1);
    return 0;
}

TASK(int, runtests)
int
runtests_CALL(lace_worker *lace)
{
    for (uint32_t rank = 0; rank < TEST_VARS; rank++) {
        test_rank[test_order[rank]] = rank;
    }

    /* Register the identities and permute the order before any node exists. */
    test_assert(sylvan_ranks_reserve(TEST_VARS) == SYLVAN_OK);
    test_assert(sylvan_ranks_permute_unsafe(test_order, TEST_VARS)
                == SYLVAN_OK);

    for (uint32_t identity = 0; identity < TEST_VARS; identity++) {
        test_variable[identity] = mtbdd_invalid;
        mtbdd_protect(&test_variable[identity]);
        test_assert(bdd_var_at_level(&test_variable[identity],
                                     test_rank[identity]) == SYLVAN_OK);
        test_assert(mtbdd_node_variable(test_variable[identity]) == identity);
    }

    printf("Testing the permutation hook.\n");
    if (test_permutation_CALL(lace)) return 1;
    printf("Testing bdd_var_at_level.\n");
    if (test_var_at_level_CALL(lace)) return 1;

    for (uint32_t identity = 0; identity < TEST_VARS; identity++) {
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
