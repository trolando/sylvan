/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
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

#include <sylvan_int.h>
#include <sylvan_gmp.h>

#include <limits.h>
#include <math.h>
#include <string.h>

static uint32_t gmp_type;

/* Each call owns its memo table and GMP values. No pointers to GMP storage are
 * placed in the global operation cache. The recursive walk is sequential and
 * has no GC safepoints, so cached node handles remain live for the whole call. */
struct gmp_count_entry {
    MTBDD dd;
    BDDSET variables;
    mpz_t count;
    struct gmp_count_entry *next;
};

struct gmp_count_cache {
    size_t bucket_count;
    size_t entry_count;
    struct gmp_count_entry **buckets;
};

static size_t
gmp_count_hash(MTBDD dd, BDDSET variables, size_t bucket_count)
{
    uint64_t hash = dd ^ (variables + UINT64_C(0x9e3779b97f4a7c15) + (dd << 6) + (dd >> 2));
    hash ^= hash >> 33;
    hash *= UINT64_C(0xff51afd7ed558ccd);
    hash ^= hash >> 33;
    return (size_t)(hash & (bucket_count - 1));
}

static int
gmp_count_cache_init(struct gmp_count_cache *cache)
{
    cache->bucket_count = 1024;
    cache->entry_count = 0;
    cache->buckets = calloc(cache->bucket_count, sizeof(*cache->buckets));
    return cache->buckets != NULL;
}

static void
gmp_count_cache_clear(struct gmp_count_cache *cache)
{
    for (size_t i = 0; i < cache->bucket_count; i++) {
        struct gmp_count_entry *entry = cache->buckets[i];
        while (entry != NULL) {
            struct gmp_count_entry *next = entry->next;
            mpz_clear(entry->count);
            free(entry);
            entry = next;
        }
    }
    free(cache->buckets);
}

static int
gmp_count_cache_get(struct gmp_count_cache *cache, mpz_t result, MTBDD dd, BDDSET variables)
{
    const size_t bucket = gmp_count_hash(dd, variables, cache->bucket_count);
    for (struct gmp_count_entry *entry = cache->buckets[bucket];
         entry != NULL; entry = entry->next) {
        if (entry->dd == dd && entry->variables == variables) {
            mpz_set(result, entry->count);
            return 1;
        }
    }
    return 0;
}

static void
gmp_count_cache_grow(struct gmp_count_cache *cache)
{
    if (cache->bucket_count > SIZE_MAX / 2 ||
        cache->entry_count < 2 * cache->bucket_count) return;

    const size_t bucket_count = 2 * cache->bucket_count;
    struct gmp_count_entry **buckets = calloc(bucket_count, sizeof(*buckets));
    if (buckets == NULL) return;

    for (size_t i = 0; i < cache->bucket_count; i++) {
        struct gmp_count_entry *entry = cache->buckets[i];
        while (entry != NULL) {
            struct gmp_count_entry *next = entry->next;
            const size_t bucket = gmp_count_hash(entry->dd, entry->variables, bucket_count);
            entry->next = buckets[bucket];
            buckets[bucket] = entry;
            entry = next;
        }
    }
    free(cache->buckets);
    cache->buckets = buckets;
    cache->bucket_count = bucket_count;
}

static int
gmp_count_cache_put(struct gmp_count_cache *cache, MTBDD dd, BDDSET variables, const mpz_t count)
{
    gmp_count_cache_grow(cache);
    const size_t bucket = gmp_count_hash(dd, variables, cache->bucket_count);
    struct gmp_count_entry *entry = malloc(sizeof(*entry));
    if (entry == NULL) return 0;

    entry->dd = dd;
    entry->variables = variables;
    mpz_init_set(entry->count, count);
    entry->next = cache->buckets[bucket];
    cache->buckets[bucket] = entry;
    cache->entry_count++;
    return 1;
}

enum gmp_count_family { GMP_COUNT_BDD, GMP_COUNT_MTBDD, GMP_COUNT_LDD, GMP_COUNT_ZDD };

