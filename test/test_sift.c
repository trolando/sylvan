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
 * The sifting driver.
 *
 * A reordering is worth nothing if it does not make the forest smaller, and
 * worse than nothing if it changes what the forest means. Every test here
 * checks both halves at once, against the same oracles the variable swap is
 * tested with: the full truth table of every root over every assignment
 * *indexed by variable identity*, which no reordering may change; the root
 * handles, which a swap rewrites nodes in place precisely so as not to
 * disturb; and mtbdd_is_valid, which fails the moment a node's child does not
 * have a strictly larger rank than the node itself.
 *
 * The function the size tests use is the standard one for this, from Bryant's
 * original paper: f = x0*y0 + x1*y1 + ... + xn*yn. Under the paired order
 * x0 y0 x1 y1 ... it needs about 3n nodes; under the split order
 * x0 x1 ... xn y0 y1 ... yn it needs about 2^(n+1). The variables are created
 * so that the split order is the one Sylvan starts in, and sifting has to find
 * its way back to something near the paired one.
 *
 * Usage: test_sift [workers] [queens rounds]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lace.h>
#include <sylvan/internal.h>

#include "test_assert.h"

/* Six pairs: 12 variables, 4096 assignments, and a good/bad gap of about 7x. */
#define TEST_PAIRS 6
#define TEST_VARS (2 * TEST_PAIRS)
#define TEST_ASSIGNMENTS (1u << TEST_VARS)
#define TEST_ROOTS 6

/* The board size and the number of rebuild rounds of the stress test. */
#define TEST_QUEENS 8
static int test_queens_rounds = 10;

static BDDSET test_variables;
static BDD test_var[TEST_VARS];
static MTBDD test_root[TEST_ROOTS];
static MTBDD test_value[TEST_ROOTS][TEST_ASSIGNMENTS];

/* Custom leaves, so that the forest a sift moves through is not all decision
   nodes and built-in leaves. The value lives in the leaf itself. */
static uint32_t test_leaf_type = UINT32_MAX;

static uint64_t
test_leaf_hash(void *context, uint64_t value, uint64_t seed)
{
    (void)context;
    return sylvan_tabhash16(value, UINT64_C(0), seed);
}

static int
test_leaf_equal(void *context, uint64_t left, uint64_t right)
{
    (void)context;
    return left == right;
}

static int
test_leaf_clone(void *context, uint64_t value, uint64_t *result)
{
    (void)context;
    *result = value;
    return SYLVAN_OK;
}

static void
test_leaf_destroy(void *context, uint64_t value)
{
    (void)context;
    (void)value;
}

static int
test_leaf_to_string(void *context, int complement, uint64_t value, char **result)
{
    (void)context;
    char *text = (char*)malloc(32);
    if (text == NULL) return SYLVAN_ERR_OOM;
    snprintf(text, 32, "%s%llu", complement ? "~" : "",
             (unsigned long long)value);
    *result = text;
    return SYLVAN_OK;
}

static void
test_leaf_string_free(void *context, char *string)
{
    (void)context;
    free(string);
}

/**
 * Turn an assignment, whose bit <i> is the value of variable *identity* <i>,
 * into the value array evaluation wants, which is indexed by position in the
 * current variable order.
 */
static void
test_values_for(const uint32_t *order, unsigned assignment, uint8_t *values)
{
    for (int i = 0; i < TEST_VARS; i++) {
        values[i] = (uint8_t)((assignment >> order[i]) & 1u);
    }
}

/** Record the value of every root at every assignment. */
static int
test_record(void)
{
    uint32_t order[TEST_VARS];
    bdd_set_to_array(test_variables, order);

    for (int r = 0; r < TEST_ROOTS; r++) {
        for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
            uint8_t values[TEST_VARS];
            test_values_for(order, a, values);
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
    uint32_t order[TEST_VARS];
    bdd_set_to_array(test_variables, order);

    for (int r = 0; r < TEST_ROOTS; r++) {
        test_assert(mtbdd_is_valid_CALL(lace, test_root[r]));
        for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
            uint8_t values[TEST_VARS];
            MTBDD evaluated = mtbdd_invalid;
            test_values_for(order, a, values);
            test_assert(mtbdd_eval(&evaluated, test_root[r], test_variables,
                                   values, TEST_VARS) == SYLVAN_OK);
            test_assert(evaluated == test_value[r][a]);
        }
    }
    return 0;
}

/**
 * Build the forest.
 *
 * Variable identity i is x_i for i < TEST_PAIRS and y_(i-TEST_PAIRS) after
 * that, so the order Sylvan starts in -- rank equals identity -- is the split
 * order, which is the expensive one.
 */
static int
test_build_CALL(lace_worker *lace)
{
    BDD pairs = bdd_false;
    BDD conjunction = bdd_false;
    mtbdd_protect(&pairs);
    mtbdd_protect(&conjunction);
    for (int i = 0; i < TEST_PAIRS; i++) {
        test_assert(bdd_and_CALL(lace, &conjunction,
                                 test_var[i], test_var[i + TEST_PAIRS])
                    == SYLVAN_OK);
        test_assert(bdd_or_CALL(lace, &pairs, pairs, conjunction) == SYLVAN_OK);
    }
    test_root[0] = pairs;
    mtbdd_unprotect(&conjunction);
    mtbdd_unprotect(&pairs);

    /* A second function over the same variables that prefers the same order,
       so that a good position for one is a good position for the other. */
    BDD chain = bdd_true;
    BDD equivalence = bdd_false;
    mtbdd_protect(&chain);
    mtbdd_protect(&equivalence);
    for (int i = 0; i < TEST_PAIRS; i++) {
        test_assert(bdd_xor_CALL(lace, &equivalence,
                                 test_var[i], test_var[i + TEST_PAIRS])
                    == SYLVAN_OK);
        test_assert(bdd_and_CALL(lace, &chain, chain, bdd_not(equivalence))
                    == SYLVAN_OK);
    }
    test_root[1] = chain;
    mtbdd_unprotect(&equivalence);
    mtbdd_unprotect(&chain);

    /* A complemented handle onto a node another root points at directly. */
    test_root[2] = bdd_not(test_root[0]);

    /* A genuine multi-terminal diagram with built-in integer leaves. */
    MTBDD integers = mtbdd_invalid;
    mtbdd_protect(&integers);
    test_assert(mtbdd_ite_CALL(lace, &integers, test_root[0],
                               mtbdd_int64(-3), mtbdd_int64(11)) == SYLVAN_OK);
    test_root[3] = integers;
    mtbdd_unprotect(&integers);

    /* And one with custom leaves, which the reference counts have to leave
       alone: a leaf is never counted and never retired. */
    MTBDD custom = mtbdd_invalid;
    mtbdd_protect(&custom);
    const MTBDD low = mtbdd_leaf(test_leaf_type, UINT64_C(101));
    const MTBDD high = mtbdd_leaf(test_leaf_type, UINT64_C(202));
    test_assert(low != mtbdd_invalid && high != mtbdd_invalid);
    test_assert(mtbdd_ite_CALL(lace, &custom, test_root[1], high, low)
                == SYLVAN_OK);
    test_root[4] = custom;
    mtbdd_unprotect(&custom);

    /* A handle onto a node deep inside another root. A sift rewrites nodes in
       place, so this must keep denoting the same function even though the node
       it names has different children afterwards. */
    test_root[5] = mtbdd_node_high(test_root[0]);

    for (int r = 0; r < TEST_ROOTS; r++) {
        test_assert(mtbdd_is_valid_CALL(lace, test_root[r]));
    }
    return 0;
}

/** The order, as identities from the top down, for a diagnostic line. */
static void
test_print_order(const char *label)
{
    printf("  %s:", label);
    for (uint32_t rank = 0; rank < TEST_VARS; rank++) {
        printf(" %u", (unsigned)sylvan_identity_at(rank));
    }
    printf("\n");
}

/**
 * The central test: a forest built under a known bad order gets substantially
 * smaller, and means exactly the same thing.
 */
static uint64_t test_first_final_size;

