#include <sylvan_int.h>
#include <math.h>
#include <limits.h>
#include <stdatomic.h>
#include "test_assert.h"
#ifdef SYLVAN_TEST_GMP
#include <sylvan_gmp.h>
#endif

static void protect_roots(MTBDD *roots, size_t count)
{
    for (size_t i=0; i<count; i++) {
        roots[i] = mtbdd_invalid;
        mtbdd_protect(&roots[i]);
    }
}

static void unprotect_roots(MTBDD *roots, size_t count)
{
    for (size_t i=0; i<count; i++) mtbdd_unprotect(&roots[i]);
}

/* Truth tables use bit v for variable v. This oracle does not use mtbdd_eval. */
static MTBDD leaf_at(MTBDD dd, unsigned assignment)
{
    while (!mtbdd_isleaf(dd)) {
        dd = assignment & (1u << mtbdd_getvar(dd)) ? mtbdd_gethigh(dd) : mtbdd_getlow(dd);
    }
    return dd;
}

static MTBDD from_table(const MTBDD *leaves, unsigned count, unsigned level, unsigned prefix)
{
    if (level == count) return leaves[prefix];
    MTBDD low = mtbdd_refs_push(from_table(leaves, count, level+1, prefix));
    MTBDD high = mtbdd_refs_push(from_table(leaves, count, level+1, prefix | (1u << level)));
    MTBDD result = mtbdd_makenode(level, low, high);
    mtbdd_refs_pop(2);
    return result;
}

static MTBDD number(unsigned type, int value)
{
    if (type == 0) return mtbdd_int64(value);
    if (type == 1) return mtbdd_double(value * 0.5);
    return mtbdd_fraction(value, 2);
}

/* Independent numeric oracle: the fraction comparison uses cross products. */
static int leaf_relation(MTBDD a, MTBDD b, unsigned relation, double tolerance)
{
    if (a == mtbdd_false || b == mtbdd_false) return 0;
    if (relation >= 4) {
        double av = mtbdd_getdouble(a), bv = mtbdd_getdouble(b);
        if (isnan(av) || isnan(bv)) return 0;
        if (relation == 4) return fabs(av-bv) < tolerance;
        return av == bv || (av != 0.0 && fabs((av-bv)/av) < tolerance);
    }
    int cmp;
    switch (mtbdd_gettype(a)) {
    case 0: {
        int64_t av = mtbdd_getint64(a), bv = mtbdd_getint64(b);
        cmp = av < bv ? -1 : av > bv;
        break;
    }
    case 1: {
        double av = mtbdd_getdouble(a), bv = mtbdd_getdouble(b);
        if (isnan(av) || isnan(bv)) return 0;
        cmp = av < bv ? -1 : av > bv;
        break;
    }
    default: {
        int64_t av = (int64_t)mtbdd_getnumer(a) * mtbdd_getdenom(b);
        int64_t bv = (int64_t)mtbdd_getnumer(b) * mtbdd_getdenom(a);
        cmp = av < bv ? -1 : av > bv;
        break;
    }
    }
    switch (relation) {
    case 0: return cmp <= 0;
    case 1: return cmp < 0;
    case 2: return cmp >= 0;
    default: return cmp > 0;
    }
}

TASK_5(MTBDD, compare, MTBDD, a, MTBDD, b, unsigned, relation, int, mode, double, tolerance)
{
    if (mode == 2) {
        switch (relation) {
        case 0: return CALL(mtbdd_all_leq, a, b);
        case 1: return CALL(mtbdd_all_lt, a, b);
        case 2: return CALL(mtbdd_all_geq, a, b);
        case 3: return CALL(mtbdd_all_gt, a, b);
        case 4: return CALL(mtbdd_all_equal_abs_double, a, b, tolerance);
        default: return CALL(mtbdd_all_equal_rel_double, a, b, tolerance);
        }
    }
    if (mode == 1) {
        switch (relation) {
        case 0: return CALL(mtbdd_any_leq, a, b);
        case 1: return CALL(mtbdd_any_lt, a, b);
        case 2: return CALL(mtbdd_any_geq, a, b);
        case 3: return CALL(mtbdd_any_gt, a, b);
        case 4: return CALL(mtbdd_any_equal_abs_double, a, b, tolerance);
        default: return CALL(mtbdd_any_equal_rel_double, a, b, tolerance);
        }
    }
    switch (relation) {
    case 0: return CALL(mtbdd_compare_leq, a, b);
    case 1: return CALL(mtbdd_compare_lt, a, b);
    case 2: return CALL(mtbdd_compare_geq, a, b);
    case 3: return CALL(mtbdd_compare_gt, a, b);
    case 4: return CALL(mtbdd_compare_equal_abs_double, a, b, tolerance);
    default: return CALL(mtbdd_compare_equal_rel_double, a, b, tolerance);
    }
}

