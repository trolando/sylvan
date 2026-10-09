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
 * The relational products under a changing variable order.
 *
 * bdd_rel_next, bdd_rel_prev and bdd_transitive_closure pair the unprimed
 * variable 2k with the primed variable 2k+1. Their fused kernels are only
 * valid while every pair in play occupies adjacent ranks with 2k on top;
 * under any other order the operations must take their generic route and
 * still compute the same functions.
 *
 * Every operation is first computed under the identity order, where the
 * kernels apply, and checked against an explicit-state computation. The
 * results are kept, and the order is then changed in place, which keeps every
 * function and every handle, so the same operations must return the very same
 * handles: under an order that separates pairs and ranks primed variables
 * above their partners, under one that separates only some of them, after a
 * real sift, and under an order that groups the pairs again, where the
 * kernels apply once more.
 *
 * Five pairs encode the 32 states. A sixth pair, x = 10 and y = 11, is never
 * in play for the calls that use it; it stands for the variables outside the
 * pairs, which rel_next keeps in its first operand and quantifies in its
 * second, and rel_prev the other way round.
 *
 * Usage: test_relprod [workers]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lace.h>
#include <sylvan/internal.h>

#include "test_assert.h"

#define PAIRS 5
#define STATES (1u << PAIRS)
#define VARS (2 * PAIRS + 2)
#define VAR_X (2 * PAIRS)
#define VAR_Y (2 * PAIRS + 1)

/* The pairs a partial relation acts on, and the mask of their state bits. */
static const int part_bits[3] = {0, 2, 3};
#define PART_MASK ((1u << 0) | (1u << 2) | (1u << 3))

#define RESULTS 14

/* Deterministic small PRNG, so a failure is reproducible. */
static uint64_t test_random_state = UINT64_C(0x2545f4914f6cdd1d);

static uint64_t
test_random(void)
{
    test_random_state ^= test_random_state << 13;
    test_random_state ^= test_random_state >> 7;
    test_random_state ^= test_random_state << 17;
    return test_random_state;
}

/* Explicit sets of states are bitmasks; a relation maps a state to a set. */
typedef uint32_t relation[STATES];

static uint32_t
image(uint32_t set, const relation rel)
{
    uint32_t result = 0;
    for (unsigned s = 0; s < STATES; s++) {
        if ((set >> s) & 1) result |= rel[s];
    }
    return result;
}

static uint32_t
preimage(uint32_t set, const relation rel)
{
    uint32_t result = 0;
    for (unsigned s = 0; s < STATES; s++) {
        if (rel[s] & set) result |= 1u << s;
    }
    return result;
}

/** <out> := first, then second. */
static void
compose(relation out, const relation first, const relation second)
{
    for (unsigned s = 0; s < STATES; s++) {
        out[s] = 0;
        for (unsigned m = 0; m < STATES; m++) {
            if ((first[s] >> m) & 1) out[s] |= second[m];
        }
    }
}

/** <out> := the transitive (not reflexive) closure, by Warshall. */
static void
closure(relation out, const relation rel)
{
    memcpy(out, rel, sizeof(relation));
    for (unsigned middle = 0; middle < STATES; middle++) {
        for (unsigned s = 0; s < STATES; s++) {
            if ((out[s] >> middle) & 1) out[s] |= out[middle];
        }
    }
}

static void
union_of(relation out, const relation a, const relation b)
{
    for (unsigned s = 0; s < STATES; s++) out[s] = a[s] | b[s];
}

/** A sparse random relation: most states have one or two successors. */
static void
random_relation(relation out)
{
    for (unsigned s = 0; s < STATES; s++) {
        out[s] = 0;
        const unsigned successors = (unsigned)(test_random() % 3);
        for (unsigned i = 0; i < successors; i++) {
            out[s] |= 1u << (test_random() % STATES);
        }
    }
}

static uint32_t
random_set(void)
{
    return (uint32_t)test_random() & (uint32_t)test_random();
}

/** The bits of the partial pairs of state <s>, packed. */
static unsigned
part_of(unsigned s)
{
    unsigned packed = 0;
    for (int i = 0; i < 3; i++) packed |= ((s >> part_bits[i]) & 1u) << i;
    return packed;
}

