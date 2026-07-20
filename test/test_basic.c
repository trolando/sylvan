#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <inttypes.h>
#include <math.h>

#include <lace.h>
#include <sylvan/internal.h>
#include <sylvan/platform.h>

#include "test_assert.h"

typedef int (*test_bdd_binary_op)(BDD*, BDD, BDD);

static BDD
test_bdd_binary(test_bdd_binary_op op, BDD a, BDD b)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = op(&result, a, b);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD test_bdd_and(BDD a, BDD b) { return test_bdd_binary(bdd_and, a, b); }
static BDD test_bdd_xor(BDD a, BDD b) { return test_bdd_binary(bdd_xor, a, b); }
static BDD test_bdd_xnor(BDD a, BDD b) { return test_bdd_binary(bdd_xnor, a, b); }
static BDD test_bdd_or(BDD a, BDD b) { return test_bdd_binary(bdd_or, a, b); }
static BDD test_bdd_nand(BDD a, BDD b) { return test_bdd_binary(bdd_nand, a, b); }
static BDD test_bdd_nor(BDD a, BDD b) { return test_bdd_binary(bdd_nor, a, b); }
static BDD test_bdd_imp(BDD a, BDD b) { return test_bdd_binary(bdd_imp, a, b); }
static BDD test_bdd_diff(BDD a, BDD b) { return test_bdd_binary(bdd_diff, a, b); }

static BDD
test_bdd_ite(BDD a, BDD b, BDD c)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    test_assert(bdd_ite(&result, a, b, c) == SYLVAN_OK);
    mtbdd_unprotect(&result);
    return result;
}