static int
test_reduces_CALL(lace_worker *lace)
{
    MTBDD handle[TEST_ROOTS];
    for (int r = 0; r < TEST_ROOTS; r++) handle[r] = test_root[r];
    const size_t before = mtbdd_shared_node_count(test_root, TEST_ROOTS);

    test_print_order("order before");

    sylvan_sift_result result;
    const uint64_t sifts = sylvan_sift_count();
    test_assert(sylvan_sift_report(NULL, &result) == SYLVAN_OK);
    test_assert(sylvan_sift_count() == sifts + 1);

    const size_t after = mtbdd_shared_node_count(test_root, TEST_ROOTS);
    printf("  forest %llu -> %llu nodes in %llu swaps over %u variables\n",
           (unsigned long long)result.initial_size,
           (unsigned long long)result.final_size,
           (unsigned long long)result.swaps,
           (unsigned)result.vars_sifted);
    printf("  roots  %zu -> %zu nodes\n", before, after);
    test_print_order("order after ");

    /* The driver's own numbers agree with a recount of the roots: everything
       in this forest is reachable from a root, so the two must move together. */
    test_assert(result.initial_size > result.final_size);
    test_assert(result.swaps > 0);
    test_assert(result.vars_sifted > 0);
    test_assert(after < before);

    /* "Substantially": the split order costs about 2^(n+1) and the paired one
       about 3n, so anything short of halving means the driver is not sifting. */
    test_assert(2 * result.final_size < result.initial_size);

    /* And nothing moved that was not supposed to. */
    for (int r = 0; r < TEST_ROOTS; r++) test_assert(test_root[r] == handle[r]);
    test_first_final_size = result.final_size;
    return test_check_roots_CALL(lace);
}

/**
 * A variable is named by its identity, not by its position. After a sift has
 * moved the variables, bdd_var() still returns every variable it returned
 * before, while bdd_var_at_level() returns whichever variable is ranked at
 * that position now.
 */
static int
test_identity_kept(void)
{
    int moved = 0;
    BDD variable = mtbdd_invalid;
    mtbdd_refs_pushptr(&variable);
    for (uint32_t i = 0; i < TEST_VARS; i++) {
        test_assert(bdd_var(&variable, i) == SYLVAN_OK);
        test_assert(variable == test_var[i]);
        test_assert(bdd_var_at_level(&variable, i) == SYLVAN_OK);
        test_assert(variable == test_var[sylvan_identity_at(i)]);
        if (variable != test_var[i]) moved = 1;
    }
    mtbdd_refs_popptr(1);

    /* The sift left the split order the variables were created in. */
    test_assert(moved);
    return 0;
}

/**
 * Sifting a forest that has just been sifted has almost nothing left to do.
 * It may still find a different position of equal value, and the population
 * order it works in is not the one it worked in last time, so this is a bound
 * rather than an equality.
 *
 * The threshold is set to zero explicitly, so this stays a test of
 * idempotence whatever the default: the sifted forest is small enough that a
 * threshold of a few dozen would leave no variable worth moving, and a run
 * that considers nothing trivially changes nothing.
 * That <initial_size> comes back equal to the previous <final_size> is worth a
 * line of its own -- it says the running size the session maintains agrees
 * with what a fresh session counts from the arena.
 */
static int
test_idempotent_CALL(lace_worker *lace)
{
    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));

    sylvan_sift_result result;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    printf("  second sift: %llu -> %llu nodes in %llu swaps over %u "
           "variables\n",
           (unsigned long long)result.initial_size,
           (unsigned long long)result.final_size,
           (unsigned long long)result.swaps,
           (unsigned)result.vars_sifted);

    test_assert(result.initial_size == test_first_final_size);
    test_assert(result.vars_sifted == TEST_VARS);
    test_assert(result.swaps > 0);
    test_assert(20 * result.final_size <= 21 * test_first_final_size);
    return test_check_roots_CALL(lace);
}

/**
 * The configuration fields. Each one is checked by what it forbids, and every
 * one of them has to leave a forest that is still correct: a limit is a limit
 * on the search, never on the invariants.
 */
static int
test_config_CALL(lace_worker *lace)
{
    sylvan_sift_config config;
    sylvan_sift_result result;

    /* A threshold no variable can reach is a no-op that still succeeds. */
    memset(&config, 0, sizeof(config));
    config.nodes_threshold = 1u << 30;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_assert(result.swaps == 0);
    test_assert(result.vars_sifted == 0);
    test_assert(result.final_size == result.initial_size);
    if (test_check_roots_CALL(lace)) return 1;

    /* One variable, and it is the most populous one. */
    memset(&config, 0, sizeof(config));
    config.max_vars = 1;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_assert(result.vars_sifted == 1);
    if (test_check_roots_CALL(lace)) return 1;

    /* A budget of n swaps buys at most n swaps of search. The return to the
       best position counts against the budget but is never cut short by it,
       and it is no longer than the search before it, so the total is bounded
       at 2n rather than at n. */
    for (uint32_t budget = 1; budget <= 4; budget++) {
        memset(&config, 0, sizeof(config));
        config.max_swaps = budget;
        test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
        test_assert(result.swaps <= 2u * budget);
        if (test_check_roots_CALL(lace)) return 1;
    }

    /* The tightest growth bound there is: no leg may leave the forest larger
       than the best it has seen. This still terminates, and still improves or
       leaves alone. */
    memset(&config, 0, sizeof(config));
    config.max_growth = 1.0f;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_assert(result.final_size <= result.initial_size);
    if (test_check_roots_CALL(lace)) return 1;

    /* A time limit that has already expired when it is set. */
    memset(&config, 0, sizeof(config));
    config.time_limit = 1e-9;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_assert(result.final_size <= result.initial_size);
    if (test_check_roots_CALL(lace)) return 1;

    /* Full legs: the interaction bound switched off. The bound is
       conservative, so the exhaustive walk finds nothing the bounded runs
       above missed -- it only spends more swaps proving that -- and the
       forest it leaves is as small as the bounded one within the same slack
       the idempotence test allows a repeated sift. */
    memset(&config, 0, sizeof(config));
    config.full_legs = 1;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_assert(result.final_size <= result.initial_size);
    test_assert(20 * result.final_size <= 21 * test_first_final_size);
    if (test_check_roots_CALL(lace)) return 1;

    /* Batching off. On a backend with the batched index primitives and more
       than one worker, every swap now unhashes and re-indexes its nodes one at
       a time instead: the same surgery in a different schedule, so everything
       that holds for a batched sift holds here, down to the reference counts
       the session recomputes when it closes (see runtests). Elsewhere every
       sift takes this path, and the run is one more sift. */
    sylvan_reorder_set_batched(0);
    memset(&config, 0, sizeof(config));
    const int unbatched = sylvan_sift_report(&config, &result);
    sylvan_reorder_set_batched(1);
    test_assert(unbatched == SYLVAN_OK);
    test_assert(result.final_size <= result.initial_size);
    test_assert(20 * result.final_size <= 21 * test_first_final_size);
    if (test_check_roots_CALL(lace)) return 1;

    /* And the plain call, which reports nothing. */
    test_assert(sylvan_sift(NULL) == SYLVAN_OK);
    return test_check_roots_CALL(lace);
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
 * Mixed content. A map spine is ordered by the ranks of its keys, so sifting
 * has to carry the maps along with the variables they are keyed by, while the
 * leaves -- built-in and custom alike -- are not part of the order at all and
 * must come through untouched.
 */
static int
test_mixed_CALL(lace_worker *lace)
{
    MTBDDMAP map = mtbdd_map_empty();
    mtbdd_protect(&map);
    for (uint32_t i = 0; i < TEST_VARS; i++) {
        test_assert(mtbdd_map_set(&map, map, TEST_VARS - 1u - i,
                                  test_root[(TEST_VARS - 1u - i) % TEST_ROOTS])
                    == SYLVAN_OK);
    }
    test_assert(mtbdd_map_count(map) == TEST_VARS);

    /* Every variable, however few nodes it carries, so that the maps really
       are dragged through swaps rather than left alone by the threshold. */
    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));

    const MTBDDMAP handle = map;
    sylvan_sift_result result;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_assert(result.swaps > 0);

    /* Handles are stable, every key still maps to the same value, and the
       spine is ordered by the ranks the sift left behind. */
    test_assert(map == handle);
    test_assert(mtbdd_map_count(map) == TEST_VARS);
    for (uint32_t i = 0; i < TEST_VARS; i++) {
        test_assert(test_map_lookup(map, i) == test_root[i % TEST_ROOTS]);
        test_assert(mtbdd_map_contains(map, i));
    }
    MTBDDMAP walk = map;
    for (uint32_t rank = 0; rank < TEST_VARS; rank++) {
        test_assert(mtbdd_map_key(walk) == sylvan_identity_at(rank));
        walk = mtbdd_map_next(walk);
    }
    test_assert(mtbdd_map_is_empty(walk));

    /* The leaves are exactly the ones that were there. */
    test_assert(mtbdd_leaf(test_leaf_type, UINT64_C(101)) != mtbdd_invalid);
    test_assert(mtbdd_leaf_int64(mtbdd_int64(-3)) == -3);

    if (test_check_roots_CALL(lace)) return 1;
    mtbdd_unprotect(&map);
    return 0;
}