/** State <s> with the bits of the partial pairs replaced by <packed>. */
static unsigned
with_part(unsigned s, unsigned packed)
{
    s &= ~PART_MASK;
    for (int i = 0; i < 3; i++) s |= ((packed >> i) & 1u) << part_bits[i];
    return s;
}

/** A partial relation on the three pairs, extended by the identity. */
static void
extend_partial(relation out, const uint8_t partial[8])
{
    for (unsigned s = 0; s < STATES; s++) {
        out[s] = 0;
        for (unsigned to = 0; to < 8; to++) {
            if ((partial[part_of(s)] >> to) & 1) out[s] |= 1u << with_part(s, to);
        }
    }
}

/* Variable sets, kept protected for the whole run. */
static BDDSET set_unprimed;   /* 0 2 4 6 8 */
static BDDSET set_primed;     /* 1 3 5 7 9 */
static BDDSET set_pairs;      /* 0 .. 9 */
static BDDSET set_part;       /* both variables of pairs 0, 2 and 3 */
static BDD var_x, var_y;

/* The operands, and the results computed under the identity order. */
static BDD in_s1, in_s2, in_r, in_r2, in_part;
static BDD in_sx, in_ry, in_rx, in_sy, in_r3y;
static BDD result[RESULTS];

/**
 * Add the minterm <assignment>, whose bit <i> is the value of the variable
 * with identity <i>, over the variables of <set> to <dd>. The cube follows
 * the set, whose spine is in rank order, so this holds under any order.
 */
TASK(int, add_minterm, BDD*, dd, BDDSET, set, uint32_t, assignment)
int
add_minterm_CALL(lace_worker *lace, BDD *dd, BDDSET set, uint32_t assignment)
{
    uint32_t order[VARS];
    uint8_t cube[VARS];
    const size_t count = bdd_set_count(set);
    bdd_set_to_array(set, order);
    for (size_t i = 0; i < count; i++) cube[i] = (assignment >> order[i]) & 1u;
    return bdd_or_cube_CALL(lace, dd, *dd, set, cube);
}

/** The set of states <states>, over the unprimed variables. */
TASK(int, set_to_bdd, BDD*, result, uint32_t, states)
int
set_to_bdd_CALL(lace_worker *lace, BDD *result, uint32_t states)
{
    *result = bdd_false;
    for (unsigned s = 0; s < STATES; s++) {
        if (!((states >> s) & 1)) continue;
        uint32_t assignment = 0;
        for (int k = 0; k < PAIRS; k++) assignment |= ((s >> k) & 1u) << (2 * k);
        const int status = add_minterm_CALL(lace, result, set_unprimed, assignment);
        if (status != SYLVAN_OK) return status;
    }
    return SYLVAN_OK;
}

/** The relation <rel>, over all variables of the five pairs. */
TASK(int, relation_to_bdd, BDD*, result, const uint32_t*, rel)
int
relation_to_bdd_CALL(lace_worker *lace, BDD *result, const uint32_t *rel)
{
    *result = bdd_false;
    for (unsigned from = 0; from < STATES; from++) {
        for (unsigned to = 0; to < STATES; to++) {
            if (!((rel[from] >> to) & 1)) continue;
            uint32_t assignment = 0;
            for (int k = 0; k < PAIRS; k++) {
                assignment |= ((from >> k) & 1u) << (2 * k);
                assignment |= ((to >> k) & 1u) << (2 * k + 1);
            }
            const int status = add_minterm_CALL(lace, result, set_pairs, assignment);
            if (status != SYLVAN_OK) return status;
        }
    }
    return SYLVAN_OK;
}

/** The partial relation, over the variables of its three pairs only. */
TASK(int, partial_to_bdd, BDD*, result, const uint8_t*, partial)
int
partial_to_bdd_CALL(lace_worker *lace, BDD *result, const uint8_t *partial)
{
    *result = bdd_false;
    for (unsigned from = 0; from < 8; from++) {
        for (unsigned to = 0; to < 8; to++) {
            if (!((partial[from] >> to) & 1)) continue;
            uint32_t assignment = 0;
            for (int i = 0; i < 3; i++) {
                assignment |= ((from >> i) & 1u) << (2 * part_bits[i]);
                assignment |= ((to >> i) & 1u) << (2 * part_bits[i] + 1);
            }
            const int status = add_minterm_CALL(lace, result, set_part, assignment);
            if (status != SYLVAN_OK) return status;
        }
    }
    return SYLVAN_OK;
}