static int
gmp_count_shift(mpz_t result, uint64_t shift)
{
    /* mp_bitcnt_t is unsigned long, which can be narrower than size_t. */
    if (shift > ULONG_MAX) return 0;
    mpz_mul_2exp(result, result, (mp_bitcnt_t)shift);
    return 1;
}

static int
satcount_gmp_rec(struct gmp_count_cache *cache, uint64_t dd, uint64_t variables,
                enum gmp_count_family family, mpz_t result)
{
    uint64_t low_dd, high_dd, next = 0;
    size_t skipped = 0;
    Sylvan_Counters counter;

    switch (family) {
    case GMP_COUNT_BDD:
        counter = BDD_SATCOUNT_GMP;
        if (mtbdd_isleaf(dd)) {
            if (dd != sylvan_false && dd != sylvan_true) return 0;
            mpz_set_ui(result, dd == sylvan_true);
            if (dd == sylvan_true) return gmp_count_shift(result, sylvan_set_count(variables));
            return 1;
        }
        while (!sylvan_set_isempty(variables) && sylvan_set_first(variables) < sylvan_var(dd)) {
            skipped++;
            variables = sylvan_set_next(variables);
        }
        if (sylvan_set_isempty(variables) || sylvan_set_first(variables) != sylvan_var(dd)) return 0;
        next = sylvan_set_next(variables);
        low_dd = sylvan_low(dd);
        high_dd = sylvan_high(dd);
        break;
    case GMP_COUNT_MTBDD:
        counter = MTBDD_SATCOUNT_GMP;
        if (mtbdd_isleaf(dd)) {
            mpz_set_ui(result, mtbdd_count_leaf_is_nonzero(dd));
            if (mpz_sgn(result)) return gmp_count_shift(result, variables);
            return 1;
        }
        if (variables == 0) return 0;
        next = variables - 1;
        low_dd = mtbdd_getlow(dd);
        high_dd = mtbdd_gethigh(dd);
        break;
    case GMP_COUNT_LDD:
        counter = LDD_SATCOUNT_GMP;
        if (dd == lddmc_false || dd == lddmc_true) {
            mpz_set_ui(result, dd == lddmc_true);
            return 1;
        }
        low_dd = lddmc_getright(dd);
        high_dd = lddmc_getdown(dd);
        break;
    case GMP_COUNT_ZDD:
        counter = ZDD_SATCOUNT_GMP;
        if (dd == zdd_false || zdd_isleaf(dd)) {
            mpz_set_ui(result, dd != zdd_false);
            return 1;
        }
        low_dd = zdd_getlow(dd);
        high_dd = zdd_gethigh(dd);
        break;
    default:
        return 0;
    }

    sylvan_stats_count(counter);
    if (gmp_count_cache_get(cache, result, dd, variables)) {
        sylvan_stats_count(counter + 2); /* CACHED */
        return gmp_count_shift(result, skipped);
    }

    mpz_t low, high;
    mpz_init(low);
    mpz_init(high);
    int ok = satcount_gmp_rec(cache, low_dd, next, family, low);
    if (ok) ok = satcount_gmp_rec(cache, high_dd, next, family, high);
    if (ok) {
        mpz_add(result, low, high);
        ok = gmp_count_cache_put(cache, dd, variables, result);
        if (ok) {
            sylvan_stats_count(counter + 1); /* CACHEDPUT */
            ok = gmp_count_shift(result, skipped);
        }
    }
    mpz_clear(low);
    mpz_clear(high);
    return ok;
}

static int
satcount_gmp(uint64_t dd, uint64_t variables, enum gmp_count_family family, mpz_t destination)
{
    if (destination == NULL || dd == UINT64_MAX) return 0;
    if (family == GMP_COUNT_BDD && !sylvan_count_is_valid_set(variables)) return 0;

    struct gmp_count_cache cache;
    if (!gmp_count_cache_init(&cache)) return 0;
    mpz_t result;
    mpz_init(result);
    int ok = satcount_gmp_rec(&cache, dd, variables, family, result);
    if (ok) mpz_set(destination, result);
    mpz_clear(result);
    gmp_count_cache_clear(&cache);
    return ok;
}