TASK(int, test_protected_destinations)
int
test_protected_destinations_CALL(lace_worker *lace)
{
    BDD a = bdd_var_at_level(0);
    BDD b = bdd_var_at_level(1);
    BDD c = bdd_var_at_level(2);
    BDD and_result = mtbdd_invalid;
    BDD ite_result = mtbdd_invalid;
    BDD xor_result = mtbdd_invalid;
    BDD expected_and = mtbdd_make_node(0, bdd_false, b);
    BDD expected_ite = mtbdd_make_node(0, c, b);
    BDD expected_xor = mtbdd_make_node(0, b, bdd_not(b));

    mtbdd_refs_pushptr(&a);
    mtbdd_refs_pushptr(&b);
    mtbdd_refs_pushptr(&c);
    mtbdd_refs_pushptr(&and_result);
    mtbdd_refs_pushptr(&ite_result);
    mtbdd_refs_pushptr(&xor_result);
    mtbdd_refs_pushptr(&expected_and);
    mtbdd_refs_pushptr(&expected_ite);
    mtbdd_refs_pushptr(&expected_xor);

    bdd_and_SPAWN(lace, &and_result, a, b);
    bdd_xor_SPAWN(lace, &xor_result, a, b);
    int ite_status = bdd_ite_CALL(lace, &ite_result, a, b, c);
    int xor_status = bdd_xor_SYNC(lace);
    int and_status = bdd_and_SYNC(lace);

    test_assert(and_status == SYLVAN_OK);
    test_assert(ite_status == SYLVAN_OK);
    test_assert(xor_status == SYLVAN_OK);

    /* The task results must already be rooted when collection starts. */
    sylvan_gc_CALL(lace);
    test_assert(and_result == expected_and);
    test_assert(ite_result == expected_ite);
    test_assert(xor_result == expected_xor);

    BDD unchanged = bdd_true;
    mtbdd_refs_pushptr(&unchanged);
    test_assert(bdd_and_CALL(lace, &unchanged, mtbdd_invalid, b) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_ite_CALL(lace, &unchanged, a, b, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_xor_CALL(lace, &unchanged, mtbdd_invalid, b) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    test_bdd_binary_op derived_ops[] = {
        bdd_xnor, bdd_or, bdd_nand, bdd_nor, bdd_imp, bdd_diff
    };
    for (size_t i = 0; i < sizeof(derived_ops) / sizeof(derived_ops[0]); i++) {
        test_assert(derived_ops[i](&unchanged, mtbdd_invalid, b) == SYLVAN_ERR_INVALID);
        test_assert(unchanged == bdd_true);
        test_assert(derived_ops[i](NULL, a, b) == SYLVAN_ERR_INVALID);
    }

    test_assert(bdd_and_CALL(lace, NULL, a, b) == SYLVAN_ERR_INVALID);
    test_assert(bdd_ite_CALL(lace, NULL, a, b, c) == SYLVAN_ERR_INVALID);
    test_assert(bdd_xor_CALL(lace, NULL, a, b) == SYLVAN_ERR_INVALID);

    mtbdd_refs_popptr(10);
    return 0;
}

SYLVAN_TLS uint64_t seed = 1;

uint64_t
xorshift_rand(void)
{
    uint64_t x = seed;
    if (seed == 0) seed = rand();
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    seed = x;
    return x * 2685821657736338717LL;
}

double
uniform_deviate(uint64_t seed)
{
    return seed * (1.0 / ((double)(UINT64_MAX) + 1.0));
}

int
rng(int low, int high)
{
    return low + (int)(uniform_deviate(xorshift_rand()) * (high-low));
}

static int
test_cache()
{
    test_assert(cache_getused() == 0);

    /**
     * Test cache for large number of random entries
     */

    size_t number_add = 4000000;
    uint64_t *arr = (uint64_t*)malloc(sizeof(uint64_t)*4*number_add);
    for (size_t i=0; i<number_add*4; i++) arr[i] = xorshift_rand();
    for (size_t i=0; i<number_add; i++) {
        test_assert(cache_put(arr[4*i], arr[4*i+1], arr[4*i+2], arr[4*i+3]));
        uint64_t val;
        int res = cache_get(arr[4*i], arr[4*i+1], arr[4*i+2], &val);
        test_assert(res == 1);
        test_assert(val == arr[4*i+3]);
    }
    size_t count = 0;
    for (size_t i=0; i<number_add; i++) {
        uint64_t val;
        int res = cache_get(arr[4*i], arr[4*i+1], arr[4*i+2], &val);
        test_assert(res == 0 || val == arr[4*i+3]);
        if (res) count++;
    }
    test_assert(count == cache_getused());

    /**
     * Now also test for double entries
     */

    for (size_t i=0; i<number_add/2; i++) {
        test_assert(cache_put6(arr[8*i], arr[8*i+1], arr[8*i+2], arr[8*i+3], arr[8*i+4], arr[8*i+5], arr[8*i+6], arr[8*i+7]));
        uint64_t val1, val2;
        int res = cache_get6(arr[8*i], arr[8*i+1], arr[8*i+2], arr[8*i+3], arr[8*i+4], arr[8*i+5], &val1, &val2);
        test_assert(res == 1);
        test_assert(val1 == arr[8*i+6]);
        test_assert(val2 == arr[8*i+7]);
    }
    for (size_t i=0; i<number_add/2; i++) {
        uint64_t val1, val2;
        int res = cache_get6(arr[8*i], arr[8*i+1], arr[8*i+2], arr[8*i+3], arr[8*i+4], arr[8*i+5], &val1, &val2);
        test_assert(res == 0 || (val1 == arr[8*i+6] && val2 == arr[8*i+7]));
    }

    /**
     * And test that single entries are not corrupted
     */
    for (size_t i=0; i<number_add; i++) {
        uint64_t val;
        int res = cache_get(arr[4*i], arr[4*i+1], arr[4*i+2], &val);
        test_assert(res == 0 || val == arr[4*i+3]);
    }

    /**
     * TODO: multithreaded test
     */

    free(arr);
    return 0;
}

static inline BDD
make_random(int i, int j)
{
    if (i == j) return rng(0, 2) ? bdd_true : bdd_false;

    BDD yes = make_random(i+1, j);
    BDD no = make_random(i+1, j);
    BDD result = mtbdd_invalid;

    switch(rng(0, 4)) {
    case 0:
        result = no;
        mtbdd_deref(yes);
        break;
    case 1:
        result = yes;
        mtbdd_deref(no);
        break;
    case 2:
        result = mtbdd_ref(mtbdd_make_node(i, yes, no));
        mtbdd_deref(no);
        mtbdd_deref(yes);
        break;
    case 3:
    default:
        result = mtbdd_ref(mtbdd_make_node(i, no, yes));
        mtbdd_deref(no);
        mtbdd_deref(yes);
        break;
    }

    return result;
}

static LISTDD
make_random_ldd_set(int depth, int maxvalue, int elements)
{
    uint32_t *values = (uint32_t*)malloc((size_t)depth * sizeof(*values));
    LISTDD result = listdd_empty; // empty set
    for (int i=0; i<elements; i++) {
        listdd_refs_push(result);
        for (int j=0; j<depth; j++) {
            values[j] = rng(0, maxvalue);
        }
        result = listdd_add(result, values, depth);
        listdd_refs_pop(1);
    }
    free(values);
    return result;
}

static int
test_mtbdd()
{
    MTBDD fraction = mtbdd_fraction(-6, 8);
    test_assert(fraction != mtbdd_invalid);
    int32_t numerator;
    uint32_t denominator;
    test_assert(mtbdd_leaf_fraction(fraction, &numerator, &denominator) == 0);
    test_assert(numerator == -3);
    test_assert(denominator == 4);

    fraction = mtbdd_fraction(INT64_MIN, UINT64_C(1) << 33);
    test_assert(fraction != mtbdd_invalid);
    test_assert(mtbdd_leaf_fraction(fraction, &numerator, &denominator) == 0);
    test_assert(numerator == -1073741824);
    test_assert(denominator == 1);

    test_assert(mtbdd_fraction(INT64_MIN, 1) == mtbdd_invalid);
    test_assert(mtbdd_fraction(1, 0) == mtbdd_invalid);
    test_assert(mtbdd_leaf_fraction(mtbdd_int64(1), &numerator, &denominator) == -1);

    MTBDD if_false, if_true;
    MTBDD root = mtbdd_make_node(3, mtbdd_int64(1), mtbdd_int64(2));
    mtbdd_cofactors(root, &if_false, &if_true);
    test_assert(if_false == mtbdd_int64(1));
    test_assert(if_true == mtbdd_int64(2));
    mtbdd_cofactors(mtbdd_int64(3), &if_false, &if_true);
    test_assert(if_false == mtbdd_int64(3) && if_true == mtbdd_int64(3));

    uint32_t variables[64];
    for (uint32_t i=0; i<64; i++) variables[i] = i;
    MTBDD variable_set = bdd_set_from_array(variables, 64);

    MTBDD result = mtbdd_abstract_add(mtbdd_double(1.0), variable_set);
    test_assert(result != mtbdd_invalid);
    test_assert(mtbdd_leaf_double(result) == ldexp(1.0, 64));

    result = mtbdd_abstract_mul(mtbdd_double(0.5), variable_set);
    test_assert(result != mtbdd_invalid);
    test_assert(mtbdd_leaf_double(result) == 0.0);

    uint32_t early_level[] = {0};
    uint32_t late_level[] = {1};
    MTBDD early_var = mtbdd_set_from_array(early_level, 1);
    MTBDD late_var = mtbdd_set_from_array(late_level, 1);
    MTBDD terminal = mtbdd_int64(42);
    MTBDD other_terminal = mtbdd_int64(7);
    MTBDD later_cube = mtbdd_cube(late_var, (uint8_t[]){1}, terminal);
    MTBDD earlier_result = mtbdd_union_cube(later_cube, early_var, (uint8_t[]){0}, other_terminal);
    test_assert(mtbdd_getvar(earlier_result) == 0);
    test_assert(mtbdd_getlow(earlier_result) == other_terminal);
    test_assert(mtbdd_gethigh(earlier_result) == later_cube);
    test_assert(mtbdd_union_cube(later_cube, early_var, (uint8_t[]){2}, other_terminal) == other_terminal);

    return 0;
}

int testEqual(BDD a, BDD b)
{
    if (a == b) return 1;

    if (a == mtbdd_invalid) {
        fprintf(stderr, "a is invalid!\n");
        return 0;
    }

    if (b == mtbdd_invalid) {
        fprintf(stderr, "b is invalid!\n");
        return 0;
    }

    fprintf(stderr, "a and b are not equal!\n");

    bdd_fprint(stderr, a);fprintf(stderr, "\n");
    bdd_fprint(stderr, b);fprintf(stderr, "\n");

    return 0;
}

int
test_bdd()
{
    test_assert(test_bdd_ite(bdd_var_at_level(1), bdd_true, bdd_true) == bdd_not(test_bdd_ite(bdd_var_at_level(1), bdd_false, bdd_false)));
    test_assert(test_bdd_ite(bdd_var_at_level(1), bdd_false, bdd_true) == bdd_not(test_bdd_ite(bdd_var_at_level(1), bdd_true, bdd_false)));
    test_assert(test_bdd_ite(bdd_var_at_level(1), bdd_true, bdd_false) == bdd_not(test_bdd_ite(bdd_var_at_level(1), bdd_false, bdd_true)));
    test_assert(test_bdd_ite(bdd_var_at_level(1), bdd_false, bdd_false) == bdd_not(test_bdd_ite(bdd_var_at_level(1), bdd_true, bdd_true)));

    return 0;
}

int
test_cube()
{
    const BDDSET vars = bdd_set_from_array(((uint32_t[]){1,2,3,4,6,8}), 6);

    uint8_t cube[6], check[6];
    int i, j;
    for (i=0;i<6;i++) cube[i] = rng(0,3);
    BDD bdd = bdd_cube(vars, cube);

    bdd_pick_cube_values(bdd, vars, check);
    for (i=0; i<6;i++) test_assert(cube[i] == check[i]);

    BDD picked_single = bdd_pick_minterm(bdd, vars);
    test_assert(testEqual(test_bdd_and(picked_single, bdd), picked_single));
    assert(bdd_sat_count(picked_single, vars)==1);

    BDD picked = bdd_pick_cube(bdd, vars);
    test_assert(testEqual(test_bdd_and(picked, bdd), picked));

    BDD t1 = bdd_cube(vars, ((uint8_t[]){1,1,2,2,0,0}));
    BDD t2 = bdd_cube(vars, ((uint8_t[]){1,1,1,0,0,2}));
    test_assert(testEqual(bdd_or_cube(t1, vars, ((uint8_t[]){1,1,1,0,0,2})), test_bdd_or(t1, t2)));
    t2 = bdd_cube(vars, ((uint8_t[]){2,2,2,1,1,0}));
    test_assert(testEqual(bdd_or_cube(t1, vars, ((uint8_t[]){2,2,2,1,1,0})), test_bdd_or(t1, t2)));
    t2 = bdd_cube(vars, ((uint8_t[]){1,1,1,0,0,0}));
    test_assert(testEqual(bdd_or_cube(t1, vars, ((uint8_t[]){1,1,1,0,0,0})), test_bdd_or(t1, t2)));

    bdd = make_random(1, 16);
    const BDDSET all_vars = bdd_set_from_array(
        ((uint32_t[]){1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}), 15);
    for (j=0;j<10;j++) {
        for (i=0;i<6;i++) cube[i] = rng(0,3);
        BDD c = bdd_cube(vars, cube);
        test_assert(bdd_or_cube(bdd, vars, cube) == test_bdd_or(bdd, c));
    }

    for (i=0;i<10;i++) {
        picked = bdd_pick_cube(bdd, all_vars);
        test_assert(testEqual(test_bdd_and(picked, bdd), picked));
    }

    const BDDSET limited_vars = bdd_set_from_array(((uint32_t[]){1,3,8}), 3);
    picked = bdd_pick_cube(bdd, limited_vars);
    test_assert(bdd == bdd_false || test_bdd_and(picked, bdd) != bdd_false);

    BDD x = bdd_var_at_level(1);
    BDD y = bdd_var_at_level(2);
    test_assert(bdd_cofactor(test_bdd_xor(x, y), x) == bdd_not(y));
    test_assert(bdd_cofactor(test_bdd_xor(x, y), bdd_not(x)) == y);
    test_assert(bdd_cofactor(test_bdd_xor(x, y), test_bdd_or(x, y)) == mtbdd_invalid);

    BDD restricted = bdd_restrict(bdd, picked);
    test_assert(mtbdd_node_count(restricted) <= mtbdd_node_count(bdd));
    test_assert(test_bdd_and(restricted, picked) == test_bdd_and(bdd, picked));

    // simple test for mtbdd_enum_all
    uint8_t arr[6];
    MTBDD leaf = mtbdd_first_minterm(bdd_true, vars, arr, NULL);
    test_assert(leaf == bdd_true);
    test_assert(mtbdd_first_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 0 && arr[5] == 0);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 0 && arr[5] == 1);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 1 && arr[5] == 0);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 1 && arr[5] == 1);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 1 && arr[4] == 0 && arr[5] == 0);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 1 && arr[4] == 0 && arr[5] == 1);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 1 && arr[4] == 1 && arr[5] == 0);

    mtbdd_first_minterm(bdd_true, vars, arr, NULL);
    size_t count = 1;
    while (mtbdd_next_minterm(bdd_true, vars, arr, NULL) != mtbdd_undefined) {
        test_assert(count < 64);
        count++;
    }
    test_assert(count == 64);

    return 0;
}

