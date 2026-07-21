#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sylvan/internal.h>
#include <sylvan/platform.h>

#include "test_assert.h"

typedef int (*test_bdd_binary_op)(BDD*, BDD, BDD);

static BDD
test_bdd_var(uint32_t level)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_var_at_level(&result, level);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDDSET
test_bdd_set_from_levels(const uint32_t *levels, size_t count)
{
    BDDSET result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_set_from_array(&result, levels, count);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDDSET
test_bdd_set_union(BDDSET set1, BDDSET set2)
{
    BDDSET result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_set_union(&result, set1, set2);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static MTBDD
test_mtbdd_cube(BDDSET variables, const uint8_t *cube, MTBDD terminal)
{
    MTBDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = mtbdd_cube(&result, variables, cube, terminal);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

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
static BDD test_bdd_or(BDD a, BDD b) { return test_bdd_binary(bdd_or, a, b); }

static BDD
test_bdd_cube(BDDSET vars, const uint8_t *cube)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_cube(&result, vars, cube);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD
test_bdd_or_cube(BDD dd, BDDSET vars, const uint8_t *cube)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_or_cube(&result, dd, vars, cube);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD
test_bdd_exists(BDD dd, BDDSET vars)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_exists(&result, dd, vars);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD
test_bdd_ite(BDD a, BDD b, BDD c)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    test_assert(bdd_ite(&result, a, b, c) == SYLVAN_OK);
    mtbdd_unprotect(&result);
    return result;
}

static ZDD
test_zdd_from_bdd_value(BDD dd, BDDSET domain)
{
    ZDD result = zdd_invalid;
    zdd_protect(&result);
    int status = zdd_from_bdd(&result, dd, domain);
    zdd_unprotect(&result);
    return status == SYLVAN_OK ? result : zdd_invalid;
}

static BDD
test_bdd_from_zdd_value(ZDD dd, BDDSET domain)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_from_zdd(&result, dd, domain);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static void*
test_alloc_array(size_t count, size_t size)
{
    if (count == 0) count = 1;
    if (size == 0) size = 1;
    if (count > SIZE_MAX / size) return NULL;
    return malloc(count * size);
}

#define TEST_ALLOC_ARRAY(type, count) ((type*)test_alloc_array((size_t)(count), sizeof(type)))

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
uniform_deviate(uint64_t value)
{
    return value * (1.0 / ((double)(UINT64_MAX) + 1.0));
}

int
rng(int low, int high)
{
    return low + (int)(uniform_deviate(xorshift_rand()) * (double)(high-low));
}

/**
 * Some infrastructure to test evaluation of ZDDs
 */

uint8_t **enum_arrs;
size_t enum_len;
size_t enum_idx;
size_t enum_max;

TASK(void, test_zdd_enum_cb, void*, ctx, uint8_t*, arr, size_t, len)
void test_zdd_enum_cb_CALL(lace_worker* lace, void* ctx, uint8_t* arr, size_t len)
{
    assert(len == enum_len);
    assert(enum_idx != enum_max);
    assert(memcmp(arr, enum_arrs[enum_idx++], len) == 0);
    (void)lace;
    (void)ctx;
    (void)arr;
    (void)len;
}

TASK(int, test_zdd_conversion)
int test_zdd_conversion_CALL(lace_worker* lace)
{
    BDDSET dom = mtbdd_invalid;
    BDD dd = mtbdd_invalid;
    BDD outside = mtbdd_invalid;
    ZDD zdd = zdd_invalid;
    ZDD parallel_zdd = zdd_invalid;
    ZDD true_zdd = zdd_invalid;
    ZDD unchanged = zdd_base;
    BDD roundtrip = mtbdd_invalid;

    mtbdd_refs_pushptr(&dom);
    mtbdd_refs_pushptr(&dd);
    mtbdd_refs_pushptr(&outside);
    mtbdd_refs_pushptr(&roundtrip);
    zdd_refs_pushptr(&zdd);
    zdd_refs_pushptr(&parallel_zdd);
    zdd_refs_pushptr(&true_zdd);
    zdd_refs_pushptr(&unchanged);

    dom = test_bdd_set_from_levels((uint32_t[]){0,1,2,3,4,5,6}, 7);
    dd = test_mtbdd_cube(dom, (uint8_t[]){0,0,2,2,0,2,0}, bdd_true);
    outside = test_bdd_var(7);

    zdd_from_bdd_SPAWN(lace, &parallel_zdd, dd, dom);
    int status = zdd_from_bdd_CALL(lace, &zdd, dd, dom);
    int parallel_status = zdd_from_bdd_SYNC(lace);
    test_assert(status == SYLVAN_OK && zdd != zdd_invalid);
    test_assert(parallel_status == SYLVAN_OK && parallel_zdd == zdd);
    test_assert(bdd_from_zdd_CALL(lace, &roundtrip, zdd, dom) == SYLVAN_OK && roundtrip == dd);
    test_assert(zdd_true(&true_zdd, dom) == SYLVAN_OK);
    test_assert(true_zdd == test_zdd_from_bdd_value(bdd_true, dom));

    sylvan_gc_CALL(lace);
    test_assert(parallel_zdd == zdd && roundtrip == dd);

    test_assert(zdd_from_bdd(NULL, dd, dom) == SYLVAN_ERR_INVALID);
    test_assert(zdd_from_bdd(&unchanged, mtbdd_invalid, dom) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == zdd_base);
    test_assert(zdd_from_bdd(&unchanged, outside, dom) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == zdd_base);
    test_assert(bdd_from_zdd(&roundtrip, zdd_invalid, dom) == SYLVAN_ERR_INVALID);
    test_assert(roundtrip == dd);

    zdd_refs_popptr(4);
    mtbdd_refs_popptr(4);

    return 0;
}

TASK(int, test_zdd_variable)
int test_zdd_variable_CALL(lace_worker* lace)
{
    uint32_t var = rng(0, 0xfffff);
    ZDD a = zdd_make_node(var, zdd_false, zdd_base);
    test_assert(a == test_zdd_from_bdd_value(test_bdd_var(var), test_bdd_var(var)));

    return 0;
    (void)lace;
}

TASK(int, test_zdd_cofactor)
int test_zdd_cofactor_CALL(lace_worker* lace)
{
    BDDSET domain = mtbdd_invalid;
    BDD x0 = mtbdd_invalid;
    BDD x1 = mtbdd_invalid;
    BDD x2 = mtbdd_invalid;
    BDD function = mtbdd_invalid;
    BDD cube = mtbdd_invalid;
    BDDSET result_domain = mtbdd_invalid;
    ZDD zdd = zdd_invalid;
    ZDD result = zdd_invalid;
    ZDD unchanged = zdd_base;

    mtbdd_refs_pushptr(&domain);
    mtbdd_refs_pushptr(&x0);
    mtbdd_refs_pushptr(&x1);
    mtbdd_refs_pushptr(&x2);
    mtbdd_refs_pushptr(&function);
    mtbdd_refs_pushptr(&cube);
    mtbdd_refs_pushptr(&result_domain);
    zdd_refs_pushptr(&zdd);
    zdd_refs_pushptr(&result);
    zdd_refs_pushptr(&unchanged);

    domain = test_bdd_set_from_levels((uint32_t[]){0,1,2}, 3);
    x0 = test_bdd_var(0);
    x1 = test_bdd_var(1);
    x2 = test_bdd_var(2);
    function = test_bdd_xor(x0, x1);
    cube = test_bdd_and(x0, bdd_not(x2));
    zdd = test_zdd_from_bdd_value(function, domain);
    result_domain = test_bdd_set_from_levels((uint32_t[]){1}, 1);

    test_assert(zdd_cofactor_CALL(lace, &result, zdd, cube, domain) == SYLVAN_OK);
    test_assert(test_bdd_from_zdd_value(result, result_domain) == bdd_not(x1));

    test_assert(zdd_cofactor_CALL(lace, &unchanged, zdd, test_bdd_or(x0, x1), domain) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == zdd_base);
    test_assert(zdd_cofactor_CALL(lace, &unchanged, zdd, test_bdd_var(3), domain) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == zdd_base);
    zdd_refs_popptr(3);
    mtbdd_refs_popptr(7);
    return 0;
}

TASK(int, test_zdd_from_mtbdd)
int test_zdd_from_mtbdd_CALL(lace_worker* lace)
{
    /**
     * Test zdd_from_bdd, bdd_from_zdd and zdd_cube with random sets
     */

    BDD bdd_dom = test_bdd_set_from_levels((uint32_t[]){0,1,2,3,4,5,6,7}, 8);
    BDDSET zdd_dom = test_bdd_set_from_levels((uint32_t[]){0,1,2,3,4,5,6,7}, 8);

    int count = rng(10,100);
    for (int i=0; i<count; i++) {
        uint8_t arr[8];
        for (int j=0; j<8; j++) arr[j] = (uint8_t)rng(0, 2);
        BDD bdd_set = test_bdd_cube(bdd_dom, arr);
        ZDD zdd_set = zdd_cube(zdd_dom, arr);
        test_assert(test_zdd_from_bdd_value(bdd_set, bdd_dom) == zdd_set);
        test_assert(test_bdd_from_zdd_value(zdd_set, zdd_dom) == bdd_set);
    }

    return 0;
    (void)lace;
}

TASK(int, test_zdd_merge_domains)
int test_zdd_merge_domains_CALL(lace_worker* lace)
{
    /*
     * Test zdd_merge_domains with random sets
     */

    // Create random domain of 20..50 variables
    int nvars = rng(20,50);

    // Create random subdomain 1
    uint32_t *subdom1_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    test_assert(subdom1_arr != NULL);
    int nsub1 = 0;
    for (int i=0; i<nvars; i++) if (rng(0,2)) subdom1_arr[nsub1++] = i;
    BDD bdd_subdom1 = test_bdd_set_from_levels(subdom1_arr, nsub1);
    BDDSET zdd_subdom1 = test_bdd_set_from_levels(subdom1_arr, nsub1);

    // Create random subdomain 2
    uint32_t *subdom2_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    test_assert(subdom2_arr != NULL);
    int nsub2 = 0;
    for (int i=0; i<nvars; i++) if (rng(0,2)) subdom2_arr[nsub2++] = i;
    BDD bdd_subdom2 = test_bdd_set_from_levels(subdom2_arr, nsub2);
    BDDSET zdd_subdom2 = test_bdd_set_from_levels(subdom2_arr, nsub2);

    // combine subdomains
    BDD bdd_subdom = test_bdd_and(bdd_subdom1, bdd_subdom2);
    BDDSET zdd_subdom = test_bdd_set_union(zdd_subdom1, zdd_subdom2);
    test_assert(zdd_subdom == bdd_subdom);

    free(subdom2_arr);
    free(subdom1_arr);
    return 0;
    (void)lace;
}

TASK(int, test_zdd_extend_domain)
int test_zdd_extend_domain_CALL(lace_worker* lace)
{
    BDD subdomain = mtbdd_invalid;
    BDD domain = mtbdd_invalid;
    BDDSET newvars = mtbdd_invalid;
    BDD variable = mtbdd_invalid;
    ZDD set = zdd_invalid;
    ZDD expected = zdd_invalid;
    ZDD result = zdd_invalid;
    ZDD parallel_result = zdd_invalid;
    ZDD unchanged = zdd_base;
    BDDSET support = mtbdd_invalid;

    mtbdd_refs_pushptr(&subdomain);
    mtbdd_refs_pushptr(&domain);
    mtbdd_refs_pushptr(&newvars);
    mtbdd_refs_pushptr(&variable);
    mtbdd_refs_pushptr(&support);
    zdd_refs_pushptr(&set);
    zdd_refs_pushptr(&expected);
    zdd_refs_pushptr(&result);
    zdd_refs_pushptr(&parallel_result);
    zdd_refs_pushptr(&unchanged);

    subdomain = test_bdd_set_from_levels((uint32_t[]){1}, 1);
    domain = test_bdd_set_from_levels((uint32_t[]){0,1,2}, 3);
    newvars = test_bdd_set_from_levels((uint32_t[]){0,2}, 2);
    variable = test_bdd_var(1);
    set = test_zdd_from_bdd_value(variable, subdomain);
    expected = test_zdd_from_bdd_value(variable, domain);

    zdd_extend_domain_SPAWN(lace, &parallel_result, set, newvars, 2);
    int status = zdd_lift_CALL(lace, &result, set, subdomain, domain);
    int parallel_status = zdd_extend_domain_SYNC(lace);
    test_assert(status == SYLVAN_OK && result == expected);
    test_assert(parallel_status == SYLVAN_OK && parallel_result == expected);
    test_assert(zdd_support_CALL(lace, &support, set) == SYLVAN_OK && support == subdomain);

    result = set;
    test_assert(zdd_lift(&result, result, subdomain, domain) == SYLVAN_OK && result == expected);
    sylvan_gc_CALL(lace);
    test_assert(result == expected);

    test_assert(zdd_lift_CALL(lace, &unchanged, set, domain, subdomain) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == zdd_base);
    test_assert(zdd_extend_domain_CALL(lace, &unchanged, set, newvars, 3) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == zdd_base);
    test_assert(zdd_support(NULL, set) == SYLVAN_ERR_INVALID);
    test_assert(zdd_extend_domain(&unchanged, zdd_invalid, newvars, 2) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == zdd_base);

    zdd_refs_popptr(5);
    mtbdd_refs_popptr(5);
    return 0;
}

TASK(int, test_zdd_refs_growth)
int test_zdd_refs_growth_CALL(lace_worker* lace)
{
    ZDD refs[4096];
    for (size_t i=0; i<4096; i++) {
        refs[i] = zdd_base;
        zdd_refs_pushptr(&refs[i]);
    }
    zdd_refs_popptr(4096);
    (void)lace;
    return 0;
}

// TASK(int, test_zdd_extend_domain)
// {
//     /**
//      * Test zdd_extend_domain with random sets
//      */
// 
//     // Create random domain of 6..14 variables
//     int nvars = rng(6,14);
//     uint32_t dom_arr[nvars];
//     for (int i=0; i<nvars; i++) dom_arr[i] = i;
//     BDD bdd_dom = test_bdd_set_from_levels(dom_arr, nvars);
//     ZDD zdd_dom = zdd_from_array(dom_arr, nvars);
//     test_assert(zdd_dom == zdd_from_bdd(bdd_dom, bdd_dom));
// 
//     // Create random subdomain
//     uint32_t subdom_arr[nvars];
//     int nsub = 0;
//     for (int i=0; i<nvars; i++) if (rng(0,2)) subdom_arr[nsub++] = i;
//     BDD bdd_subdom = test_bdd_set_from_levels(subdom_arr, nsub);
//     ZDD zdd_subdom = zdd_from_array(subdom_arr, nsub);
//     test_assert(zdd_subdom == zdd_from_bdd(bdd_subdom, bdd_subdom));
// 
//     // Create random set on subdomain
//     BDD bdd_set = bdd_false;
//     ZDD zdd_set = zdd_false;
//     {
//         int count = rng(10,200);
//         for (int i=0; i<count; i++) {
//             uint8_t arr[nsub];
//             for (int j=0; j<nsub; j++) arr[j] = (uint8_t)rng(0, 2);
//             bdd_set = test_bdd_or_cube(bdd_set, bdd_subdom, arr);
//             zdd_set = zdd_or_cube(zdd_set, zdd_subdom, arr);
//         }
//     }
//     test_assert(zdd_set == zdd_from_bdd(bdd_set, bdd_subdom));
// 
//     ZDD zdd_test_result = zdd_extend_domain(zdd_set, zdd_subdom, zdd_dom);
//     test_assert(zdd_test_result == zdd_from_bdd(bdd_set, bdd_dom));
// 
//     return 0;
// }

TASK(int, test_zdd_union_cube)
int test_zdd_union_cube_CALL(lace_worker* lace)
{
    /**
     * Test zdd_or_cube with random sets
     * This also tests zdd_from_bdd...
     */

    BDD bdd_dom = test_bdd_set_from_levels((uint32_t[]){0,1,2,3,4,5,6,7}, 8);
    BDDSET zdd_dom = test_bdd_set_from_levels((uint32_t[]){0,1,2,3,4,5,6,7}, 8);

    BDD bdd_set = bdd_false;
    ZDD zdd_set = zdd_false;
    int count = rng(100,1000);
    for (int i=0; i<count; i++) {
        uint8_t arr[8];
        for (int j=0; j<8; j++) arr[j] = (uint8_t)rng(0, 3);
        bdd_set = test_bdd_or_cube(bdd_set, bdd_dom, arr);
        zdd_set = zdd_or_cube(zdd_set, zdd_dom, arr);
        test_assert(test_zdd_from_bdd_value(bdd_set, bdd_dom) == zdd_set);
    }

    return 0;
    (void)lace;;
}

TASK(int, test_zdd_satcount)
int test_zdd_satcount_CALL(lace_worker* lace)
{
    /**
     * Test zdd_path_count with random sets
     * This also tests zdd_from_bdd...
     */

    BDD bdd_dom = test_bdd_set_from_levels((uint32_t[]){0,1,2,3,4,5,6,7}, 8);

    int count = rng(0,100);
    BDD bdd_set = bdd_false;
    for (int i=0; i<count; i++) {
        uint8_t arr[8];
        for (int j=0; j<8; j++) arr[j] = (uint8_t)rng(0, 2);
        bdd_set = test_bdd_or_cube(bdd_set, bdd_dom, arr);
    }

    ZDD zdd_set = test_zdd_from_bdd_value(bdd_set, bdd_dom);

    test_assert((size_t)mtbdd_sat_count(bdd_set, 8) == (size_t)zdd_path_count(zdd_set));

    return 0;
    (void)lace;
}

TASK(int, test_zdd_enum)
int test_zdd_enum_CALL(lace_worker* lace)
{
    /**
     * Test zdd_enum with random sets
     */

    int nvars = rng(8,12);
    uint8_t *arr = TEST_ALLOC_ARRAY(uint8_t, nvars);
    uint32_t *dom_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    test_assert(arr != NULL);
    test_assert(dom_arr != NULL);

    // Create random source set
    for (int i=0; i<nvars; i++) dom_arr[i] = i*2;
    BDDSET zdd_dom = test_bdd_set_from_levels(dom_arr, nvars);

    ZDD zdd_set = zdd_false;
    int count = rng(0,1000);
    for (int i=0; i<count; i++) {
        for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
        zdd_set = zdd_or_cube(zdd_set, zdd_dom, arr);
    }

    size_t expected = (size_t)zdd_path_count(zdd_set);
    size_t seen = 0;
    ZDD res = zdd_first_minterm(zdd_set, zdd_dom, arr, NULL);
    while (res != zdd_false) {
        seen++;
        res = zdd_next_minterm(zdd_set, zdd_dom, arr, NULL);
    }
    test_assert(seen == expected);

    free(dom_arr);
    free(arr);
    return 0;
    (void)lace;
}

TASK(int, test_zdd_and)
int test_zdd_and_CALL(lace_worker* lace)
{
    /**
     * Test zdd_and with random sets
     */

    // Create random domain of 6..14 variables
    int nvars = rng(6,14);
    uint32_t *dom_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    uint8_t *arr = TEST_ALLOC_ARRAY(uint8_t, nvars);
    test_assert(dom_arr != NULL);
    test_assert(arr != NULL);
    for (int i=0; i<nvars; i++) dom_arr[i] = i;
    BDD bdd_dom = test_bdd_set_from_levels(dom_arr, nvars);

    BDD bdd_set_a = bdd_false;
    BDD bdd_set_b = bdd_false;

    int count = rng(0,100);
    for (int i=0; i<count; i++) {
        for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
        bdd_set_a = test_bdd_or_cube(bdd_set_a, bdd_dom, arr);
        for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
        bdd_set_b = test_bdd_or_cube(bdd_set_b, bdd_dom, arr);
    }

    BDD bdd_set = test_bdd_and(bdd_set_a, bdd_set_b);

    ZDD zdd_set_a = test_zdd_from_bdd_value(bdd_set_a, bdd_dom);
    ZDD zdd_set_b = test_zdd_from_bdd_value(bdd_set_b, bdd_dom);
    ZDD zdd_set = test_zdd_from_bdd_value(bdd_set, bdd_dom);

    ZDD zdd_test_result = zdd_and(zdd_set_a, zdd_set_b);
    test_assert(zdd_set == zdd_test_result);

    free(arr);
    free(dom_arr);
    return 0;
    (void)lace;
}

TASK(int, test_zdd_or)
int test_zdd_or_CALL(lace_worker* lace)
{
    /**
     * Test zdd_or with random sets
     */

    // Create random domain of 6..14 variables
    int nvars = rng(6,14);
    uint32_t *dom_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    uint8_t *arr = TEST_ALLOC_ARRAY(uint8_t, nvars);
    test_assert(dom_arr != NULL);
    test_assert(arr != NULL);
    for (int i=0; i<nvars; i++) dom_arr[i] = i;
    BDD bdd_dom = test_bdd_set_from_levels(dom_arr, nvars);

    BDD bdd_set_a = bdd_false;
    BDD bdd_set_b = bdd_false;

    int count = rng(0,100);
    for (int i=0; i<count; i++) {
        for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
        bdd_set_a = test_bdd_or_cube(bdd_set_a, bdd_dom, arr);
        for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
        bdd_set_b = test_bdd_or_cube(bdd_set_b, bdd_dom, arr);
    }

    BDD bdd_set = test_bdd_or(bdd_set_a, bdd_set_b);

    ZDD zdd_set_a = test_zdd_from_bdd_value(bdd_set_a, bdd_dom);
    ZDD zdd_set_b = test_zdd_from_bdd_value(bdd_set_b, bdd_dom);
    ZDD zdd_set = test_zdd_from_bdd_value(bdd_set, bdd_dom);

    ZDD zdd_test_result = zdd_or(zdd_set_a, zdd_set_b);
    test_assert(zdd_set == zdd_test_result);

    free(arr);
    free(dom_arr);
    return 0;
    (void)lace;
}

TASK(int, test_zdd_not)
int test_zdd_not_CALL(lace_worker* lace)
{
    /**
     * Test negation with random sets
     */

    BDD bdd_dom = test_bdd_set_from_levels((uint32_t[]){0,1,2,3,4,5,6,7}, 8);

    int count = rng(0,100);
    BDD bdd_set = bdd_false;
    for (int i=0; i<count; i++) {
        uint8_t arr[8];
        for (int j=0; j<8; j++) arr[j] = (uint8_t)rng(0, 2);
        bdd_set = test_bdd_or_cube(bdd_set, bdd_dom, arr);
    }

    ZDD zdd_set = test_zdd_from_bdd_value(bdd_set, bdd_dom);
    ZDD zdd_set_inv = test_zdd_from_bdd_value(bdd_not(bdd_set), bdd_dom);
    test_assert((size_t)mtbdd_sat_count(bdd_not(bdd_set), 8) == (size_t)zdd_path_count(zdd_set_inv));
    test_assert(zdd_set_inv == zdd_not(zdd_set, bdd_dom));

    return 0;
    (void)lace;
}

TASK(int, test_zdd_ite)
int test_zdd_ite_CALL(lace_worker* lace)
{
    /**
     * Test zdd_ite with random sets
     */

    // Create random domain of 6..12 variables
    int nvars = rng(6, 12);
    uint32_t *dom_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    uint8_t *arr = TEST_ALLOC_ARRAY(uint8_t, nvars);
    test_assert(dom_arr != NULL);
    test_assert(arr != NULL);
    for (int i=0; i<nvars; i++) dom_arr[i] = i;
    BDD bdd_dom = test_bdd_set_from_levels(dom_arr, nvars);

    // Create three random sets
    BDD set_a, set_b, set_c;
    set_a = set_b = set_c = bdd_false;

    {
        int count = rng(0, 100);
        for (int i=0; i<count; i++) {
            for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
            set_a = test_bdd_or_cube(set_a, bdd_dom, arr);
        }
    }

    {
        int count = rng(0, 100);
        for (int i=0; i<count; i++) {
            for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
            set_b = test_bdd_or_cube(set_b, bdd_dom, arr);
        }
    }

    {
        int count = rng(0, 100);
        for (int i=0; i<count; i++) {
            for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
            set_c = test_bdd_or_cube(set_c, bdd_dom, arr);
        }
    }

    ZDD zdd_set_a = test_zdd_from_bdd_value(set_a, bdd_dom);
    ZDD zdd_set_b = test_zdd_from_bdd_value(set_b, bdd_dom);
    ZDD zdd_set_c = test_zdd_from_bdd_value(set_c, bdd_dom);
    MTBDD bdd_test_result = test_bdd_ite(set_a, set_b, set_c);
    ZDD zdd_test_result = zdd_ite(zdd_set_a, zdd_set_b, zdd_set_c, bdd_dom);
    test_assert(test_zdd_from_bdd_value(bdd_test_result, bdd_dom) == zdd_test_result);

    free(arr);
    free(dom_arr);
    return 0;
    (void)lace;
}

TASK(int, test_zdd_exists)
int test_zdd_exists_CALL(lace_worker* lace)
{
    /**
     * Test zdd_exists with random sets
     */

    // Create random domain of 6..12 variables
    int nvars = rng(6, 12);
    uint32_t *dom_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    uint32_t *subdom_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    uint32_t *q_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    uint8_t *arr = TEST_ALLOC_ARRAY(uint8_t, nvars);
    test_assert(dom_arr != NULL);
    test_assert(subdom_arr != NULL);
    test_assert(q_arr != NULL);
    test_assert(arr != NULL);

    for (int i=0; i<nvars; i++) dom_arr[i] = i;
    BDD bdd_dom = test_bdd_set_from_levels(dom_arr, nvars);
    BDDSET zdd_dom = test_bdd_set_from_levels(dom_arr, nvars);

    // Create random subdomain and quotiented variables (qdom)
    int nsub = 0, nq = 0;
    for (int i=0; i<nvars; i++) {
        if (rng(0,2)) subdom_arr[nsub++] = i;
        else q_arr[nq++] = i;
    }
    BDD bdd_subdom = test_bdd_set_from_levels(subdom_arr, nsub);
    BDDSET zdd_subdom = test_bdd_set_from_levels(subdom_arr, nsub);
    BDD bdd_qdom = test_bdd_set_from_levels(q_arr, nq);
    BDDSET zdd_qdom = test_bdd_set_from_levels(q_arr, nq);

    // Create random set on subdomain
    BDD bdd_set = bdd_false;
    ZDD zdd_set = zdd_false;
    int count = rng(10,200);
    for (int i=0; i<count; i++) {
        for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
        bdd_set = test_bdd_or_cube(bdd_set, bdd_dom, arr);
        zdd_set = zdd_or_cube(zdd_set, zdd_dom, arr);
    }
    test_assert(zdd_set == test_zdd_from_bdd_value(bdd_set, bdd_dom));

    BDD bdd_qset = test_bdd_exists(bdd_set, bdd_qdom);
    ZDD zdd_test_result = zdd_exists(zdd_set, zdd_qdom);
    test_assert(zdd_test_result == test_zdd_from_bdd_value(bdd_qset, bdd_dom));
    ZDD zdd_test_result2 = zdd_project(zdd_set, zdd_subdom);
    test_assert(zdd_test_result2 == test_zdd_from_bdd_value(bdd_qset, bdd_subdom));

    free(arr);
    free(q_arr);
    free(subdom_arr);
    free(dom_arr);
    return 0;
    (void)lace;
}

// TASK(int, test_zdd_relnext)
// {
//     /**
//      * Test zdd_relnext with random sets
//      */
//     int nvars = rng(8,12);
// 
//     // Create random source set
//     uint32_t dom_arr[nvars];
//     for (int i=0; i<nvars; i++) dom_arr[i] = i*2;
//     BDD bdd_dom = test_bdd_set_from_levels(dom_arr, nvars);
//     ZDD zdd_dom = zdd_from_array(dom_arr, nvars);
// 
//     BDD bdd_set = bdd_false;
//     ZDD zdd_set = zdd_false;
//     {
//         int count = rng(4,100);
//         for (int i=0; i<count; i++) {
//             uint8_t arr[nvars];
//             for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
//             bdd_set = test_bdd_or_cube(bdd_set, bdd_dom, arr);
//             zdd_set = zdd_or_cube(zdd_set, zdd_dom, arr);
//         }
//     }
//     test_assert(zdd_set == zdd_from_bdd(bdd_set, bdd_dom));
// 
//     // Create random transition relation domain
//     BDD bdd_vars;
//     ZDD zdd_vars;
//     uint32_t vars_arr[2*nvars];
//     int len = 0;
//     {
//         int _vars = rng(1, 256);
//         for (int i=0; i<nvars; i++) {
//             if (_vars & (1<<i)) {
//                 vars_arr[len++] = i*2;
//                 vars_arr[len++] = i*2+1;
//             }
//         }
//         bdd_vars = test_bdd_set_from_levels(vars_arr, len);
//         zdd_vars = zdd_from_array(vars_arr, len);
//     }
//     test_assert(zdd_vars == zdd_from_bdd(bdd_vars, bdd_vars));
// 
//     // Create random transitions
//     BDD bdd_rel = bdd_false;
//     ZDD zdd_rel = zdd_false;
//     {
//         int count = rng(100, 200);
//         for (int i=0; i<count; i++) {
//             uint8_t arr[len];
//             for (int j=0; j<len; j++) arr[j] = (uint8_t)rng(0, 2);
//             bdd_rel = test_bdd_or_cube(bdd_rel, bdd_vars, arr);
//             zdd_rel = zdd_or_cube(zdd_rel, zdd_vars, arr);
//         }
//     }
//     test_assert(zdd_rel == zdd_from_bdd(bdd_rel, bdd_vars));
// 
//     // Check if sat counts are the same
//     test_assert(bdd_sat_count(bdd_set, bdd_dom) == zdd_path_count(zdd_set, zdd_dom));
//     test_assert(bdd_sat_count(bdd_rel, bdd_vars) == zdd_path_count(zdd_rel, zdd_vars));
// 
//     BDD bdd_succ = mtbdd_invalid;
//     bdd_rel_next(&bdd_succ, bdd_set, bdd_rel, bdd_vars);
//     ZDD zdd_succ = zdd_relnext(zdd_set, zdd_rel, zdd_vars, zdd_dom);
// 
//     test_assert(zdd_succ == zdd_from_bdd(bdd_succ, bdd_dom));
// 
//     return 0;
// }
// 
// TASK(int, test_zdd_and_dom)
// {
//     /**
//      * Test zdd_and_dom with random sets
//      */
// 
//     // Create random domain of 6..14 variables
//     int nvars = rng(6,14);
//     uint32_t dom_arr[nvars];
//     for (int i=0; i<nvars; i++) dom_arr[i] = i;
//     BDD bdd_dom = test_bdd_set_from_levels(dom_arr, nvars);
//     ZDD zdd_dom = zdd_from_array(dom_arr, nvars);
//     test_assert(zdd_dom == zdd_from_bdd(bdd_dom, bdd_dom));
// 
//     // Create random subdomain 1
//     uint32_t subdom1_arr[nvars];
//     int nsub1 = 0;
//     for (int i=0; i<nvars; i++) if (rng(0,2)) subdom1_arr[nsub1++] = i;
//     BDD bdd_subdom1 = test_bdd_set_from_levels(subdom1_arr, nsub1);
//     ZDD zdd_subdom1 = zdd_from_array(subdom1_arr, nsub1);
//     test_assert(zdd_subdom1 == zdd_from_bdd(bdd_subdom1, bdd_subdom1));
// 
//     // Create random subdomain 2
//     uint32_t subdom2_arr[nvars];
//     int nsub2 = 0;
//     for (int i=0; i<nvars; i++) if (rng(0,2)) subdom2_arr[nsub2++] = i;
//     BDD bdd_subdom2 = test_bdd_set_from_levels(subdom2_arr, nsub2);
//     ZDD zdd_subdom2 = zdd_from_array(subdom2_arr, nsub2);
//     test_assert(zdd_subdom2 == zdd_from_bdd(bdd_subdom2, bdd_subdom2));
// 
//     // Create random set on subdomain 1
//     BDD bdd_set1 = bdd_false;
//     ZDD zdd_set1 = zdd_false;
//     {
//         int count = rng(10,200);
//         for (int i=0; i<count; i++) {
//             uint8_t arr[nsub1];
//             for (int j=0; j<nsub1; j++) arr[j] = (uint8_t)rng(0, 2);
//             bdd_set1 = test_bdd_or_cube(bdd_set1, bdd_subdom1, arr);
//             zdd_set1 = zdd_or_cube(zdd_set1, zdd_subdom1, arr);
//         }
//     }
//     test_assert(zdd_set1 == zdd_from_bdd(bdd_set1, bdd_subdom1));
// 
//     // Create random set on subdomain 2
//     BDD bdd_set2 = bdd_false;
//     ZDD zdd_set2 = zdd_false;
//     {
//         int count = rng(10,200);
//         for (int i=0; i<count; i++) {
//             uint8_t arr[nsub2];
//             for (int j=0; j<nsub2; j++) arr[j] = (uint8_t)rng(0, 2);
//             bdd_set2 = test_bdd_or_cube(bdd_set2, bdd_subdom2, arr);
//             zdd_set2 = zdd_or_cube(zdd_set2, zdd_subdom2, arr);
//         }
//     }
//     test_assert(zdd_set2 == zdd_from_bdd(bdd_set2, bdd_subdom2));
// 
//     BDD bdd_set = test_bdd_and(bdd_set1, bdd_set2);
//     BDD bdd_subdom = test_bdd_and(bdd_subdom1, bdd_subdom2);
//     ZDD zdd_set = zdd_and_dom(zdd_set1, zdd_subdom1, zdd_set2, zdd_subdom2);
//     test_assert(zdd_set == zdd_from_bdd(bdd_set, bdd_subdom));
// 
//     return 0;
// }

/**
 * Basic test for ISOP on a known small case.
 */
TASK(int, test_zdd_isop_basic)
int test_zdd_isop_basic_CALL(lace_worker* lace)
{
    BDD a = test_bdd_var(1);
    BDD b = test_bdd_var(2);

    BDD a_and_b = test_bdd_and(a, b);
    BDD aNot_and_b = test_bdd_and(bdd_not(a), b);
    BDD redundant_b = test_bdd_or(a_and_b, aNot_and_b);

    // ab + ~ab == b

    MTBDD bddres;
    ZDD isop_zdd = zdd_isop(redundant_b, redundant_b, &bddres);
    MTBDD bdd2 = bdd_from_zdd_cover(isop_zdd);

    test_assert(bddres == redundant_b);
    test_assert(bdd2 == redundant_b);
    test_assert(zdd_top_var(isop_zdd) == 4);
    test_assert(zdd_node_high(isop_zdd) == zdd_base);
    test_assert(zdd_node_low(isop_zdd) == zdd_false);
    return 0;
    (void)lace;
}

TASK(int, test_zdd_isop_random)
int test_zdd_isop_random_CALL(lace_worker* lace)
{
    BDD bdd_dom = test_bdd_set_from_levels((uint32_t[]){0,1,2,3,4,5,6,7,8,9,10,11}, 12);

    // create a random BDD
    MTBDD bdd_set = bdd_false;
    int cubecount = rng(1,200);
    for (int j=0; j<cubecount; j++) {
        uint8_t arr[12];
        for (int j=0; j<12; j++) arr[j] = (uint8_t)rng(0, 2);
        bdd_set = test_bdd_or(bdd_set, test_bdd_cube(bdd_dom, arr));
    }

    // convert to ISOP cover
    MTBDD isop_bdd;
    ZDD isop_zdd = zdd_isop(bdd_set, bdd_set, &isop_bdd);
    MTBDD remade_bdd = bdd_from_zdd_cover(isop_zdd);

    // manually count cubes
    int arr[13];
    ZDD res = zdd_cover_first_cube(isop_zdd, arr);
    int count1 = 0;
    while (res != zdd_false) {
        res = zdd_cover_next_cube(isop_zdd, arr);
        count1++;
    }

    // count cubes by counting paths
    long zdd_cubes = (long)zdd_path_count(isop_zdd);

    // printf("%6d cubes, %6ld PIs\n", cubecount, zdd_cubes);

    // check if all is right
    test_assert(isop_bdd == bdd_set);
    test_assert(remade_bdd == bdd_set);
    test_assert(count1 <= cubecount);
    test_assert(count1 == zdd_cubes);

    return 0;
    (void)lace;
}

TASK(int, test_zdd_read_write)
int test_zdd_read_write_CALL(lace_worker* lace)
{
    /**
     * Test reading/writing with random sets
     */
    int nvars = rng(8,12);

    // Create random source sets
    uint32_t *dom_arr = TEST_ALLOC_ARRAY(uint32_t, nvars);
    uint8_t *arr = TEST_ALLOC_ARRAY(uint8_t, nvars);
    test_assert(dom_arr != NULL);
    test_assert(arr != NULL);
    for (int i=0; i<nvars; i++) dom_arr[i] = i*2;
    BDDSET zdd_dom = test_bdd_set_from_levels(dom_arr, nvars);

    int set_count = rng(1,10);
    ZDD *zdd_set = TEST_ALLOC_ARRAY(ZDD, set_count);
    ZDD *test = TEST_ALLOC_ARRAY(ZDD, set_count);
    test_assert(zdd_set != NULL);
    test_assert(test != NULL);

    for (int k=0; k<set_count; k++) {
        zdd_set[k] = zdd_false;
        int count = rng(4,100);
        for (int i=0; i<count; i++) {
            for (int j=0; j<nvars; j++) arr[j] = (uint8_t)rng(0, 2);
            zdd_set[k] = zdd_or_cube(zdd_set[k], zdd_dom, arr);
        }
    }

    FILE *f = tmpfile();
    test_assert(f != NULL);
    zdd_write_binary(f, zdd_set, set_count);
    rewind(f);
    test_assert(zdd_read_binary(f, test, set_count) == 0);
    for (int i=0; i<set_count; i++) test_assert(test[i] == zdd_set[i]);

    fclose(f);
    free(test);
    free(zdd_set);
    free(arr);
    free(dom_arr);
    return 0;
    (void)lace;
}

TASK(int, runtests)
int runtests_CALL(lace_worker* lace)
{
    // Testing without garbage collection
    sylvan_gc_disable();

    int test_iterations = 100;

    printf("test_zdd_conversion...\n");
    for (int i=0; i<test_iterations; i++) if (test_zdd_conversion_CALL(lace)) return 1;
    printf("test_zdd_variable...\n");
    for (int i=0; i<test_iterations; i++) if (test_zdd_variable_CALL(lace)) return 1;
    printf("test_zdd_cofactor...\n");
    if (test_zdd_cofactor_CALL(lace)) return 1;
    printf("test_zdd_from_mtbdd...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_from_mtbdd_CALL(lace)) return 1;
    printf("test_zdd_satcount...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_satcount_CALL(lace)) return 1;
    printf("test_zdd_merge_domains...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_merge_domains_CALL(lace)) return 1;
    printf("test_zdd_extend_domain...\n");
    if (test_zdd_extend_domain_CALL(lace)) return 1;
    printf("test_zdd_refs_growth...\n");
    if (test_zdd_refs_growth_CALL(lace)) return 1;
    printf("test_zdd_union_cube...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_union_cube_CALL(lace)) return 1;
    printf("test_zdd_enum...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_enum_CALL(lace)) return 1;
    printf("test_zdd_ite...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_ite_CALL(lace)) return 1;
    printf("test_zdd_and...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_and_CALL(lace)) return 1;
    printf("test_zdd_or...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_or_CALL(lace)) return 1;
    printf("test_zdd_not...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_not_CALL(lace)) return 1;
    printf("test_zdd_exists...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_exists_CALL(lace)) return 1;
    // for (int k=0; k<test_iterations; k++) if (test_zdd_relnext_CALL(lace)) return 1;
    // for (int k=0; k<test_iterations; k++) if (test_zdd_and_dom_CALL(lace)) return 1;
    // printf("test_zdd_read_write...\n");
    // for (int k=0; k<10; k++) if (test_zdd_read_write_CALL(lace)) return 1;
    // for (int k=0; k<test_iterations; k++) if (test_zdd_extend_domain_CALL(lace)) return 1;
    printf("test_zdd_isop_basic...\n");
    if (test_zdd_isop_basic_CALL(lace)) return 1;
    printf("test_zdd_isop_random...\n");
    for (int k=0; k<test_iterations; k++) if (test_zdd_isop_random_CALL(lace)) return 1;

    return 0;
    (void)lace;
}

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);

    // Standard Lace initialization with 1 worker
	lace_start(1, 0, 0);

    // Simple Sylvan initialization, also initialize BDD, MTBDD and LDD support
	sylvan_set_sizes(1LL<<26, 1LL<<26, 1LL<<20, 1LL<<20);
	sylvan_init_package();
    mtbdd_init();
    zdd_init();

    int res = runtests();

    sylvan_quit();
    lace_stop();

    return res;
}