TASK_4(int, check_comparisons, MTBDD, a, MTBDD, b, unsigned, relations, double, tolerance)
{
    for (unsigned relation=0; relation<relations; relation++) {
        MTBDD result = mtbdd_refs_push(CALL(compare, a, b, relation, 0, tolerance));
        test_assert(result != mtbdd_invalid);
        int any = 0, all = 1;
        for (unsigned assignment=0; assignment<16; assignment++) {
            MTBDD av = leaf_at(a, assignment), bv = leaf_at(b, assignment);
            int expected = leaf_relation(av, bv, relation, tolerance);
            test_assert(leaf_at(result, assignment) == (expected ? mtbdd_true : mtbdd_false));
            any |= expected;
            /* Universal tolerance comparisons keep upstream's identical-leaf shortcut. */
            if (av != mtbdd_false && bv != mtbdd_false && (relation < 4 || av != bv)) all &= expected;
        }
        test_assert(CALL(compare, a, b, relation, 1, tolerance) == (any ? mtbdd_true : mtbdd_false));
        test_assert(CALL(compare, a, b, relation, 2, tolerance) == (all ? mtbdd_true : mtbdd_false));
        mtbdd_refs_pop(1);
    }
    return 0;
}

TASK_0(int, test_comparisons)
{
    MTBDD leaves[32], roots[4];
    protect_roots(leaves, 32);
    protect_roots(roots, 4);
    for (unsigned type=0; type<4; type++) {
        for (unsigned i=0; i<16; i++) {
            unsigned leaf_type = type == 3 ? (i & 1 ? 2 : (i & 4 ? 1 : 0)) : type;
            leaves[i] = i & 8 ? mtbdd_false : number(leaf_type, (int)((i & 1) + (i & 4)) - 2);
            leaves[16+i] = (i & 5) == 5 ? mtbdd_false : number(leaf_type, (int)(i & 4) - 1);
        }
        roots[0] = from_table(leaves, 4, 0, 0);
        roots[1] = from_table(leaves+16, 4, 0, 0);
        for (int repeat=0; repeat<2; repeat++) {
            test_assert(CALL(check_comparisons, roots[0], roots[1], type == 1 ? 6 : 4, 0.6) == 0);
            test_assert(CALL(check_comparisons, roots[0], roots[1], type == 1 ? 6 : 4, 0.1) == 0);
            test_assert(CALL(check_comparisons, roots[1], roots[0], type == 1 ? 6 : 4, 0.0) == 0);
            test_assert(CALL(check_comparisons, roots[0], roots[0], type == 1 ? 6 : 4, 0.0) == 0);
            /* Query subproblems after parents: shared short-circuit results must not leak into cache. */
            if (type != 3) test_assert(CALL(check_comparisons, mtbdd_getlow(roots[0]), mtbdd_gethigh(roots[1]), type == 1 ? 6 : 4, 0.1) == 0);
        }
        for (unsigned i=0; i<16; i++) {
            for (unsigned j=0; j<16; j++) {
                if (type != 3 || leaves[i] == mtbdd_false || leaves[16+j] == mtbdd_false ||
                    mtbdd_gettype(leaves[i]) == mtbdd_gettype(leaves[16+j])) {
                    test_assert(CALL(check_comparisons, leaves[i], leaves[16+j], type == 1 ? 6 : 4, 0.5) == 0);
                }
            }
        }
    }
    const double values[] = {0.0, -0.0, 1.0, -2.0, INFINITY, -INFINITY, NAN};
    for (unsigned i=0; i<7; i++) leaves[i] = mtbdd_double(values[i]);
    for (unsigned i=0; i<7; i++) {
        for (unsigned j=0; j<7; j++) {
            test_assert(CALL(check_comparisons, leaves[i], leaves[j], 6, 0.0) == 0);
            test_assert(CALL(check_comparisons, leaves[i], leaves[j], 6, INFINITY) == 0);
            test_assert(CALL(check_comparisons, leaves[i], leaves[j], 6, NAN) == 0);
        }
    }
    leaves[0] = mtbdd_int64(INT64_MIN);
    leaves[1] = mtbdd_int64(INT64_MAX);
    leaves[2] = mtbdd_fraction(-INT32_MAX, UINT32_MAX);
    leaves[3] = mtbdd_fraction(INT32_MAX, UINT32_MAX-1);
    test_assert(CALL(check_comparisons, leaves[0], leaves[1], 4, 0.0) == 0);
    test_assert(CALL(check_comparisons, leaves[2], leaves[3], 4, 0.0) == 0);
    for (unsigned relation=0; relation<6; relation++) {
        for (int mode=0; mode<3; mode++) {
            test_assert(CALL(compare, mtbdd_invalid, leaves[0], relation, mode, 0.1) == mtbdd_invalid);
            test_assert(CALL(compare, mtbdd_true, leaves[0], relation, mode, 0.1) == mtbdd_invalid);
            test_assert(CALL(compare, leaves[0], leaves[2], relation, mode, 0.1) == mtbdd_invalid);
            MTBDD empty = mode == 2 ? mtbdd_true : mtbdd_false;
            test_assert(CALL(compare, mtbdd_false, leaves[0], relation, mode, 0.1) == empty);
            test_assert(CALL(compare, mtbdd_false, mtbdd_false, relation, mode, 0.1) == empty);
        }
    }
    /* Legacy functions retain their names and partial-domain behavior. */
    test_assert(mtbdd_equal_norm_d(leaves[4], mtbdd_false, 0.1) == mtbdd_false);
    test_assert(mtbdd_equal_norm_rel_d(leaves[4], mtbdd_false, 0.1) == mtbdd_false);
    test_assert(mtbdd_all_equal_abs_double(leaves[4], mtbdd_false, 0.1) == mtbdd_true);
    test_assert(mtbdd_all_equal_rel_double(leaves[4], mtbdd_false, 0.1) == mtbdd_true);
    test_assert(mtbdd_leq(mtbdd_false, leaves[0]) == mtbdd_true);
    test_assert(mtbdd_less(mtbdd_false, leaves[0]) == mtbdd_true);
    test_assert(mtbdd_geq(mtbdd_false, leaves[0]) == mtbdd_true);
    test_assert(mtbdd_greater(mtbdd_false, leaves[0]) == mtbdd_true);
    unprotect_roots(roots, 4);
    unprotect_roots(leaves, 32);
    return 0;
}