static int
test_operators()
{
    // We need to test: xor, and, or, nand, nor, imp, biimp, invimp, diff, less

    //int i;
    BDD a = bdd_var_at_level(1);
    BDD b = bdd_var_at_level(2);
    BDD one = make_random(1, 12);
    BDD two = make_random(6, 24);

    // Test or
    test_assert(testEqual(test_bdd_or(a, b), mtbdd_make_node(1, b, bdd_true)));
    test_assert(testEqual(test_bdd_or(a, b), test_bdd_or(b, a)));
    test_assert(testEqual(test_bdd_or(one, two), test_bdd_or(two, one)));

    // Test and
    test_assert(testEqual(test_bdd_and(a, b), mtbdd_make_node(1, bdd_false, b)));
    test_assert(testEqual(test_bdd_and(a, b), test_bdd_and(b, a)));
    test_assert(testEqual(test_bdd_and(one, two), test_bdd_and(two, one)));

    // Test xor
    test_assert(testEqual(test_bdd_xor(a, b), mtbdd_make_node(1, b, bdd_not(b))));
    test_assert(testEqual(test_bdd_xor(a, b), test_bdd_xor(a, b)));
    test_assert(testEqual(test_bdd_xor(a, b), test_bdd_xor(b, a)));
    test_assert(testEqual(test_bdd_xor(one, two), test_bdd_xor(two, one)));
    test_assert(testEqual(test_bdd_xor(a, b), test_bdd_ite(a, bdd_not(b), b)));

    // Test diff
    test_assert(testEqual(test_bdd_diff(a, b), test_bdd_diff(a, b)));
    test_assert(testEqual(test_bdd_diff(a, b), test_bdd_diff(a, test_bdd_and(a, b))));
    test_assert(testEqual(test_bdd_diff(a, b), test_bdd_and(a, bdd_not(b))));
    test_assert(testEqual(test_bdd_diff(a, b), test_bdd_ite(b, bdd_false, a)));
    test_assert(testEqual(test_bdd_diff(one, two), test_bdd_diff(one, two)));
    test_assert(testEqual(test_bdd_diff(one, two), test_bdd_diff(one, test_bdd_and(one, two))));
    test_assert(testEqual(test_bdd_diff(one, two), test_bdd_and(one, bdd_not(two))));
    test_assert(testEqual(test_bdd_diff(one, two), test_bdd_ite(two, bdd_false, one)));

    // Test biimp
    test_assert(testEqual(test_bdd_xnor(a, b), mtbdd_make_node(1, bdd_not(b), b)));
    test_assert(testEqual(test_bdd_xnor(a, b), test_bdd_xnor(b, a)));
    test_assert(testEqual(test_bdd_xnor(one, two), test_bdd_xnor(two, one)));

    // Test nand / and
    test_assert(testEqual(bdd_not(test_bdd_and(a, b)), test_bdd_nand(b, a)));
    test_assert(testEqual(bdd_not(test_bdd_and(one, two)), test_bdd_nand(two, one)));

    // Test nor / or
    test_assert(testEqual(bdd_not(test_bdd_or(a, b)), test_bdd_nor(b, a)));
    test_assert(testEqual(bdd_not(test_bdd_or(one, two)), test_bdd_nor(two, one)));

    // Test xor / biimp
    test_assert(testEqual(test_bdd_xor(a, b), bdd_not(test_bdd_xnor(b, a))));
    test_assert(testEqual(test_bdd_xor(one, two), bdd_not(test_bdd_xnor(two, one))));

    // Test imp
    test_assert(testEqual(test_bdd_imp(a, b), test_bdd_ite(a, b, bdd_true)));
    test_assert(testEqual(test_bdd_imp(one, two), test_bdd_ite(one, two, bdd_true)));
    test_assert(testEqual(test_bdd_imp(one, two), bdd_not(test_bdd_diff(one, two))));
    test_assert(testEqual(test_bdd_imp(two, one), bdd_not(test_bdd_diff(two, one))));
    test_assert(testEqual(test_bdd_imp(a, b), bdd_not(test_bdd_diff(a, b))));
    test_assert(testEqual(test_bdd_imp(one, two), bdd_not(test_bdd_diff(one, two))));

    return 0;
}