/**
 * Stress: build the n-queens relation in stages and sift after each of them,
 * over and over. Sifting a forest that is still being built is the case a real
 * consumer runs into, and it is the one where a stale node list, a wrong
 * reference count or a swap that lost a node shows up as a wrong answer rather
 * than as a crash. The oracle is the number of solutions, which is 92 for the
 * standard board and does not depend on the variable order.
 */
static uint64_t test_queens_swaps;
static uint64_t test_queens_reduced;

static int
test_queens_stage_CALL(lace_worker *lace, BDD *result, BDD addend)
{
    return bdd_and_CALL(lace, result, *result, addend);
}

/** Sift, and fold what it did into the totals reported for round zero. */
static int
test_queens_sift(int report)
{
    sylvan_sift_result result;
    test_assert(sylvan_sift_report(NULL, &result) == SYLVAN_OK);
    /* Sifting returns every variable to the best position it saw, so the
       forest never grows -- provided no swap on a return leg was refused. A
       refusal is a documented outcome that leaves the variable where it
       struck, and can legitimately make final_size exceed initial_size, so
       the size relation is only an invariant when refusals is zero. On this
       table, which has room to spare, no swap is refused (see nodes_reserve);
       assert both, so a failure names its cause. */
    test_assert(result.refusals == 0);
    test_assert(result.final_size <= result.initial_size);
    if (report) {
        test_queens_swaps += result.swaps;
        test_queens_reduced += result.initial_size - result.final_size;
    }
    return 0;
}

static int
test_queens_CALL(lace_worker *lace)
{
    const int size = TEST_QUEENS;
    const int cells = size * size;

    BDD *board = (BDD*)malloc((size_t)cells * sizeof(BDD));
    test_assert(board != NULL);
    for (int i = 0; i < cells; i++) {
        board[i] = mtbdd_invalid;
        mtbdd_protect(board + i);
    }

    BDD relation = bdd_true, temp = bdd_true, variables = bdd_true;
    mtbdd_protect(&relation);
    mtbdd_protect(&temp);
    mtbdd_protect(&variables);

    /* The board variables are created once and reused by every round. They
       have to be: bdd_var_at_level names a *position* in the order, so asking
       for the same position again after a sift would hand back a different
       variable and quietly change the problem being solved. Their handles
       survive every reordering, because a variable's own node has no child
       that any swap can rewrite. */
    int failed = 0;
    for (int i = 0; i < cells && !failed; i++) {
        if (bdd_var_at_level(board + i, (uint32_t)(TEST_VARS + i))
            != SYLVAN_OK) failed = 1;
    }
    for (int i = cells - 1; i >= 0 && !failed; i--) {
        if (bdd_and_CALL(lace, &variables, variables, board[i]) != SYLVAN_OK) {
            failed = 1;
        }
    }
    test_assert(!failed);

    for (int round = 0; round < test_queens_rounds && !failed; round++) {
        const int report = round == 0;
        relation = bdd_true;

        /* Stage one: at most one queen in every row, and every column. */
        for (int i = 0; i < size && !failed; i++) {
            for (int j = 0; j < size && !failed; j++) {
                temp = bdd_true;
                for (int k = 0; k < size && !failed; k++) {
                    if (k == j) continue;
                    if (bdd_and_CALL(lace, &temp, temp,
                                     bdd_not(board[i * size + k])) != SYLVAN_OK) {
                        failed = 1;
                    }
                }
                if (failed) break;
                if (bdd_or_CALL(lace, &temp, temp,
                                bdd_not(board[i * size + j])) != SYLVAN_OK ||
                    test_queens_stage_CALL(lace, &relation, temp) != SYLVAN_OK) {
                    failed = 1;
                }
            }
        }
        if (!failed && test_queens_sift(report)) return 1;

        for (int j = 0; j < size && !failed; j++) {
            for (int i = 0; i < size && !failed; i++) {
                temp = bdd_true;
                for (int k = 0; k < size && !failed; k++) {
                    if (k == i) continue;
                    if (bdd_and_CALL(lace, &temp, temp,
                                     bdd_not(board[k * size + j])) != SYLVAN_OK) {
                        failed = 1;
                    }
                }
                if (failed) break;
                if (bdd_or_CALL(lace, &temp, temp,
                                bdd_not(board[i * size + j])) != SYLVAN_OK ||
                    test_queens_stage_CALL(lace, &relation, temp) != SYLVAN_OK) {
                    failed = 1;
                }
            }
        }
        if (!failed && test_queens_sift(report)) return 1;

        /* Stage two: the two diagonals. */
        for (int i = 0; i < size && !failed; i++) {
            for (int j = 0; j < size && !failed; j++) {
                temp = bdd_true;
                for (int k = 0; k < size && !failed; k++) {
                    if (j + k >= i && j + k < size + i && k != i) {
                        if (bdd_and_CALL(lace, &temp, temp,
                                         bdd_not(board[k * size + (j + k - i)]))
                            != SYLVAN_OK) failed = 1;
                    }
                }
                if (failed) break;
                if (bdd_or_CALL(lace, &temp, temp,
                                bdd_not(board[i * size + j])) != SYLVAN_OK ||
                    test_queens_stage_CALL(lace, &relation, temp) != SYLVAN_OK) {
                    failed = 1;
                }
            }
        }
        if (!failed && test_queens_sift(report)) return 1;

        for (int i = 0; i < size && !failed; i++) {
            for (int j = 0; j < size && !failed; j++) {
                temp = bdd_true;
                for (int k = 0; k < size && !failed; k++) {
                    if (j + i >= k && j + i < size + k && k != i) {
                        if (bdd_and_CALL(lace, &temp, temp,
                                         bdd_not(board[k * size + (j + i - k)]))
                            != SYLVAN_OK) failed = 1;
                    }
                }
                if (failed) break;
                if (bdd_or_CALL(lace, &temp, temp,
                                bdd_not(board[i * size + j])) != SYLVAN_OK ||
                    test_queens_stage_CALL(lace, &relation, temp) != SYLVAN_OK) {
                    failed = 1;
                }
            }
        }
        if (!failed && test_queens_sift(report)) return 1;

        /* Stage three: a queen on every row, which is what makes it 92. */
        for (int i = 0; i < size && !failed; i++) {
            temp = bdd_false;
            for (int j = 0; j < size && !failed; j++) {
                if (bdd_or_CALL(lace, &temp, temp, board[i * size + j])
                    != SYLVAN_OK) failed = 1;
            }
            if (failed) break;
            if (test_queens_stage_CALL(lace, &relation, temp) != SYLVAN_OK) {
                failed = 1;
            }
        }

        test_assert(!failed);
        sylvan_sift_result result;
        test_assert(sylvan_sift_report(NULL, &result) == SYLVAN_OK);

        uint64_t solutions = 0;
        test_assert(bdd_sat_count_u64(&solutions, relation, variables)
                    == SYLVAN_OK);
        test_assert(solutions == 92);
        test_assert(mtbdd_is_valid_CALL(lace, relation));
        if (report) {
            test_queens_swaps += result.swaps;
            test_queens_reduced += result.initial_size - result.final_size;
            printf("  queens(%d): five sifts over one build removed %llu nodes "
                   "in %llu swaps; the last went %llu -> %llu, %llu solutions\n",
                   size,
                   (unsigned long long)test_queens_reduced,
                   (unsigned long long)test_queens_swaps,
                   (unsigned long long)result.initial_size,
                   (unsigned long long)result.final_size,
                   (unsigned long long)solutions);
        }

        /* Drop the round's diagrams so the next one starts from a forest that
           holds nothing but the variables, whatever order this one settled on.
           The board and its cube stay: they are what the next round builds
           from, and re-asking for them by position would not give them back. */
        relation = bdd_true;
        temp = bdd_true;
    }

    mtbdd_unprotect(&variables);
    mtbdd_unprotect(&temp);
    mtbdd_unprotect(&relation);
    for (int i = 0; i < cells; i++) mtbdd_unprotect(board + i);
    free(board);
    return 0;
}

/**
 * A CONSUMER'S OWN ROOTS.
 *
 * The two diagrams below are protected by nothing and referenced by nothing:
 * the only thing in the process that knows they are roots is a hook that hands
 * their values back when Sylvan asks. The session pins exactly what the hook
 * yields, so the sift runs, and both diagrams come out of it at the same
 * handles denoting the same functions.
 *
 * The hook yields both roots in one array, which is the shape a consumer with
 * a handle table has (see ROOT VISITORS in include/sylvan/common.h).
 */