TASK_0(int, test_universal_ordering)
{
    MTBDD roots[12];
    protect_roots(roots, 12);
    roots[0] = mtbdd_int64(1);
    roots[1] = mtbdd_int64(2);
    roots[2] = mtbdd_int64(3);
    roots[3] = mtbdd_double(1.0);
    roots[4] = mtbdd_makeleaf(sylvan_mt_create_type(), 42);
    for (unsigned relation=0; relation<4; relation++) {
        int reversed = relation >= 2;
        roots[5] = mtbdd_makenode(2, roots[0], roots[1]);
        roots[6] = mtbdd_makenode(2, roots[1], roots[2]);
        MTBDD good_a = roots[reversed ? 6 : 5], good_b = roots[reversed ? 5 : 6];
        MTBDD bad_a = roots[reversed ? 0 : 2], bad_b = roots[reversed ? 2 : 0];
        roots[7] = mtbdd_makenode(0, bad_a, good_a);
        roots[8] = mtbdd_makenode(0, bad_b, good_b);
        cache_clear();
        for (int repeat=0; repeat<2; repeat++) {
            /* A false parent must not cache false for its true high subtree. */
            test_assert(CALL(compare, roots[7], roots[8], relation, 2, 0) == mtbdd_false);
            test_assert(CALL(compare, good_a, good_b, relation, 2, 0) == mtbdd_true);
        }
        roots[9] = mtbdd_makenode(0, mtbdd_false, good_a);
        roots[10] = mtbdd_makenode(0, mtbdd_false, good_b);
        test_assert(CALL(compare, roots[9], roots[10], relation, 2, 0) == mtbdd_true);
        roots[10] = mtbdd_makenode(0, good_b, mtbdd_false);
        test_assert(CALL(compare, roots[9], roots[10], relation, 2, 0) == mtbdd_true);
        test_assert(CALL(compare, roots[0], roots[3], relation, 2, 0) == mtbdd_invalid);
        test_assert(CALL(compare, roots[4], roots[4], relation, 2, 0) == mtbdd_invalid);
        test_assert(CALL(compare, mtbdd_true, mtbdd_true, relation, 2, 0) == mtbdd_invalid);
        test_assert(CALL(compare, mtbdd_false, mtbdd_invalid, relation, 2, 0) == mtbdd_invalid);
        /* A false branch must not hide an unsupported common assignment. */
        roots[9] = mtbdd_makenode(0, bad_a, roots[4]);
        roots[10] = mtbdd_makenode(0, bad_b, roots[4]);
        test_assert(CALL(compare, roots[9], roots[10], relation, 2, 0) == mtbdd_invalid);
        roots[11] = mtbdd_makenode(0, mtbdd_false, roots[4]);
        test_assert(CALL(compare, roots[11], roots[11], relation, 2, 0) == mtbdd_invalid);
    }
    unprotect_roots(roots, 12);
    return 0;
}

static double numeric_value(MTBDD leaf)
{
    if (leaf == mtbdd_false) return 0;
    if (leaf == mtbdd_true) return 1;
    switch (mtbdd_gettype(leaf)) {
    case 0: return (double)mtbdd_getint64(leaf);
    case 1: return mtbdd_getdouble(leaf);
    case 2: return (double)mtbdd_getnumer(leaf) / mtbdd_getdenom(leaf);
    default:
#ifdef SYLVAN_TEST_GMP
        return mpq_get_d((mpq_ptr)(size_t)mtbdd_getvalue(leaf));
#else
        return NAN;
#endif
    }
}

static atomic_uint map_calls, reduce_calls, collections;

VOID_TASK_0(count_collection)
{
    atomic_fetch_add(&collections, 1);
}

TASK_2(MTBDD, map_number, MTBDD, leaf, size_t, param)
{
    atomic_fetch_add(&map_calls, 1);
    return mtbdd_int64((int64_t)numeric_value(leaf) + (int64_t)param);
}

TASK_2(MTBDD, map_twice, MTBDD, leaf, size_t, param)
{
    return mtbdd_int64(2 * (int64_t)numeric_value(leaf) + (int64_t)param);
}

