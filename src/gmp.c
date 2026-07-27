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

#include <sylvan/internal.h>
#include <sylvan/gmp.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

static uint32_t gmp_type;

static_assert(sizeof(size_t) >= sizeof(double), "GMP double parameters require 64-bit size_t");

static inline size_t
gmp_double_parameter(double value)
{
    size_t parameter = 0;
    memcpy(&parameter, &value, sizeof(value));
    return parameter;
}

static inline double
gmp_parameter_double(size_t parameter)
{
    double value;
    memcpy(&value, &parameter, sizeof(value));
    return value;
}

static int
gmp_count_leaf_is_nonzero(MTBDD dd)
{
    if (dd == mtbdd_undefined) return 0;
    if (dd == bdd_true) return 1;

    switch (mtbdd_leaf_type(dd)) {
    case 0:
        return mtbdd_leaf_int64(dd) != 0;
    case 1:
        return mtbdd_leaf_double(dd) != 0.0;
    case 2:
        return mtbdd_fraction_numerator(dd) != 0;
    default:
        return 1;
    }
}

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
    return cache->buckets == NULL ? SYLVAN_ERR_OOM : SYLVAN_OK;
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
    if (entry == NULL) return SYLVAN_ERR_OOM;

    entry->dd = dd;
    entry->variables = variables;
    mpz_init_set(entry->count, count);
    entry->next = cache->buckets[bucket];
    cache->buckets[bucket] = entry;
    cache->entry_count++;
    return SYLVAN_OK;
}

static int
sat_count_gmp_rec(struct gmp_count_cache *cache, mpz_t result,
                  MTBDD dd, BDDSET variables, int bdd_only)
{
    if (mtbdd_is_leaf(dd)) {
        if (bdd_only && dd != bdd_false && dd != bdd_true) return SYLVAN_ERR_INVALID;
        if ((bdd_only && dd == bdd_false) || (!bdd_only && !gmp_count_leaf_is_nonzero(dd))) {
            mpz_set_ui(result, 0);
        } else {
            mpz_set_ui(result, 1);
            mpz_mul_2exp(result, result, (mp_bitcnt_t)bdd_set_count(variables));
        }
        return SYLVAN_OK;
    }

    size_t skipped = 0;
    const uint32_t variable = mtbdd_node_variable(dd);
    while (!bdd_set_is_empty(variables) && bdd_set_first(variables) < variable) {
        skipped++;
        variables = bdd_set_next(variables);
    }
    if (bdd_set_is_empty(variables) || bdd_set_first(variables) != variable) {
        return SYLVAN_ERR_INVALID;
    }

    if (gmp_count_cache_get(cache, result, dd, variables)) {
        mpz_mul_2exp(result, result, (mp_bitcnt_t)skipped);
        return SYLVAN_OK;
    }

    const BDDSET next = bdd_set_next(variables);
    mpz_t low, high;
    mpz_init(low);
    mpz_init(high);
    int status = sat_count_gmp_rec(cache, low, mtbdd_node_low(dd), next, bdd_only);
    if (status == SYLVAN_OK) {
        status = sat_count_gmp_rec(cache, high, mtbdd_node_high(dd), next, bdd_only);
    }
    if (status == SYLVAN_OK) {
        mpz_add(result, low, high);
        status = gmp_count_cache_put(cache, dd, variables, result);
        if (status == SYLVAN_OK) {
            mpz_mul_2exp(result, result, (mp_bitcnt_t)skipped);
        }
    }
    mpz_clear(low);
    mpz_clear(high);
    return status;
}