/** (a & p) | (b & q), for the operands and expectations with x and y. */
TASK(int, select2, BDD*, result, BDD, a, BDD, p, BDD, b, BDD, q)
int
select2_CALL(lace_worker *lace, BDD *result, BDD a, BDD p, BDD b, BDD q)
{
    BDD left = mtbdd_invalid, right = mtbdd_invalid;
    mtbdd_refs_pushptr(&left);
    mtbdd_refs_pushptr(&right);
    int status = bdd_and_CALL(lace, &left, a, p);
    if (status == SYLVAN_OK) status = bdd_and_CALL(lace, &right, b, q);
    if (status == SYLVAN_OK) status = bdd_or_CALL(lace, result, left, right);
    mtbdd_refs_popptr(2);
    return status;
}

/** Run every operation; test_build checks each result by its index. */
TASK(int, compute_all, BDD*, out)
int
compute_all_CALL(lace_worker *lace, BDD *out)
{
    int status = SYLVAN_OK;
#define RUN_OP(call) if (status == SYLVAN_OK) status = (call)
    RUN_OP(bdd_rel_next_CALL(lace, &out[0], in_s1, in_r, bdd_false));
    RUN_OP(bdd_rel_next_CALL(lace, &out[1], in_s1, in_r, set_unprimed));
    RUN_OP(bdd_rel_prev_CALL(lace, &out[2], in_r, in_s1, bdd_false));
    RUN_OP(bdd_rel_prev_CALL(lace, &out[3], in_r, in_s1, set_unprimed));
    RUN_OP(bdd_rel_prev_CALL(lace, &out[4], in_r, in_r2, bdd_false));
    RUN_OP(bdd_transitive_closure_CALL(lace, &out[5], in_r));
    RUN_OP(bdd_rel_next_CALL(lace, &out[6], in_s1, in_part, set_part));
    RUN_OP(bdd_rel_prev_CALL(lace, &out[7], in_part, in_s1, set_part));
    RUN_OP(bdd_rel_next_CALL(lace, &out[8], in_sx, in_ry, set_unprimed));
    RUN_OP(bdd_rel_prev_CALL(lace, &out[9], in_rx, in_sy, set_unprimed));
    RUN_OP(bdd_rel_prev_CALL(lace, &out[10], in_rx, in_r3y, set_unprimed));
    RUN_OP(bdd_rel_prev_CALL(lace, &out[11], in_part, in_r2, set_part));
    RUN_OP(bdd_rel_next_CALL(lace, &out[12], in_s1, in_r, set_primed));
    RUN_OP(bdd_rel_prev_CALL(lace, &out[13], in_r, in_s1, set_primed));
#undef RUN_OP
    return status;
}

/** Recompute everything and demand the results kept from the first run. */
TASK(int, test_same)
int
test_same_CALL(lace_worker *lace)
{
    BDD again[RESULTS];
    for (int i = 0; i < RESULTS; i++) {
        again[i] = mtbdd_invalid;
        mtbdd_refs_pushptr(&again[i]);
    }
    test_assert(compute_all_CALL(lace, again) == SYLVAN_OK);
    for (int i = 0; i < RESULTS; i++) {
        if (again[i] != result[i]) {
            fprintf(stderr, "operation %d changed its result\n", i);
        }
        test_assert(again[i] == result[i]);
        test_assert(mtbdd_is_valid_CALL(lace, again[i]));
    }
    mtbdd_refs_popptr(RESULTS);
    return 0;
}

/** Print the order as identities from the top down. */
static void
print_order(const char *label)
{
    printf("  %s:", label);
    for (uint32_t rank = 0; rank < VARS; rank++) {
        printf(" %u", (unsigned)sylvan_identity_at(rank));
    }
    printf("\n");
}

