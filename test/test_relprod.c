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

/* Identity-order explicit-state oracle from sylvan-2. Sylvan 1.x has no
 * variable reordering. Five pairs encode 32 states; x and y are outside
 * the pairs and exercise the operands' different quantification rules. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lace.h>
#include <sylvan.h>

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
 * with index <i>, over the variables of <set> to <dd>. The cube follows
 * the set in increasing variable order.
 */
static void
add_minterm(BDD *dd, BDDSET set, uint32_t assignment)
{
    uint32_t order[VARS];
    uint8_t cube[VARS];
    const size_t count = sylvan_set_count(set);
    sylvan_set_toarray(set, order);
    for (size_t i = 0; i < count; i++) cube[i] = (assignment >> order[i]) & 1u;
    *dd = sylvan_union_cube(*dd, set, cube);
}

/** The set of states <states>, over the unprimed variables. */
static void
set_to_bdd(BDD *result, uint32_t states)
{
    *result = sylvan_false;
    for (unsigned s = 0; s < STATES; s++) {
        if (!((states >> s) & 1)) continue;
        uint32_t assignment = 0;
        for (int k = 0; k < PAIRS; k++) assignment |= ((s >> k) & 1u) << (2 * k);
        add_minterm(result, set_unprimed, assignment);
    }
}

/** The relation <rel>, over all variables of the five pairs. */
static void
relation_to_bdd(BDD *result, const uint32_t *rel)
{
    *result = sylvan_false;
    for (unsigned from = 0; from < STATES; from++) {
        for (unsigned to = 0; to < STATES; to++) {
            if (!((rel[from] >> to) & 1)) continue;
            uint32_t assignment = 0;
            for (int k = 0; k < PAIRS; k++) {
                assignment |= ((from >> k) & 1u) << (2 * k);
                assignment |= ((to >> k) & 1u) << (2 * k + 1);
            }
            add_minterm(result, set_pairs, assignment);
        }
    }
}

/** The partial relation, over the variables of its three pairs only. */
static void
partial_to_bdd(BDD *result, const uint8_t *partial)
{
    *result = sylvan_false;
    for (unsigned from = 0; from < 8; from++) {
        for (unsigned to = 0; to < 8; to++) {
            if (!((partial[from] >> to) & 1)) continue;
            uint32_t assignment = 0;
            for (int i = 0; i < 3; i++) {
                assignment |= ((from >> i) & 1u) << (2 * part_bits[i]);
                assignment |= ((to >> i) & 1u) << (2 * part_bits[i] + 1);
            }
            add_minterm(result, set_part, assignment);
        }
    }
}

/** (a & p) | (b & q), for the operands and expectations with x and y. */
static void
select2(BDD *result, BDD a, BDD p, BDD b, BDD q)
{
    BDD left = sylvan_false, right = sylvan_false;
    mtbdd_refs_pushptr(&left);
    mtbdd_refs_pushptr(&right);
    left = sylvan_and(a, p);
    right = sylvan_and(b, q);
    *result = sylvan_or(left, right);
    mtbdd_refs_popptr(2);
}

/** Run every operation; test_build checks each result by its index. */
static void
compute_all(BDD *out)
{
    out[0] = sylvan_relnext(in_s1, in_r, sylvan_false);
    out[1] = sylvan_relnext(in_s1, in_r, set_unprimed);
    out[2] = sylvan_relprev(in_r, in_s1, sylvan_false);
    out[3] = sylvan_relprev(in_r, in_s1, set_unprimed);
    out[4] = sylvan_relprev(in_r, in_r2, sylvan_false);
    out[5] = sylvan_closure(in_r);
    out[6] = sylvan_relnext(in_s1, in_part, set_part);
    out[7] = sylvan_relprev(in_part, in_s1, set_part);
    out[8] = sylvan_relnext(in_sx, in_ry, set_unprimed);
    out[9] = sylvan_relprev(in_rx, in_sy, set_unprimed);
    out[10] = sylvan_relprev(in_rx, in_r3y, set_unprimed);
    out[11] = sylvan_relprev(in_part, in_r2, set_part);
    out[12] = sylvan_relnext(in_s1, in_r, set_primed);
    out[13] = sylvan_relprev(in_r, in_s1, set_primed);
}