TASK_IMPL_3(int, sylvan_satcount_gmp, BDD, dd, BDDSET, variables, mpz_ptr, result)
{
    return satcount_gmp(dd, variables, GMP_COUNT_BDD, result);
}

TASK_IMPL_3(int, mtbdd_satcount_gmp, MTBDD, dd, size_t, nvars, mpz_ptr, result)
{
    return satcount_gmp(dd, nvars, GMP_COUNT_MTBDD, result);
}

TASK_IMPL_2(int, lddmc_satcount_gmp, MDD, dd, mpz_ptr, result)
{
    return satcount_gmp(dd, 0, GMP_COUNT_LDD, result);
}

TASK_IMPL_2(int, zdd_satcount_gmp, ZDD, dd, mpz_ptr, result)
{
    return satcount_gmp(dd, 0, GMP_COUNT_ZDD, result);
}

/**
 * helper function for hash
 */
#ifndef rotl64
static inline uint64_t
rotl64(uint64_t x, int8_t r)
{
    return ((x<<r) | (x>>(64-r)));
}
#endif

static uint64_t
gmp_hash(const uint64_t v, const uint64_t seed)
{
    /* Hash the mpq in pointer v 
     * A simpler way would be to hash the result of mpq_get_d.
     * We just hash on the contents of the memory */
    
    mpq_ptr x = (mpq_ptr)(size_t)v;

    const uint64_t prime = 1099511628211;
    uint64_t hash = seed;
    mp_limb_t *limbs;

    // hash "numerator" limbs
    limbs = x[0]._mp_num._mp_d;
    for (int i=0; i<abs(x[0]._mp_num._mp_size); i++) {
        hash = hash ^ limbs[i];
        hash = rotl64(hash, 47);
        hash = hash * prime;
    }

    // hash "denominator" limbs
    limbs = x[0]._mp_den._mp_d;
    for (int i=0; i<abs(x[0]._mp_den._mp_size); i++) {
        hash = hash ^ limbs[i];
        hash = rotl64(hash, 31);
        hash = hash * prime;
    }

    return hash ^ (hash >> 32);
}

static int
gmp_equals(const uint64_t left, const uint64_t right)
{
    /* This function is called by the unique table when comparing a new
       leaf with an existing leaf */
    mpq_ptr x = (mpq_ptr)(size_t)left;
    mpq_ptr y = (mpq_ptr)(size_t)right;

    /* Just compare x and y */
    return mpq_equal(x, y) ? 1 : 0;
}

static void
gmp_create(uint64_t *val)
{
    /* This function is called by the unique table when a leaf does not yet exist.
       We make a copy, which will be stored in the hash table. */
    mpq_ptr x = (mpq_ptr)malloc(sizeof(__mpq_struct));
    mpq_init(x);
    mpq_set(x, *(mpq_ptr*)val);
    *(mpq_ptr*)val = x;
}

static void
gmp_destroy(uint64_t val)
{
    /* This function is called by the unique table
       when a leaf is removed during garbage collection. */
    mpq_clear((mpq_ptr)val);
    free((void*)val);
}

static char*
gmp_to_str(int comp, uint64_t val, char *buf, size_t buflen)
{
    mpq_ptr op = (mpq_ptr)val;
    size_t minsize = mpz_sizeinbase(mpq_numref(op), 10) + mpz_sizeinbase (mpq_denref(op), 10) + 3;
    if (buflen >= minsize) return mpq_get_str(buf, 10, op);
    else return mpq_get_str(NULL, 10, op);
    (void)comp;
}

static int
gmp_write_binary(FILE* out, uint64_t val)
{
    mpq_ptr op = (mpq_ptr)val;

    mpz_t i;
    mpz_init(i);
    mpq_get_num(i, op);
    if (mpz_out_raw(out, i) == 0) return -1;
    mpq_get_den(i, op);
    if (mpz_out_raw(out, i) == 0) return -1;
    mpz_clear(i);

    return 0;
}