/**
 * Move to the order <target> (identities from the top down) by adjacent
 * swaps, planned on a copy of the current order and run as one sequence.
 */
static int
move_to(const uint32_t target[VARS])
{
    uint32_t want[VARS];
    uint32_t order[VARS];
    uint32_t swaps[VARS * VARS];
    size_t count = 0;
    for (uint32_t rank = 0; rank < VARS; rank++) {
        want[target[rank]] = rank;
        order[rank] = sylvan_identity_at(rank);
    }
    for (int changed = 1; changed;) {
        changed = 0;
        for (uint32_t rank = 0; rank + 1 < VARS; rank++) {
            if (want[order[rank]] > want[order[rank + 1]]) {
                const uint32_t moved = order[rank];
                order[rank] = order[rank + 1];
                order[rank + 1] = moved;
                swaps[count++] = rank;
                changed = 1;
            }
        }
    }
    if (count > 0) {
        test_assert(sylvan_test_varswap_sequence(swaps, count, NULL) == SYLVAN_OK);
    }
    for (uint32_t rank = 0; rank < VARS; rank++) {
        test_assert(sylvan_identity_at(rank) == target[rank]);
    }
    return 0;
}

/** Build the operands and check the first results against explicit ones. */
TASK(int, test_build)
int
test_build_CALL(lace_worker *lace)
{
    const uint32_t s1 = random_set();
    const uint32_t s2 = random_set();
    relation r, r2, r3, r4, full_part, rq;
    random_relation(r);
    random_relation(r2);
    random_relation(r3);
    random_relation(r4);
    uint8_t partial[8];
    for (int i = 0; i < 8; i++) partial[i] = (uint8_t)(test_random() & test_random());
    extend_partial(full_part, partial);
    union_of(rq, r, r2);

    BDD t0 = mtbdd_invalid, t1 = mtbdd_invalid, t2 = mtbdd_invalid;
    mtbdd_refs_pushptr(&t0);
    mtbdd_refs_pushptr(&t1);
    mtbdd_refs_pushptr(&t2);

    test_assert(set_to_bdd_CALL(lace, &in_s1, s1) == SYLVAN_OK);
    test_assert(set_to_bdd_CALL(lace, &in_s2, s2) == SYLVAN_OK);
    test_assert(relation_to_bdd_CALL(lace, &in_r, r) == SYLVAN_OK);
    test_assert(relation_to_bdd_CALL(lace, &in_r2, r2) == SYLVAN_OK);
    test_assert(partial_to_bdd_CALL(lace, &in_part, partial) == SYLVAN_OK);

    /* sx = s1 & x | s2 & !x, ry = r & y | r2 & !y & x, rx = r & x | r2 & !x,
       sy = s1 & y | s2 & x, r3y = r3 & y | r4 & x. */
    test_assert(select2_CALL(lace, &in_sx, in_s1, var_x, in_s2, bdd_not(var_x)) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &t0, bdd_not(var_y), var_x) == SYLVAN_OK);
    test_assert(select2_CALL(lace, &in_ry, in_r, var_y, in_r2, t0) == SYLVAN_OK);
    test_assert(select2_CALL(lace, &in_rx, in_r, var_x, in_r2, bdd_not(var_x)) == SYLVAN_OK);
    test_assert(select2_CALL(lace, &in_sy, in_s1, var_y, in_s2, var_x) == SYLVAN_OK);
    test_assert(relation_to_bdd_CALL(lace, &t1, r3) == SYLVAN_OK);
    test_assert(relation_to_bdd_CALL(lace, &t2, r4) == SYLVAN_OK);
    test_assert(select2_CALL(lace, &in_r3y, t1, var_y, t2, var_x) == SYLVAN_OK);

    /* Under the identity order every call takes its kernel. */
    test_assert(bdd_rel_pairs_adjacent(bdd_false));
    test_assert(compute_all_CALL(lace, result) == SYLVAN_OK);

    relation scratch, scratch2;
    test_assert(set_to_bdd_CALL(lace, &t0, image(s1, r)) == SYLVAN_OK);
    test_assert(result[0] == t0);
    test_assert(result[1] == t0);
    test_assert(result[12] == t0);
    test_assert(set_to_bdd_CALL(lace, &t0, preimage(s1, r)) == SYLVAN_OK);
    test_assert(result[2] == t0);
    test_assert(result[3] == t0);
    test_assert(result[13] == t0);
    compose(scratch, r, r2);
    test_assert(relation_to_bdd_CALL(lace, &t0, scratch) == SYLVAN_OK);
    test_assert(result[4] == t0);
    closure(scratch, r);
    test_assert(relation_to_bdd_CALL(lace, &t0, scratch) == SYLVAN_OK);
    test_assert(result[5] == t0);
    test_assert(set_to_bdd_CALL(lace, &t0, image(s1, full_part)) == SYLVAN_OK);
    test_assert(result[6] == t0);
    test_assert(set_to_bdd_CALL(lace, &t0, preimage(s1, full_part)) == SYLVAN_OK);
    test_assert(result[7] == t0);

    /* With x and y outside the pairs: ry and rx both quantify to r | r2. */
    test_assert(set_to_bdd_CALL(lace, &t1, image(s1, rq)) == SYLVAN_OK);
    test_assert(set_to_bdd_CALL(lace, &t2, image(s2, rq)) == SYLVAN_OK);
    test_assert(select2_CALL(lace, &t0, t1, var_x, t2, bdd_not(var_x)) == SYLVAN_OK);
    test_assert(result[8] == t0);
    test_assert(set_to_bdd_CALL(lace, &t1, preimage(s1, rq)) == SYLVAN_OK);
    test_assert(set_to_bdd_CALL(lace, &t2, preimage(s2, rq)) == SYLVAN_OK);
    test_assert(select2_CALL(lace, &t0, t1, var_y, t2, var_x) == SYLVAN_OK);
    test_assert(result[9] == t0);
    compose(scratch, rq, r3);
    compose(scratch2, rq, r4);
    test_assert(relation_to_bdd_CALL(lace, &t1, scratch) == SYLVAN_OK);
    test_assert(relation_to_bdd_CALL(lace, &t2, scratch2) == SYLVAN_OK);
    test_assert(select2_CALL(lace, &t0, t1, var_y, t2, var_x) == SYLVAN_OK);
    test_assert(result[10] == t0);

    /* The partial relation keeps the other pairs of r2 as they are. */
    compose(scratch, full_part, r2);
    test_assert(relation_to_bdd_CALL(lace, &t0, scratch) == SYLVAN_OK);
    test_assert(result[11] == t0);

    for (int i = 0; i < RESULTS; i++) {
        test_assert(mtbdd_is_valid_CALL(lace, result[i]));
    }
    mtbdd_refs_popptr(3);
    return 0;
}