int
bdd_sat_count_gmp(mpz_t destination, BDD dd, BDDSET variables)
{
    if (destination == NULL || dd == mtbdd_invalid || variables == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    struct gmp_count_cache cache;
    int status = gmp_count_cache_init(&cache);
    if (status != SYLVAN_OK) return status;

    mpz_t result;
    mpz_init(result);
    status = sat_count_gmp_rec(&cache, result, dd, variables, 1);
    if (status == SYLVAN_OK) {
        mpz_set(destination, result);
        sylvan_stats_count(BDD_SAT_COUNT_GMP);
    }
    mpz_clear(result);
    gmp_count_cache_clear(&cache);
    return status;
}

int
mtbdd_sat_count_gmp(mpz_t destination, MTBDD dd, BDDSET variables)
{
    if (destination == NULL || dd == mtbdd_invalid || variables == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    struct gmp_count_cache cache;
    int status = gmp_count_cache_init(&cache);
    if (status != SYLVAN_OK) return status;

    mpz_t result;
    mpz_init(result);
    status = sat_count_gmp_rec(&cache, result, dd, variables, 0);
    if (status == SYLVAN_OK) {
        mpz_set(destination, result);
        sylvan_stats_count(MTBDD_SAT_COUNT_GMP);
    }
    mpz_clear(result);
    gmp_count_cache_clear(&cache);
    return status;
}

static int
zdd_count_gmp_rec(struct gmp_count_cache *cache, mpz_t result, ZDD dd)
{
    if (dd == zdd_false) {
        mpz_set_ui(result, 0);
        return SYLVAN_OK;
    }
    if (zdd_is_leaf(dd)) {
        mpz_set_ui(result, 1);
        return SYLVAN_OK;
    }
    if (gmp_count_cache_get(cache, result, dd, bdd_true)) return SYLVAN_OK;

    mpz_t low;
    mpz_t high;
    mpz_init(low);
    mpz_init(high);
    int status = zdd_count_gmp_rec(cache, low, zdd_node_low(dd));
    if (status == SYLVAN_OK) {
        status = zdd_count_gmp_rec(cache, high, zdd_node_high(dd));
    }
    if (status == SYLVAN_OK) {
        mpz_add(result, low, high);
        status = gmp_count_cache_put(cache, dd, bdd_true, result);
    }
    mpz_clear(low);
    mpz_clear(high);
    return status;
}

int
zdd_count_gmp(mpz_t destination, ZDD dd)
{
    if (destination == NULL || dd == zdd_invalid) return SYLVAN_ERR_INVALID;

    struct gmp_count_cache cache;
    int status = gmp_count_cache_init(&cache);
    if (status != SYLVAN_OK) return status;

    mpz_t result;
    mpz_init(result);
    status = zdd_count_gmp_rec(&cache, result, dd);
    if (status == SYLVAN_OK) {
        mpz_set(destination, result);
        sylvan_stats_count(ZDD_COUNT_GMP);
    }
    mpz_clear(result);
    gmp_count_cache_clear(&cache);
    return status;
}

static int
listdd_count_gmp_rec(struct gmp_count_cache *cache, mpz_t result, LISTDD dd)
{
    if (dd == listdd_empty) {
        mpz_set_ui(result, 0);
        return SYLVAN_OK;
    }
    if (dd == listdd_empty_list) {
        mpz_set_ui(result, 1);
        return SYLVAN_OK;
    }
    if (gmp_count_cache_get(cache, result, dd, listdd_empty_list)) {
        return SYLVAN_OK;
    }

    const mddnode *node = LDD_GETNODE(dd);
    mpz_t down;
    mpz_t right;
    mpz_init(down);
    mpz_init(right);
    int status = listdd_count_gmp_rec(
        cache, down, mddnode_getdown(node));
    if (status == SYLVAN_OK) {
        status = listdd_count_gmp_rec(
            cache, right, mddnode_getright(node));
    }
    if (status == SYLVAN_OK) {
        mpz_add(result, down, right);
        status = gmp_count_cache_put(
            cache, dd, listdd_empty_list, result);
    }
    mpz_clear(down);
    mpz_clear(right);
    return status;
}

int
listdd_count_gmp(mpz_t destination, LISTDD dd)
{
    if (destination == NULL || dd == listdd_invalid) return SYLVAN_ERR_INVALID;

    struct gmp_count_cache cache;
    int status = gmp_count_cache_init(&cache);
    if (status != SYLVAN_OK) return status;

    mpz_t result;
    mpz_init(result);
    status = listdd_count_gmp_rec(&cache, result, dd);
    if (status == SYLVAN_OK) {
        mpz_set(destination, result);
        sylvan_stats_count(LDD_SATCOUNT_GMP);
    }
    mpz_clear(result);
    gmp_count_cache_clear(&cache);
    return status;
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
gmp_init(void)
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
    return mtbdd_leaf(gmp_type, (size_t)val);
}

static int
gmp_apply_result(MTBDD *destination, MTBDD result)
{
    if (result == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    *destination = result;
    return SYLVAN_OK;
}

/**
 * Operation "plus" for two mpq MTBDDs
 * Undefined values propagate.
 */
int gmp_op_plus_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        return gmp_apply_result(destination, mtbdd_undefined);
    }

    /* If both leaves, compute plus */
    if (mtbdd_is_leaf(a) && mtbdd_is_leaf(b)) {
        if (mtbdd_leaf_type(a) != gmp_type || mtbdd_leaf_type(b) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a) || mtbdd_is_nan(b)) {
            return gmp_apply_result(destination, mtbdd_nan(gmp_type));
        }

        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_leaf_value(b);

        mpq_t mres;
        mpq_init(mres);
        mpq_add(mres, ma, mb);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return gmp_apply_result(destination, res);
    }

    /* Commutative, so swap a,b for better cache performance */
    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Operation "minus" for two mpq MTBDDs
 * Undefined values propagate.
 */
int gmp_op_minus_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        return gmp_apply_result(destination, mtbdd_undefined);
    }

    /* If both leaves, compute plus */
    if (mtbdd_is_leaf(a) && mtbdd_is_leaf(b)) {
        if (mtbdd_leaf_type(a) != gmp_type || mtbdd_leaf_type(b) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a) || mtbdd_is_nan(b)) {
            return gmp_apply_result(destination, mtbdd_nan(gmp_type));
        }

        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_leaf_value(b);

        mpq_t mres;
        mpq_init(mres);
        mpq_sub(mres, ma, mb);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return gmp_apply_result(destination, res);
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Operation "times" for two mpq MTBDDs.
 * One of the parameters can be a BDD, then it is interpreted as a filter.
 * For partial functions, domain is intersection
 */