static int
gmp_read_binary(FILE* in, uint64_t *val)
{
    mpq_ptr mres = (mpq_ptr)malloc(sizeof(__mpq_struct));
    mpq_init(mres);

    mpz_t i;
    mpz_init(i);
    if (mpz_inp_raw(i, in) == 0) return -1;
    mpq_set_num(mres, i);
    if (mpz_inp_raw(i, in) == 0) return -1;
    mpq_set_den(mres, i);
    mpz_clear(i);

    *(mpq_ptr*)val = mres;

    return 0;
}

/**
 * Initialize gmp custom leaves
 */
void
gmp_init()
{
    /* Register custom leaf */
    gmp_type = sylvan_mt_create_type();
    sylvan_mt_set_hash(gmp_type, gmp_hash);
    sylvan_mt_set_equals(gmp_type, gmp_equals);
    sylvan_mt_set_create(gmp_type, gmp_create);
    sylvan_mt_set_destroy(gmp_type, gmp_destroy);
    sylvan_mt_set_to_str(gmp_type, gmp_to_str);
    sylvan_mt_set_write_binary(gmp_type, gmp_write_binary);
    sylvan_mt_set_read_binary(gmp_type, gmp_read_binary);
}

/**
 * Create GMP mpq leaf
 */
MTBDD
mtbdd_gmp(mpq_t val)
{
    mpq_canonicalize(val);
    return mtbdd_makeleaf(gmp_type, (size_t)val);
}

/**
 * Operation "plus" for two mpq MTBDDs
 * Interpret partial function as "0"
 */
TASK_IMPL_2(MTBDD, gmp_op_plus, MTBDD*, pa, MTBDD*, pb)
{
    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_false) return b;
    if (b == mtbdd_false) return a;

    /* If both leaves, compute plus */
    if (mtbdd_isleaf(a) && mtbdd_isleaf(b)) {
        assert(mtbdd_gettype(a) == gmp_type && mtbdd_gettype(b) == gmp_type);

        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_getvalue(b);

        mpq_t mres;
        mpq_init(mres);
        mpq_add(mres, ma, mb);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return res;
    }

    /* Commutative, so swap a,b for better cache performance */
    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return mtbdd_invalid;
}

/**
 * Operation "minus" for two mpq MTBDDs
 * Interpret partial function as "0"
 */
TASK_IMPL_2(MTBDD, gmp_op_minus, MTBDD*, pa, MTBDD*, pb)
{
    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_false) return gmp_neg(b);
    if (b == mtbdd_false) return a;

    /* If both leaves, compute plus */
    if (mtbdd_isleaf(a) && mtbdd_isleaf(b)) {
        assert(mtbdd_gettype(a) == gmp_type && mtbdd_gettype(b) == gmp_type);

        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_getvalue(b);

        mpq_t mres;
        mpq_init(mres);
        mpq_sub(mres, ma, mb);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return res;
    }

    return mtbdd_invalid;
}

/**
 * Operation "times" for two mpq MTBDDs.
 * One of the parameters can be a BDD, then it is interpreted as a filter.
 * For partial functions, domain is intersection
 */
TASK_IMPL_2(MTBDD, gmp_op_times, MTBDD*, pa, MTBDD*, pb)
{
    MTBDD a = *pa, b = *pb;

    /* Check for partial functions and for Boolean (filter) */
    if (a == mtbdd_false || b == mtbdd_false) return mtbdd_false;

    /* If one of Boolean, interpret as filter */
    if (a == mtbdd_true) return b;
    if (b == mtbdd_true) return a;

    /* Handle multiplication of leaves */
    if (mtbdd_isleaf(a) && mtbdd_isleaf(b)) {
        assert(mtbdd_gettype(a) == gmp_type && mtbdd_gettype(b) == gmp_type);

        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_getvalue(b);

        // compute result
        mpq_t mres;
        mpq_init(mres);
        mpq_mul(mres, ma, mb);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return res;
    }

    /* Commutative, so make "a" the lowest for better cache performance */
    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return mtbdd_invalid;
}