#define TEST_VISITOR_ROOTS 2

static MTBDD test_visitor_root[TEST_VISITOR_ROOTS];
static MTBDD test_visitor_value[TEST_VISITOR_ROOTS][TEST_ASSIGNMENTS];
static int test_visitor_calls;
static int test_visitor_context_wrong;

static void
test_visitor(void *context, sylvan_roots_yield yield, void *arg)
{
    if (context != (void*)&test_visitor_calls) test_visitor_context_wrong = 1;
    test_visitor_calls++;
    yield(arg, test_visitor_root, TEST_VISITOR_ROOTS);
}

static int
test_roots_visitor_CALL(lace_worker *lace)
{
    uint32_t order[TEST_VARS];
    bdd_set_to_array(test_variables, order);

    /* Built while protected, so that a collection during construction has
       something to hold on to, and handed over to the visitor afterwards. */
    MTBDD built[TEST_VISITOR_ROOTS];
    for (int r = 0; r < TEST_VISITOR_ROOTS; r++) {
        built[r] = mtbdd_invalid;
        mtbdd_protect(&built[r]);
    }
    test_assert(bdd_xor_CALL(lace, &built[0], test_root[0], test_root[1])
                == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &built[1], bdd_not(test_root[0]),
                             test_var[TEST_PAIRS - 1]) == SYLVAN_OK);

    for (int r = 0; r < TEST_VISITOR_ROOTS; r++) {
        for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
            uint8_t values[TEST_VARS];
            test_values_for(order, a, values);
            test_assert(mtbdd_eval(&test_visitor_value[r][a], built[r],
                                   test_variables, values, TEST_VARS)
                        == SYLVAN_OK);
        }
        test_visitor_root[r] = built[r];
    }

    test_visitor_calls = 0;
    test_visitor_context_wrong = 0;
    test_assert(sylvan_gc_add_roots(test_visitor, &test_visitor_calls)
                == SYLVAN_OK);
    test_assert(sylvan_gc_add_roots(NULL, NULL) == SYLVAN_ERR_INVALID);

    /* From here the hook is the only reference either diagram has. */
    for (int r = 0; r < TEST_VISITOR_ROOTS; r++) mtbdd_unprotect(&built[r]);

    const MTBDD handle[TEST_VISITOR_ROOTS] = {
        test_visitor_root[0], test_visitor_root[1]
    };

    sylvan_sift_result result;
    test_assert(sylvan_sift_report(NULL, &result) == SYLVAN_OK);

    /* Marking asked once and pinning asked once, both inside the one call. */
    test_assert(!test_visitor_context_wrong);
    test_assert(test_visitor_calls >= 2);

    for (int r = 0; r < TEST_VISITOR_ROOTS; r++) {
        test_assert(test_visitor_root[r] == handle[r]);
        test_assert(mtbdd_is_valid_CALL(lace, test_visitor_root[r]));
    }

    bdd_set_to_array(test_variables, order);
    for (int r = 0; r < TEST_VISITOR_ROOTS; r++) {
        for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
            uint8_t values[TEST_VARS];
            MTBDD evaluated = mtbdd_invalid;
            test_values_for(order, a, values);
            test_assert(mtbdd_eval(&evaluated, test_visitor_root[r],
                                   test_variables, values, TEST_VARS)
                        == SYLVAN_OK);
            test_assert(evaluated == test_visitor_value[r][a]);
        }
    }

    /* Removal takes one registration and rejects an unknown pair. */
    test_assert(sylvan_gc_remove_roots(test_visitor, &test_visitor_calls)
                == SYLVAN_OK);
    test_assert(sylvan_gc_remove_roots(test_visitor, &test_visitor_calls)
                == SYLVAN_ERR_INVALID);

    /* Nothing refers to them any more; do not name them again. */
    test_visitor_root[0] = bdd_false;
    test_visitor_root[1] = bdd_false;

    /* And a session still opens once the visitor is gone. */
    test_assert(sylvan_sift(NULL) == SYLVAN_OK);
    return test_check_roots_CALL(lace);
}

/**
 * HANDLES ON A WORKER'S TEMPORARY REFERENCE STACK.
 *
 * A diagram that lives only on mtbdd_refs_push/pushptr is in no protected
 * table, and a session that read only that table would retire its nodes
 * underneath the caller. The stacks are root visitors like every other root
 * source, so a sift pins what they hold.
 *
 * This test keeps to the precondition pinning cannot lift: no operation is in
 * flight (see sylvan_sift in include/sylvan/reorder.h).
 */
static int
test_temporary_refs_CALL(lace_worker *lace)
{
    uint32_t order[TEST_VARS];
    bdd_set_to_array(test_variables, order);

    MTBDD built = mtbdd_invalid;
    mtbdd_refs_pushptr(&built);
    test_assert(bdd_and_CALL(lace, &built, bdd_not(test_root[1]),
                             test_var[0]) == SYLVAN_OK);

    MTBDD *expected = (MTBDD*)malloc(TEST_ASSIGNMENTS * sizeof(MTBDD));
    test_assert(expected != NULL);
    for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
        uint8_t values[TEST_VARS];
        test_values_for(order, a, values);
        if (mtbdd_eval(&expected[a], built, test_variables, values, TEST_VARS)
            != SYLVAN_OK) {
            free(expected);
            test_assert(0);
        }
    }

    /* The value stack as well as the pointer stack: they are separate arrays
       and the visitor yields both. */
    const MTBDD handle = mtbdd_refs_push(built);

    const int status = sylvan_sift(NULL);
    int failed = status != SYLVAN_OK;
    failed = failed || built != handle;
    failed = failed || !mtbdd_is_valid_CALL(lace, built);

    bdd_set_to_array(test_variables, order);
    for (unsigned a = 0; a < TEST_ASSIGNMENTS && !failed; a++) {
        uint8_t values[TEST_VARS];
        MTBDD evaluated = mtbdd_invalid;
        test_values_for(order, a, values);
        failed = mtbdd_eval(&evaluated, built, test_variables, values,
                            TEST_VARS) != SYLVAN_OK ||
                 evaluated != expected[a];
    }

    free(expected);
    mtbdd_refs_pop(1);
    mtbdd_refs_popptr(1);
    test_assert(!failed);
    return test_check_roots_CALL(lace);
}

/**
 * ROOTS THAT HOLD NO DIAGRAM.
 *
 * mtbdd_invalid is what a protected variable or a pushed pointer holds before
 * an operation has filled it in -- every root in this file starts out that
 * way -- and marking skips it. A session has to skip it too: it pins through
 * the same visitors a collection marks through, and the invalid handle's
 * index is all ones, far past the end of the arena, so pinning has to check a
 * handle before it reads the record at its index. All four root sources yield
 * it here: the protected table, the value stack, the pointer stack, and a
 * consumer's visitor.
 */
static MTBDD test_invalid_visitor_roots[2];

static void
test_invalid_visitor(void *context, sylvan_roots_yield yield, void *arg)
{
    (void)context;
    yield(arg, test_invalid_visitor_roots, 2);
}

static int
test_invalid_roots_CALL(lace_worker *lace)
{
    MTBDD protected_slot = mtbdd_invalid;
    MTBDD pointed_slot = mtbdd_invalid;
    mtbdd_protect(&protected_slot);
    mtbdd_refs_pushptr(&pointed_slot);
    mtbdd_refs_push(mtbdd_invalid);
    test_invalid_visitor_roots[0] = mtbdd_invalid;
    test_invalid_visitor_roots[1] = test_root[0];
    test_assert(sylvan_gc_add_roots(test_invalid_visitor, NULL) == SYLVAN_OK);

    /* A collection tolerates them ... */
    sylvan_gc();
    /* ... and a session has to as well. */
    const int status = sylvan_sift(NULL);

    mtbdd_refs_pop(1);
    mtbdd_refs_popptr(1);
    test_assert(sylvan_gc_remove_roots(test_invalid_visitor, NULL)
                == SYLVAN_OK);
    mtbdd_unprotect(&protected_slot);

    test_assert(status == SYLVAN_OK);
    test_assert(protected_slot == mtbdd_invalid);
    test_assert(pointed_slot == mtbdd_invalid);
    return test_check_roots_CALL(lace);
}