int gmp_op_times_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;

    /* Check for partial functions and for Boolean (filter) */
    if (a == mtbdd_undefined || b == mtbdd_undefined) return gmp_apply_result(destination, mtbdd_undefined);

    /* If one of Boolean, interpret as filter */
    if (a == bdd_true) return gmp_apply_result(destination, b);
    if (b == bdd_true) return gmp_apply_result(destination, a);

    /* Handle multiplication of leaves */
    if (mtbdd_is_leaf(a) && mtbdd_is_leaf(b)) {
        if (mtbdd_leaf_type(a) != gmp_type || mtbdd_leaf_type(b) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a) || mtbdd_is_nan(b)) {
            return gmp_apply_result(destination, mtbdd_nan(gmp_type));
        }

        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_leaf_value(b);

        // compute result
        mpq_t mres;
        mpq_init(mres);
        mpq_mul(mres, ma, mb);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return gmp_apply_result(destination, res);
    }

    /* Commutative, so make "a" the lowest for better cache performance */
    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Operation "divide" for two mpq MTBDDs.
 * For partial functions, domain is intersection
 */
int gmp_op_divide_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_undefined || b == mtbdd_undefined) return gmp_apply_result(destination, mtbdd_undefined);

    /* Handle division of leaves */
    if (mtbdd_is_leaf(a) && mtbdd_is_leaf(b)) {
        if (mtbdd_leaf_type(a) != gmp_type || mtbdd_leaf_type(b) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a) || mtbdd_is_nan(b)) {
            return gmp_apply_result(destination, mtbdd_nan(gmp_type));
        }

        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_leaf_value(b);
        if (mpq_sgn(mb) == 0) {
            return gmp_apply_result(destination, mtbdd_nan(gmp_type));
        }

        // compute result
        mpq_t mres;
        mpq_init(mres);
        mpq_div(mres, ma, mb);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return gmp_apply_result(destination, res);
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Operation "min" for two mpq MTBDDs.
 */
int gmp_op_min_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;

    /* Handle partial functions */
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        return gmp_apply_result(destination, mtbdd_undefined);
    }

    /* Handle trivial case */
    if (a == b) return gmp_apply_result(destination, a);

    /* Compute result for leaves */
    if (mtbdd_is_leaf(a) && mtbdd_is_leaf(b)) {
        if (mtbdd_leaf_type(a) != gmp_type || mtbdd_leaf_type(b) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a) || mtbdd_is_nan(b)) {
            return gmp_apply_result(destination, mtbdd_nan(gmp_type));
        }

        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_leaf_value(b);
        int cmp = mpq_cmp(ma, mb);
        return gmp_apply_result(destination, cmp < 0 ? a : b);
    }

    /* For cache performance */
    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Operation "max" for two mpq MTBDDs.
 */