TASK_2(MTBDD, map_identity, MTBDD, leaf, size_t, param)
{
    (void)param;
    return leaf;
}

TASK_2(MTBDD, map_collect, MTBDD, leaf, size_t, param)
{
    CALL(sylvan_gc);
    return CALL(map_number, leaf, param);
}

TASK_2(MTBDD, map_partial_failure, MTBDD, leaf, size_t, param)
{
    if (mtbdd_getint64(leaf) == 1) return mtbdd_invalid;
    CALL(sylvan_gc);
    return CALL(map_number, leaf, param);
}

TASK_2(MTBDD, map_bad, MTBDD, leaf, size_t, param)
{
    (void)leaf;
    return param ? mtbdd_ithvar(10) : mtbdd_invalid;
}

TASK_4(MTBDD, reduce_sum, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    (void)param;
    atomic_fetch_add(&reduce_calls, 1);
    if (skipped > INT_MAX) return mtbdd_invalid;
    return CALL(mtbdd_abstract_op_plus, a, b, (int)skipped);
}

TASK_4(MTBDD, reduce_max, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    (void)param;
    return skipped ? a : CALL(mtbdd_apply, a, b, mtbdd_op_max_CALL);
}

TASK_4(MTBDD, reduce_choose, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    return param ? CALL(reduce_max, a, b, skipped, 0) : CALL(reduce_sum, a, b, skipped, 0);
}

TASK_4(MTBDD, reduce_collect, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    CALL(sylvan_gc);
    return CALL(reduce_sum, a, b, skipped, param);
}

TASK_4(MTBDD, reduce_bad, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    (void)a; (void)b; (void)skipped; (void)param;
    return mtbdd_invalid;
}

static BDDSET variable_set(unsigned mask)
{
    uint32_t variables[4];
    size_t count = 0;
    for (unsigned i=0; i<4; i++) if (mask & (1u<<i)) variables[count++] = i;
    return mtbdd_set_fromarray(variables, count);
}

TASK_1(int, test_mapping, unsigned, type)
{
    MTBDD leaves[16], roots[7];
    protect_roots(leaves, 16);
    protect_roots(roots, 7);
    for (unsigned i=0; i<16; i++) {
        /* Skip variable 0 everywhere and variable 2 on one branch. */
        int value = i & 2 ? (int)(i & 4) + 2 : 1;
        leaves[i] = i & 8 ? mtbdd_false : number(type == 3 ? i % 3 : type, value * 2);
    }
    roots[0] = from_table(leaves, 4, 0, 0);
    roots[1] = mtbdd_int64(0);
    mtbdd_map_op mapper = map_number_CALL;
    mtbdd_map_reduce_op reducer = reduce_sum_CALL;
    for (size_t param=1; param<=2; param++) {
        roots[2] = mtbdd_map(roots[0], mapper, param);
        roots[3] = mtbdd_map(roots[0], map_twice_CALL, param);
        for (unsigned assignment=0; assignment<16; assignment++) {
            int64_t value = (int64_t)numeric_value(leaves[assignment]);
            test_assert(mtbdd_getint64(leaf_at(roots[2], assignment)) == value + (int64_t)param);
            test_assert(mtbdd_getint64(leaf_at(roots[3], assignment)) == 2 * value + (int64_t)param);
        }
        for (unsigned mask=0; mask<16; mask++) {
            roots[4] = variable_set(mask);
            roots[5] = mtbdd_map_reduce(roots[0], roots[4], mapper, reducer, roots[1], param);
            test_assert(roots[5] != mtbdd_invalid);
            for (unsigned assignment=0; assignment<16; assignment++) {
                int64_t sum = 0;
                for (unsigned choice=0; choice<16; choice++) {
                    if ((choice & ~mask) == (assignment & ~mask)) {
                        sum += (int64_t)numeric_value(leaves[choice]) + (int64_t)param;
                    }
                }
                test_assert(mtbdd_getint64(leaf_at(roots[5], assignment)) == sum);
            }
            if (!mask) test_assert(roots[5] == roots[2]);
        }
    }
    /* Cache keys must distinguish both callback pointers, parameter and set. */
    roots[4] = variable_set(15);
    roots[5] = mtbdd_map_reduce(roots[0], roots[4], map_number_CALL, reduce_sum_CALL, roots[1], 1);
    roots[6] = mtbdd_map_reduce(roots[0], roots[4], map_twice_CALL, reduce_sum_CALL, roots[1], 1);
    test_assert(mtbdd_getint64(roots[6]) == 2 * mtbdd_getint64(roots[5]) - 16);
    roots[6] = mtbdd_map_reduce(roots[0], roots[4], map_number_CALL, reduce_max_CALL, roots[1], 1);
    test_assert(mtbdd_getint64(roots[6]) < mtbdd_getint64(roots[5]));
    roots[5] = mtbdd_map_reduce(roots[2], roots[4], map_identity_CALL, reduce_choose_CALL, roots[1], 0);
    roots[6] = mtbdd_map_reduce(roots[2], roots[4], map_identity_CALL, reduce_choose_CALL, roots[1], 1);
    test_assert(mtbdd_getint64(roots[6]) < mtbdd_getint64(roots[5]));
    /* false and typed zero both act as identities on positive integer operands. */
    cache_clear(); // clear earlier runs of these same semantic arguments
    roots[5] = mtbdd_map_reduce(roots[2], roots[4], map_identity_CALL, reduce_sum_CALL, roots[1], 123);
    unsigned before = atomic_load(&reduce_calls);
    roots[6] = mtbdd_map_reduce(roots[2], roots[4], map_identity_CALL, reduce_sum_CALL, mtbdd_false, 123);
    test_assert(roots[5] == roots[6] && atomic_load(&reduce_calls) > before);
    for (unsigned i=0; i<16; i++) {
        roots[5] = mtbdd_map_reduce(leaves[i], roots[4], mapper, reducer, roots[1], 7);
        test_assert(mtbdd_getint64(roots[5]) == 16 * ((int64_t)numeric_value(leaves[i]) + 7));
    }
    roots[5] = mtbdd_map(mtbdd_true, mapper, 3);
    test_assert(mtbdd_getint64(roots[5]) == 4);
    roots[5] = mtbdd_map_reduce(mtbdd_true, roots[4], mapper, reducer, roots[1], 3);
    test_assert(mtbdd_getint64(roots[5]) == 64);
    unprotect_roots(roots, 7);
    unprotect_roots(leaves, 16);
    return 0;
}