/**
 * Operation "divide" for two mpq MTBDDs.
 * For partial functions, domain is intersection
 */
TASK_IMPL_2(MTBDD, gmp_op_divide, MTBDD*, pa, MTBDD*, pb)
{
    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_false || b == mtbdd_false) return mtbdd_false;

    /* Handle division of leaves */
    if (mtbdd_isleaf(a) && mtbdd_isleaf(b)) {
        assert(mtbdd_gettype(a) == gmp_type && mtbdd_gettype(b) == gmp_type);

        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_getvalue(b);

        // compute result
        mpq_t mres;
        mpq_init(mres);
        mpq_div(mres, ma, mb);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return res;
    }

    return mtbdd_invalid;
}

/**
 * Operation "min" for two mpq MTBDDs.
 */
TASK_IMPL_2(MTBDD, gmp_op_min, MTBDD*, pa, MTBDD*, pb)
{
    MTBDD a = *pa, b = *pb;

    /* Handle partial functions */
    if (a == mtbdd_false) return b;
    if (b == mtbdd_false) return a;

    /* Handle trivial case */
    if (a == b) return a;

    /* Compute result for leaves */
    if (mtbdd_isleaf(a) && mtbdd_isleaf(b)) {
        assert(mtbdd_gettype(a) == gmp_type && mtbdd_gettype(b) == gmp_type);

        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_getvalue(b);
        int cmp = mpq_cmp(ma, mb);
        return cmp < 0 ? a : b;
    }

    /* For cache performance */
    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return mtbdd_invalid;
}

/**
 * Operation "max" for two mpq MTBDDs.
 */
TASK_IMPL_2(MTBDD, gmp_op_max, MTBDD*, pa, MTBDD*, pb)
{
    MTBDD a = *pa, b = *pb;

    /* Handle partial functions */
    if (a == mtbdd_false) return b;
    if (b == mtbdd_false) return a;

    /* Handle trivial case */
    if (a == b) return a;

    /* Compute result for leaves */
    if (mtbdd_isleaf(a) && mtbdd_isleaf(b)) {
        assert(mtbdd_gettype(a) == gmp_type && mtbdd_gettype(b) == gmp_type);

        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_getvalue(b);
        int cmp = mpq_cmp(ma, mb);
        return cmp > 0 ? a : b;
    }

    /* For cache performance */
    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return mtbdd_invalid;
}

/**
 * Operation "neg" for one mpq MTBDD
 */
TASK_IMPL_2(MTBDD, gmp_op_neg, MTBDD, dd, size_t, p)
{
    /* Handle partial functions */
    if (dd == mtbdd_false) return mtbdd_false;

    /* Compute result for leaf */
    if (mtbdd_isleaf(dd)) {
        assert(mtbdd_gettype(dd) == gmp_type);

        mpq_ptr m = (mpq_ptr)mtbdd_getvalue(dd);

        mpq_t mres;
        mpq_init(mres);
        mpq_neg(mres, m);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return res;
    }

    return mtbdd_invalid;
    (void)p;
}

/**
 * Operation "abs" for one mpq MTBDD
 */
TASK_IMPL_2(MTBDD, gmp_op_abs, MTBDD, dd, size_t, p)
{
    /* Handle partial functions */
    if (dd == mtbdd_false) return mtbdd_false;

    /* Compute result for leaf */
    if (mtbdd_isleaf(dd)) {
        assert(mtbdd_gettype(dd) == gmp_type);

        mpq_ptr m = (mpq_ptr)mtbdd_getvalue(dd);

        mpq_t mres;
        mpq_init(mres);
        mpq_abs(mres, m);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return res;
    }

    return mtbdd_invalid;
    (void)p;
}

/**
 * The abstraction operators are called in either of two ways:
 * - with k=0, then just calculate "a op b"
 * - with k<>0, then just calculate "a := a op a", k times
 */