int gmp_op_max_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;

    /* Handle partial functions */
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        return gmp_apply_result(destination, mtbdd_undefined);
    }

    /* Handle trivial case */
    if (a == b) return gmp_apply_result(destination, a);

    /* Compute result for leaves */
    if (mtbdd_is_leaf(a) && mtbdd_is_leaf(b)) {
        if (mtbdd_leaf_type(a) != gmp_type || mtbdd_leaf_type(b) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a) || mtbdd_is_nan(b)) {
            return gmp_apply_result(destination, mtbdd_nan(gmp_type));
        }

        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_leaf_value(b);
        int cmp = mpq_cmp(ma, mb);
        return gmp_apply_result(destination, cmp > 0 ? a : b);
    }

    /* For cache performance */
    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Operation "neg" for one mpq MTBDD
 */
int gmp_op_neg_CALL(lace_worker* lace, MTBDD *destination, MTBDD dd, size_t p)
{
    (void)lace;
    (void)p;

    /* Handle partial functions */
    if (dd == mtbdd_undefined) return gmp_apply_result(destination, mtbdd_undefined);

    /* Compute result for leaf */
    if (mtbdd_is_leaf(dd)) {
        if (mtbdd_leaf_type(dd) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(dd)) return gmp_apply_result(destination, mtbdd_nan(gmp_type));

        mpq_ptr m = (mpq_ptr)mtbdd_leaf_value(dd);

        mpq_t mres;
        mpq_init(mres);
        mpq_neg(mres, m);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return gmp_apply_result(destination, res);
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Operation "abs" for one mpq MTBDD
 */
int gmp_op_abs_CALL(lace_worker* lace, MTBDD *destination, MTBDD dd, size_t p)
{
    (void)lace;
    (void)p;

    /* Handle partial functions */
    if (dd == mtbdd_undefined) return gmp_apply_result(destination, mtbdd_undefined);

    /* Compute result for leaf */
    if (mtbdd_is_leaf(dd)) {
        if (mtbdd_leaf_type(dd) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(dd)) return gmp_apply_result(destination, mtbdd_nan(gmp_type));

        mpq_ptr m = (mpq_ptr)mtbdd_leaf_value(dd);

        mpq_t mres;
        mpq_init(mres);
        mpq_abs(mres, m);
        MTBDD res = mtbdd_gmp(mres);
        mpq_clear(mres);
        return gmp_apply_result(destination, res);
    }

    return SYLVAN_APPLY_RECURSE;
}

int
gmp_neg_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    return mtbdd_apply_unary_CALL(
        lace, destination, dd, gmp_op_neg_CALL, 0);
}

int
gmp_abs_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    return mtbdd_apply_unary_CALL(
        lace, destination, dd, gmp_op_abs_CALL, 0);
}

/**
 * The abstraction operators are called in either of two ways:
 * - with k=0, then just calculate "a op b"
 * - with k<>0, then just calculate "a := a op a", k times
 */

int gmp_abstract_op_plus_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || k < 0) return SYLVAN_ERR_INVALID;

    if (k==0) {
        return mtbdd_apply_CALL(lace, destination, a, b, gmp_op_plus_CALL);
    } else {
        MTBDD res = a;
        mtbdd_refs_pushptr(&res);
        int status = SYLVAN_OK;
        for (int i=0; i<k; i++) {
            status = mtbdd_apply_CALL(lace, &res, res, res, gmp_op_plus_CALL);
            if (status != SYLVAN_OK) break;
        }
        if (status == SYLVAN_OK) *destination = res;
        mtbdd_refs_popptr(1);
        return status;
    }
}

int gmp_abstract_op_times_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || k < 0) return SYLVAN_ERR_INVALID;

    if (k==0) {
        return mtbdd_apply_CALL(lace, destination, a, b, gmp_op_times_CALL);
    } else {
        MTBDD res = a;
        mtbdd_refs_pushptr(&res);
        int status = SYLVAN_OK;
        for (int i=0; i<k; i++) {
            status = mtbdd_apply_CALL(lace, &res, res, res, gmp_op_times_CALL);
            if (status != SYLVAN_OK) break;
        }
        if (status == SYLVAN_OK) *destination = res;
        mtbdd_refs_popptr(1);
        return status;
    }
}