TASK_0(int, test_mapping_cache)
{
    MTBDD roots[7];
    protect_roots(roots, 7);
    roots[0] = mtbdd_int64(1);
    roots[1] = mtbdd_int64(2);
    roots[2] = mtbdd_makenode(0, roots[0], roots[1]);
    roots[3] = variable_set(3);
    roots[4] = mtbdd_int64(0);
    cache_clear();
    roots[5] = mtbdd_map(roots[2], map_number_CALL, 7);
    unsigned mapped = atomic_load(&map_calls);
    roots[6] = mtbdd_map(roots[2], map_number_CALL, 7);
    test_assert(roots[5] == roots[6] && atomic_load(&map_calls) == mapped);
    roots[5] = mtbdd_map_reduce(roots[2], roots[3], map_number_CALL, reduce_sum_CALL, roots[4], 7);
    mapped = atomic_load(&map_calls);
    unsigned reduced = atomic_load(&reduce_calls);
    roots[6] = mtbdd_map_reduce(roots[2], roots[3], map_number_CALL, reduce_sum_CALL, roots[4], 7);
    test_assert(roots[5] == roots[6] && atomic_load(&map_calls) == mapped);
    test_assert(atomic_load(&reduce_calls) == reduced);
    unprotect_roots(roots, 7);
    return 0;
}

/* Decimal strings encoded with a leading 1 give an associative, noncommutative
 * reducer with identity 1. Eight digits fit in int64_t. */
static int64_t concatenate(int64_t a, int64_t b)
{
    int64_t power = 1;
    while (power <= b/10) power *= 10;
    return a * power + b - power;
}

TASK_2(MTBDD, concat_op, MTBDD*, a, MTBDD*, b)
{
    if (!mtbdd_isleaf(*a) || !mtbdd_isleaf(*b)) return mtbdd_invalid;
    return mtbdd_int64(concatenate(mtbdd_getint64(*a), mtbdd_getint64(*b)));
}

TASK_2(MTBDD, map_digit, MTBDD, leaf, size_t, param)
{
    (void)param;
    return mtbdd_int64(10 + mtbdd_getint64(leaf));
}

TASK_4(MTBDD, reduce_concat, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    (void)param;
    if (!skipped) return CALL(mtbdd_apply, a, b, concat_op_CALL);
    while (skipped--) {
        mtbdd_refs_push(a);
        a = CALL(mtbdd_apply, a, a, concat_op_CALL);
        mtbdd_refs_pop(1);
    }
    return a;
}