static int
test_disjoint_subset()
{
    // We need to test: disjoint, subset
#define VARS 3    
    BDD v[VARS];
    for (int i=0; i<VARS; i++) v[i] = bdd_not(bdd_var_at_level(i));
#undef VARS

    BDD test_input[] = {
        bdd_true, bdd_false,
        bdd_false, bdd_true,
        v[0], v[1],
        v[1], v[1],
        v[0], bdd_not(v[0]),
        test_bdd_and(v[0],v[1]), v[2],
        test_bdd_and(v[0],v[1]), test_bdd_and(bdd_not(v[0]),v[1]),
        test_bdd_and(v[0],v[1]), test_bdd_or(bdd_not(v[0]),v[2]),
        test_bdd_and(v[0],v[1]), test_bdd_and(v[0],bdd_not(v[1])),
        test_bdd_or(v[0],v[1]), test_bdd_and(v[0],bdd_not(v[1])),
        test_bdd_and(v[1],test_bdd_or(v[0],v[2])), test_bdd_or(v[1],test_bdd_and(v[0],bdd_not(v[2])))
    };

    for (int i=0; i<11; i++) {
        BDD t1 = test_input[2*i];
        BDD t2 = test_input[2*i+1];
        test_assert(bdd_disjoint(t1,t2) == (test_bdd_and(t1,t2)==bdd_false));
        test_assert(bdd_subseteq(t1,t2) == (test_bdd_or(bdd_not(t1),t2) == bdd_true));
    }

    return 0;
}

