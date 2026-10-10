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
 * The sift recommendation, and the automatic sift built on it.
 *
 * TWO THINGS ARE TESTED HERE.
 *
 * The first is the recommendation itself: false on a forest that has not grown
 * enough to be worth reordering, true once it has, false again after a sift,
 * and switchable off entirely by a factor of zero.
 *
 * The second is WHERE AN AUTOMATIC SIFT MAY RUN, which is a soundness property
 * and not a policy. Sylvan collects from two places. sylvan_gc() is the
 * explicit entry: its caller is between operations and owns the same
 * precondition sifting needs. The internal entry is the one an operation uses
 * when the nodes table has no free bucket -- it is called from the middle of a
 * recursion, with sub-results already computed under the current rank order
 * and not yet combined, and a sift there would make that operation's answer
 * silently wrong (see sylvan_gc_set_auto_sift in include/sylvan/reorder.h). So
 * an automatic sift must follow the first and never the second, and this file
 * drives a table that is too small for the work it is given in order to prove
 * that it does not follow the second.
 *
 * The forest is Bryant's function, f = x0*y0 + ... + xn*yn, built in the split
 * order x0 x1 ... y0 y1 ..., which needs about 2^(n+1) nodes where the paired
 * order needs about 3n. That gives a forest large enough to recommend, and a
 * sift that is unmistakable when it happens: the node count falls by three
 * orders of magnitude. The oracle for the function itself is the number of
 * satisfying assignments, 2^(2n) - 3^n, which no reordering may change.
 *
 * Usage: test_autosift [workers]
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lace.h>
#include <sylvan/internal.h>

#include "test_assert.h"

/* Thirteen pairs: 26 variables, about 16k nodes in the split order. */
#define TEST_PAIRS 13
#define TEST_VARS (2 * TEST_PAIRS)

/* Operations the churn performs. Each one allocates a diagram of its own and
   drops it, so the table fills several times over. */
#define TEST_CHURN 200

/* The fixed table this file runs on, in buckets. Small enough that the churn
   fills it, large enough that no single operation exhausts it. */
#define TEST_TABLE_SIZE (1LL << 17)

static BDD test_var[TEST_VARS];
static BDDSET test_variables;
static BDD test_f;
static uint64_t test_solutions;

/** The oracle: 2^(2n) - 3^n satisfying assignments, computed independently. */
static uint64_t
test_expected_solutions(void)
{
    uint64_t threes = 1;
    for (int i = 0; i < TEST_PAIRS; i++) threes *= 3;
    return (UINT64_C(1) << TEST_VARS) - threes;
}

/** f still denotes the same function, whatever the order now is. */
static int
test_check_f_CALL(lace_worker *lace)
{
    uint64_t solutions = 0;
    test_assert(mtbdd_is_valid_CALL(lace, test_f));
    test_assert(bdd_sat_count_u64(&solutions, test_f, test_variables)
                == SYLVAN_OK);
    test_assert(solutions == test_solutions);
    return 0;
}

/** Live buckets right now. */
static size_t
test_live(void)
{
    size_t filled = 0, total = 0;
    sylvan_table_usage(&filled, &total);
    return filled;
}

/**
 * Nothing has grown yet, so nothing is recommended.
 *
 * The first half of this holds before any collection has run at all, which is
 * worth stating separately: the flag is only ever written by a collection, so
 * a process that has not collected has no opinion rather than a default one.
 */
static int
test_fresh(void)
{
    test_assert(!sylvan_sift_recommended());
    test_assert(sylvan_sift_count() == 0);

    const uint64_t collections = sylvan_gc_count();
    sylvan_gc();
    test_assert(sylvan_gc_count() > collections);

    /* A forest of 26 variables and nothing else is far below the floor. */
    printf("  fresh forest: %zu live buckets, recommended=%d\n",
           test_live(), sylvan_sift_recommended());
    test_assert(!sylvan_sift_recommended());
    return 0;
}