TASK_0(int, test_order_and_failures)
{
    MTBDD leaves[8], roots[5];
    protect_roots(leaves, 8);
    protect_roots(roots, 5);
    for (unsigned i=0; i<8; i++) leaves[i] = mtbdd_int64(1 + (i & 2 ? i & 1 : 0));
    roots[0] = from_table(leaves, 3, 0, 0);
    roots[1] = mtbdd_int64(1);
    for (unsigned mask=0; mask<8; mask++) {
        roots[2] = variable_set(mask);
        roots[3] = mtbdd_map_reduce(roots[0], roots[2], map_digit_CALL, reduce_concat_CALL, roots[1], 0);
        for (unsigned assignment=0; assignment<8; assignment++) {
            int64_t expected = 1;
            for (unsigned lex=0; lex<8; lex++) {
                unsigned choice = ((lex & 1) << 2) | (lex & 2) | ((lex & 4) >> 2);
                if ((choice & ~mask) == (assignment & ~mask)) {
                    expected = concatenate(expected, 10 + mtbdd_getint64(leaves[choice]));
                }
            }
            test_assert(mtbdd_getint64(leaf_at(roots[3], assignment)) == expected);
        }
    }
    for (size_t bad=0; bad<2; bad++) {
        test_assert(mtbdd_map(roots[0], map_bad_CALL, bad) == mtbdd_invalid);
        test_assert(mtbdd_map_reduce(roots[0], roots[2], map_bad_CALL, reduce_sum_CALL, roots[1], bad) == mtbdd_invalid);
    }
    roots[4] = mtbdd_int64(0);
    test_assert(mtbdd_map_reduce(roots[0], roots[2], map_identity_CALL, reduce_bad_CALL, roots[4], 0) == mtbdd_invalid);
    test_assert(mtbdd_map(mtbdd_invalid, map_identity_CALL, 0) == mtbdd_invalid);
    test_assert(mtbdd_map(roots[0], NULL, 0) == mtbdd_invalid);
    test_assert(mtbdd_map_reduce(mtbdd_invalid, roots[2], map_identity_CALL, reduce_sum_CALL, roots[1], 0) == mtbdd_invalid);
    test_assert(mtbdd_map_reduce(roots[0], mtbdd_invalid, map_identity_CALL, reduce_sum_CALL, roots[1], 0) == mtbdd_invalid);
    test_assert(mtbdd_map_reduce(roots[0], mtbdd_false, map_identity_CALL, reduce_sum_CALL, roots[1], 0) == mtbdd_invalid);
    test_assert(mtbdd_map_reduce(roots[0], mtbdd_not(roots[2]), map_identity_CALL, reduce_sum_CALL, roots[1], 0) == mtbdd_invalid);
    test_assert(mtbdd_map_reduce(roots[0], roots[2], NULL, reduce_sum_CALL, roots[1], 0) == mtbdd_invalid);
    test_assert(mtbdd_map_reduce(roots[0], roots[2], map_identity_CALL, NULL, roots[1], 0) == mtbdd_invalid);
    test_assert(mtbdd_map_reduce(roots[0], roots[2], map_identity_CALL, reduce_sum_CALL, roots[0], 0) == mtbdd_invalid);
    test_assert(mtbdd_map_reduce(roots[0], roots[2], map_identity_CALL, reduce_sum_CALL, mtbdd_invalid, 0) == mtbdd_invalid);
    /* Identity calls may be omitted, including for skipped variables. */
    test_assert(mtbdd_map_reduce(roots[4], roots[2], map_identity_CALL, reduce_bad_CALL, roots[4], 0) == roots[4]);
    roots[2] = variable_set(1);
    roots[3] = mtbdd_makenode(0, roots[1], roots[4]);
    test_assert(mtbdd_map_reduce(roots[3], roots[2], map_identity_CALL, reduce_bad_CALL, roots[4], 0) == roots[1]);
    /* One failed child remains on the reference stack while its sibling collects. */
    test_assert(mtbdd_map(roots[3], map_partial_failure_CALL, 0) == mtbdd_invalid);
    test_assert(mtbdd_map_reduce(roots[3], roots[2], map_partial_failure_CALL, reduce_sum_CALL, roots[4], 0) == mtbdd_invalid);
    uint32_t many[70];
    for (uint32_t i=0; i<70; i++) many[i] = i;
    roots[2] = mtbdd_set_fromarray(many, 70);
    test_assert(mtbdd_map_reduce(roots[1], roots[2], map_identity_CALL, reduce_max_CALL, roots[4], 0) == roots[1]);
    roots[4] = mtbdd_double(NAN);
    test_assert(mtbdd_map(roots[4], map_identity_CALL, 0) == roots[4]);
    test_assert(mtbdd_map_reduce(roots[4], mtbdd_true, map_identity_CALL, reduce_sum_CALL, roots[1], 0) == roots[4]);
    unprotect_roots(roots, 5);
    unprotect_roots(leaves, 8);
    return 0;
}

#ifdef SYLVAN_TEST_GMP
TASK_4(MTBDD, reduce_gmp, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    (void)param;
    if (skipped > INT_MAX) return mtbdd_invalid;
    return CALL(gmp_abstract_op_plus, a, b, (int)skipped);
}

TASK_2(MTBDD, map_gmp_collect, MTBDD, leaf, size_t, param)
{
    CALL(sylvan_gc);
    return CALL(gmp_op_neg, leaf, param);
}

TASK_4(MTBDD, reduce_gmp_collect, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    CALL(sylvan_gc);
    return CALL(reduce_gmp, a, b, skipped, param);
}

static MTBDD rational(int value)
{
    mpq_t q;
    mpq_init(q);
    mpq_set_si(q, value, 2);
    mpq_canonicalize(q);
    MTBDD result = mtbdd_gmp(q);
    mpq_clear(q);
    return result;
}
#endif