TASK_IMPL_3(MTBDD, gmp_abstract_op_plus, MTBDD, a, MTBDD, b, int, k)
{
    if (k==0) {
        return mtbdd_apply(a, b, gmp_op_plus_CALL);
    } else {
        MTBDD res = a;
        for (int i=0; i<k; i++) {
            mtbdd_refs_push(res);
            res = mtbdd_apply(res, res, gmp_op_plus_CALL);
            mtbdd_refs_pop(1);
        }
        return res;
    }
}

TASK_IMPL_3(MTBDD, gmp_abstract_op_times, MTBDD, a, MTBDD, b, int, k)
{
    if (k==0) {
        return mtbdd_apply(a, b, gmp_op_times_CALL);
    } else {
        MTBDD res = a;
        for (int i=0; i<k; i++) {
            mtbdd_refs_push(res);
            res = mtbdd_apply(res, res, gmp_op_times_CALL);
            mtbdd_refs_pop(1);
        }
        return res;
    }
}

TASK_IMPL_3(MTBDD, gmp_abstract_op_min, MTBDD, a, MTBDD, b, int, k)
{
    if (k == 0) {
        return mtbdd_apply(a, b, gmp_op_min_CALL);
    } else {
        // nothing to do: min(a, a) = a
        return a;
    }
}

TASK_IMPL_3(MTBDD, gmp_abstract_op_max, MTBDD, a, MTBDD, b, int, k)
{
    if (k == 0) {
        return mtbdd_apply(a, b, gmp_op_max_CALL);
    } else {
        // nothing to do: max(a, a) = a
        return a;
    }
}

/**
 * Convert to Boolean MTBDD, terminals >= value (double) to True, or False otherwise.
 */
TASK_2(MTBDD, gmp_op_threshold_d, MTBDD, a, size_t, svalue)
{
    /* Handle partial function */
    if (a == mtbdd_false) return mtbdd_false;

    /* Compute result */
    if (mtbdd_isleaf(a)) {
        assert(mtbdd_gettype(a) == gmp_type);

        double value = *(double*)&svalue;
        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        return mpq_get_d(ma) >= value ? mtbdd_true : mtbdd_false;
    }

    return mtbdd_invalid;
}

/**
 * Convert to Boolean MTBDD, terminals > value (double) to True, or False otherwise.
 */
TASK_2(MTBDD, gmp_op_strict_threshold_d, MTBDD, a, size_t, svalue)
{
    /* Handle partial function */
    if (a == mtbdd_false) return mtbdd_false;

    /* Compute result */
    if (mtbdd_isleaf(a)) {
        assert(mtbdd_gettype(a) == gmp_type);

        double value = *(double*)&svalue;
        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        return mpq_get_d(ma) > value ? mtbdd_true : mtbdd_false;
    }

    return mtbdd_invalid;
}

TASK_IMPL_2(MTBDD, gmp_threshold_d, MTBDD, dd, double, d)
{
    return mtbdd_uapply(dd, gmp_op_threshold_d_CALL, *(size_t*)&d);
}

TASK_IMPL_2(MTBDD, gmp_strict_threshold_d, MTBDD, dd, double, d)
{
    return mtbdd_uapply(dd, gmp_op_strict_threshold_d_CALL, *(size_t*)&d);
}

/**
 * Operation "threshold" for mpq MTBDDs.
 * The second parameter must be a mpq leaf.
 */
TASK_IMPL_2(MTBDD, gmp_op_threshold, MTBDD*, pa, MTBDD*, pb)
{
    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_false) return mtbdd_false;

    /* Handle comparison of leaves */
    if (mtbdd_isleaf(a)) {
        assert(mtbdd_gettype(a) == gmp_type);

        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_getvalue(b);
        int cmp = mpq_cmp(ma, mb);
        return cmp >= 0 ? mtbdd_true : mtbdd_false;
    }

    return mtbdd_invalid;
}