int
test_relprod()
{
    uint32_t vars[] = {0,2,4};
    uint32_t all_vars[] = {0,1,2,3,4,5};

    BDDSET vars_set = bdd_set_from_array(vars, 3);
    BDDSET all_vars_set = bdd_set_from_array(all_vars, 6);

    BDD s, t, next, prev;
    BDD zeroes, ones;

    // transition relation: 000 --> 111 and !000 --> 000
    t = bdd_false;
    t = bdd_or_cube(t, all_vars_set, ((uint8_t[]){0,1,0,1,0,1}));
    t = bdd_or_cube(t, all_vars_set, ((uint8_t[]){1,0,2,0,2,0}));
    t = bdd_or_cube(t, all_vars_set, ((uint8_t[]){2,0,1,0,2,0}));
    t = bdd_or_cube(t, all_vars_set, ((uint8_t[]){2,0,2,0,1,0}));

    s = bdd_cube(vars_set, (uint8_t[]){0,0,1});
    zeroes = bdd_cube(vars_set, (uint8_t[]){0,0,0});
    ones = bdd_cube(vars_set, (uint8_t[]){1,1,1});

    next = bdd_rel_next(s, t, all_vars_set);
    prev = bdd_rel_prev(t, next, all_vars_set);
    test_assert(next == zeroes);
    test_assert(prev == bdd_not(zeroes));

    next = bdd_rel_next(next, t, all_vars_set);
    prev = bdd_rel_prev(t, next, all_vars_set);
    test_assert(next == ones);
    test_assert(prev == zeroes);

    t = bdd_cube(all_vars_set, (uint8_t[]){0,0,0,0,0,1});
    test_assert(bdd_rel_prev(t, s, all_vars_set) == zeroes);
    test_assert(bdd_rel_prev(t, bdd_not(s), all_vars_set) == bdd_false);
    test_assert(bdd_rel_next(s, t, all_vars_set) == bdd_false);
    test_assert(bdd_rel_next(zeroes, t, all_vars_set) == s);

    t = bdd_cube(all_vars_set, (uint8_t[]){0,0,0,0,0,2});
    test_assert(bdd_rel_prev(t, s, all_vars_set) == zeroes);
    test_assert(bdd_rel_prev(t, zeroes, all_vars_set) == zeroes);
    test_assert(bdd_rel_next(bdd_not(zeroes), t, all_vars_set) == bdd_false);

    return 0;
}