int gmp_abstract_op_min_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || k < 0) return SYLVAN_ERR_INVALID;

    if (k == 0) {
        return mtbdd_apply_CALL(lace, destination, a, b, gmp_op_min_CALL);
    } else {
        // nothing to do: min(a, a) = a
        *destination = a;
        return SYLVAN_OK;
    }
}

int gmp_abstract_op_max_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || k < 0) return SYLVAN_ERR_INVALID;

    if (k == 0) {
        return mtbdd_apply_CALL(lace, destination, a, b, gmp_op_max_CALL);
    } else {
        // nothing to do: max(a, a) = a
        *destination = a;
        return SYLVAN_OK;
    }
}

/**
 * Convert to Boolean MTBDD, terminals >= value (double) to True, or False otherwise.
 */
int gmp_op_threshold_d_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, size_t svalue)
{
    (void)lace;

    /* Handle partial function */
    if (a == mtbdd_undefined) return gmp_apply_result(destination, mtbdd_undefined);

    /* Compute result */
    if (mtbdd_is_leaf(a)) {
        if (mtbdd_leaf_type(a) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a)) return gmp_apply_result(destination, mtbdd_undefined);

        double value = gmp_parameter_double(svalue);
        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        return gmp_apply_result(destination, mpq_get_d(ma) >= value ? bdd_true : mtbdd_undefined);
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Convert to Boolean MTBDD, terminals > value (double) to True, or False otherwise.
 */
TASK(int, gmp_op_strict_threshold_d, MTBDD*, result, MTBDD, a, size_t, svalue)
int gmp_op_strict_threshold_d_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, size_t svalue)
{
    (void)lace;

    /* Handle partial function */
    if (a == mtbdd_undefined) return gmp_apply_result(destination, mtbdd_undefined);

    /* Compute result */
    if (mtbdd_is_leaf(a)) {
        if (mtbdd_leaf_type(a) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a)) return gmp_apply_result(destination, mtbdd_undefined);

        double value = gmp_parameter_double(svalue);
        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        return gmp_apply_result(destination, mpq_get_d(ma) > value ? bdd_true : mtbdd_undefined);
    }

    return SYLVAN_APPLY_RECURSE;
}

int gmp_threshold_d_CALL(lace_worker* lace, MTBDD *destination, MTBDD dd, double d)
{
    return mtbdd_apply_unary_CALL(lace, destination, dd, gmp_op_threshold_d_CALL, gmp_double_parameter(d));
}

int gmp_strict_threshold_d_CALL(lace_worker* lace, MTBDD *destination, MTBDD dd, double d)
{
    return mtbdd_apply_unary_CALL(lace, destination, dd, gmp_op_strict_threshold_d_CALL, gmp_double_parameter(d));
}

/**
 * Operation "threshold" for mpq MTBDDs.
 * The second parameter must be a mpq leaf.
 */
int gmp_op_threshold_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_undefined) return gmp_apply_result(destination, mtbdd_undefined);

    /* Handle comparison of leaves */
    if (mtbdd_is_leaf(a)) {
        if (mtbdd_leaf_type(a) != gmp_type || !mtbdd_is_leaf(b) || mtbdd_leaf_type(b) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a) || mtbdd_is_nan(b)) {
            return gmp_apply_result(destination, mtbdd_undefined);
        }

        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_leaf_value(b);
        int cmp = mpq_cmp(ma, mb);
        return gmp_apply_result(destination, cmp >= 0 ? bdd_true : mtbdd_undefined);
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Operation "strict threshold" for mpq MTBDDs.
 * The second parameter must be a mpq leaf.
 */