/**
 * Operation "strict threshold" for mpq MTBDDs.
 * The second parameter must be a mpq leaf.
 */
TASK_IMPL_2(MTBDD, gmp_op_strict_threshold, MTBDD*, pa, MTBDD*, pb)
{
    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_false) return mtbdd_false;

    /* Handle comparison of leaves */
    if (mtbdd_isleaf(a)) {
        assert(mtbdd_gettype(a) == gmp_type);

        mpq_ptr ma = (mpq_ptr)mtbdd_getvalue(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_getvalue(b);
        int cmp = mpq_cmp(ma, mb);
        return cmp > 0 ? mtbdd_true : mtbdd_false;
    }

    return mtbdd_invalid;
}

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 */
TASK_IMPL_3(MTBDD, gmp_and_abstract_plus, MTBDD, a, MTBDD, b, MTBDD, v)
{
    /* Check terminal cases */

    /* If v == true, then <vars> is an empty set */
    if (v == mtbdd_true) return mtbdd_apply(a, b, gmp_op_times_CALL);

    /* Try the times operator on a and b */
    MTBDD result = CALL(gmp_op_times, &a, &b);
    if (result != mtbdd_invalid) {
        /* Times operator successful, store reference (for garbage collection) */
        mtbdd_refs_push(result);
        /* ... and perform abstraction */
        result = mtbdd_abstract(result, v, gmp_abstract_op_plus_CALL);
        mtbdd_refs_pop(1);
        /* Note that the operation cache is used in mtbdd_abstract */
        return result;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Count operation */
    sylvan_stats_count(MTBDD_AND_ABSTRACT_PLUS);

    /* Check cache. Note that we do this now, since the times operator might swap a and b (commutative) */
    if (cache_get3(CACHE_MTBDD_AND_ABSTRACT_PLUS, a, b, v, &result)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_PLUS_CACHED);
        return result;
    }

    /* Now, v is not a constant, and either a or b is not a constant */

    /* Get top variable */
    int la = mtbdd_isleaf(a);
    int lb = mtbdd_isleaf(b);
    mtbddnode_t na = la ? 0 : MTBDD_GETNODE(a);
    mtbddnode_t nb = lb ? 0 : MTBDD_GETNODE(b);
    uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
    uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
    uint32_t var = va < vb ? va : vb;

    mtbddnode_t nv = MTBDD_GETNODE(v);
    uint32_t vv = mtbddnode_getvariable(nv);

    if (vv < var) {
        /* Recursive, then abstract result */
        result = CALL(gmp_and_abstract_plus, a, b, node_gethigh(v, nv));
        mtbdd_refs_push(result);
        result = mtbdd_apply(result, result, gmp_op_plus_CALL);
        mtbdd_refs_pop(1);
    } else {
        /* Get cofactors */
        MTBDD alow, ahigh, blow, bhigh;
        alow  = (!la && va == var) ? node_getlow(a, na)  : a;
        ahigh = (!la && va == var) ? node_gethigh(a, na) : a;
        blow  = (!lb && vb == var) ? node_getlow(b, nb)  : b;
        bhigh = (!lb && vb == var) ? node_gethigh(b, nb) : b;

        if (vv == var) {
            /* Recursive, then abstract result */
            mtbdd_refs_spawn(SPAWN(gmp_and_abstract_plus, ahigh, bhigh, node_gethigh(v, nv)));
            MTBDD low = mtbdd_refs_push(CALL(gmp_and_abstract_plus, alow, blow, node_gethigh(v, nv)));
            MTBDD high = mtbdd_refs_push(mtbdd_refs_sync(SYNC(gmp_and_abstract_plus)));
            result = CALL(mtbdd_apply, low, high, gmp_op_plus_CALL);
            mtbdd_refs_pop(2);
        } else /* vv > v */ {
            /* Recursive, then create node */
            mtbdd_refs_spawn(SPAWN(gmp_and_abstract_plus, ahigh, bhigh, v));
            MTBDD low = mtbdd_refs_push(CALL(gmp_and_abstract_plus, alow, blow, v));
            MTBDD high = mtbdd_refs_sync(SYNC(gmp_and_abstract_plus));
            mtbdd_refs_pop(1);
            result = mtbdd_makenode(var, low, high);
        }
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_AND_ABSTRACT_PLUS, a, b, v, result)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_PLUS_CACHEDPUT);
    }

    return result;
}