TASK_0(int, test_numeric_maps)
{
    MTBDD leaves[16], roots[6];
    protect_roots(leaves, 16);
    protect_roots(roots, 6);
    unsigned types = 3;
#ifdef SYLVAN_TEST_GMP
    types++;
#endif
    for (unsigned type=0; type<types; type++) {
        for (unsigned i=0; i<16; i++) {
            int value = -2 - (int)(i & 5);
#ifdef SYLVAN_TEST_GMP
            if (type == 3) leaves[i] = rational(value);
            else
#endif
            leaves[i] = number(type, value);
        }
        roots[0] = from_table(leaves, 4, 0, 0);
        mtbdd_map_op neg = mtbdd_op_negate_CALL;
        mtbdd_map_reduce_op sum = reduce_sum_CALL;
#ifdef SYLVAN_TEST_GMP
        if (type == 3) {
            roots[1] = rational(0);
            neg = gmp_op_neg_CALL;
            sum = reduce_gmp_CALL;
            roots[5] = gmp_neg(roots[0]);
            roots[2] = mtbdd_map(roots[0], neg, 0);
            test_assert(roots[2] == roots[5]);
            roots[5] = gmp_abs(roots[0]);
            roots[2] = mtbdd_map(roots[0], gmp_op_abs_CALL, 0);
            test_assert(roots[2] == roots[5]);
            test_assert(mtbdd_map(mtbdd_false, neg, 0) == mtbdd_false);
            test_assert(mtbdd_map(mtbdd_false, gmp_op_abs_CALL, 0) == mtbdd_false);
            test_assert(mtbdd_compare_leq(leaves[0], leaves[1]) == mtbdd_invalid);
            test_assert(mtbdd_any_leq(leaves[0], leaves[1]) == mtbdd_invalid);
            for (unsigned relation=0; relation<4; relation++) {
                test_assert(CALL(compare, leaves[0], leaves[1], relation, 2, 0) == mtbdd_invalid);
            }
        } else
#endif
        roots[1] = number(type, 0);
        roots[2] = mtbdd_map(roots[0], neg, 0);
        for (unsigned assignment=0; assignment<16; assignment++) {
            test_assert(numeric_value(leaf_at(roots[2], assignment)) == -numeric_value(leaves[assignment]));
        }
        for (unsigned mask=0; mask<16; mask++) {
            roots[3] = variable_set(mask);
            roots[4] = mtbdd_map_reduce(roots[0], roots[3], neg, sum, roots[1], 0);
            test_assert(roots[4] != mtbdd_invalid);
            for (unsigned assignment=0; assignment<16; assignment++) {
                double expected = 0;
                for (unsigned choice=0; choice<16; choice++) {
                    if ((choice & ~mask) == (assignment & ~mask)) expected -= numeric_value(leaves[choice]);
                }
                test_assert(numeric_value(leaf_at(roots[4], assignment)) == expected);
            }
        }
        for (unsigned i=0; i<16; i++) {
            roots[4] = mtbdd_map_reduce(leaves[i], roots[3], neg, sum, roots[1], 0);
            test_assert(numeric_value(roots[4]) == -16 * numeric_value(leaves[i]));
        }
#ifdef SYLVAN_TEST_GMP
        if (type == 3) {
            roots[4] = mtbdd_map_reduce(roots[0], roots[3], map_gmp_collect_CALL, reduce_gmp_collect_CALL, roots[1], 0);
            double expected = 0;
            for (unsigned i=0; i<16; i++) expected -= numeric_value(leaves[i]);
            test_assert(numeric_value(roots[4]) == expected);
            roots[4] = mtbdd_map(roots[0], map_gmp_collect_CALL, 0);
            for (unsigned i=0; i<16; i++) test_assert(numeric_value(leaf_at(roots[4], i)) == -numeric_value(leaves[i]));
        }
#endif
        roots[4] = mtbdd_map_reduce(roots[0], roots[3], map_number_CALL, reduce_sum_CALL, mtbdd_false, 9);
        int64_t expected = 0;
        for (unsigned i=0; i<16; i++) expected += (int64_t)numeric_value(leaves[i]) + 9;
        test_assert(mtbdd_getint64(roots[4]) == expected);
    }
    unprotect_roots(roots, 6);
    unprotect_roots(leaves, 16);
    return 0;
}

static void fill_table(void)
{
    /* Insert disposable valid leaves without invoking the automatic collector. */
    size_t size = llmsset_get_size(nodes);
    size_t count = llmsset_count_marked(nodes);
    for (size_t i=0; i<4*size && count<size; i++) {
        struct mtbddnode leaf;
        mtbddnode_makeleaf(&leaf, 0, UINT64_C(1000000) + i);
        int created = 0;
        if (llmsset_lookup(nodes, leaf.a, leaf.b, &created) && created) count++;
    }
}

