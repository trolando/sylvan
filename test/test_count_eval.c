#include <sylvan_int.h>
#include "test_assert.h"
#ifdef SYLVAN_TEST_GMP
#include <limits.h>
#include <sylvan_gmp.h>
#endif

/* Fixed seed keeps assignment and diagram failures reproducible. */
static uint32_t seed = 1;
static uint32_t random_u32(void)
{
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

static int test_counts(void)
{
    uint32_t vars[70];
    for (uint32_t i=0; i<70; i++) vars[i] = i;
    BDDSET all = sylvan_set_fromarray(vars, 70);
    BDDSET v65 = sylvan_set_fromarray(vars, 65);
    BDDSET v64 = sylvan_set_fromarray(vars, 64);
    BDDSET v63 = sylvan_set_fromarray(vars, 63);
    uint64_t count = 123;

    test_assert(sylvan_satcount_u64(sylvan_false, all, &count) && count == 0);
    test_assert(sylvan_satcount_u64(sylvan_true, v63, &count) && count == (UINT64_C(1)<<63));
    count = 123;
    test_assert(!sylvan_satcount_u64(sylvan_true, all, &count) && count == 123);
    test_assert(!sylvan_satcount_u64(sylvan_true, v64, &count) && count == 123);
    test_assert(!sylvan_satcount_u64(sylvan_ithvar(0), sylvan_set_empty(), &count) && count == 123);
    test_assert(!sylvan_satcount_u64(sylvan_invalid, all, &count) && count == 123);
    test_assert(!sylvan_satcount_u64(sylvan_true, sylvan_false, &count) && count == 123);
    test_assert(!sylvan_satcount_u64(sylvan_true, all, NULL));

    BDD x = sylvan_ithvar(0), y = sylvan_ithvar(1);
    BDD invalid_sets[] = { sylvan_or(x, y), sylvan_not(x) };
    BDD bdds[] = { sylvan_false, sylvan_true, x };
    for (size_t i=0; i<sizeof(invalid_sets)/sizeof(invalid_sets[0]); i++) {
        for (size_t j=0; j<sizeof(bdds)/sizeof(bdds[0]); j++) {
            test_assert(!sylvan_satcount_u64(bdds[j], invalid_sets[i], &count) && count == 123);
        }
    }

    /* Both halves fit in u64; only their addition overflows. */
    BDD addition_overflow = sylvan_xor(x, y);
    BDD halves[] = { sylvan_low(addition_overflow), sylvan_high(addition_overflow) };
    for (size_t i=0; i<sizeof(halves)/sizeof(halves[0]); i++) {
        test_assert(sylvan_satcount_u64(halves[i], sylvan_set_next(v65), &count) && count == (UINT64_C(1)<<63));
        test_assert(mtbdd_satcount_u64(halves[i], 64, &count) && count == (UINT64_C(1)<<63));
    }
    count = 123;
    test_assert(!sylvan_satcount_u64(addition_overflow, v65, &count) && count == 123);
    test_assert(!mtbdd_satcount_u64(addition_overflow, 65, &count) && count == 123);

    /* UINT64_MAX is a valid count, not an overflow sentinel. */
    BDD disjunction = sylvan_false;
    for (int i=63; i>=0; i--) disjunction = sylvan_makenode((uint32_t)i, disjunction, sylvan_true);
    for (int repeat=0; repeat<2; repeat++) {
        test_assert(sylvan_satcount_u64(disjunction, v64, &count) && count == UINT64_MAX);
        test_assert(mtbdd_satcount_u64(disjunction, 64, &count) && count == UINT64_MAX);
        test_assert(sylvan_satcount_u64(sylvan_not(disjunction), v64, &count) && count == 1);
    }
    count = 123;
    test_assert(!sylvan_satcount_u64(disjunction, all, &count) && count == 123);
    test_assert(!mtbdd_satcount_u64(disjunction, 70, &count) && count == 123);
    test_assert(!mtbdd_satcount_u64(mtbdd_true, 64, &count) && count == 123);
    test_assert(!mtbdd_satcount_u64(mtbdd_ithvar(0), 0, &count) && count == 123);
    test_assert(!mtbdd_satcount_u64(mtbdd_invalid, 0, &count) && count == 123);
    test_assert(!mtbdd_satcount_u64(mtbdd_true, 0, NULL));
    test_assert(mtbdd_satcount_u64(mtbdd_false, SIZE_MAX, &count) && count == 0);

    MTBDD leaves[] = { mtbdd_false, mtbdd_true, mtbdd_int64(0), mtbdd_int64(-7),
        mtbdd_double(0.0), mtbdd_double(1.5), mtbdd_fraction(0, 1), mtbdd_fraction(2, 3) };
    for (size_t i=0; i<sizeof(leaves)/sizeof(leaves[0]); i++) {
        MTBDD dd = mtbdd_makenode(100, leaves[i], mtbdd_makenode(300, mtbdd_false, mtbdd_int64(4)));
        test_assert(mtbdd_satcount_u64(dd, 10, &count));
        test_assert((double)count == mtbdd_satcount(dd, 10));
        test_assert(mtbdd_satcount_u64(leaves[i], 3, &count));
        test_assert((double)count == mtbdd_satcount(leaves[i], 3));
        if (i == 0 || i == 2 || i == 4 || i == 6) {
            test_assert(mtbdd_satcount_u64(leaves[i], SIZE_MAX, &count) && count == 0);
        }
#ifdef SYLVAN_TEST_GMP
        mpz_t exact;
        mpz_init(exact);
        test_assert(mtbdd_satcount_gmp(dd, 10, exact));
        test_assert(mpz_get_d(exact) == mtbdd_satcount(dd, 10));
        mpz_clear(exact);
#endif
    }

    /* Two choices at every LDD level / ZDD variable, with shared children. */
    MDD ldd = lddmc_true;
    ZDD zdd = zdd_true;
    test_assert(lddmc_satcount_u64(lddmc_false, &count) && count == 0);
    test_assert(zdd_satcount_u64(zdd_false, &count) && count == 0);
    for (unsigned i=0; i<=70; i++) {
        count = 123;
        if (i < 64) {
            test_assert(lddmc_satcount_u64(ldd, &count) && count == (UINT64_C(1)<<i));
            test_assert((long double)count == lddmc_satcount(ldd));
            test_assert((double)count == lddmc_satcount_cached(ldd));
            test_assert(zdd_satcount_u64(zdd, &count) && count == (UINT64_C(1)<<i));
            test_assert((double)count == zdd_satcount(zdd));
        } else {
            test_assert(!lddmc_satcount_u64(ldd, &count) && count == 123);
            test_assert(!zdd_satcount_u64(zdd, &count) && count == 123);
        }
#ifdef SYLVAN_TEST_GMP
        mpz_t exact, expected;
        mpz_init(exact);
        mpz_init_set_ui(expected, 1);
        mpz_mul_2exp(expected, expected, i);
        test_assert(lddmc_satcount_gmp(ldd, exact) && mpz_cmp(exact, expected) == 0);
        test_assert(zdd_satcount_gmp(zdd, exact) && mpz_cmp(exact, expected) == 0);
        mpz_clear(exact);
        mpz_clear(expected);
#endif
        if (i != 70) {
            MDD right = lddmc_makenode(1, ldd, lddmc_false);
            ldd = lddmc_makenode(0, ldd, right);
            zdd = zdd_makenode(70-i, zdd, zdd);
        }
    }
    count = 123;
    test_assert(!lddmc_satcount_u64(UINT64_MAX, &count) && count == 123);
    test_assert(!zdd_satcount_u64(zdd_invalid, &count) && count == 123);
    test_assert(!lddmc_satcount_u64(ldd, NULL));
    test_assert(!zdd_satcount_u64(zdd, NULL));
    MDD copy = lddmc_make_copynode(lddmc_true, lddmc_false);
    test_assert(lddmc_satcount_u64(copy, &count) && count == 1);

#ifdef SYLVAN_TEST_GMP
    mpz_t exact, expected;
    mpz_init_set_ui(exact, 123);
    mpz_init_set_ui(expected, 1);
    mpz_mul_2exp(expected, expected, 70);
    test_assert(sylvan_satcount_gmp(sylvan_true, all, exact) && mpz_cmp(exact, expected) == 0);
    test_assert(mtbdd_satcount_gmp(mtbdd_true, 70, exact) && mpz_cmp(exact, expected) == 0);
    mpz_sub_ui(expected, expected, 64); /* (2^64 - 1) * 2^6 */
    test_assert(sylvan_satcount_gmp(disjunction, all, exact) && mpz_cmp(exact, expected) == 0);
    test_assert(mtbdd_satcount_gmp(disjunction, 70, exact) && mpz_cmp(exact, expected) == 0);
    for (size_t i=0; i<sizeof(invalid_sets)/sizeof(invalid_sets[0]); i++) {
        for (size_t j=0; j<sizeof(bdds)/sizeof(bdds[0]); j++) {
            test_assert(!sylvan_satcount_gmp(bdds[j], invalid_sets[i], exact) && mpz_cmp(exact, expected) == 0);
        }
    }
#if SIZE_MAX > ULONG_MAX
    /* LLP64: reject before narrowing the exponent to mp_bitcnt_t. */
    const size_t too_many = (size_t)ULONG_MAX + 1;
    test_assert(!mtbdd_satcount_gmp(mtbdd_true, too_many, exact) && mpz_cmp(exact, expected) == 0);
    test_assert(!mtbdd_satcount_gmp(x, too_many + 1, exact) && mpz_cmp(exact, expected) == 0);
#endif
    test_assert(!sylvan_satcount_gmp(sylvan_ithvar(0), sylvan_set_empty(), exact));
    test_assert(mpz_cmp(exact, expected) == 0);
    test_assert(!mtbdd_satcount_gmp(mtbdd_ithvar(0), 0, exact) && mpz_cmp(exact, expected) == 0);
    test_assert(!lddmc_satcount_gmp(UINT64_MAX, exact) && mpz_cmp(exact, expected) == 0);
    test_assert(!zdd_satcount_gmp(zdd_invalid, exact) && mpz_cmp(exact, expected) == 0);
    test_assert(sylvan_satcount_gmp(sylvan_false, all, exact) && mpz_sgn(exact) == 0);
    test_assert(mtbdd_satcount_gmp(mtbdd_int64(0), 70, exact) && mpz_sgn(exact) == 0);
    test_assert(lddmc_satcount_gmp(lddmc_false, exact) && mpz_sgn(exact) == 0);
    test_assert(zdd_satcount_gmp(zdd_false, exact) && mpz_sgn(exact) == 0);
    mpz_clear(exact);
    mpz_clear(expected);
#endif
    return 0;
}

static int test_evaluation(void)
{
    uint32_t indices[] = { 1, 4, 9, 15, 21, 28, 36, 45 };
    BDDSET variables = sylvan_set_fromarray(indices, 8);
    uint8_t values[8];
    for (int trial=0; trial<30; trial++) {
        BDD f = sylvan_false;
        for (int j=0; j<20; j++) {
            for (int k=0; k<8; k++) values[k] = random_u32() % 3;
            f = sylvan_union_cube(f, variables, values);
        }
        if (trial & 1) f = sylvan_not(f);
        uint64_t count;
        test_assert(sylvan_satcount_u64(f, variables, &count));
        test_assert((double)count == sylvan_satcount(f, variables));
        test_assert(mtbdd_satcount_u64(f, 8, &count));
        test_assert((double)count == mtbdd_satcount(f, 8));
#ifdef SYLVAN_TEST_GMP
        mpz_t exact;
        mpz_init(exact);
        test_assert(sylvan_satcount_gmp(f, variables, exact));
        test_assert(mpz_get_d(exact) == sylvan_satcount(f, variables));
        mpz_clear(exact);
#endif
        MTBDD numeric = mtbdd_ite(f, mtbdd_int64(17), mtbdd_double(-2.5));
        for (int assignment=0; assignment<40; assignment++) {
            for (int k=0; k<8; k++) values[k] = random_u32() & 1;
            BDD cube = sylvan_cube(variables, values);
            BDD expected = sylvan_restrict(f, cube);
            test_assert(expected == sylvan_false || expected == sylvan_true);
            test_assert(sylvan_eval(f, variables, values, 8) == expected);
            test_assert(mtbdd_eval(f, variables, values, 8) == expected);
            test_assert(sylvan_eval(sylvan_not(f), variables, values, 8) == sylvan_not(expected));
            test_assert(sylvan_cofactor(f, cube) == expected);
            test_assert(mtbdd_eval(numeric, variables, values, 8) ==
                        (expected == sylvan_true ? mtbdd_int64(17) : mtbdd_double(-2.5)));

            /* MTBDD composition with a complete constant map is restriction. */
            MTBDDMAP map = mtbdd_map_empty();
            for (int k=0; k<8; k++) map = mtbdd_map_add(map, indices[k], values[k] ? mtbdd_true : mtbdd_false);
            test_assert(mtbdd_eval(numeric, variables, values, 8) == mtbdd_compose(numeric, map));
        }
    }

    BDD x = sylvan_ithvar(1), y = sylvan_ithvar(4);
    test_assert(sylvan_cofactor(x, sylvan_true) == x);
    test_assert(sylvan_cofactor(sylvan_and(x, y), sylvan_not(x)) == sylvan_false);
    test_assert(sylvan_cofactor(sylvan_and(x, y), x) == y);
    test_assert(sylvan_cofactor(x, sylvan_false) == sylvan_invalid);
    test_assert(sylvan_cofactor(x, sylvan_or(x, y)) == sylvan_invalid);
    test_assert(sylvan_cofactor(x, sylvan_xor(x, y)) == sylvan_invalid);
    test_assert(sylvan_cofactor(sylvan_false, sylvan_or(x, y)) == sylvan_invalid);
    test_assert(sylvan_cofactor(x, mtbdd_int64(1)) == sylvan_invalid);
    test_assert(sylvan_cofactor(x, sylvan_invalid) == sylvan_invalid);
    test_assert(sylvan_cofactor(sylvan_invalid, x) == sylvan_invalid);
    test_assert(sylvan_eval(sylvan_true, sylvan_set_empty(), NULL, 0) == sylvan_true);
    test_assert(mtbdd_eval(mtbdd_false, sylvan_set_empty(), NULL, 0) == mtbdd_false);
    test_assert(mtbdd_eval(mtbdd_int64(42), sylvan_set_empty(), NULL, 0) == mtbdd_int64(42));
    test_assert(sylvan_eval(mtbdd_int64(42), sylvan_set_empty(), NULL, 0) == sylvan_invalid);
    test_assert(sylvan_eval(x, sylvan_set_empty(), NULL, 0) == sylvan_invalid);
    test_assert(sylvan_eval(x, variables, NULL, 8) == sylvan_invalid);
    test_assert(mtbdd_eval(x, variables, values, 7) == mtbdd_invalid);
    test_assert(mtbdd_eval(x, variables, values, 9) == mtbdd_invalid);
    test_assert(mtbdd_eval(mtbdd_invalid, variables, values, 8) == mtbdd_invalid);
    test_assert(mtbdd_eval(x, mtbdd_invalid, values, 8) == mtbdd_invalid);
    test_assert(mtbdd_eval(x, mtbdd_false, values, 1) == mtbdd_invalid);
    test_assert(mtbdd_eval(x, sylvan_or(x, y), values, 2) == mtbdd_invalid);
    values[7] = 2;
    test_assert(sylvan_eval(sylvan_true, variables, values, 8) == sylvan_invalid);
    test_assert(mtbdd_eval(x, variables, values, 8) == mtbdd_invalid);
    return 0;
}

/* Run again after shrinking the hash table. Live node indices may exceed it. */
static int test_after_gc(void)
{
    sylvan_gc_enable();
    sylvan_gc();
    size_t initial_size = llmsset_get_size(nodes);
    const size_t garbage_count = 100000;
    MTBDD *garbage = malloc(garbage_count * sizeof(*garbage));
    test_assert(garbage != NULL);
    for (size_t i=0; i<garbage_count; i++) garbage[i] = mtbdd_ref(mtbdd_int64((int64_t)i + 100000));
    test_assert(llmsset_get_size(nodes) > initial_size);
    uint32_t indices[70];
    for (uint32_t i=0; i<70; i++) indices[i] = i;
    BDDSET variables = sylvan_set_fromarray(indices, 70);
    BDD f = sylvan_ithvar(69);
    MDD ldd = lddmc_cube((uint32_t[]){1, 2, 3}, 3);
    ZDD zdd = zdd_true;
    for (int i=69; i>=0; i--) zdd = zdd_makenode((uint32_t)i, zdd_false, zdd);
    sylvan_protect(&variables);
    sylvan_protect(&f);
    lddmc_protect(&ldd);
    zdd_protect(&zdd);
    for (size_t i=0; i<garbage_count; i++) mtbdd_deref(garbage[i]);
    free(garbage);
    sylvan_gc();
    test_assert(llmsset_get_size(nodes) == initial_size);
    test_assert(llmsset_get_data_size(nodes) > llmsset_get_size(nodes));
    uint64_t count = 123;
    test_assert(!sylvan_satcount_u64(f, variables, &count) && count == 123);
    test_assert(sylvan_satcount_u64(f, sylvan_ithvar(69), &count) && count == 1);
    test_assert(lddmc_satcount_u64(ldd, &count) && count == 1);
    test_assert(zdd_satcount_u64(zdd, &count) && count == 1);
    uint8_t values[70] = {0};
    values[69] = 1;
    test_assert(sylvan_eval(f, variables, values, 70) == sylvan_true);
    test_assert(sylvan_cofactor(f, sylvan_not(f)) == sylvan_false);
#ifdef SYLVAN_TEST_GMP
    mpz_t exact, expected;
    mpz_init(exact);
    mpz_init_set_ui(expected, 1);
    mpz_mul_2exp(expected, expected, 69);
    test_assert(sylvan_satcount_gmp(f, variables, exact) && mpz_cmp(exact, expected) == 0);
    mpz_clear(exact);
    mpz_clear(expected);
#endif
    sylvan_unprotect(&variables);
    sylvan_unprotect(&f);
    lddmc_unprotect(&ldd);
    zdd_unprotect(&zdd);
    return 0;
}

TASK_0(int, runtests)
{
    sylvan_gc_disable();
    if (test_counts()) return 1;
    if (test_evaluation()) return 1;
    if (test_after_gc()) return 1;
#if SYLVAN_STATS
    sylvan_stats_t stats;
    sylvan_stats_snapshot(&stats);
    test_assert(stats.counters[BDD_SATCOUNT_U64] > 0);
    test_assert(stats.counters[MTBDD_SATCOUNT_U64] > 0);
    test_assert(stats.counters[LDD_SATCOUNT_U64] > 0);
    test_assert(stats.counters[ZDD_SATCOUNT_U64] > 0);
    test_assert(stats.counters[BDD_EVAL] > 0);
    test_assert(stats.counters[MTBDD_EVAL] > 0);
    test_assert(stats.counters[BDD_COFACTOR] > 0);
#ifdef SYLVAN_TEST_GMP
    test_assert(stats.counters[BDD_SATCOUNT_GMP] > 0);
    test_assert(stats.counters[MTBDD_SATCOUNT_GMP] > 0);
    test_assert(stats.counters[LDD_SATCOUNT_GMP] > 0);
    test_assert(stats.counters[ZDD_SATCOUNT_GMP] > 0);
#endif
#endif
    return 0;
}

int main(int argc, char **argv)
{
    unsigned workers = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    lace_start(workers, 0);
    sylvan_set_sizes(1<<16, 1<<20, 1<<14, 1<<18);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_ldd();
    sylvan_init_zdd();
    int result = RUN(runtests);
    sylvan_quit();
    lace_stop();
    return result;
}