/**
 * Multiply <a> and <b>, and abstract variables <vars> by taking the maximum.
 */
TASK_IMPL_3(MTBDD, gmp_and_abstract_max, MTBDD, a, MTBDD, b, MTBDD, v)
{
    /* Check terminal cases */

    /* If v == true, then <vars> is an empty set */
    if (v == mtbdd_true) return mtbdd_apply(a, b, gmp_op_times_CALL);

    /* Try the times operator on a and b */
    MTBDD result = CALL(gmp_op_times, &a, &b);
    if (result != mtbdd_invalid) {
        /* Times operator successful, store reference (for garbage collection) */
        mtbdd_refs_push(result);
        /* ... and perform abstraction */
        result = mtbdd_abstract(result, v, gmp_abstract_op_max_CALL);
        mtbdd_refs_pop(1);
        /* Note that the operation cache is used in mtbdd_abstract */
        return result;
    }

    /* Now, v is not a constant, and either a or b is not a constant */

    /* Get top variable */
    int la = mtbdd_isleaf(a);
    int lb = mtbdd_isleaf(b);
    mtbddnode_t na = la ? 0 : MTBDD_GETNODE(a);
    mtbddnode_t nb = lb ? 0 : MTBDD_GETNODE(b);
    uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
    uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
    uint32_t var = va < vb ? va : vb;

    mtbddnode_t nv = MTBDD_GETNODE(v);
    uint32_t vv = mtbddnode_getvariable(nv);

    while (vv < var) {
        /* we can skip variables, because max(r,r) = r */
        v = node_high(v, nv);
        if (v == mtbdd_true) return mtbdd_apply(a, b, gmp_op_times_CALL);
        nv = MTBDD_GETNODE(v);
        vv = mtbddnode_getvariable(nv);
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Count operation */
    sylvan_stats_count(MTBDD_AND_ABSTRACT_MAX);

    /* Check cache. Note that we do this now, since the times operator might swap a and b (commutative) */
    if (cache_get3(CACHE_MTBDD_AND_ABSTRACT_MAX, a, b, v, &result)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_MAX_CACHED);
        return result;
    }

    /* Get cofactors */
    MTBDD alow, ahigh, blow, bhigh;
    alow  = (!la && va == var) ? node_getlow(a, na)  : a;
    ahigh = (!la && va == var) ? node_gethigh(a, na) : a;
    blow  = (!lb && vb == var) ? node_getlow(b, nb)  : b;
    bhigh = (!lb && vb == var) ? node_gethigh(b, nb) : b;

    if (vv == var) {
        /* Recursive, then abstract result */
        mtbdd_refs_spawn(SPAWN(gmp_and_abstract_max, ahigh, bhigh, node_gethigh(v, nv)));
        MTBDD low = mtbdd_refs_push(CALL(gmp_and_abstract_max, alow, blow, node_gethigh(v, nv)));
        MTBDD high = mtbdd_refs_push(mtbdd_refs_sync(SYNC(gmp_and_abstract_max)));
        result = CALL(mtbdd_apply, low, high, gmp_op_max_CALL);
        mtbdd_refs_pop(2);
    } else /* vv > v */ {
        /* Recursive, then create node */
        mtbdd_refs_spawn(SPAWN(gmp_and_abstract_max, ahigh, bhigh, v));
        MTBDD low = mtbdd_refs_push(CALL(gmp_and_abstract_max, alow, blow, v));
        MTBDD high = mtbdd_refs_sync(SYNC(gmp_and_abstract_max));
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(var, low, high);
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_AND_ABSTRACT_MAX, a, b, v, result)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_MAX_CACHEDPUT);
    }

    return result;
}