TASK_0(int, test_collection)
{
    MTBDD leaves[32], roots[5];
    protect_roots(leaves, 32);
    protect_roots(roots, 5);
    for (unsigned i=0; i<16; i++) {
        leaves[i] = mtbdd_double((double)((i * 7 + 3) % 17));
        leaves[16+i] = mtbdd_double((double)((i * 11 + 2) % 19));
    }
    roots[0] = from_table(leaves, 4, 0, 0);
    roots[1] = from_table(leaves+16, 4, 0, 0);
    /* One ordering and one tolerance comparison exercise allocation under GC. */
    const unsigned relations[] = {0, 4};
    for (unsigned r=0; r<2; r++) {
        unsigned relation = relations[r];
        roots[2] = mtbdd_invalid;
        CALL(sylvan_gc);
        fill_table();
        unsigned before = atomic_load(&collections);
        roots[2] = CALL(compare, roots[0], roots[1], relation, 0, 3.5);
        test_assert(atomic_load(&collections) > before);
        for (unsigned assignment=0; assignment<16; assignment++) {
            int expected = leaf_relation(leaves[assignment], leaves[16+assignment], relation, 3.5);
            test_assert(leaf_at(roots[2], assignment) == (expected ? mtbdd_true : mtbdd_false));
        }
    }
    /* Parallel scalar and pointwise traversals with collections requested by
     * another task; inputs are retained by the outer protected roots. */
    for (int repeat=0; repeat<2; repeat++) {
        SPAWN(check_comparisons, roots[0], roots[1], 6, 3.5);
        for (int gc=0; gc<2; gc++) CALL(sylvan_gc);
        test_assert(SYNC(check_comparisons) == 0);
    }

    /* Retain inputs above the hash-table size after explicit shrinking. */
    const size_t count = 12000;
    MTBDD *garbage = malloc(count * sizeof(*garbage));
    test_assert(garbage != NULL);
    for (size_t i=0; i<count; i++) garbage[i] = mtbdd_ref(mtbdd_int64((int64_t)i + 50000));
    roots[0] = mtbdd_makenode(1, leaves[0], leaves[1]);
    roots[1] = mtbdd_makenode(2, leaves[17], leaves[19]);
    for (size_t i=0; i<count; i++) mtbdd_deref(garbage[i]);
    free(garbage);
    CALL(sylvan_gc);
    test_assert(MTBDD_GETINDEX(roots[0]) >= llmsset_get_size(nodes));
    test_assert(MTBDD_GETINDEX(roots[0]) < llmsset_get_data_size(nodes));
    test_assert(CALL(check_comparisons, roots[0], roots[1], 6, 0.1) == 0);
    roots[3] = variable_set(15);
    unsigned before = atomic_load(&collections);
    roots[4] = mtbdd_map(roots[0], map_collect_CALL, 1);
    for (unsigned i=0; i<16; i++) {
        test_assert(mtbdd_getint64(leaf_at(roots[4], i)) == (int64_t)numeric_value(leaf_at(roots[0], i)) + 1);
    }
    roots[4] = mtbdd_map_reduce(roots[0], roots[3], map_collect_CALL, reduce_collect_CALL, mtbdd_false, 1);
    int64_t expected = 0;
    for (unsigned i=0; i<16; i++) expected += (int64_t)numeric_value(leaf_at(roots[0], i)) + 1;
    test_assert(mtbdd_getint64(roots[4]) == expected);
    test_assert(atomic_load(&collections) > before);
    unprotect_roots(roots, 5);
    unprotect_roots(leaves, 32);
    return 0;
}

TASK_0(int, runtests)
{
    if (CALL(test_comparisons)) return 1;
    if (CALL(test_universal_ordering)) return 1;
    for (unsigned type=0; type<4; type++) if (CALL(test_mapping, type)) return 1;
    if (CALL(test_mapping_cache)) return 1;
    if (CALL(test_order_and_failures)) return 1;
    if (CALL(test_numeric_maps)) return 1;
    if (CALL(test_collection)) return 1;
#if SYLVAN_STATS
    sylvan_stats_t stats;
    sylvan_stats_snapshot(&stats);
    const size_t counters[] = {MTBDD_MAP, MTBDD_MAP_REDUCE,
        MTBDD_COMPARE_LEQ, MTBDD_COMPARE_LT, MTBDD_COMPARE_GEQ, MTBDD_COMPARE_GT,
        MTBDD_COMPARE_EQUAL_ABS, MTBDD_COMPARE_EQUAL_REL,
        MTBDD_ANY_LEQ, MTBDD_ANY_LT, MTBDD_ANY_GEQ, MTBDD_ANY_GT,
        MTBDD_ANY_EQUAL_ABS, MTBDD_ANY_EQUAL_REL, MTBDD_ALL_EQUAL_ABS, MTBDD_ALL_EQUAL_REL,
        MTBDD_ALL_LEQ, MTBDD_ALL_LT, MTBDD_ALL_GEQ, MTBDD_ALL_GT};
    for (size_t i=0; i<sizeof(counters)/sizeof(counters[0]); i++) test_assert(stats.counters[counters[i]] > 0);
#endif
    return 0;
}

int main(int argc, char **argv)
{
    lace_start(argc > 1 ? (unsigned)atoi(argv[1]) : 1, 0);
    sylvan_set_sizes(1<<10, 1<<18, 1<<10, 1<<16);
    sylvan_init_package();
    sylvan_init_mtbdd();
#ifdef SYLVAN_TEST_GMP
    gmp_init();
#endif
    sylvan_gc_hook_pregc(count_collection_CALL);
    int result = RUN(runtests);
    sylvan_quit();
    lace_stop();
    return result;
}