/** Build f in the split order, which is the expensive one. */
static int
test_build_CALL(lace_worker *lace)
{
    BDD conjunction = bdd_false;
    mtbdd_protect(&conjunction);
    for (int i = 0; i < TEST_PAIRS; i++) {
        if (bdd_and_CALL(lace, &conjunction, test_var[i],
                         test_var[i + TEST_PAIRS]) != SYLVAN_OK ||
            bdd_or_CALL(lace, &test_f, test_f, conjunction) != SYLVAN_OK) {
            mtbdd_unprotect(&conjunction);
            test_assert(0);
        }
    }
    mtbdd_unprotect(&conjunction);

    test_solutions = test_expected_solutions();
    return test_check_f_CALL(lace);
}

/** A forest that has grown past the factor recommends a sift. */
static int
test_growth(void)
{
    sylvan_gc();
    printf("  grown forest: %zu live buckets, recommended=%d\n",
           test_live(), sylvan_sift_recommended());
    test_assert(sylvan_sift_recommended());

    /* And a factor of zero switches the whole thing off, flag included. */
    sylvan_sift_set_recommend_factor(0.0);
    test_assert(!sylvan_sift_recommended());
    sylvan_gc();
    test_assert(!sylvan_sift_recommended());

    sylvan_sift_set_recommend_factor(2.0);
    sylvan_gc();
    test_assert(sylvan_sift_recommended());
    return 0;
}

/**
 * THE SOUNDNESS TEST. Auto-sift is on, a sift is recommended, and the table is
 * driven until operations have to collect from inside their own recursion. Not
 * one of those collections may sift.
 *
 * The explicit collect-only entry is checked the same way in the same state,
 * because it is the entry the table-full path uses and testing it directly
 * says what the churn only implies.
 */
static int
test_no_sift_from_internal_CALL(lace_worker *lace)
{
    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));
    sylvan_gc_set_auto_sift(&config);

    test_assert(sylvan_sift_recommended());
    const uint64_t sifts = sylvan_sift_count();
    uint64_t collections = sylvan_gc_count();

    /* The collect-only entry, in a state where the explicit entry would sift. */
    sylvan_gc_collect();
    test_assert(sylvan_gc_count() > collections);
    test_assert(sylvan_sift_count() == sifts);
    test_assert(sylvan_sift_recommended());

    /* And the table-full path itself, reached from inside an operation. */
    collections = sylvan_gc_count();
    int failed = 0;
    for (int i = 0; i < TEST_CHURN && !failed; i++) {
        MTBDD temporary = mtbdd_invalid;
        mtbdd_refs_pushptr(&temporary);
        failed = bdd_xor_CALL(lace, &temporary, test_f,
                              test_var[i % TEST_VARS]) != SYLVAN_OK;
        mtbdd_refs_popptr(1);
    }
    test_assert(!failed);

    printf("  churn: %llu internal collections, %llu sifts\n",
           (unsigned long long)(sylvan_gc_count() - collections),
           (unsigned long long)(sylvan_sift_count() - sifts));

    /* The churn really did run out of buckets inside an operation ... */
    test_assert(sylvan_gc_count() > collections);
    /* ... and not one of those collections reordered anything. */
    test_assert(sylvan_sift_count() == sifts);

    return test_check_f_CALL(lace);
}

/**
 * Off is off. The recommendation is still true and the entry is still the
 * explicit one; only the configuration has gone, and that is enough.
 *
 * This runs before the sift below rather than after it, because a sift resets
 * the baseline to the forest it leaves behind and there would then be no
 * recommendation left to ignore.
 */
static int
test_auto_off(void)
{
    sylvan_gc_set_auto_sift(NULL);
    test_assert(sylvan_sift_recommended());

    const uint64_t sifts = sylvan_sift_count();
    const uint64_t collections = sylvan_gc_count();
    sylvan_gc();

    test_assert(sylvan_gc_count() > collections);
    test_assert(sylvan_sift_count() == sifts);
    test_assert(sylvan_sift_recommended());
    return 0;
}