int
test_compose()
{
    BDD a = bdd_var_at_level(1);
    BDD b = bdd_var_at_level(2);

    BDD a_or_b = test_bdd_or(a, b);

    BDD one = make_random(3, 16);
    BDD two = make_random(8, 24);

    MTBDDMAP map = mtbdd_map_empty();

    map = mtbdd_map_set(map, 1, one);
    map = mtbdd_map_set(map, 2, two);

    test_assert(mtbdd_map_key(map) == 1);
    test_assert(mtbdd_map_value(map) == one);
    test_assert(mtbdd_map_key(mtbdd_map_next(map)) == 2);
    test_assert(mtbdd_map_value(mtbdd_map_next(map)) == two);

    test_assert(testEqual(one, bdd_compose(a, map)));
    test_assert(testEqual(two, bdd_compose(b, map)));

    test_assert(testEqual(test_bdd_or(one, two), bdd_compose(a_or_b, map)));

    map = mtbdd_map_set(map, 2, one);
    test_assert(testEqual(bdd_compose(a_or_b, map), one));

    map = mtbdd_map_set(map, 1, two);
    test_assert(testEqual(test_bdd_or(one, two), bdd_compose(a_or_b, map)));

    test_assert(testEqual(test_bdd_and(one, two), bdd_compose(test_bdd_and(a, b), map)));

    // test that composing [0:=true] on "0" yields true
    map = mtbdd_map_set(mtbdd_map_empty(), 1, bdd_true);
    test_assert(testEqual(bdd_compose(a, map), bdd_true));

    // test that composing [0:=false] on "0" yields false
    map = mtbdd_map_set(mtbdd_map_empty(), 1, bdd_false);
    test_assert(testEqual(bdd_compose(a, map), bdd_false));

    return 0;
}