int gmp_op_strict_threshold_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;

    /* Check for partial functions */
    if (a == mtbdd_undefined) return gmp_apply_result(destination, mtbdd_undefined);

    /* Handle comparison of leaves */
    if (mtbdd_is_leaf(a)) {
        if (mtbdd_leaf_type(a) != gmp_type || !mtbdd_is_leaf(b) || mtbdd_leaf_type(b) != gmp_type) return SYLVAN_ERR_INVALID;
        if (mtbdd_is_nan(a) || mtbdd_is_nan(b)) {
            return gmp_apply_result(destination, mtbdd_undefined);
        }

        mpq_ptr ma = (mpq_ptr)mtbdd_leaf_value(a);
        mpq_ptr mb = (mpq_ptr)mtbdd_leaf_value(b);
        int cmp = mpq_cmp(ma, mb);
        return gmp_apply_result(destination, cmp > 0 ? bdd_true : mtbdd_undefined);
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 */
int gmp_and_abstract_plus_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, MTBDD v)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || v == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check terminal cases */

    /* If v == true, then <vars> is an empty set */
    if (v == bdd_true) return mtbdd_apply_CALL(lace, destination, a, b, gmp_op_times_CALL);

    /* Try the times operator on a and b */
    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = gmp_op_times_CALL(lace, &computed, &a, &b);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) status = SYLVAN_ERR_CALLBACK;
        else status = mtbdd_abstract_CALL(lace, &computed, computed, v, gmp_abstract_op_plus_CALL);
        /* Note that the operation cache is used in mtbdd_abstract */
        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(1);
        return status;
    }
    if (status != SYLVAN_APPLY_RECURSE) {
        mtbdd_refs_popptr(1);
        return status < 0 ? status : SYLVAN_ERR_CALLBACK;
    }
    if (mtbdd_is_leaf(a) && mtbdd_is_leaf(b)) {
        mtbdd_refs_popptr(1);
        return SYLVAN_ERR_CALLBACK;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_AND_ABSTRACT_PLUS);

    /* Check cache. Note that we do this now, since the times operator might swap a and b (commutative) */
    if (cache_get3(CACHE_MTBDD_AND_ABSTRACT_PLUS, a, b, v, &computed)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_PLUS_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Now, v is not a constant, and either a or b is not a constant */

    /* Get top variable */
    int la = mtbdd_is_leaf(a);
    int lb = mtbdd_is_leaf(b);
    mtbddnode* na = la ? 0 : MTBDD_GETNODE(a);
    mtbddnode* nb = lb ? 0 : MTBDD_GETNODE(b);
    uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
    uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
    uint32_t var = va < vb ? va : vb;

    mtbddnode* nv = MTBDD_GETNODE(v);
    uint32_t vv = mtbddnode_getvariable(nv);

    if (vv < var) {
        /* Recursive, then abstract result */
        status = gmp_and_abstract_plus_CALL(lace, &computed, a, b, node_gethigh(v, nv));
        if (status == SYLVAN_OK) status = mtbdd_apply_CALL(lace, &computed, computed, computed, gmp_op_plus_CALL);
    } else {
        /* Get cofactors */
        MTBDD alow, ahigh, blow, bhigh;
        alow  = (!la && va == var) ? node_getlow(a, na)  : a;
        ahigh = (!la && va == var) ? node_gethigh(a, na) : a;
        blow  = (!lb && vb == var) ? node_getlow(b, nb)  : b;
        bhigh = (!lb && vb == var) ? node_gethigh(b, nb) : b;

        if (vv == var) {
            /* Recursive, then abstract result */
            MTBDD low = mtbdd_invalid;
            MTBDD high = mtbdd_invalid;
            mtbdd_refs_pushptr(&low);
            mtbdd_refs_pushptr(&high);
            MTBDD next_v = node_gethigh(v, nv);
            gmp_and_abstract_plus_SPAWN(lace, &high, ahigh, bhigh, next_v);
            status = gmp_and_abstract_plus_CALL(lace, &low, alow, blow, next_v);
            int high_status = gmp_and_abstract_plus_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
            if (status == SYLVAN_OK) status = mtbdd_apply_CALL(lace, &computed, low, high, gmp_op_plus_CALL);
            mtbdd_refs_popptr(2);
        } else /* vv > v */ {
            /* Recursive, then create node */
            MTBDD low = mtbdd_invalid;
            MTBDD high = mtbdd_invalid;
            mtbdd_refs_pushptr(&low);
            mtbdd_refs_pushptr(&high);
            gmp_and_abstract_plus_SPAWN(lace, &high, ahigh, bhigh, v);
            status = gmp_and_abstract_plus_CALL(lace, &low, alow, blow, v);
            int high_status = gmp_and_abstract_plus_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
            if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, var, low, high);
            mtbdd_refs_popptr(2);
        }
    }

    /* Store in cache */
    if (status == SYLVAN_OK && cache_put3(CACHE_MTBDD_AND_ABSTRACT_PLUS, a, b, v, computed)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_PLUS_CACHEDPUT);
    }

    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * Multiply <a> and <b>, and abstract variables <vars> by taking the maximum.
 */