/**
 * KEYS THAT COLLIDE COMPLETELY.
 *
 * The trie descends one hash bit per level and bounds the depth nowhere. That
 * is sound because the next slot is the branch pair of the node just passed,
 * not a function of (anchor, depth, hash): two keys with the same anchor and
 * the same 64-bit hash part the moment one passes the other, and a descent
 * ends within as many steps as the family has nodes. This drives the worst
 * case: a custom leaf type whose hash is a constant, so that every one of its
 * leaves shares the leaf anchor, the whole 64-bit hash and the fingerprint,
 * and the leaf family degenerates into one chain on every backend. Decision
 * nodes anchored at those leaves share their branch pairs with that chain, so
 * the trie's batched removal collects and rebuilds stretches of it. Lookups,
 * the rebuild after a collection, and a sift must all terminate and keep
 * every leaf at its handle.
 */
#define TEST_COLLIDING_LEAVES 192

static uint32_t test_collide_type = UINT32_MAX;

static uint64_t
test_collide_hash(void *context, uint64_t value, uint64_t seed)
{
    (void)context;
    (void)value;
    (void)seed;
    return UINT64_C(0x9e3779b97f4a7c15);
}

static int
test_collide_leaves_found(const MTBDD *handle)
{
    for (unsigned i = 0; i < TEST_COLLIDING_LEAVES; i++) {
        if (mtbdd_leaf(test_collide_type, (uint64_t)i) != handle[i]) return 0;
    }
    return 1;
}

static int
test_collide_values_kept(MTBDD f, const uint64_t *expected)
{
    uint32_t order[TEST_VARS];
    bdd_set_to_array(test_variables, order);
    for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
        uint8_t values[TEST_VARS];
        MTBDD evaluated = mtbdd_invalid;
        test_values_for(order, a, values);
        if (mtbdd_eval(&evaluated, f, test_variables, values, TEST_VARS)
            != SYLVAN_OK) {
            return 0;
        }
        if (mtbdd_leaf_value(evaluated) != expected[a]) return 0;
    }
    return 1;
}

static int
test_collisions_CALL(lace_worker *lace)
{
    if (test_collide_type == UINT32_MAX) {
        const sylvan_mt_type_descriptor descriptor = {
            "test.sift.collide",
            UINT64_C(0x2d4b6e1f0a3c5978),
            NULL,
            test_collide_hash,
            test_leaf_equal,
            test_leaf_clone,
            test_leaf_destroy,
            test_leaf_to_string,
            test_leaf_string_free
        };
        test_assert(sylvan_mt_register_type(&test_collide_type, &descriptor)
                    == SYLVAN_OK);
    }

    uint32_t order[TEST_VARS];
    bdd_set_to_array(test_variables, order);

    /* Assignment a goes to leaf a for the first TEST_COLLIDING_LEAVES
       assignments, by identity, and to the default leaf after that. */
    const uint64_t fallback = TEST_COLLIDING_LEAVES;
    MTBDD f = mtbdd_leaf(test_collide_type, fallback);
    test_assert(f != mtbdd_invalid);
    mtbdd_protect(&f);
    for (unsigned i = 0; i < TEST_COLLIDING_LEAVES; i++) {
        uint8_t cube[TEST_VARS];
        test_values_for(order, i, cube);
        const MTBDD leaf = mtbdd_leaf(test_collide_type, (uint64_t)i);
        test_assert(leaf != mtbdd_invalid);
        mtbdd_refs_push(leaf);
        const int status = mtbdd_set_cube(&f, f, test_variables, cube, leaf);
        mtbdd_refs_pop(1);
        test_assert(status == SYLVAN_OK);
    }

    MTBDD handle[TEST_COLLIDING_LEAVES];
    for (unsigned i = 0; i < TEST_COLLIDING_LEAVES; i++) {
        handle[i] = mtbdd_leaf(test_collide_type, (uint64_t)i);
    }
    uint64_t *expected = (uint64_t*)malloc(TEST_ASSIGNMENTS * sizeof(uint64_t));
    test_assert(expected != NULL);
    for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
        expected[a] = a < TEST_COLLIDING_LEAVES ? (uint64_t)a : fallback;
    }

    int failed = !test_collide_leaves_found(handle) ||
                 !test_collide_values_kept(f, expected);

    /* The rebuild after a collection walks the chain once per leaf. */
    sylvan_gc();
    failed = failed || !test_collide_leaves_found(handle) ||
             !test_collide_values_kept(f, expected);

    /* And a sift, every variable, through the batched index calls. */
    const MTBDD before = f;
    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));
    sylvan_sift_result result;
    failed = failed || sylvan_sift_report(&config, &result) != SYLVAN_OK;
    failed = failed || f != before || !mtbdd_is_valid_CALL(lace, f);
    failed = failed || !test_collide_leaves_found(handle) ||
             !test_collide_values_kept(f, expected);
    printf("  %u colliding leaves: %llu swaps\n", (unsigned)TEST_COLLIDING_LEAVES,
           (unsigned long long)result.swaps);

    free(expected);
    mtbdd_unprotect(&f);
    test_assert(!failed);
    return test_check_roots_CALL(lace);
}

/**
 * Put the order back to the identity with adjacent swaps, whatever the sifts
 * left behind: the pair tests below start from it, in which every pair is
 * together, and with it the ZDD refusal can be attributed to ZDD.
 *
 * Bubble sort, simulated first and then run as one swap sequence inside a
 * single session: a session collects when it opens, and one session per swap
 * would be one collection per swap.
 */
static int
test_restore_identity_order(void)
{
    const uint32_t count = sylvan_ranks_registered();
    if (count < 2) return 0;

    uint32_t *at = (uint32_t*)malloc((size_t)count * sizeof(uint32_t));
    uint32_t *ranks = (uint32_t*)malloc(
        (size_t)count * (count - 1u) / 2u * sizeof(uint32_t));
    test_assert(at != NULL && ranks != NULL);
    for (uint32_t rank = 0; rank < count; rank++) at[rank] = sylvan_identity_at(rank);

    size_t swaps = 0;
    for (uint32_t pass = 0; pass < count; pass++) {
        for (uint32_t rank = 0; rank + 1 < count; rank++) {
            if (at[rank] < at[rank + 1]) continue;
            const uint32_t upper = at[rank];
            at[rank] = at[rank + 1];
            at[rank + 1] = upper;
            ranks[swaps++] = rank;
        }
    }

    sylvan_varswap_result result;
    const int status = sylvan_test_varswap_sequence(ranks, swaps, &result);
    free(ranks);
    free(at);
    test_assert(status == SYLVAN_OK);
    test_assert(sylvan_ranks_is_identity());
    return 0;
}

/**
 * The swap trace (sylvan_sift_set_trace), which the benchmark harness compares
 * with CUDD's swap for swap. The hook has to see every swap and only swaps, so
 * its calls number exactly result.swaps; each call names the two variables
 * that stood at its rank and the next, so replaying the calls on the order
 * before the sift has to give the order after it; and the size each call
 * reports is the running forest size, so the last one is the final size.
 */
struct test_trace {
    uint32_t order[TEST_VARS];  /* the order before the sift, replayed */
    uint64_t swaps;             /* calls */
    uint64_t last_size;         /* the size the last call reported */
    uint64_t legs[4];           /* calls per sylvan_sift_leg */
    uint64_t wrong;             /* calls that do not fit the replayed order */
};

static void
test_trace_swap(void *context, uint32_t rank, uint32_t upper, uint32_t lower,
                uint64_t size, sylvan_sift_leg leg)
{
    struct test_trace *trace = (struct test_trace*)context;
    trace->swaps++;
    trace->last_size = size;
    if ((unsigned)leg < 4u) trace->legs[leg]++;
    else trace->wrong++;
    if (rank + 1u >= TEST_VARS || trace->order[rank] != upper ||
        trace->order[rank + 1u] != lower) {
        trace->wrong++;
        return;
    }
    trace->order[rank] = lower;
    trace->order[rank + 1u] = upper;
}