int
test_ldd()
{
    // very basic testing of makenode
    for (int i=0; i<10; i++) {
        uint32_t value = rng(0, 100);
        LISTDD m = listdd_make_node(value, listdd_empty_list, listdd_empty);
        test_assert(listdd_node_value(m) == value);
        test_assert(listdd_node_down(m) == listdd_empty_list);
        test_assert(listdd_node_right(m) == listdd_empty);
        test_assert(listdd_is_copy_node(m) == 0);
        test_assert(listdd_follow(m, value) == listdd_empty_list);
        for (int j=0; j<100; j++) {
            uint32_t other_value = rng(0, 100);
            if (value != other_value) test_assert(listdd_follow(m, other_value) == listdd_empty);
        }
    }

    // test handling of the copy node by primitives
    LISTDD m = listdd_make_copy_node(listdd_empty_list, listdd_empty);
    test_assert(listdd_is_copy_node(m) == 1);
    test_assert(listdd_node_value(m) == 0);
    test_assert(listdd_node_down(m) == listdd_empty_list);
    test_assert(listdd_node_right(m) == listdd_empty);
    m = listdd_extend_node(m, 0, listdd_empty_list);
    test_assert(listdd_is_copy_node(m) == 1);
    test_assert(listdd_node_value(m) == 0);
    test_assert(listdd_node_down(m) == listdd_empty_list);
    test_assert(listdd_node_right(m) != listdd_empty);
    test_assert(listdd_follow(m, 0) == listdd_empty_list);
    test_assert(listdd_node_value(listdd_node_right(m)) == 0);
    test_assert(listdd_is_copy_node(listdd_node_right(m)) == 0);
    test_assert(listdd_make_node(0, listdd_empty_list, listdd_empty) == listdd_node_right(m));

    // test union_cube
    for (int i=0; i<100; i++) {
        int depth = rng(1, 6);
        int elements = rng(1, 30);
        m = make_random_ldd_set(depth, 10, elements);
        assert(m != listdd_empty_list);
        assert(m != listdd_empty);
        assert(listdd_count(m) <= elements);
        assert(listdd_count(m) >= 1);
    }

    // test simply transition relation
    {
        LISTDD states, rel, meta, expected;

        // relation: (0,0) to (1,1)
        rel = listdd_singleton((uint32_t[]){0,1,0,1}, 4);
        test_assert(listdd_count(rel) == 1);
        // relation: (0,0) to (2,2)
        rel = listdd_add(rel, (uint32_t[]){0,2,0,2}, 4);
        test_assert(listdd_count(rel) == 2);
        // meta: read write read write
        meta = listdd_singleton((uint32_t[]){1,2,1,2}, 4);
        test_assert(listdd_count(meta) == 1);
        // initial state: (0,0)
        states = listdd_singleton((uint32_t[]){0,0}, 2);
        test_assert(listdd_count(states) == 1);
        // relprod should give two states
        states = listdd_rel_next(states, rel, meta);
        test_assert(listdd_count(states) == 2);
        // relprod should give states (1,1) and (2,2)
        expected = listdd_singleton((uint32_t[]){1,1}, 2);
        expected = listdd_add(expected, (uint32_t[]){2,2}, 2);
        test_assert(states == expected);

        // now test relprod union on the simple example
        states = listdd_singleton((uint32_t[]){0,0}, 2);
        states = listdd_rel_next_union(states, rel, meta, states);
        test_assert(listdd_count(states) == 3);
        test_assert(states == listdd_union(states, expected));

        // now create transition (1,1) --> (1,1) (using copy nodes)
        rel = listdd_relation_singleton((uint32_t[]){1,0,1,0}, (int[]){0,1,0,1}, 4);
        states = listdd_rel_next(states, rel, meta);
        // the result should be just state (1,1)
        test_assert(states == listdd_singleton((uint32_t[]){1,1}, 2));

        LISTDD statezero = listdd_singleton((uint32_t[]){0,0}, 2);
        states = listdd_add(statezero, (uint32_t[]){1,1}, 2);
        test_assert(listdd_rel_next_union(states, rel, meta, statezero) == states);

        // now create transition (*,*) --> (*,*) (copy nodes)
        rel = listdd_relation_singleton((uint32_t[]){0,0}, (int[]){1,1}, 2);
        meta = listdd_singleton((uint32_t[]){4,4}, 2);
        states = make_random_ldd_set(2, 10, 10);
        LISTDD states2 = make_random_ldd_set(2, 10, 10);
        test_assert(listdd_union(states, states2) == listdd_rel_next_union(states, rel, meta, states2));
    }

    return 0;
}