int gmp_and_abstract_max_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, MTBDD v)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || v == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check terminal cases */

    /* If v == true, then <vars> is an empty set */
    if (v == bdd_true) return mtbdd_apply_CALL(lace, destination, a, b, gmp_op_times_CALL);

    /* Try the times operator on a and b */
    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = gmp_op_times_CALL(lace, &computed, &a, &b);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) status = SYLVAN_ERR_CALLBACK;
        else status = mtbdd_abstract_CALL(lace, &computed, computed, v, gmp_abstract_op_max_CALL);
        /* Note that the operation cache is used in mtbdd_abstract */
        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(1);
        return status;
    }
    if (status != SYLVAN_APPLY_RECURSE) {
        mtbdd_refs_popptr(1);
        return status < 0 ? status : SYLVAN_ERR_CALLBACK;
    }
    if (mtbdd_is_leaf(a) && mtbdd_is_leaf(b)) {
        mtbdd_refs_popptr(1);
        return SYLVAN_ERR_CALLBACK;
    }

    /* Now, v is not a constant, and either a or b is not a constant */

    /* Get top variable */
    int la = mtbdd_is_leaf(a);
    int lb = mtbdd_is_leaf(b);
    mtbddnode* na = la ? 0 : MTBDD_GETNODE(a);
    mtbddnode* nb = lb ? 0 : MTBDD_GETNODE(b);
    uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
    uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
    uint32_t var = va < vb ? va : vb;

    mtbddnode* nv = MTBDD_GETNODE(v);
    uint32_t vv = mtbddnode_getvariable(nv);

    while (vv < var) {
        /* we can skip variables, because max(r,r) = r */
        v = node_high(v, nv);
        if (v == bdd_true) {
            status = mtbdd_apply_CALL(lace, &computed, a, b, gmp_op_times_CALL);
            if (status == SYLVAN_OK) *destination = computed;
            mtbdd_refs_popptr(1);
            return status;
        }
        nv = MTBDD_GETNODE(v);
        vv = mtbddnode_getvariable(nv);
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_AND_ABSTRACT_MAX);

    /* Check cache. Note that we do this now, since the times operator might swap a and b (commutative) */
    if (cache_get3(CACHE_MTBDD_AND_ABSTRACT_MAX, a, b, v, &computed)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_MAX_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Get cofactors */
    MTBDD alow, ahigh, blow, bhigh;
    alow  = (!la && va == var) ? node_getlow(a, na)  : a;
    ahigh = (!la && va == var) ? node_gethigh(a, na) : a;
    blow  = (!lb && vb == var) ? node_getlow(b, nb)  : b;
    bhigh = (!lb && vb == var) ? node_gethigh(b, nb) : b;

    if (vv == var) {
        /* Recursive, then abstract result */
        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        MTBDD next_v = node_gethigh(v, nv);
        gmp_and_abstract_max_SPAWN(lace, &high, ahigh, bhigh, next_v);
        status = gmp_and_abstract_max_CALL(lace, &low, alow, blow, next_v);
        int high_status = gmp_and_abstract_max_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status == SYLVAN_OK) status = mtbdd_apply_CALL(lace, &computed, low, high, gmp_op_max_CALL);
        mtbdd_refs_popptr(2);
    } else /* vv > v */ {
        /* Recursive, then create node */
        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        gmp_and_abstract_max_SPAWN(lace, &high, ahigh, bhigh, v);
        status = gmp_and_abstract_max_CALL(lace, &low, alow, blow, v);
        int high_status = gmp_and_abstract_max_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, var, low, high);
        mtbdd_refs_popptr(2);
    }

    /* Store in cache */
    if (status == SYLVAN_OK && cache_put3(CACHE_MTBDD_AND_ABSTRACT_MAX, a, b, v, computed)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_MAX_CACHEDPUT);
    }

    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}