static int
test_trace_CALL(lace_worker *lace)
{
    /* From the split order, so that there is a lot to trace. */
    if (test_restore_identity_order()) return 1;

    struct test_trace trace;
    memset(&trace, 0, sizeof(trace));
    for (uint32_t rank = 0; rank < TEST_VARS; rank++) {
        trace.order[rank] = sylvan_identity_at(rank);
    }

    sylvan_sift_result result;
    sylvan_sift_set_trace(test_trace_swap, &trace);
    const int status = sylvan_sift_report(NULL, &result);
    sylvan_sift_set_trace(NULL, NULL);
    test_assert(status == SYLVAN_OK);
    printf("  %llu swaps traced (%llu up, %llu down, %llu back), "
           "%llu reported; last size %llu, final size %llu\n",
           (unsigned long long)trace.swaps,
           (unsigned long long)trace.legs[SYLVAN_SIFT_LEG_UP],
           (unsigned long long)trace.legs[SYLVAN_SIFT_LEG_DOWN],
           (unsigned long long)trace.legs[SYLVAN_SIFT_LEG_RETURN],
           (unsigned long long)result.swaps,
           (unsigned long long)trace.last_size,
           (unsigned long long)result.final_size);

    test_assert(result.swaps > 0);
    test_assert(trace.wrong == 0);
    test_assert(trace.swaps == result.swaps);
    test_assert(trace.last_size == result.final_size);
    for (uint32_t rank = 0; rank < TEST_VARS; rank++) {
        test_assert(trace.order[rank] == sylvan_identity_at(rank));
    }
    /* Ungrouped: every swap is search or return, and there was search. */
    test_assert(trace.legs[SYLVAN_SIFT_LEG_UP] +
                trace.legs[SYLVAN_SIFT_LEG_DOWN] > 0);
    test_assert(trace.legs[SYLVAN_SIFT_LEG_REUNITE] == 0);

    /* Switched off, the hook is not called again. */
    const uint64_t traced = trace.swaps;
    test_assert(sylvan_sift_report(NULL, &result) == SYLVAN_OK);
    test_assert(trace.swaps == traced);
    return test_check_roots_CALL(lace);
}

/**
 * PAIRS.
 *
 * bdd_rel_next and bdd_rel_prev read identity 2k as a current-state variable
 * and 2k+1 as its next-state copy, and want each pair directly together in
 * the order, the even one first (src/bdd.c says why). The relation below is
 * built that way over the same twelve variables as the other roots: six pairs
 * (s_k, t_k) = (2k, 2k+1), each t_k a random function of s_k and of the s of
 * another pair, as separate partitions and partly conjoined, plus a random
 * set of states. Every pair interacts, and the cross-pair dependencies mean
 * the order of the pairs matters, so a grouped sift has something to find.
 */
#define TEST_RELATIONS 9

static MTBDD test_relation[TEST_RELATIONS];
static MTBDD test_relation_value[TEST_RELATIONS][TEST_ASSIGNMENTS];

/* A fixed xorshift, so that the random relation is the same on every run. */
static uint64_t test_random_state = UINT64_C(0x2545f4914f6cdd1d);

static uint32_t
test_random(uint32_t bound)
{
    test_random_state ^= test_random_state << 13;
    test_random_state ^= test_random_state >> 7;
    test_random_state ^= test_random_state << 17;
    return (uint32_t)(test_random_state % bound);
}

static int
test_pairs_build_CALL(lace_worker *lace)
{
    BDD part = bdd_false;
    BDD cube = bdd_true;
    mtbdd_protect(&part);
    mtbdd_protect(&cube);

    /* t_k <-> op(s_k, s_j) for a random other pair j. */
    for (int k = 0; k < TEST_PAIRS; k++) {
        const BDD s = test_var[2 * k];
        const BDD t = test_var[2 * k + 1];
        const int other = (k + 1 + (int)test_random(TEST_PAIRS - 1)) % TEST_PAIRS;
        const BDD u = test_var[2 * other];
        switch (test_random(3)) {
        case 0:  test_assert(bdd_xor_CALL(lace, &part, s, u) == SYLVAN_OK); break;
        case 1:  test_assert(bdd_and_CALL(lace, &part, s, u) == SYLVAN_OK); break;
        default: test_assert(bdd_or_CALL(lace, &part, s, u) == SYLVAN_OK); break;
        }
        test_assert(bdd_xor_CALL(lace, &test_relation[k], t, part) == SYLVAN_OK);
        test_relation[k] = bdd_not(test_relation[k]);
    }

    /* Four partitions conjoined, which makes most variables interact. */
    test_relation[TEST_PAIRS] = bdd_true;
    for (int k = 0; k < 4; k++) {
        test_assert(bdd_and_CALL(lace, &test_relation[TEST_PAIRS],
                                 test_relation[TEST_PAIRS], test_relation[k])
                    == SYLVAN_OK);
    }

    /* A set of states: four random cubes of three current-state literals. */
    test_relation[TEST_PAIRS + 1] = bdd_false;
    for (int c = 0; c < 4; c++) {
        cube = bdd_true;
        for (int l = 0; l < 3; l++) {
            const BDD s = test_var[2 * (int)test_random(TEST_PAIRS)];
            const BDD literal = test_random(2) ? s : bdd_not(s);
            test_assert(bdd_and_CALL(lace, &cube, cube, literal) == SYLVAN_OK);
        }
        test_assert(bdd_or_CALL(lace, &test_relation[TEST_PAIRS + 1],
                                test_relation[TEST_PAIRS + 1], cube)
                    == SYLVAN_OK);
    }

    /* The relation restricted to those states, as an image computation
       would build it. */
    test_assert(bdd_and_CALL(lace, &test_relation[TEST_PAIRS + 2],
                             test_relation[TEST_PAIRS],
                             test_relation[TEST_PAIRS + 1]) == SYLVAN_OK);

    mtbdd_unprotect(&cube);
    mtbdd_unprotect(&part);

    uint32_t order[TEST_VARS];
    bdd_set_to_array(test_variables, order);
    for (int r = 0; r < TEST_RELATIONS; r++) {
        test_assert(mtbdd_is_valid_CALL(lace, test_relation[r]));
        for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
            uint8_t values[TEST_VARS];
            test_values_for(order, a, values);
            test_assert(mtbdd_eval(&test_relation_value[r][a], test_relation[r],
                                   test_variables, values, TEST_VARS)
                        == SYLVAN_OK);
        }
    }
    return 0;
}

/** The relation still denotes the same functions, and so does everything else. */
static int
test_pairs_check_CALL(lace_worker *lace)
{
    uint32_t order[TEST_VARS];
    bdd_set_to_array(test_variables, order);

    for (int r = 0; r < TEST_RELATIONS; r++) {
        test_assert(mtbdd_is_valid_CALL(lace, test_relation[r]));
        for (unsigned a = 0; a < TEST_ASSIGNMENTS; a++) {
            uint8_t values[TEST_VARS];
            MTBDD evaluated = mtbdd_invalid;
            test_values_for(order, a, values);
            test_assert(mtbdd_eval(&evaluated, test_relation[r], test_variables,
                                   values, TEST_VARS) == SYLVAN_OK);
            test_assert(evaluated == test_relation_value[r][a]);
        }
    }
    return test_check_roots_CALL(lace);
}

/**
 * Every registered pair together, the even identity directly above the odd
 * one -- asked of the order directly, and of the query, which must agree.
 */
static int
test_pairs_together(void)
{
    const uint32_t count = sylvan_ranks_registered();
    for (uint32_t even = 0; even + 1u < count; even += 2u) {
        test_assert(sylvan_identity_at(sylvan_rank_of(even) + 1u) == even + 1u);
    }
    test_assert(sylvan_sift_split_pairs() == 0);
    return 0;
}

static void
test_pairs_print(const char *label, const sylvan_sift_result *result)
{
    printf("  %s: %llu -> %llu nodes in %llu swaps over %u variables, "
           "%u pairs apart\n", label,
           (unsigned long long)result->initial_size,
           (unsigned long long)result->final_size,
           (unsigned long long)result->swaps,
           (unsigned)result->vars_sifted, (unsigned)result->split_pairs);
}

/**
 * A grouped sift from an order in which every pair is together leaves every
 * pair together, the forest no larger, and every function as it was.
 */
static uint64_t test_pairs_initial;

static int
test_pairs_grouped_CALL(lace_worker *lace)
{
    if (test_restore_identity_order()) return 1;
    if (test_pairs_together()) return 1;

    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));
    config.group_pairs = 1;

    sylvan_sift_result result;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_pairs_print("grouped, from the identity order  ", &result);
    test_print_order("order after");

    test_pairs_initial = result.initial_size;
    test_assert(result.refusals == 0);
    test_assert(result.split_pairs == 0);
    test_assert(result.final_size <= result.initial_size);
    test_assert(result.vars_sifted > 0 && result.vars_sifted % 2 == 0);
    if (test_pairs_together()) return 1;
    if (test_pairs_check_CALL(lace)) return 1;

    /* And again, from where it left off: still together, still no larger. */
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_assert(result.split_pairs == 0);
    test_assert(result.final_size <= result.initial_size);
    if (test_pairs_together()) return 1;
    return test_pairs_check_CALL(lace);
}

/**
 * An ungrouped sift from the same order is free to pull the pairs apart, and
 * on this forest it does; a grouped sift afterwards puts them back together
 * before it moves anything else.
 */