TASK(int, runtests)
int runtests_CALL(lace_worker* lace)
{
    printf("Testing protected destinations.\n");
    for (int j = 0; j < 10; j++) {
        if (test_protected_destinations_CALL(lace)) return 1;
    }

    // we are not testing garbage collection
    sylvan_gc_disable();

    printf("Testing cache.\n");
    if (test_cache()) return 1;
    printf("Testing bdd.\n");
    if (test_bdd()) return 1;
    printf("Testing mtbdd.\n");
    if (test_mtbdd()) return 1;
    printf("Testing cube.\n");
    for (int j=0;j<10;j++) if (test_cube()) return 1;
    printf("Testing relprod.\n");
    for (int j=0;j<10;j++) if (test_relprod()) return 1;
    printf("Testing compose.\n");
    for (int j=0;j<10;j++) if (test_compose()) return 1;
    printf("Testing operators.\n");
    for (int j=0;j<10;j++) if (test_operators()) return 1;
    printf("Testing disjoint and subset.\n");
    for (int j=0;j<10;j++) if (test_disjoint_subset()) return 1;

    printf("Testing ldd.\n");
    if (test_ldd()) return 1;

    return 0;
    (void)lace; // suppress unused parameter error
}

int main()
{
    // Use multiple workers to exercise the protected-destination SPAWN/SYNC path.
    lace_start(4, 0, 0);

    // Simple Sylvan initialization, also initialize BDD, MTBDD and LDD support
    sylvan_set_sizes(1LL<<20, 1LL<<20, 1LL<<16, 1LL<<16);
    sylvan_init_package();
    mtbdd_init();
    listdd_init();

    printf("Sylvan initialization complete.\n");

    int res = runtests();

    sylvan_quit();
    lace_stop();

    return res;
}