/** Recompute everything and demand the results kept from the first run. */
static int
test_same(void)
{
    BDD again[RESULTS];
    for (int i = 0; i < RESULTS; i++) {
        again[i] = sylvan_false;
        mtbdd_refs_pushptr(&again[i]);
    }
    compute_all(again);
    for (int i = 0; i < RESULTS; i++) {
        if (again[i] != result[i]) {
            fprintf(stderr, "operation %d changed its result\n", i);
        }
        test_assert(again[i] == result[i]);
        test_assert(mtbdd_test_isvalid(again[i]));
    }
    mtbdd_refs_popptr(RESULTS);
    return 0;
}

/** Build the operands and check the first results against explicit ones. */
static int
test_build(void)
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

    BDD t0 = sylvan_false, t1 = sylvan_false, t2 = sylvan_false;
    mtbdd_refs_pushptr(&t0);
    mtbdd_refs_pushptr(&t1);
    mtbdd_refs_pushptr(&t2);

    set_to_bdd(&in_s1, s1);
    set_to_bdd(&in_s2, s2);
    relation_to_bdd(&in_r, r);
    relation_to_bdd(&in_r2, r2);
    partial_to_bdd(&in_part, partial);

    /* sx = s1 & x | s2 & !x, ry = r & y | r2 & !y & x, rx = r & x | r2 & !x,
       sy = s1 & y | s2 & x, r3y = r3 & y | r4 & x. */
    select2(&in_sx, in_s1, var_x, in_s2, sylvan_not(var_x));
    t0 = sylvan_and(sylvan_not(var_y), var_x);
    select2(&in_ry, in_r, var_y, in_r2, t0);
    select2(&in_rx, in_r, var_x, in_r2, sylvan_not(var_x));
    select2(&in_sy, in_s1, var_y, in_s2, var_x);
    relation_to_bdd(&t1, r3);
    relation_to_bdd(&t2, r4);
    select2(&in_r3y, t1, var_y, t2, var_x);

    /* Under the identity order every call takes its kernel. */
    compute_all(result);

    relation scratch, scratch2;
    set_to_bdd(&t0, image(s1, r));
    test_assert(result[0] == t0);
    test_assert(result[1] == t0);
    test_assert(result[12] == t0);
    set_to_bdd(&t0, preimage(s1, r));
    test_assert(result[2] == t0);
    test_assert(result[3] == t0);
    test_assert(result[13] == t0);
    compose(scratch, r, r2);
    relation_to_bdd(&t0, scratch);
    test_assert(result[4] == t0);
    closure(scratch, r);
    relation_to_bdd(&t0, scratch);
    test_assert(result[5] == t0);
    set_to_bdd(&t0, image(s1, full_part));
    test_assert(result[6] == t0);
    set_to_bdd(&t0, preimage(s1, full_part));
    test_assert(result[7] == t0);

    /* With x and y outside the pairs: ry and rx both quantify to r | r2. */
    set_to_bdd(&t1, image(s1, rq));
    set_to_bdd(&t2, image(s2, rq));
    select2(&t0, t1, var_x, t2, sylvan_not(var_x));
    test_assert(result[8] == t0);
    set_to_bdd(&t1, preimage(s1, rq));
    set_to_bdd(&t2, preimage(s2, rq));
    select2(&t0, t1, var_y, t2, var_x);
    test_assert(result[9] == t0);
    compose(scratch, rq, r3);
    compose(scratch2, rq, r4);
    relation_to_bdd(&t1, scratch);
    relation_to_bdd(&t2, scratch2);
    select2(&t0, t1, var_y, t2, var_x);
    test_assert(result[10] == t0);

    /* The partial relation keeps the other pairs of r2 as they are. */
    compose(scratch, full_part, r2);
    relation_to_bdd(&t0, scratch);
    test_assert(result[11] == t0);

    for (int i = 0; i < RESULTS; i++) {
        test_assert(mtbdd_test_isvalid(result[i]));
    }
    mtbdd_refs_popptr(3);
    return 0;
}