static int
test_pairs_reunite_CALL(lace_worker *lace)
{
    if (test_restore_identity_order()) return 1;

    sylvan_sift_result result;
    test_assert(sylvan_sift_report(NULL, &result) == SYLVAN_OK);
    test_pairs_print("ungrouped, from the identity order", &result);
    test_print_order("order after");
    test_assert(result.initial_size == test_pairs_initial);
    test_assert(result.split_pairs > 0);
    test_assert(result.split_pairs == sylvan_sift_split_pairs());
    if (test_pairs_check_CALL(lace)) return 1;

    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));
    config.group_pairs = 1;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_pairs_print("grouped, after the ungrouped sift ", &result);
    test_print_order("order after");
    test_assert(result.refusals == 0);
    test_assert(result.split_pairs == 0);
    if (test_pairs_together()) return 1;
    return test_pairs_check_CALL(lace);
}

/**
 * The budgets count in moves. Every block here is a pair -- the registered
 * identities are an even number -- so every move is four swaps: a swap budget
 * below four admits none, and one of n admits at most n swaps of search and
 * as many back. <max_vars> counts variables and never admits half a pair.
 */
static int
test_pairs_budgets_CALL(lace_worker *lace)
{
    test_assert(sylvan_ranks_registered() % 2 == 0);
    if (test_pairs_together()) return 1;

    sylvan_sift_config config;
    sylvan_sift_result result;

    for (uint32_t budget = 1; budget <= 9; budget++) {
        memset(&config, 0, sizeof(config));
        config.group_pairs = 1;
        config.max_swaps = budget;
        test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
        if (budget < 4) test_assert(result.swaps == 0);
        test_assert(result.swaps <= 2u * budget);
        test_assert(result.swaps % 4 == 0);
        test_assert(result.split_pairs == 0);
        if (test_pairs_together()) return 1;
        if (test_pairs_check_CALL(lace)) return 1;
    }

    for (uint32_t vars = 1; vars <= 5; vars++) {
        memset(&config, 0, sizeof(config));
        config.group_pairs = 1;
        config.max_vars = vars;
        test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
        test_assert(result.vars_sifted == (vars & ~1u));
        test_assert(result.split_pairs == 0);
        if (test_pairs_together()) return 1;
        if (test_pairs_check_CALL(lace)) return 1;
    }
    return 0;
}

/**
 * Refusals in the middle of a move. The first swap attempt of a sift from
 * this order is the first of a four-swap move, so the window picks out which
 * of that move's swaps the table refuses: the first (nothing has happened),
 * the second (the first is undone), the second and its undo (the move is
 * finished instead). Each leaves every pair together and is counted exactly.
 * Only refusing the finish as well leaves pairs apart -- the two the move was
 * crossing -- and ends the sift there, and the next grouped sift mends it.
 */
static int
test_pairs_refusals_CALL(lace_worker *lace)
{
    static const struct { uint64_t first, count; } windows[] = {
        { 1, 1 }, { 2, 1 }, { 2, 2 }
    };

    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));
    config.group_pairs = 1;
    sylvan_sift_result result;

    for (size_t w = 0; w < sizeof(windows) / sizeof(windows[0]); w++) {
        if (test_pairs_together()) return 1;
        sylvan_sift_set_refusal_window(windows[w].first, windows[w].count);
        const int status = sylvan_sift_report(&config, &result);
        sylvan_sift_set_refusal_window(0, 0);
        test_assert(status == SYLVAN_OK);
        test_assert(result.refusals == windows[w].count);
        test_assert(result.split_pairs == 0);
        if (test_pairs_together()) return 1;
        if (test_pairs_check_CALL(lace)) return 1;
    }

    sylvan_sift_set_refusal_window(2, 3);
    const int status = sylvan_sift_report(&config, &result);
    sylvan_sift_set_refusal_window(0, 0);
    test_assert(status == SYLVAN_OK);
    test_pairs_print("refused, undo refused, finish refused", &result);
    test_assert(result.refusals == 3);
    test_assert(result.split_pairs == 2);
    test_assert(sylvan_sift_split_pairs() == 2);
    test_assert(result.vars_sifted == 2);
    if (test_pairs_check_CALL(lace)) return 1;

    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_assert(result.refusals == 0);
    test_assert(result.split_pairs == 0);
    if (test_pairs_together()) return 1;
    return test_pairs_check_CALL(lace);
}

/**
 * A variable without a partner -- here one more identity, the odd one out --
 * is a block of its own: a pair crosses it in two swaps, and it crosses a
 * pair in two, and no pair is split around it. It counts one variable.
 */
static int
test_pairs_lone_CALL(lace_worker *lace)
{
    const uint32_t identity = sylvan_ranks_registered();
    test_assert(identity % 2 == 0);

    BDD lone = bdd_false, both = bdd_false, again = bdd_false;
    mtbdd_protect(&lone);
    mtbdd_protect(&both);
    mtbdd_protect(&again);
    test_assert(bdd_var_at_level(&lone, identity) == SYLVAN_OK);
    test_assert(mtbdd_node_variable(lone) == identity);
    test_assert(sylvan_ranks_registered() == identity + 1u);
    const BDD image = test_relation[TEST_PAIRS + 2];
    test_assert(bdd_and_CALL(lace, &both, lone, image) == SYLVAN_OK);

    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));
    config.group_pairs = 1;
    sylvan_sift_result result;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_pairs_print("grouped, with a variable alone    ", &result);
    test_assert(result.refusals == 0);
    test_assert(result.split_pairs == 0);
    test_assert(result.final_size <= result.initial_size);
    test_assert(result.vars_sifted % 2 == 1);
    if (test_pairs_together()) return 1;

    /* One function, one node: the conjunction recomputed under the new order
       is the node the sift rewrote in place. */
    test_assert(mtbdd_is_valid_CALL(lace, both));
    test_assert(bdd_and_CALL(lace, &again, lone, image) == SYLVAN_OK);
    test_assert(again == both);

    mtbdd_unprotect(&again);
    mtbdd_unprotect(&both);
    mtbdd_unprotect(&lone);
    return test_pairs_check_CALL(lace);
}

/**
 * A table with no free slot, filled the way test_varswap fills it: with
 * distinct live nodes, alternating with collections until a collection no
 * longer buys room, so that the sift's own opening collection buys none
 * either. Every block with nodes here is a pair, so every move takes at least
 * two swaps, and none can be promised room: the grouped sift refuses each
 * before its first swap. Nothing moves, every refusal is counted, and no pair
 * is apart.
 *
 * The filler is not leaves alone, as in test_varswap, because this table has
 * grown to millions of slots and the list backend chains every leaf on one
 * list, which would make filling it quadratic. Instead it is a few hundred
 * leaves, the nodes of a fresh variable over pairs of them, and the nodes of
 * the fresh variable above it over pairs of those: wide enough at every level
 * that no index chain grows long, as long as the loop also lets llchst, which
 * sizes the index of a level only at a safepoint, grow it as the level fills.
 * The two fresh variables are a pair of their own, together at the bottom of
 * the order. Every node is yielded by a root visitor rather than protected one
 * by one.
 */
#define TEST_FILL_LEAVES 256u
#define TEST_FILL_MIDDLE (TEST_FILL_LEAVES * (TEST_FILL_LEAVES - 1u))

static MTBDD *test_fill;
static size_t test_fill_count;

static void
test_fill_visitor(void *context, sylvan_roots_yield yield, void *arg)
{
    (void)context;
    if (test_fill_count != 0) yield(arg, test_fill, test_fill_count);
}

/** The <k>th ordered pair of distinct indices below <n>. */
static void
test_fill_pair(size_t k, size_t n, size_t *a, size_t *b)
{
    *a = k / (n - 1u);
    *b = k % (n - 1u);
    if (*b >= *a) (*b)++;
}

/** The next node of the filler, a new one every time. */
static MTBDD
test_fill_next(uint32_t upper, uint32_t lower)
{
    const size_t n = test_fill_count;
    size_t a, b;
    if (n < TEST_FILL_LEAVES) {
        return mtbdd_int64(INT64_C(1000000007) + (int64_t)n);
    }
    if (n < TEST_FILL_LEAVES + TEST_FILL_MIDDLE) {
        test_fill_pair(n - TEST_FILL_LEAVES, TEST_FILL_LEAVES, &a, &b);
        return mtbdd_make_node(lower, test_fill[a], test_fill[b]);
    }
    test_fill_pair(n - TEST_FILL_LEAVES - TEST_FILL_MIDDLE, TEST_FILL_MIDDLE,
                   &a, &b);
    return mtbdd_make_node(upper, test_fill[TEST_FILL_LEAVES + a],
                           test_fill[TEST_FILL_LEAVES + b]);
}