/**
 * The explicit entry, in the same state that the internal one refused to sift
 * in, does sift -- and the sift clears the recommendation, because the forest
 * it leaves behind is the new baseline. A second explicit collection right
 * afterwards therefore does not sift.
 */
static int
test_sift_from_explicit_CALL(lace_worker *lace)
{
    sylvan_sift_config config;
    memset(&config, 0, sizeof(config));
    sylvan_gc_set_auto_sift(&config);

    test_assert(sylvan_sift_recommended());
    const uint64_t sifts = sylvan_sift_count();
    const size_t before = test_live();

    sylvan_gc();

    const size_t after = test_live();
    printf("  explicit gc: %zu -> %zu live buckets, %llu sift\n",
           before, after, (unsigned long long)(sylvan_sift_count() - sifts));

    test_assert(sylvan_sift_count() == sifts + 1);
    test_assert(!sylvan_sift_recommended());
    /* The split order costs about 2^(n+1) and the paired one about 3n. */
    test_assert(after * 4 < before);

    /* Cleared, so the next explicit collection sifts nothing. */
    sylvan_gc();
    test_assert(sylvan_sift_count() == sifts + 1);
    test_assert(!sylvan_sift_recommended());

    sylvan_gc_set_auto_sift(NULL);
    return test_check_f_CALL(lace);
}

TASK(int, runtests)

int
runtests_CALL(lace_worker *lace)
{
    test_f = bdd_false;
    test_variables = mtbdd_invalid;
    mtbdd_protect(&test_f);
    mtbdd_protect(&test_variables);

    for (uint32_t i = 0; i < TEST_VARS; i++) {
        test_var[i] = mtbdd_invalid;
        mtbdd_protect(&test_var[i]);
        test_assert(bdd_var_at_level(&test_var[i], i) == SYLVAN_OK);
    }
    uint32_t identities[TEST_VARS];
    for (uint32_t i = 0; i < TEST_VARS; i++) identities[i] = i;
    test_assert(bdd_set_from_array(&test_variables, identities, TEST_VARS)
                == SYLVAN_OK);

    printf("Testing that a fresh forest recommends nothing.\n");
    if (test_fresh()) return 1;

    printf("Testing that a grown forest recommends a sift.\n");
    if (test_build_CALL(lace)) return 1;
    if (test_growth()) return 1;

    if (nodes_reorder_caps(nodes) == 0) {
        /* No reorder primitives: the recommendation is still computed and an
           automatic sift is still refused rather than performed, which is the
           only part of the rest that can be checked here. */
        sylvan_sift_config config;
        memset(&config, 0, sizeof(config));
        sylvan_gc_set_auto_sift(&config);
        printf("Nodes table has no reorder primitives; testing the refusal.\n");
        const uint64_t sifts = sylvan_sift_count();
        sylvan_gc();
        test_assert(sylvan_sift_count() == sifts);
        sylvan_gc_set_auto_sift(NULL);
    } else {
        printf("Testing that an internal collection never sifts.\n");
        if (test_no_sift_from_internal_CALL(lace)) return 1;

        printf("Testing that switching it off switches it off.\n");
        if (test_auto_off()) return 1;

        printf("Testing that an explicit collection sifts.\n");
        if (test_sift_from_explicit_CALL(lace)) return 1;
    }

    for (uint32_t i = 0; i < TEST_VARS; i++) mtbdd_unprotect(&test_var[i]);
    mtbdd_unprotect(&test_variables);
    mtbdd_unprotect(&test_f);
    return 0;
}

int
main(int argc, char **argv)
{
    int workers = 4;
    if (argc > 1) workers = atoi(argv[1]);
    if (workers < 1) workers = 1;

    lace_start((unsigned)workers, 0, 64u * 1024u * 1024u);

    /* A table that never grows. The point of this file is the collection an
       operation runs when it cannot find a bucket, and a table that doubled
       instead would postpone that indefinitely. */
    sylvan_set_sizes(TEST_TABLE_SIZE, TEST_TABLE_SIZE,
                     TEST_TABLE_SIZE, TEST_TABLE_SIZE);
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