static int
test_flip(void)
{
    BDD flip = sylvan_xor(sylvan_ithvar(0), sylvan_ithvar(1));
    mtbdd_protect(&flip);
    BDD closed = sylvan_closure(flip);
    int correct = closed == sylvan_true;
    correct &= sylvan_and(flip, sylvan_not(closed)) == sylvan_false;
    mtbdd_unprotect(&flip);
    test_assert(correct);
    return 0;
}

static int
test_outside_pair(void)
{
    BDD x = sylvan_ithvar(0), s = sylvan_ithvar(2), t = sylvan_ithvar(3);
    BDD pair = sylvan_set_fromarray((uint32_t[]){2}, 1);
    BDD left = sylvan_and(x, s), right = sylvan_and(x, t);
    mtbdd_protect(&pair);
    mtbdd_protect(&left);
    mtbdd_protect(&right);
    BDD expected = sylvan_and(left, t);
    mtbdd_protect(&expected);
    int correct = sylvan_relprev(left, right, pair) == expected;
    mtbdd_unprotect(&expected);
    mtbdd_unprotect(&right);
    mtbdd_unprotect(&left);
    mtbdd_unprotect(&pair);
    test_assert(correct);
    return 0;
}

TASK_0(int, runtests)
{

    BDD *roots[] = {
        &set_unprimed, &set_primed, &set_pairs, &set_part, &var_x, &var_y,
        &in_s1, &in_s2, &in_r, &in_r2, &in_part,
        &in_sx, &in_ry, &in_rx, &in_sy, &in_r3y,
    };
    const size_t root_count = sizeof(roots) / sizeof(roots[0]);
    for (size_t i = 0; i < root_count; i++) {
        *roots[i] = sylvan_false;
        mtbdd_protect(roots[i]);
    }
    for (int i = 0; i < RESULTS; i++) {
        result[i] = sylvan_false;
        mtbdd_protect(&result[i]);
    }

    var_x = sylvan_ithvar(VAR_X);
    var_y = sylvan_ithvar(VAR_Y);

    uint32_t identities[2 * PAIRS];
    for (uint32_t i = 0; i < PAIRS; i++) identities[i] = 2 * i;
    set_unprimed = sylvan_set_fromarray(identities, PAIRS);
    for (uint32_t i = 0; i < PAIRS; i++) identities[i] = 2 * i + 1;
    set_primed = sylvan_set_fromarray(identities, PAIRS);
    for (uint32_t i = 0; i < 2 * PAIRS; i++) identities[i] = i;
    set_pairs = sylvan_set_fromarray(identities, 2 * PAIRS);
    for (int i = 0; i < 3; i++) {
        identities[2 * i] = (uint32_t)(2 * part_bits[i]);
        identities[2 * i + 1] = (uint32_t)(2 * part_bits[i] + 1);
    }
    set_part = sylvan_set_fromarray(identities, 6);

    printf("Testing the relational products under the identity order.\n");
    int failed = test_flip();
    failed |= test_outside_pair();
    failed |= test_build();
    if (failed) return 1;
    CALL(sylvan_gc);
    if (test_same()) return 1;

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

    lace_start((unsigned)workers, 0);

    sylvan_set_sizes(1LL << 16, 1LL << 20, 1LL << 16, 1LL << 18);
    sylvan_init_package();
    sylvan_init_mtbdd();

    const int result_status = RUN(runtests);

    sylvan_quit();
    lace_stop();
    return result_status;
}