TASK(int, runtests)
int
runtests_CALL(lace_worker *lace)
{
    sylvan_reorder_set_validate(1);

    BDD *roots[] = {
        &set_unprimed, &set_primed, &set_pairs, &set_part, &var_x, &var_y,
        &in_s1, &in_s2, &in_r, &in_r2, &in_part,
        &in_sx, &in_ry, &in_rx, &in_sy, &in_r3y,
    };
    const size_t root_count = sizeof(roots) / sizeof(roots[0]);
    for (size_t i = 0; i < root_count; i++) {
        *roots[i] = mtbdd_invalid;
        mtbdd_protect(roots[i]);
    }
    for (int i = 0; i < RESULTS; i++) {
        result[i] = mtbdd_invalid;
        mtbdd_protect(&result[i]);
    }

    BDD variable = mtbdd_invalid;
    mtbdd_protect(&variable);
    for (uint32_t i = 0; i < VARS; i++) {
        test_assert(bdd_var_at_level(&variable, i) == SYLVAN_OK);
    }
    mtbdd_unprotect(&variable);
    test_assert(bdd_var_at_level(&var_x, VAR_X) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&var_y, VAR_Y) == SYLVAN_OK);

    uint32_t identities[2 * PAIRS];
    for (uint32_t i = 0; i < PAIRS; i++) identities[i] = 2 * i;
    test_assert(bdd_set_from_array(&set_unprimed, identities, PAIRS) == SYLVAN_OK);
    for (uint32_t i = 0; i < PAIRS; i++) identities[i] = 2 * i + 1;
    test_assert(bdd_set_from_array(&set_primed, identities, PAIRS) == SYLVAN_OK);
    for (uint32_t i = 0; i < 2 * PAIRS; i++) identities[i] = i;
    test_assert(bdd_set_from_array(&set_pairs, identities, 2 * PAIRS) == SYLVAN_OK);
    for (int i = 0; i < 3; i++) {
        identities[2 * i] = (uint32_t)(2 * part_bits[i]);
        identities[2 * i + 1] = (uint32_t)(2 * part_bits[i] + 1);
    }
    test_assert(bdd_set_from_array(&set_part, identities, 6) == SYLVAN_OK);

    printf("Testing the relational products under the identity order.\n");
    if (test_build_CALL(lace)) return 1;
    if (test_same_CALL(lace)) return 1;

    if (nodes_reorder_caps(nodes) == 0) {
        printf("Nodes table has no reorder primitives; not reordering.\n");
    } else {
        /* Pair 0 adjacent but primed first, pairs 1 to 4 separated, two of
           them primed first, and the outside pair adjacent. */
        static const uint32_t split[VARS] = {1, 0, 2, 5, 3, 4, 9, 6, 8, 7, 10, 11};
        printf("Testing an order that separates pairs.\n");
        if (move_to(split)) return 1;
        print_order("order");
        test_assert(!bdd_rel_pairs_adjacent(bdd_false));
        test_assert(!bdd_rel_pairs_adjacent(set_unprimed));
        test_assert(!bdd_rel_pairs_adjacent(set_primed));
        test_assert(!bdd_rel_pairs_adjacent(set_part));
        if (test_same_CALL(lace)) return 1;

        /* The partial pairs 0, 2 and 3 adjacent, pairs 1 and 4 not. */
        static const uint32_t partly[VARS] = {0, 1, 3, 4, 5, 2, 6, 7, 9, 10, 11, 8};
        printf("Testing an order that separates only some pairs.\n");
        if (move_to(partly)) return 1;
        print_order("order");
        test_assert(!bdd_rel_pairs_adjacent(bdd_false));
        test_assert(!bdd_rel_pairs_adjacent(set_unprimed));
        test_assert(bdd_rel_pairs_adjacent(set_part));
        if (test_same_CALL(lace)) return 1;

        printf("Testing after a sift.\n");
        const uint64_t sifts = sylvan_sift_count();
        test_assert(sylvan_sift(NULL) == SYLVAN_OK);
        test_assert(sylvan_sift_count() == sifts + 1);
        print_order("order");
        if (test_same_CALL(lace)) return 1;

        /* Every pair adjacent with its unprimed variable first, but in a
           different order than they started in. */
        static const uint32_t grouped[VARS] = {8, 9, 0, 1, 10, 11, 4, 5, 2, 3, 6, 7};
        printf("Testing an order that groups the pairs again.\n");
        if (move_to(grouped)) return 1;
        print_order("order");
        test_assert(bdd_rel_pairs_adjacent(bdd_false));
        test_assert(bdd_rel_pairs_adjacent(set_unprimed));
        test_assert(bdd_rel_pairs_adjacent(set_part));
        if (test_same_CALL(lace)) return 1;
    }

    for (int i = 0; i < RESULTS; i++) mtbdd_unprotect(&result[i]);
    for (size_t i = 0; i < root_count; i++) mtbdd_unprotect(roots[i]);
    return 0;
}

int
main(int argc, char **argv)
{
    int workers = 4;
    if (argc > 1) workers = atoi(argv[1]);
    if (workers < 1) workers = 1;

    lace_start((unsigned)workers, 0, 0);

    sylvan_set_sizes(1LL << 16, 1LL << 20, 1LL << 16, 1LL << 18);
    if (sylvan_init_package() != SYLVAN_OK ||
        mtbdd_init() != SYLVAN_OK) {
        fprintf(stderr, "Sylvan initialization failed.\n");
        return 1;
    }

    const int result_status = runtests();

    sylvan_quit();
    lace_stop();
    return result_status;
}