static int
test_pairs_full_table_CALL(lace_worker *lace)
{
    /* Two fresh identities, registered at the end of the order and so
       together, even one first. */
    const uint32_t upper = sylvan_ranks_registered();
    const uint32_t lower = upper + 1u;
    test_assert(upper % 2 == 0);
    BDD fresh = bdd_false;
    mtbdd_protect(&fresh);
    test_assert(bdd_var_at_level(&fresh, lower) == SYLVAN_OK);
    test_assert(mtbdd_node_variable(fresh) == lower);
    test_assert(sylvan_rank_of(upper) == upper);
    mtbdd_unprotect(&fresh);

    size_t capacity = 0;
    test_fill = NULL;
    test_fill_count = 0;
    test_assert(sylvan_gc_add_roots(test_fill_visitor, NULL) == SYLVAN_OK);

    for (int sweep = 0; ; sweep++) {
        test_assert(sweep < 12);
        sylvan_gc_enable();
        sylvan_gc();
        sylvan_gc_disable();

        size_t filled = 0, total = 0;
        sylvan_table_usage(&filled, &total);
        if (filled >= total) break;
        if (test_fill_count + (total - filled) > capacity) {
            capacity = test_fill_count + (total - filled);
            MTBDD *grown = (MTBDD*)realloc(test_fill, capacity * sizeof(MTBDD));
            test_assert(grown != NULL);
            test_fill = grown;
        }
        while (filled < total) {
            /* The safepoint every operation passes through, which calling
               mtbdd_make_node directly bypasses: it is where llchst grows the
               index of a level that has outgrown it between collections. */
            sylvan_gc_test(lace);
            const MTBDD node = test_fill_next(upper, lower);
            test_assert(node != mtbdd_invalid);
            test_fill[test_fill_count++] = node;
            filled++;
        }
    }
    test_assert(nodes_reserve(nodes, 1) != SYLVAN_OK);

    /* A session refuses to open while collection is disabled. Nothing below
       allocates a node before the sift, so no collection can run underneath
       it, and its own opening collection buys nothing. */
    sylvan_gc_enable();

    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));
    config.group_pairs = 1;
    sylvan_sift_result result;
    test_assert(sylvan_sift_report(&config, &result) == SYLVAN_OK);
    test_pairs_print("grouped, on a full table           ", &result);
    test_assert(result.swaps == 0);
    test_assert(result.refusals > 0);
    test_assert(result.final_size == result.initial_size);
    test_assert(result.split_pairs == 0);

    test_assert(sylvan_gc_remove_roots(test_fill_visitor, NULL) == SYLVAN_OK);
    free(test_fill);
    test_fill = NULL;
    test_fill_count = 0;
    sylvan_gc_enable();
    sylvan_gc();

    if (test_pairs_together()) return 1;
    return test_pairs_check_CALL(lace);
}

static int
test_pairs_CALL(lace_worker *lace)
{
    for (int r = 0; r < TEST_RELATIONS; r++) {
        test_relation[r] = mtbdd_invalid;
        mtbdd_protect(&test_relation[r]);
    }
    if (test_pairs_build_CALL(lace)) return 1;

    if (test_pairs_grouped_CALL(lace)) return 1;
    if (test_pairs_reunite_CALL(lace)) return 1;
    if (test_pairs_budgets_CALL(lace)) return 1;
    if (test_pairs_refusals_CALL(lace)) return 1;
    if (test_pairs_full_table_CALL(lace)) return 1;
    if (test_pairs_lone_CALL(lace)) return 1;

    for (int r = 0; r < TEST_RELATIONS; r++) mtbdd_unprotect(&test_relation[r]);
    return 0;
}

/**
 * The refusals. Both are decided before anything is touched, so the forest and
 * the order come through a refused call bit for bit.
 */
static int
test_guards_CALL(lace_worker *lace)
{
    sylvan_sift_result result;

    if (test_restore_identity_order()) return 1;
    if (test_check_roots_CALL(lace)) return 1;

    /* ZDD orders its nodes by identity rather than by rank and shares the
       arena, so a permuted order would be meaningless for it. */
    test_assert(zdd_init() == SYLVAN_OK);
    const uint64_t sifts = sylvan_sift_count();
    test_assert(sylvan_sift_report(NULL, &result) == SYLVAN_ERR_INVALID);
    /* Refused before its session opened: nothing to report, and no sift. */
    test_assert(result.initial_size == 0 && result.swaps == 0);
    test_assert(sylvan_sift_count() == sifts);
    test_assert(sylvan_sift(NULL) == SYLVAN_ERR_INVALID);

    test_assert(listdd_init() == SYLVAN_OK);
    test_assert(sylvan_sift(NULL) == SYLVAN_ERR_INVALID);

    return test_check_roots_CALL(lace);
}

TASK(int, runtests)

int
runtests_CALL(lace_worker *lace)
{
    /* Every session end recomputes the reference counts from the arena and
       compares them with the ones the swaps maintained. */
    sylvan_reorder_set_validate(1);

    const sylvan_mt_type_descriptor descriptor = {
        "test.sift.uint64",
        UINT64_C(0x5c1f7a3e9b264d18),
        NULL,
        test_leaf_hash,
        test_leaf_equal,
        test_leaf_clone,
        test_leaf_destroy,
        test_leaf_to_string,
        test_leaf_string_free
    };
    test_assert(sylvan_mt_register_type(&test_leaf_type, &descriptor)
                == SYLVAN_OK);

    for (uint32_t i = 0; i < TEST_VARS; i++) {
        test_var[i] = mtbdd_invalid;
        mtbdd_protect(&test_var[i]);
        test_assert(bdd_var_at_level(&test_var[i], i) == SYLVAN_OK);
        test_assert(mtbdd_node_variable(test_var[i]) == i);
    }

    if (nodes_reorder_caps(nodes) == 0) {
        sylvan_sift_result result;
        printf("Nodes table has no reorder primitives; testing the guard.\n");
        test_assert(sylvan_sift_report(NULL, &result) == SYLVAN_ERR_INVALID);
        test_assert(result.initial_size == 0 && result.final_size == 0);
        test_assert(result.swaps == 0 && result.vars_sifted == 0);
        test_assert(sylvan_sift(NULL) == SYLVAN_ERR_INVALID);
        for (uint32_t i = 0; i < TEST_VARS; i++) mtbdd_unprotect(&test_var[i]);
        return 0;
    }

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
    if (test_build_CALL(lace)) return 1;
    if (test_record()) return 1;

    printf("Testing that sifting a bad order makes the forest smaller.\n");
    if (test_reduces_CALL(lace)) return 1;

    printf("Testing that variables keep their identity across a sift.\n");
    if (test_identity_kept()) return 1;

    printf("Testing that sifting a sifted forest changes little.\n");
    if (test_idempotent_CALL(lace)) return 1;

    printf("Testing the configuration limits.\n");
    if (test_config_CALL(lace)) return 1;

    printf("Testing the swap trace.\n");
    if (test_trace_CALL(lace)) return 1;

    printf("Testing maps, integer leaves and custom leaves together.\n");
    if (test_mixed_CALL(lace)) return 1;

    printf("Testing roots a consumer's own visitor owns.\n");
    if (test_roots_visitor_CALL(lace)) return 1;

    printf("Testing roots held only on a temporary reference stack.\n");
    if (test_temporary_refs_CALL(lace)) return 1;

    printf("Testing roots that hold the invalid handle.\n");
    if (test_invalid_roots_CALL(lace)) return 1;

    printf("Testing keys whose whole hash collides.\n");
    if (test_collisions_CALL(lace)) return 1;

    printf("Testing %d rounds of queens(%d) sifted at every stage.\n",
           test_queens_rounds, TEST_QUEENS);
    if (test_queens_CALL(lace)) return 1;

    printf("Testing pairs kept together by grouped sifting.\n");
    if (test_pairs_CALL(lace)) return 1;

    printf("Testing the refusals.\n");
    if (test_guards_CALL(lace)) return 1;

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
    if (argc > 2) test_queens_rounds = atoi(argv[2]);
    if (test_queens_rounds < 0) test_queens_rounds = 0;

    /* 64 MB of worker stack rather than the 16 MB default. The queens relation
       is built by deeply recursive parallel operations, and a worker's C stack
       carries not the depth of the diagram but the depth of the tasks it has
       stolen and not yet finished; a sanitizer build, whose frames are several
       times larger, overflows the default. */
    lace_start((unsigned)workers, 0, 64u * 1024u * 1024u);

    sylvan_set_sizes(1LL << 16, 1LL << 22, 1LL << 16, 1LL << 22);
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
