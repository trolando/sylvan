#include <gmp.h>
#include <lace.h>

#include <sylvan/internal.h>
#include <sylvan/gmp.h>

#include "test_assert.h"

static uint32_t gmp_test_type;

static int
gmp_leaf_equals(MTBDD leaf, long numerator, unsigned long denominator)
{
    if (!mtbdd_is_leaf(leaf) || leaf == mtbdd_undefined || mtbdd_leaf_type(leaf) != gmp_test_type) return 0;

    mpq_t expected;
    mpq_init(expected);
    mpq_set_si(expected, numerator, denominator);
    mpq_canonicalize(expected);
    int equal = mpq_cmp((mpq_ptr)mtbdd_leaf_value(leaf), expected) == 0;
    mpq_clear(expected);
    return equal;
}

TASK(int, run_gmp_tests)
int
run_gmp_tests_CALL(lace_worker *lace)
{
    mpq_t three_halves;
    mpq_t one_half;
    mpq_init(three_halves);
    mpq_init(one_half);
    mpq_set_si(three_halves, 3, 2);
    mpq_set_si(one_half, 1, 2);

    MTBDD a = mtbdd_gmp(three_halves);
    mtbdd_refs_pushptr(&a);
    MTBDD b = mtbdd_gmp(one_half);
    mtbdd_refs_pushptr(&b);
    gmp_test_type = mtbdd_leaf_type(a);
    BDD x = mtbdd_invalid;
    MTBDD f = mtbdd_invalid;
    MTBDD g = mtbdd_invalid;
    MTBDD sum = mtbdd_invalid;
    MTBDD difference = mtbdd_invalid;
    MTBDD product = mtbdd_invalid;
    MTBDD quotient = mtbdd_invalid;
    MTBDD minimum = mtbdd_invalid;
    MTBDD maximum = mtbdd_invalid;
    MTBDD negated = mtbdd_invalid;
    MTBDD absolute = mtbdd_invalid;
    BDDSET vars = mtbdd_invalid;
    MTBDD abstract_sum = mtbdd_invalid;
    MTBDD abstract_product = mtbdd_invalid;
    MTBDD abstract_minimum = mtbdd_invalid;
    MTBDD abstract_maximum = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&f);
    mtbdd_refs_pushptr(&g);
    mtbdd_refs_pushptr(&sum);
    mtbdd_refs_pushptr(&difference);
    mtbdd_refs_pushptr(&product);
    mtbdd_refs_pushptr(&quotient);
    mtbdd_refs_pushptr(&minimum);
    mtbdd_refs_pushptr(&maximum);
    mtbdd_refs_pushptr(&negated);
    mtbdd_refs_pushptr(&absolute);
    mtbdd_refs_pushptr(&vars);
    mtbdd_refs_pushptr(&abstract_sum);
    mtbdd_refs_pushptr(&abstract_product);
    mtbdd_refs_pushptr(&abstract_minimum);
    mtbdd_refs_pushptr(&abstract_maximum);
    mtbdd_refs_pushptr(&unchanged);

    mpq_clear(three_halves);
    mpq_clear(one_half);

    test_assert(bdd_var_at_level(&x, 0) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &f, x, b, a) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &g, x, a, b) == SYLVAN_OK);

    test_assert(gmp_plus(&sum, f, g) == SYLVAN_OK);
    test_assert(gmp_minus(&difference, f, g) == SYLVAN_OK);
    test_assert(gmp_times(&product, f, g) == SYLVAN_OK);
    test_assert(gmp_divide(&quotient, f, g) == SYLVAN_OK);
    test_assert(gmp_min(&minimum, f, g) == SYLVAN_OK);
    test_assert(gmp_max(&maximum, f, g) == SYLVAN_OK);
    test_assert(gmp_neg(&negated, f) == SYLVAN_OK);
    test_assert(gmp_abs(&absolute, negated) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&vars, (uint32_t[]){0}, 1) == SYLVAN_OK);
    test_assert(gmp_abstract_plus(&abstract_sum, f, vars) == SYLVAN_OK);
    test_assert(gmp_abstract_times(&abstract_product, f, vars) == SYLVAN_OK);
    test_assert(gmp_abstract_min(&abstract_minimum, f, vars) == SYLVAN_OK);
    test_assert(gmp_abstract_max(&abstract_maximum, f, vars) == SYLVAN_OK);

    test_assert(gmp_leaf_equals(sum, 2, 1));
    test_assert(gmp_leaf_equals(product, 3, 4));
    test_assert(gmp_leaf_equals(minimum, 1, 2));
    test_assert(gmp_leaf_equals(maximum, 3, 2));
    test_assert(absolute == f);
    test_assert(gmp_leaf_equals(abstract_sum, 2, 1));
    test_assert(gmp_leaf_equals(abstract_product, 3, 4));
    test_assert(gmp_leaf_equals(abstract_minimum, 1, 2));
    test_assert(gmp_leaf_equals(abstract_maximum, 3, 2));

    MTBDD low, high;
    mtbdd_cofactors(difference, &low, &high);
    test_assert(gmp_leaf_equals(low, 1, 1));
    test_assert(gmp_leaf_equals(high, -1, 1));
    mtbdd_cofactors(quotient, &low, &high);
    test_assert(gmp_leaf_equals(low, 3, 1));
    test_assert(gmp_leaf_equals(high, 1, 3));
    mtbdd_cofactors(negated, &low, &high);
    test_assert(gmp_leaf_equals(low, -3, 2));
    test_assert(gmp_leaf_equals(high, -1, 2));

    test_assert(gmp_plus(&f, f, g) == SYLVAN_OK);
    test_assert(f == sum);
    sylvan_gc_CALL(lace);
    test_assert(gmp_leaf_equals(f, 2, 1));

    test_assert(gmp_plus(NULL, a, b) == SYLVAN_ERR_INVALID);
    test_assert(gmp_plus(&unchanged, mtbdd_invalid, b) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(gmp_plus(&unchanged, a, mtbdd_int64(1)) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(gmp_abstract_plus(NULL, f, vars) == SYLVAN_ERR_INVALID);
    test_assert(gmp_abstract_plus(&unchanged, mtbdd_invalid, vars) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    mtbdd_refs_popptr(19);
    return 0;
}

int
main(void)
{
    lace_start(4, 0, 0);
    sylvan_set_sizes(1LL << 20, 1LL << 20, 1LL << 16, 1LL << 16);
    sylvan_init_package();
    mtbdd_init();
    gmp_init();

    int result = run_gmp_tests();

    sylvan_quit();
    lace_stop();
    return result;
}
