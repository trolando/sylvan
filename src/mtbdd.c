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

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "refs.h"
#include "sl.h"
#include "sha2.h"

static_assert(sizeof(size_t) >= sizeof(double), "MTBDD double parameters require 64-bit size_t");

static inline size_t
mtbdd_double_parameter(double value)
{
    size_t parameter = 0;
    memcpy(&parameter, &value, sizeof(value));
    return parameter;
}

static inline double
mtbdd_parameter_double(size_t parameter)
{
    double value;
    memcpy(&value, &parameter, sizeof(value));
    return value;
}

/* Primitives */
int
mtbdd_is_leaf(MTBDD bdd)
{
    if (bdd == bdd_true || bdd == mtbdd_undefined) return 1;
    return mtbddnode_isleaf(MTBDD_GETNODE(bdd));
}

int
mtbdd_is_nan(MTBDD leaf)
{
    return leaf != mtbdd_invalid && leaf != mtbdd_undefined && leaf != bdd_true &&
           mtbdd_is_leaf(leaf) && mtbddnode_isnan(MTBDD_GETNODE(leaf));
}

// for nodes
uint32_t
mtbdd_node_variable(MTBDD node)
{
    return mtbddnode_getvariable(MTBDD_GETNODE(node));
}

MTBDD
mtbdd_node_low(MTBDD mtbdd)
{
    return node_getlow(mtbdd, MTBDD_GETNODE(mtbdd));
}

MTBDD
mtbdd_node_high(MTBDD mtbdd)
{
    return node_gethigh(mtbdd, MTBDD_GETNODE(mtbdd));
}

void
mtbdd_cofactors(MTBDD dd, MTBDD *if_false, MTBDD *if_true)
{
    if (mtbdd_is_leaf(dd)) {
        *if_false = dd;
        *if_true = dd;
    } else {
        *if_false = mtbdd_node_low(dd);
        *if_true = mtbdd_node_high(dd);
    }
}

int
_mtbdd_eval(MTBDD *destination, MTBDD dd, BDDSET variables, const uint8_t *values, size_t count)
{
    if (destination == NULL || dd == mtbdd_invalid || variables == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }
    if (count != bdd_set_count(variables) || (count != 0 && values == NULL)) {
        return SYLVAN_ERR_INVALID;
    }

    for (size_t i = 0; i < count; i++) {
        if (values[i] > 1) return SYLVAN_ERR_INVALID;
    }

    size_t index = 0;
    while (!mtbdd_is_leaf(dd)) {
        uint32_t dd_level = mtbdd_node_variable(dd);

        while (!bdd_set_is_empty(variables) && bdd_set_first(variables) < dd_level) {
            variables = bdd_set_next(variables);
            index++;
        }

        if (bdd_set_is_empty(variables) || bdd_set_first(variables) != dd_level) {
            return SYLVAN_ERR_INVALID;
        }

        dd = values[index] ? mtbdd_node_high(dd) : mtbdd_node_low(dd);
        variables = bdd_set_next(variables);
        index++;
    }

    *destination = dd;
    return SYLVAN_OK;
}

int
mtbdd_eval(MTBDD *destination, MTBDD dd, BDDSET variables, const uint8_t *values, size_t count)
{
    int status = _mtbdd_eval(destination, dd, variables, values, count);
    if (status == SYLVAN_OK) sylvan_stats_count(MTBDD_EVAL);
    return status;
}

// for leaves
uint32_t
mtbdd_leaf_type(MTBDD leaf)
{
    return mtbddnode_gettype(MTBDD_GETNODE(leaf));
}

uint64_t
mtbdd_leaf_value(MTBDD leaf)
{
    return mtbddnode_getvalue(MTBDD_GETNODE(leaf));
}

// for leaf type 0 (integer)
int64_t
mtbdd_leaf_int64(MTBDD leaf)
{
    uint64_t value = mtbdd_leaf_value(leaf);
    return *(int64_t*)&value;
}

// for leaf type 1 (double)
double
mtbdd_leaf_double(MTBDD leaf)
{
    if (mtbdd_is_nan(leaf)) return NAN;
    uint64_t value = mtbdd_leaf_value(leaf);
    return *(double*)&value;
}

/**
 * Implementation of garbage collection
 */

/* Recursively mark LISTDD nodes as 'in use' */
void mtbdd_gc_mark_CALL(lace_worker* lace, LISTDD mtbdd)
{
    if (mtbdd == bdd_true) return;
    if (mtbdd == mtbdd_undefined) return;
    if (mtbdd == mtbdd_invalid) return;

    nodes_mark_rec_CALL(lace, nodes, MTBDD_STRIPMARK(mtbdd));
}

/**
 * External references
 */

refs_table_t mtbdd_refs;
refs_table_t mtbdd_protected;
static int mtbdd_protected_created = 0;

LISTDD
mtbdd_ref(LISTDD a)
{
    if (a == bdd_true || a == mtbdd_undefined) return a;
    refs_up(&mtbdd_refs, MTBDD_STRIPMARK(a));
    return a;
}

void
mtbdd_deref(LISTDD a)
{
    if (a == bdd_true || a == mtbdd_undefined) return;
    refs_down(&mtbdd_refs, MTBDD_STRIPMARK(a));
}

size_t
mtbdd_ref_count(void)
{
    return refs_count(&mtbdd_refs);
}

void
mtbdd_protect(MTBDD *a)
{
    if (!mtbdd_protected_created) {
        // In C++, sometimes mtbdd_protect is called before Sylvan is initialized. Just create a table.
        protect_create(&mtbdd_protected, 4096);
        mtbdd_protected_created = 1;
    }
    protect_up(&mtbdd_protected, (size_t)a);
}

void
mtbdd_unprotect(MTBDD *a)
{
    if (mtbdd_protected.refs_table != NULL) protect_down(&mtbdd_protected, (size_t)a);
}

size_t
mtbdd_protected_count(void)
{
    return protect_count(&mtbdd_protected);
}

/* Called during garbage collection */
TASK(void, mtbdd_gc_mark_external_refs)

void mtbdd_gc_mark_external_refs_CALL(lace_worker* lace)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = refs_iter(&mtbdd_refs, 0, mtbdd_refs.refs_size);
    while (it != NULL) {
        mtbdd_gc_mark_SPAWN(lace, refs_next(&mtbdd_refs, &it, mtbdd_refs.refs_size));
        count++;
    }
    while (count--) {
        mtbdd_gc_mark_SYNC(lace);
    }
}

TASK(void, mtbdd_gc_mark_protected)

void mtbdd_gc_mark_protected_CALL(lace_worker* lace)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = protect_iter(&mtbdd_protected, 0, mtbdd_protected.refs_size);
    while (it != NULL) {
        BDD *to_mark = (BDD*)protect_next(&mtbdd_protected, &it, mtbdd_protected.refs_size);
        mtbdd_gc_mark_SPAWN(lace, *to_mark);
        count++;
    }
    while (count--) {
        mtbdd_gc_mark_SYNC(lace);
    }
}

/* Infrastructure for internal markings */
typedef struct mtbdd_refs_internal
{
    const MTBDD **pbegin, **pend, **pcur;
    MTBDD *rbegin, *rend, *rcur;
} *mtbdd_refs_internal_t;

SYLVAN_TLS mtbdd_refs_internal_t mtbdd_refs_key;

TASK(void, mtbdd_refs_mark_p_par, const MTBDD**, begin, size_t, count)

void mtbdd_refs_mark_p_par_CALL(lace_worker* lace, const MTBDD** begin, size_t count)
{
    if (count < 32) {
        while (count) {
            mtbdd_gc_mark(**(begin++));
            count--;
        }
    } else {
        mtbdd_refs_mark_p_par_SPAWN(lace, begin, count / 2);
        mtbdd_refs_mark_p_par_CALL(lace, begin + (count / 2), count - count / 2);
        mtbdd_refs_mark_p_par_SYNC(lace);
    }
}

TASK(void, mtbdd_refs_mark_r_par, MTBDD*, begin, size_t, count)

void mtbdd_refs_mark_r_par_CALL(lace_worker* lace, MTBDD* begin, size_t count)
{
    if (count < 32) {
        while (count) {
            mtbdd_gc_mark(*begin++);
            count--;
        }
    } else {
        mtbdd_refs_mark_r_par_SPAWN(lace, begin, count / 2);
        mtbdd_refs_mark_r_par_CALL(lace, begin + (count / 2), count - count / 2);
        mtbdd_refs_mark_r_par_SYNC(lace);
    }
}

TASK(void, mtbdd_refs_mark_task)

void mtbdd_refs_mark_task_CALL(lace_worker* lace)
{
    mtbdd_refs_mark_p_par_SPAWN(lace, mtbdd_refs_key->pbegin, (size_t)(mtbdd_refs_key->pcur-mtbdd_refs_key->pbegin));
    mtbdd_refs_mark_r_par_CALL(lace, mtbdd_refs_key->rbegin, (size_t)(mtbdd_refs_key->rcur-mtbdd_refs_key->rbegin));
    mtbdd_refs_mark_p_par_SYNC(lace);
}

TASK(void, mtbdd_refs_mark)

void mtbdd_refs_mark_CALL(lace_worker* lace)
{
    (void)lace;
    mtbdd_refs_mark_task_TOGETHER();
}

void
mtbdd_refs_init_key(void)
{
    assert(lace_is_worker()); // only use inside Lace workers
    mtbdd_refs_internal_t s = (mtbdd_refs_internal_t)malloc(sizeof(struct mtbdd_refs_internal));
    s->pcur = s->pbegin = (const MTBDD**)malloc(sizeof(MTBDD*) * 1024);
    s->pend = s->pbegin + 1024;
    s->rcur = s->rbegin = (MTBDD*)malloc(sizeof(MTBDD) * 1024);
    s->rend = s->rbegin + 1024;
    mtbdd_refs_key = s;
}

TASK(void, mtbdd_refs_free)

void mtbdd_refs_free_CALL(lace_worker* lace)
{
    (void)lace;
    free(mtbdd_refs_key->pbegin);
    free(mtbdd_refs_key->rbegin);
    free(mtbdd_refs_key);
}

TASK(void, mtbdd_refs_init_task)

void mtbdd_refs_init_task_CALL(lace_worker* lace)
{
    (void)lace;
    mtbdd_refs_init_key();
}

TASK(void, mtbdd_refs_init)

void mtbdd_refs_init_CALL(lace_worker* lace)
{
    (void)lace;
    mtbdd_refs_init_task_TOGETHER();
    sylvan_gc_add_mark(mtbdd_refs_mark_CALL);
}

void
mtbdd_refs_ptrs_up(mtbdd_refs_internal_t refs)
{
    size_t cur = (size_t)(refs->pcur - refs->pbegin);
    size_t size = (size_t)(refs->pend - refs->pbegin);
    refs->pbegin = (const MTBDD**)realloc(refs->pbegin, sizeof(MTBDD*) * size * 2);
    refs->pcur = refs->pbegin + cur;
    refs->pend = refs->pbegin + (size * 2);
}

MTBDD SYLVAN_NOINLINE
mtbdd_refs_refs_up(mtbdd_refs_internal_t refs, MTBDD res)
{
    size_t size = (size_t)(refs->rend - refs->rbegin);
    refs->rbegin = (MTBDD*)realloc(refs->rbegin, sizeof(MTBDD) * size * 2);
    refs->rcur = refs->rbegin + size;
    refs->rend = refs->rbegin + (size * 2);
    return res;
}

void
mtbdd_refs_pushptr(const MTBDD *ptr)
{
    // If you get a segfault here (null dereference) then you're running this from outside Lace threads
    *mtbdd_refs_key->pcur++ = ptr;
    if (mtbdd_refs_key->pcur == mtbdd_refs_key->pend) mtbdd_refs_ptrs_up(mtbdd_refs_key);
}

void
mtbdd_refs_popptr(size_t amount)
{
    mtbdd_refs_key->pcur -= amount;
}

MTBDD
mtbdd_refs_push(MTBDD mtbdd)
{
    // If you get a segfault here (null dereference) then you're running this from outside Lace threads
    *(mtbdd_refs_key->rcur++) = mtbdd;
    if (mtbdd_refs_key->rcur == mtbdd_refs_key->rend) return mtbdd_refs_refs_up(mtbdd_refs_key, mtbdd);
    else return mtbdd;
}

void
mtbdd_refs_pop(long amount)
{
    mtbdd_refs_key->rcur -= amount;
}

/**
 * Initialize and quit functions
 */

static int mtbdd_initialized = 0;
static _Atomic(uint32_t) bdd_next_level;
static int mtbdd_abs_leaf(
    lace_worker*, MTBDD*, MTBDD, void*);
static int mtbdd_floor_leaf(
    lace_worker*, MTBDD*, MTBDD, void*);
static int mtbdd_ceil_leaf(
    lace_worker*, MTBDD*, MTBDD, void*);
static int mtbdd_log_leaf(
    lace_worker*, MTBDD*, MTBDD, void*);
static mtbdd_map_op mtbdd_abs_operation;
static mtbdd_map_op mtbdd_floor_operation;
static mtbdd_map_op mtbdd_ceil_operation;
static mtbdd_map_op mtbdd_log_operation;

static void
mtbdd_quit(void)
{
    mtbdd_refs_free_TOGETHER();
    refs_free(&mtbdd_refs);
    if (mtbdd_protected_created) {
        protect_free(&mtbdd_protected);
        mtbdd_protected_created = 0;
    }

    mtbdd_initialized = 0;
}

void
mtbdd_init(void)
{
    sylvan_init_mt();

    if (mtbdd_initialized) return;
    mtbdd_initialized = 1;

    sylvan_register_quit(mtbdd_quit);
    sylvan_gc_add_mark(mtbdd_gc_mark_external_refs_CALL);
    sylvan_gc_add_mark(mtbdd_gc_mark_protected_CALL);

    refs_create(&mtbdd_refs, 1024);
    if (!mtbdd_protected_created) {
        protect_create(&mtbdd_protected, 4096);
        mtbdd_protected_created = 1;
    }

    if (mtbdd_abs_operation.cache_id == 0) {
        mtbdd_abs_operation.map = mtbdd_abs_leaf;
        mtbdd_abs_operation.cache_id = cache_next_opid();
        mtbdd_floor_operation.map = mtbdd_floor_leaf;
        mtbdd_floor_operation.cache_id = cache_next_opid();
        mtbdd_ceil_operation.map = mtbdd_ceil_leaf;
        mtbdd_ceil_operation.cache_id = cache_next_opid();
        mtbdd_log_operation.map = mtbdd_log_leaf;
        mtbdd_log_operation.cache_id = cache_next_opid();
    }

    mtbdd_refs_init();
}

/**
 * Primitives
 */
MTBDD
mtbdd_leaf(uint32_t type, uint64_t value)
{
    struct mtbddnode n;
    mtbddnode_makeleaf(&n, type, value);

    int custom = sylvan_mt_has_custom_hash(type);

    int created = 0;
    uint64_t index = custom ? nodes_lookupc(nodes, n.a, n.b, &created) : nodes_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        if (custom && created < 0) return mtbdd_invalid;
        sylvan_gc(); // FIXME ?

        index = custom ? nodes_lookupc(nodes, n.a, n.b, &created) : nodes_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            if (custom && created < 0) return mtbdd_invalid;
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", nodes_count_nodes(nodes), nodes_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return (MTBDD)index;
}

MTBDD
mtbdd_nan(uint32_t type)
{
    struct mtbddnode n;
    mtbddnode_makenan(&n, type);

    int created;
    uint64_t index = nodes_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        sylvan_gc();
        index = nodes_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n",
                    nodes_count_nodes(nodes), nodes_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);
    return (MTBDD)index;
}

void SYLVAN_NOINLINE
_mtbdd_makenode_gc(MTBDD low, MTBDD high)
{
    mtbdd_refs_push(low);
    mtbdd_refs_push(high);
    sylvan_gc();
    mtbdd_refs_pop(2);
}

void SYLVAN_NOINLINE
_mtbdd_makenode_exit(void)
{
    fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", nodes_count_nodes(nodes), nodes_get_size(nodes));
    exit(1);
}

MTBDD
_mtbdd_make_node(uint32_t var, MTBDD low, MTBDD high)
{
    MTBDD result;
    if (_mtbdd_try_make_node(&result, var, low, high) != SYLVAN_OK) {
        _mtbdd_makenode_exit();
    }
    return result;
}

int
_mtbdd_try_make_node(MTBDD *destination, uint32_t var, MTBDD low, MTBDD high)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;
    if (low == high) {
        *destination = low;
        return SYLVAN_OK;
    }

    // Normalization to keep canonicity
    // low will have no mark

    MTBDD result = low & bdd_complement;
    low ^= result;
    high ^= result;

    struct mtbddnode n;
    mtbddnode_makenode(&n, var, low, high);

    int created;
    uint64_t index = nodes_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        _mtbdd_makenode_gc(low, high);
        index = nodes_lookup(nodes, n.a, n.b, &created);
        if (index == 0) return SYLVAN_ERR_OOM;
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    result |= index;
    *destination = result;
    return SYLVAN_OK;
}

static int
_mtbdd_try_make_map_node(MTBDDMAP *destination, uint32_t key, MTBDDMAP next, MTBDD value)
{
    if (destination == NULL || key > UINT32_C(0x00ffffff) ||
        next == mtbdd_invalid || value == mtbdd_invalid || MTBDD_HASMARK(next)) {
        return SYLVAN_ERR_INVALID;
    }

    struct mtbddnode n;
    uint64_t index;
    int created;

    // in an MTBDDMAP, the low edges eventually lead to 0 and cannot have a low mark
    mtbddnode_makemapnode(&n, key, next, value);
    index = nodes_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        _mtbdd_makenode_gc(next, value);
        index = nodes_lookup(nodes, n.a, n.b, &created);
        if (index == 0) return SYLVAN_ERR_OOM;
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    *destination = index;
    return SYLVAN_OK;
}

int
bdd_var_at_level(BDD *destination, uint32_t level)
{
    if (destination == NULL || level > UINT32_C(0x00ffffff)) return SYLVAN_ERR_INVALID;

    uint32_t next = atomic_load_explicit(&bdd_next_level, memory_order_relaxed);
    while (next <= level && !atomic_compare_exchange_weak_explicit(
        &bdd_next_level, &next, level + 1,
        memory_order_relaxed, memory_order_relaxed)) {
    }

    return _mtbdd_try_make_node(destination, level, bdd_false, bdd_true);
}

int
mtbdd_leaf_fraction(MTBDD leaf, int32_t *numerator, uint32_t *denominator)
{
    if (numerator == NULL || denominator == NULL || !mtbdd_is_leaf(leaf) ||
        leaf == mtbdd_undefined || leaf == bdd_true || mtbdd_is_nan(leaf) ||
        mtbdd_leaf_type(leaf) != 2) {
        return -1;
    }
    *numerator = mtbdd_fraction_numerator(leaf);
    *denominator = mtbdd_fraction_denominator(leaf);
    return 0;
}

int
bdd_new_var(BDD *destination)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;

    uint32_t level = atomic_load_explicit(&bdd_next_level, memory_order_relaxed);
    for (;;) {
        if (level > UINT32_C(0x00ffffff)) return SYLVAN_ERR_INVALID;
        if (atomic_compare_exchange_weak_explicit(
            &bdd_next_level, &level, level + 1,
            memory_order_relaxed, memory_order_relaxed)) {
            break;
        }
    }

    return _mtbdd_try_make_node(destination, level, bdd_false, bdd_true);
}

/* Operations */

/**
 * Calculate greatest common divisor
 * Source: http://lemire.me/blog/archives/2013/12/26/fastest-way-to-compute-the-greatest-common-divisor/
 */
uint32_t
gcd(uint32_t u, uint32_t v)
{
    unsigned int shift;
    if (u == 0) return v;
    if (v == 0) return u;
    shift = ctz_uint32(u | v);
    u >>= ctz_uint32(u);
    do {
        v >>= ctz_uint32(v);
        if (u > v) {
            unsigned int t = v;
            v = u;
            u = t;
        }
        v = v - u;
    } while (v != 0);
    return u << shift;
}

static uint64_t
gcd64(uint64_t u, uint64_t v)
{
    while (v != 0) {
        const uint64_t remainder = u % v;
        u = v;
        v = remainder;
    }
    return u;
}

/**
 * Create leaves of unsigned/signed integers and doubles
 */

MTBDD
mtbdd_int64(int64_t value)
{
    return mtbdd_leaf(0, *(uint64_t*)&value);
}

MTBDD
mtbdd_double(double value)
{
    if (isnan(value)) return mtbdd_nan(1);
    // normalize all 0.0 to 0.0
    if (value == 0.0) value = 0.0;
    return mtbdd_leaf(1, *(uint64_t*)&value);
}

MTBDD
mtbdd_fraction(int64_t nom, uint64_t denom)
{
    if (denom == 0) {
        fprintf(stderr, "mtbdd_fraction: denominator must not be zero\n");
        return mtbdd_invalid;
    }

    if (nom == 0) return mtbdd_leaf(2, 1);

    const int negative = nom < 0;
    uint64_t magnitude = negative ? (uint64_t)(-(nom + 1)) + 1 : (uint64_t)nom;
    const uint64_t c = gcd64(magnitude, denom);
    magnitude /= c;
    denom /= c;

    if (magnitude > INT32_MAX || denom > UINT32_MAX) {
        fprintf(stderr, "mtbdd_fraction: reduced fraction does not fit in a terminal\n");
        return mtbdd_invalid;
    }

    const int32_t numerator = negative ? -(int32_t)magnitude : (int32_t)magnitude;
    const uint64_t value = ((uint64_t)(uint32_t)numerator << 32) | denom;
    return mtbdd_leaf(2, value);
}

static int
int64_add_checked(int64_t a, int64_t b, int64_t *result)
{
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) return 0;
    *result = a + b;
    return 1;
}

static int
int64_sub_checked(int64_t a, int64_t b, int64_t *result)
{
    if ((b < 0 && a > INT64_MAX + b) || (b > 0 && a < INT64_MIN + b)) return 0;
    *result = a - b;
    return 1;
}

static uint64_t
int64_magnitude(int64_t value)
{
    return value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
}

static int
int64_mul_checked(int64_t a, int64_t b, int64_t *result)
{
    const int negative = (a < 0) != (b < 0);
    const uint64_t magnitude_a = int64_magnitude(a);
    const uint64_t magnitude_b = int64_magnitude(b);
    const uint64_t limit = negative ? (UINT64_C(1) << 63) : INT64_MAX;

    if (magnitude_a != 0 && magnitude_b > limit / magnitude_a) return 0;
    const uint64_t magnitude = magnitude_a * magnitude_b;
    if (negative) {
        *result = magnitude == (UINT64_C(1) << 63)
            ? INT64_MIN : -(int64_t)magnitude;
    } else {
        *result = (int64_t)magnitude;
    }
    return 1;
}

static int
uint64_mul_checked(uint64_t a, uint64_t b, uint64_t *result)
{
    if (a != 0 && b > UINT64_MAX / a) return 0;
    *result = a * b;
    return 1;
}

static int
int64_pow_checked(int64_t base, size_t exponent, int64_t *result)
{
    int64_t value = 1;
    while (exponent != 0) {
        if (exponent & 1) {
            if (!int64_mul_checked(value, base, &value)) return 0;
        }
        exponent >>= 1;
        if (exponent != 0 && !int64_mul_checked(base, base, &base)) return 0;
    }
    *result = value;
    return 1;
}

static int
uint64_pow_checked(uint64_t base, size_t exponent, uint64_t *result)
{
    uint64_t value = 1;
    while (exponent != 0) {
        if (exponent & 1) {
            if (!uint64_mul_checked(value, base, &value)) return 0;
        }
        exponent >>= 1;
        if (exponent != 0 && !uint64_mul_checked(base, base, &base)) return 0;
    }
    *result = value;
    return 1;
}

static MTBDD
mtbdd_fraction_magnitude_result(int negative, uint64_t magnitude, uint64_t denominator)
{
    if (denominator == 0) return mtbdd_nan(2);
    if (magnitude == 0) return mtbdd_fraction(0, 1);

    const uint64_t divisor = gcd64(magnitude, denominator);
    magnitude /= divisor;
    denominator /= divisor;
    if (magnitude > INT32_MAX || denominator > UINT32_MAX) return mtbdd_nan(2);

    const int32_t reduced_numerator = negative ? -(int32_t)magnitude : (int32_t)magnitude;
    const uint64_t value = ((uint64_t)(uint32_t)reduced_numerator << 32) | denominator;
    return mtbdd_leaf(2, value);
}

static MTBDD
mtbdd_fraction_result(int64_t numerator, uint64_t denominator)
{
    return mtbdd_fraction_magnitude_result(
        numerator < 0, int64_magnitude(numerator), denominator);
}

static MTBDD
mtbdd_fraction_add_result(MTBDD a, MTBDD b, int subtract)
{
    const int64_t numerator_a = mtbdd_fraction_numerator(a);
    const int64_t numerator_b = mtbdd_fraction_numerator(b);
    const uint64_t denominator_a = mtbdd_fraction_denominator(a);
    const uint64_t denominator_b = mtbdd_fraction_denominator(b);
    const uint64_t divisor = gcd((uint32_t)denominator_a, (uint32_t)denominator_b);
    int64_t scaled_a, scaled_b;
    uint64_t denominator;

    if (!int64_mul_checked(numerator_a, (int64_t)(denominator_b / divisor), &scaled_a) ||
        !int64_mul_checked(numerator_b, (int64_t)(denominator_a / divisor), &scaled_b) ||
        !uint64_mul_checked(denominator_a, denominator_b / divisor, &denominator)) {
        return mtbdd_nan(2);
    }

    const int negative_a = scaled_a < 0;
    const int negative_b = (scaled_b < 0) != subtract;
    const uint64_t magnitude_a = int64_magnitude(scaled_a);
    const uint64_t magnitude_b = int64_magnitude(scaled_b);
    int negative;
    uint64_t magnitude;
    if (negative_a == negative_b) {
        negative = negative_a;
        if (UINT64_MAX - magnitude_a < magnitude_b) return mtbdd_nan(2);
        magnitude = magnitude_a + magnitude_b;
    } else if (magnitude_a >= magnitude_b) {
        negative = negative_a;
        magnitude = magnitude_a - magnitude_b;
    } else {
        negative = negative_b;
        magnitude = magnitude_b - magnitude_a;
    }
    return mtbdd_fraction_magnitude_result(negative, magnitude, denominator);
}

static MTBDD
mtbdd_fraction_mul_result(MTBDD a, MTBDD b)
{
    int64_t numerator_a = mtbdd_fraction_numerator(a);
    int64_t numerator_b = mtbdd_fraction_numerator(b);
    uint64_t denominator_a = mtbdd_fraction_denominator(a);
    uint64_t denominator_b = mtbdd_fraction_denominator(b);
    const uint64_t divisor_a = gcd64(int64_magnitude(numerator_a), denominator_b);
    const uint64_t divisor_b = gcd64(int64_magnitude(numerator_b), denominator_a);
    int64_t numerator;
    uint64_t denominator;

    numerator_a /= (int64_t)divisor_a;
    numerator_b /= (int64_t)divisor_b;
    denominator_a /= divisor_b;
    denominator_b /= divisor_a;
    if (!int64_mul_checked(numerator_a, numerator_b, &numerator) ||
        !uint64_mul_checked(denominator_a, denominator_b, &denominator)) {
        return mtbdd_nan(2);
    }
    return mtbdd_fraction_result(numerator, denominator);
}

static MTBDD
mtbdd_fraction_div_result(MTBDD a, MTBDD b)
{
    int64_t numerator_a = mtbdd_fraction_numerator(a);
    int64_t numerator_b = mtbdd_fraction_numerator(b);
    uint64_t denominator_a = mtbdd_fraction_denominator(a);
    uint64_t denominator_b = mtbdd_fraction_denominator(b);
    if (numerator_b == 0) return mtbdd_nan(2);

    const int negative = (numerator_a < 0) != (numerator_b < 0);
    uint64_t magnitude_a = int64_magnitude(numerator_a);
    uint64_t magnitude_b = int64_magnitude(numerator_b);
    const uint64_t divisor_a = gcd64(magnitude_a, magnitude_b);
    const uint64_t divisor_b = gcd64(denominator_a, denominator_b);
    magnitude_a /= divisor_a;
    magnitude_b /= divisor_a;
    denominator_a /= divisor_b;
    denominator_b /= divisor_b;

    uint64_t numerator_magnitude, denominator;
    if (!uint64_mul_checked(magnitude_a, denominator_b, &numerator_magnitude) ||
        !uint64_mul_checked(denominator_a, magnitude_b, &denominator) ||
        numerator_magnitude > (negative ? (UINT64_C(1) << 63) : INT64_MAX)) {
        return mtbdd_nan(2);
    }
    const int64_t numerator = negative
        ? (numerator_magnitude == (UINT64_C(1) << 63)
            ? INT64_MIN : -(int64_t)numerator_magnitude)
        : (int64_t)numerator_magnitude;
    return mtbdd_fraction_result(numerator, denominator);
}

/**
 * Create a MTBDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * Use cube[idx]==3 for "s=s'" in interleaved variables (matches with next variable)
 * <variables> is the cube of variables
 */
int
mtbdd_cube(MTBDD *destination, BDDSET variables, const uint8_t *cube, MTBDD terminal)
{
    if (destination == NULL || variables == mtbdd_invalid || terminal == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }
    if (variables == bdd_true) {
        *destination = terminal;
        return SYLVAN_OK;
    }
    if (cube == NULL) return SYLVAN_ERR_INVALID;

    mtbddnode* n = MTBDD_GETNODE(variables);
    const BDDSET next = node_gethigh(variables, n);
    const uint32_t variable = mtbddnode_getvariable(n);

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = SYLVAN_OK;
    switch (*cube) {
    case 0:
        status = mtbdd_cube(&computed, next, cube+1, terminal);
        if (status == SYLVAN_OK) {
            status = _mtbdd_try_make_node(&computed, variable, computed, mtbdd_undefined);
        }
        break;
    case 1:
        status = mtbdd_cube(&computed, next, cube+1, terminal);
        if (status == SYLVAN_OK) {
            status = _mtbdd_try_make_node(&computed, variable, mtbdd_undefined, computed);
        }
        break;
    case 2:
        status = mtbdd_cube(&computed, next, cube+1, terminal);
        break;
    case 3:
    {
        BDDSET variables2 = next;
        if (variables2 == bdd_true) {
            status = SYLVAN_ERR_INVALID;
            break;
        }
        mtbddnode* n2 = MTBDD_GETNODE(variables2);
        uint32_t var2 = mtbddnode_getvariable(n2);
        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        status = mtbdd_cube(&computed, node_gethigh(variables2, n2), cube+2, terminal);
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&low, var2, computed, mtbdd_undefined);
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&high, var2, mtbdd_undefined, computed);
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, variable, low, high);
        mtbdd_refs_popptr(2);
        break;
    }
    default:
        status = SYLVAN_ERR_INVALID;
        break;
    }

    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * Same as mtbdd_cube, but also performs "or" with existing MTBDD,
 * effectively adding an item to the set
 */
int mtbdd_set_cube_CALL(lace_worker* lace, MTBDD *destination, MTBDD mtbdd, BDDSET vars, const uint8_t *cube, MTBDD terminal)
{
    if (destination == NULL || mtbdd == mtbdd_invalid || vars == mtbdd_invalid || terminal == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Terminal cases */
    if (mtbdd == terminal) { *destination = terminal; return SYLVAN_OK; }
    if (mtbdd == mtbdd_undefined) return mtbdd_cube(destination, vars, cube, terminal);
    if (vars == bdd_true) { *destination = terminal; return SYLVAN_OK; }
    if (cube == NULL) return SYLVAN_ERR_INVALID;

    sylvan_gc_test(lace);

    mtbddnode* nv = MTBDD_GETNODE(vars);
    uint32_t v = mtbddnode_getvariable(nv);

    const int is_leaf = mtbdd_is_leaf(mtbdd);
    mtbddnode* na = is_leaf ? NULL : MTBDD_GETNODE(mtbdd);
    uint32_t va = is_leaf ? UINT32_MAX : mtbddnode_getvariable(na);

    if (va < v) {
        MTBDD low = node_getlow(mtbdd, na);
        MTBDD high = node_gethigh(mtbdd, na);
        MTBDD new_low = mtbdd_invalid;
        MTBDD new_high = mtbdd_invalid;
        MTBDD computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&new_low);
        mtbdd_refs_pushptr(&new_high);
        mtbdd_refs_pushptr(&computed);
        mtbdd_set_cube_SPAWN(lace, &new_high, high, vars, cube, terminal);
        int status = mtbdd_set_cube_CALL(lace, &new_low, low, vars, cube, terminal);
        int high_status = mtbdd_set_cube_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status == SYLVAN_OK) {
            if (new_low == low && new_high == high) computed = mtbdd;
            else status = _mtbdd_try_make_node(&computed, va, new_low, new_high);
        }
        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(3);
        return status;
    } else if (va == v) {
        MTBDD low = node_getlow(mtbdd, na);
        MTBDD high = node_gethigh(mtbdd, na);
        BDDSET next = node_gethigh(vars, nv);
        switch (*cube) {
        case 0:
        {
            MTBDD new_low = mtbdd_invalid;
            MTBDD computed = mtbdd_invalid;
            mtbdd_refs_pushptr(&new_low);
            mtbdd_refs_pushptr(&computed);
            int status = mtbdd_set_cube_CALL(lace, &new_low, low, next, cube+1, terminal);
            if (status == SYLVAN_OK) {
                if (new_low == low) computed = mtbdd;
                else status = _mtbdd_try_make_node(&computed, v, new_low, high);
            }
            if (status == SYLVAN_OK) *destination = computed;
            mtbdd_refs_popptr(2);
            return status;
        }
        case 1:
        {
            MTBDD new_high = mtbdd_invalid;
            MTBDD computed = mtbdd_invalid;
            mtbdd_refs_pushptr(&new_high);
            mtbdd_refs_pushptr(&computed);
            int status = mtbdd_set_cube_CALL(lace, &new_high, high, next, cube+1, terminal);
            if (status == SYLVAN_OK) {
                if (new_high == high) computed = mtbdd;
                else status = _mtbdd_try_make_node(&computed, v, low, new_high);
            }
            if (status == SYLVAN_OK) *destination = computed;
            mtbdd_refs_popptr(2);
            return status;
        }
        case 2:
        {
            MTBDD new_low = mtbdd_invalid;
            MTBDD new_high = mtbdd_invalid;
            MTBDD computed = mtbdd_invalid;
            mtbdd_refs_pushptr(&new_low);
            mtbdd_refs_pushptr(&new_high);
            mtbdd_refs_pushptr(&computed);
            mtbdd_set_cube_SPAWN(lace, &new_high, high, next, cube+1, terminal);
            int status = mtbdd_set_cube_CALL(lace, &new_low, low, next, cube+1, terminal);
            int high_status = mtbdd_set_cube_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
            if (status == SYLVAN_OK) {
                if (new_low == low && new_high == high) computed = mtbdd;
                else status = _mtbdd_try_make_node(&computed, v, new_low, new_high);
            }
            if (status == SYLVAN_OK) *destination = computed;
            mtbdd_refs_popptr(3);
            return status;
        }
        case 3:
        default:
            return SYLVAN_ERR_INVALID;
        }
    } else /* va > v */ {
        BDDSET next = node_gethigh(vars, nv);
        switch (*cube) {
        case 0:
        {
            MTBDD new_low = mtbdd_invalid;
            MTBDD computed = mtbdd_invalid;
            mtbdd_refs_pushptr(&new_low);
            mtbdd_refs_pushptr(&computed);
            int status = mtbdd_set_cube_CALL(lace, &new_low, mtbdd, next, cube+1, terminal);
            if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, v, new_low, mtbdd);
            if (status == SYLVAN_OK) *destination = computed;
            mtbdd_refs_popptr(2);
            return status;
        }
        case 1:
        {
            MTBDD new_high = mtbdd_invalid;
            MTBDD computed = mtbdd_invalid;
            mtbdd_refs_pushptr(&new_high);
            mtbdd_refs_pushptr(&computed);
            int status = mtbdd_set_cube_CALL(lace, &new_high, mtbdd, next, cube+1, terminal);
            if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, v, mtbdd, new_high);
            if (status == SYLVAN_OK) *destination = computed;
            mtbdd_refs_popptr(2);
            return status;
        }
        case 2:
            return mtbdd_set_cube_CALL(lace, destination, mtbdd, next, cube+1, terminal);
        case 3:
        default:
            return SYLVAN_ERR_INVALID;
        }
    }
}

/**
 * Apply a binary operation <op> to <a> and <b>.
 */
int mtbdd_apply_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, mtbdd_apply_cb op)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || op == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);

    /* Check terminal case */
    int status = op(lace, &computed, &a, &b);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) status = SYLVAN_ERR_CALLBACK;
        else *destination = computed;
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
    sylvan_stats_count(MTBDD_APPLY);

    /* Check cache */
    if (cache_get3(CACHE_MTBDD_APPLY, a, b, (size_t)op, &computed)) {
        sylvan_stats_count(MTBDD_APPLY_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Get top variable */
    int la = mtbdd_is_leaf(a);
    int lb = mtbdd_is_leaf(b);
    assert(!la || !lb);
    mtbddnode* na;
    mtbddnode* nb;
    uint32_t va, vb;
    if (!la) {
        na = MTBDD_GETNODE(a);
        va = mtbddnode_getvariable(na);
    } else {
        na = 0;
        va = 0xffffffff;
    }
    if (!lb) {
        nb = MTBDD_GETNODE(b);
        vb = mtbddnode_getvariable(nb);
    } else {
        nb = 0;
        vb = 0xffffffff;
    }
    uint32_t v = va < vb ? va : vb;

    /* Get cofactors */
    MTBDD alow, ahigh, blow, bhigh;
    if (!la && va == v) {
        alow = node_getlow(a, na);
        ahigh = node_gethigh(a, na);
    } else {
        alow = a;
        ahigh = a;
    }
    if (!lb && vb == v) {
        blow = node_getlow(b, nb);
        bhigh = node_gethigh(b, nb);
    } else {
        blow = b;
        bhigh = b;
    }

    /* Recursive */
    MTBDD low = mtbdd_invalid;
    MTBDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_apply_SPAWN(lace, &high, ahigh, bhigh, op);
    status = mtbdd_apply_CALL(lace, &low, alow, blow, op);
    int high_status = mtbdd_apply_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;
    if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, v, low, high);
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_APPLY, a, b, (size_t)op, computed)) {
        sylvan_stats_count(MTBDD_APPLY_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

/**
 * Apply a binary operation <op> to <a> and <b> with parameter <p>
 */
int mtbdd_apply_param_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, size_t p, mtbdd_apply_param_cb op, uint64_t opid)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || op == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);

    /* Check terminal case */
    int status = op(lace, &computed, &a, &b, p);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) status = SYLVAN_ERR_CALLBACK;
        else *destination = computed;
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
    sylvan_stats_count(MTBDD_APPLY);

    /* Check cache */
    if (cache_get3(opid, a, b, p, &computed)) {
        sylvan_stats_count(MTBDD_APPLY_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Get top variable */
    int la = mtbdd_is_leaf(a);
    int lb = mtbdd_is_leaf(b);
    mtbddnode* na, *nb;
    uint32_t va, vb;
    if (!la) {
        na = MTBDD_GETNODE(a);
        va = mtbddnode_getvariable(na);
    } else {
        na = 0;
        va = 0xffffffff;
    }
    if (!lb) {
        nb = MTBDD_GETNODE(b);
        vb = mtbddnode_getvariable(nb);
    } else {
        nb = 0;
        vb = 0xffffffff;
    }
    uint32_t v = va < vb ? va : vb;

    /* Get cofactors */
    MTBDD alow, ahigh, blow, bhigh;
    if (!la && va == v) {
        alow = node_getlow(a, na);
        ahigh = node_gethigh(a, na);
    } else {
        alow = a;
        ahigh = a;
    }
    if (!lb && vb == v) {
        blow = node_getlow(b, nb);
        bhigh = node_gethigh(b, nb);
    } else {
        blow = b;
        bhigh = b;
    }

    /* Recursive */
    MTBDD low = mtbdd_invalid;
    MTBDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_apply_param_SPAWN(lace, &high, ahigh, bhigh, p, op, opid);
    status = mtbdd_apply_param_CALL(lace, &low, alow, blow, p, op, opid);
    int high_status = mtbdd_apply_param_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;
    if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, v, low, high);
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    /* Store in cache */
    if (cache_put3(opid, a, b, p, computed)) {
        sylvan_stats_count(MTBDD_APPLY_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

/**
 * Apply a unary operation <op> to <dd>.
 */
int mtbdd_apply_unary_CALL(lace_worker* lace, MTBDD *destination, MTBDD dd, mtbdd_apply_unary_cb op, size_t param)
{
    if (destination == NULL || dd == mtbdd_invalid || op == NULL) return SYLVAN_ERR_INVALID;

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_UAPPLY);

    /* Check cache */
    if (cache_get3(CACHE_MTBDD_UAPPLY, dd, (size_t)op, param, &computed)) {
        sylvan_stats_count(MTBDD_UAPPLY_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Check terminal case */
    int status = op(lace, &computed, dd, param);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) {
            mtbdd_refs_popptr(1);
            return SYLVAN_ERR_CALLBACK;
        }
        /* Store in cache */
        if (cache_put3(CACHE_MTBDD_UAPPLY, dd, (size_t)op, param, computed)) {
            sylvan_stats_count(MTBDD_UAPPLY_CACHEDPUT);
        }

        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }
    if (status != SYLVAN_APPLY_RECURSE) {
        mtbdd_refs_popptr(1);
        return status < 0 ? status : SYLVAN_ERR_CALLBACK;
    }
    if (mtbdd_is_leaf(dd)) {
        mtbdd_refs_popptr(1);
        return SYLVAN_ERR_CALLBACK;
    }

    /* Get cofactors */
    mtbddnode* ndd = MTBDD_GETNODE(dd);
    MTBDD ddlow = node_getlow(dd, ndd);
    MTBDD ddhigh = node_gethigh(dd, ndd);

    /* Recursive */
    MTBDD low = mtbdd_invalid;
    MTBDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_apply_unary_SPAWN(lace, &high, ddhigh, op, param);
    status = mtbdd_apply_unary_CALL(lace, &low, ddlow, op, param);
    int high_status = mtbdd_apply_unary_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;
    if (status == SYLVAN_OK) {
        status = _mtbdd_try_make_node(&computed, mtbddnode_getvariable(ndd), low, high);
    }
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_UAPPLY, dd, (size_t)op, param, computed)) {
        sylvan_stats_count(MTBDD_UAPPLY_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

TASK(int, mtbdd_map_rec, MTBDD*, result, MTBDD, dd,
     const mtbdd_map_op*, operation)

int
mtbdd_map_rec_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd,
                   const mtbdd_map_op *operation)
{
    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);

    sylvan_gc_test(lace);
    sylvan_stats_count(MTBDD_MAP);

    if (operation->cache_id != 0 &&
        cache_get3(operation->cache_id, dd, 0, 0, &computed)) {
        sylvan_stats_count(MTBDD_MAP_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    int status;
    if (mtbdd_is_leaf(dd)) {
        status = operation->map(
            lace, &computed, dd, operation->context);
        if (status == SYLVAN_OK) {
            if (computed == mtbdd_invalid || !mtbdd_is_leaf(computed)) {
                status = SYLVAN_ERR_CALLBACK;
            }
        } else if (status > SYLVAN_OK) {
            status = SYLVAN_ERR_CALLBACK;
        }
    } else {
        const mtbddnode *node = MTBDD_GETNODE(dd);
        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);

        mtbdd_map_rec_SPAWN(
            lace, &high, node_gethigh(dd, node), operation);
        status = mtbdd_map_rec_CALL(
            lace, &low, node_getlow(dd, node), operation);
        const int high_status = mtbdd_map_rec_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status == SYLVAN_OK) {
            status = _mtbdd_try_make_node(
                &computed, mtbddnode_getvariable(node), low, high);
        }

        mtbdd_refs_popptr(2);
    }

    if (status == SYLVAN_OK && operation->cache_id != 0) {
        if (cache_put3(operation->cache_id, dd, 0, 0, computed)) {
            sylvan_stats_count(MTBDD_MAP_CACHEDPUT);
        }
    }
    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

int
mtbdd_map_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd,
               const mtbdd_map_op *operation)
{
    if (destination == NULL || dd == mtbdd_invalid || operation == NULL ||
        operation->map == NULL) {
        return SYLVAN_ERR_INVALID;
    }
    if (operation->cache_id != 0 &&
        (operation->cache_id < (UINT64_C(512) << 40) ||
         (operation->cache_id & UINT64_C(0x000000ffffffffff)) != 0)) {
        return SYLVAN_ERR_INVALID;
    }

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    const int status = mtbdd_map_rec_CALL(
        lace, &computed, dd, operation);
    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

TASK(int, mtbdd_map_reduce_rec, MTBDD*, result, MTBDD, dd, BDDSET, variables, const mtbdd_map_reduce_op*, operation)

static int
mtbdd_map_reduce_map_leaf_CALL(lace_worker *lace, MTBDD *destination, MTBDD leaf,
                               const mtbdd_map_reduce_op *operation)
{
    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = operation->map(lace, &computed, leaf, operation->context);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid || !mtbdd_is_leaf(computed)) {
            status = SYLVAN_ERR_CALLBACK;
        } else {
            *destination = computed;
        }
    } else if (status > SYLVAN_OK) {
        status = SYLVAN_ERR_CALLBACK;
    }
    mtbdd_refs_popptr(1);
    return status;
}

static int
mtbdd_map_reduce_reduce_CALL(lace_worker *lace, MTBDD *destination,
                             MTBDD a, MTBDD b, size_t skipped,
                             const mtbdd_map_reduce_op *operation)
{
    if (a == operation->identity) {
        *destination = skipped == 0 ? b : a;
        return SYLVAN_OK;
    }
    if (skipped == 0 && b == operation->identity) {
        *destination = a;
        return SYLVAN_OK;
    }

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = operation->reduce(
        lace, &computed, a, b, skipped, operation->context);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) {
            status = SYLVAN_ERR_CALLBACK;
        } else {
            *destination = computed;
        }
    } else if (status > SYLVAN_OK) {
        status = SYLVAN_ERR_CALLBACK;
    }
    mtbdd_refs_popptr(1);
    return status;
}

int
mtbdd_map_reduce_rec_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd,
                          BDDSET variables,
                          const mtbdd_map_reduce_op *operation)
{
    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);

    sylvan_gc_test(lace);
    sylvan_stats_count(MTBDD_MAP_REDUCE);

    if (operation->cache_id != 0 &&
        cache_get3(operation->cache_id, dd, variables, 0, &computed)) {
        sylvan_stats_count(MTBDD_MAP_REDUCE_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    BDDSET remaining = variables;
    size_t skipped = 0;
    int status = SYLVAN_OK;

    if (mtbdd_is_leaf(dd)) {
        while (!bdd_set_is_empty(remaining)) {
            if (skipped == SIZE_MAX) {
                mtbdd_refs_popptr(1);
                return SYLVAN_ERR_OVERFLOW;
            }
            skipped++;
            remaining = bdd_set_next(remaining);
        }
        status = mtbdd_map_reduce_map_leaf_CALL(
            lace, &computed, dd, operation);
    } else {
        const mtbddnode *node = MTBDD_GETNODE(dd);
        const uint32_t level = mtbddnode_getvariable(node);
        while (!bdd_set_is_empty(remaining) &&
               bdd_set_first(remaining) < level) {
            if (skipped == SIZE_MAX) {
                mtbdd_refs_popptr(1);
                return SYLVAN_ERR_OVERFLOW;
            }
            skipped++;
            remaining = bdd_set_next(remaining);
        }

        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);

        if (bdd_set_is_empty(remaining) ||
            level < bdd_set_first(remaining)) {
            mtbdd_map_reduce_rec_SPAWN(
                lace, &high, node_gethigh(dd, node), remaining, operation);
            status = mtbdd_map_reduce_rec_CALL(
                lace, &low, node_getlow(dd, node), remaining, operation);
            const int high_status = mtbdd_map_reduce_rec_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
            if (status == SYLVAN_OK) {
                status = _mtbdd_try_make_node(
                    &computed, level, low, high);
            }
        } else {
            const BDDSET next = bdd_set_next(remaining);
            mtbdd_map_reduce_rec_SPAWN(
                lace, &high, node_gethigh(dd, node), next, operation);
            status = mtbdd_map_reduce_rec_CALL(
                lace, &low, node_getlow(dd, node), next, operation);
            const int high_status = mtbdd_map_reduce_rec_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
            if (status == SYLVAN_OK) {
                status = mtbdd_map_reduce_reduce_CALL(
                    lace, &computed, low, high, 0, operation);
            }
        }

        mtbdd_refs_popptr(2);
    }

    if (status == SYLVAN_OK && skipped != 0) {
        MTBDD reduced = mtbdd_invalid;
        mtbdd_refs_pushptr(&reduced);
        status = mtbdd_map_reduce_reduce_CALL(
            lace, &reduced, computed, computed, skipped, operation);
        if (status == SYLVAN_OK) computed = reduced;
        mtbdd_refs_popptr(1);
    }

    if (status == SYLVAN_OK && operation->cache_id != 0) {
        if (cache_put3(operation->cache_id, dd, variables, 0, computed)) {
            sylvan_stats_count(MTBDD_MAP_REDUCE_CACHEDPUT);
        }
    }
    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

int
mtbdd_map_reduce_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd,
                      BDDSET variables,
                      const mtbdd_map_reduce_op *operation)
{
    if (destination == NULL || dd == mtbdd_invalid ||
        variables == mtbdd_invalid || operation == NULL ||
        operation->map == NULL || operation->reduce == NULL ||
        operation->identity == mtbdd_invalid ||
        !mtbdd_is_leaf(operation->identity)) {
        return SYLVAN_ERR_INVALID;
    }

    if (operation->cache_id != 0 &&
        (operation->cache_id < (UINT64_C(512) << 40) ||
         (operation->cache_id & UINT64_C(0x000000ffffffffff)) != 0)) {
        return SYLVAN_ERR_INVALID;
    }

    MTBDD computed = mtbdd_invalid;
    MTBDD identity = operation->identity;
    mtbdd_refs_pushptr(&computed);
    mtbdd_refs_pushptr(&identity);
    const int status = mtbdd_map_reduce_rec_CALL(
        lace, &computed, dd, variables, operation);
    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(2);
    return status;
}

TASK(int, mtbdd_combine_reduce_rec, MTBDD*, result, MTBDD, a, MTBDD, b,
     BDDSET, variables, const mtbdd_combine_reduce_op*, operation)

static int
mtbdd_combine_reduce_identity_map(
    lace_worker *lace, MTBDD *destination, MTBDD leaf, void *context)
{
    (void)lace;
    (void)context;
    *destination = leaf;
    return SYLVAN_OK;
}

static int
mtbdd_combine_reduce_reduce_CALL(
    lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b,
    size_t skipped, const mtbdd_combine_reduce_op *operation)
{
    if (a == operation->identity) {
        *destination = skipped == 0 ? b : a;
        return SYLVAN_OK;
    }
    if (skipped == 0 && b == operation->identity) {
        *destination = a;
        return SYLVAN_OK;
    }

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = operation->reduce(
        lace, &computed, a, b, skipped, operation->context);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) {
            status = SYLVAN_ERR_CALLBACK;
        } else {
            *destination = computed;
        }
    } else if (status > SYLVAN_OK) {
        status = SYLVAN_ERR_CALLBACK;
    }
    mtbdd_refs_popptr(1);
    return status;
}

static int
mtbdd_combine_reduce_abstract_CALL(
    lace_worker *lace, MTBDD *destination, MTBDD dd, BDDSET variables,
    const mtbdd_combine_reduce_op *operation)
{
    const mtbdd_map_reduce_op abstraction = {
        mtbdd_combine_reduce_identity_map,
        operation->reduce,
        operation->identity,
        operation->context,
        0
    };
    return mtbdd_map_reduce_CALL(
        lace, destination, dd, variables, &abstraction);
}

int
mtbdd_combine_reduce_rec_CALL(
    lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b,
    BDDSET variables, const mtbdd_combine_reduce_op *operation)
{
    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);

    int status = operation->combine(
        lace, &computed, &a, &b, operation->context);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) {
            mtbdd_refs_popptr(1);
            return SYLVAN_ERR_CALLBACK;
        }
        if (!bdd_set_is_empty(variables)) {
            MTBDD reduced = mtbdd_invalid;
            mtbdd_refs_pushptr(&reduced);
            status = mtbdd_combine_reduce_abstract_CALL(
                lace, &reduced, computed, variables, operation);
            if (status == SYLVAN_OK) computed = reduced;
            mtbdd_refs_popptr(1);
        }
        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(1);
        return status;
    }
    if (status != SYLVAN_APPLY_RECURSE) {
        mtbdd_refs_popptr(1);
        return status < SYLVAN_OK ? status : SYLVAN_ERR_CALLBACK;
    }
    if (mtbdd_is_leaf(a) && mtbdd_is_leaf(b)) {
        mtbdd_refs_popptr(1);
        return SYLVAN_ERR_CALLBACK;
    }

    sylvan_gc_test(lace);
    sylvan_stats_count(MTBDD_COMBINE_REDUCE);

    const BDDSET cache_variables = variables;
    if (operation->cache_id != 0 &&
        cache_get3(operation->cache_id, a, b, cache_variables, &computed)) {
        sylvan_stats_count(MTBDD_COMBINE_REDUCE_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    const int a_is_leaf = mtbdd_is_leaf(a);
    const int b_is_leaf = mtbdd_is_leaf(b);
    const mtbddnode *a_node = a_is_leaf ? NULL : MTBDD_GETNODE(a);
    const mtbddnode *b_node = b_is_leaf ? NULL : MTBDD_GETNODE(b);
    const uint32_t a_level = a_is_leaf
        ? UINT32_MAX : mtbddnode_getvariable(a_node);
    const uint32_t b_level = b_is_leaf
        ? UINT32_MAX : mtbddnode_getvariable(b_node);
    const uint32_t level = a_level < b_level ? a_level : b_level;

    BDDSET remaining = variables;
    size_t skipped = 0;
    while (!bdd_set_is_empty(remaining) &&
           bdd_set_first(remaining) < level) {
        if (skipped == SIZE_MAX) {
            mtbdd_refs_popptr(1);
            return SYLVAN_ERR_OVERFLOW;
        }
        skipped++;
        remaining = bdd_set_next(remaining);
    }

    if (skipped != 0) {
        status = mtbdd_combine_reduce_rec_CALL(
            lace, &computed, a, b, remaining, operation);
        if (status == SYLVAN_OK) {
            MTBDD reduced = mtbdd_invalid;
            mtbdd_refs_pushptr(&reduced);
            status = mtbdd_combine_reduce_reduce_CALL(
                lace, &reduced, computed, computed, skipped, operation);
            if (status == SYLVAN_OK) computed = reduced;
            mtbdd_refs_popptr(1);
        }
    } else {
        const MTBDD a_low =
            a_level == level ? node_getlow(a, a_node) : a;
        const MTBDD a_high =
            a_level == level ? node_gethigh(a, a_node) : a;
        const MTBDD b_low =
            b_level == level ? node_getlow(b, b_node) : b;
        const MTBDD b_high =
            b_level == level ? node_gethigh(b, b_node) : b;
        const int quantify = !bdd_set_is_empty(remaining) &&
                             bdd_set_first(remaining) == level;
        const BDDSET next_variables =
            quantify ? bdd_set_next(remaining) : remaining;
        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        mtbdd_combine_reduce_rec_SPAWN(
            lace, &high, a_high, b_high, next_variables, operation);
        status = mtbdd_combine_reduce_rec_CALL(
            lace, &low, a_low, b_low, next_variables, operation);
        const int high_status = mtbdd_combine_reduce_rec_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status == SYLVAN_OK && quantify) {
            status = mtbdd_combine_reduce_reduce_CALL(
                lace, &computed, low, high, 0, operation);
        } else if (status == SYLVAN_OK) {
            status = _mtbdd_try_make_node(
                &computed, level, low, high);
        }
        mtbdd_refs_popptr(2);
    }

    if (status == SYLVAN_OK && operation->cache_id != 0 &&
        cache_put3(operation->cache_id, a, b, cache_variables, computed)) {
        sylvan_stats_count(MTBDD_COMBINE_REDUCE_CACHEDPUT);
    }
    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

int
mtbdd_combine_reduce_CALL(
    lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b,
    BDDSET variables, const mtbdd_combine_reduce_op *operation)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid ||
        variables == mtbdd_invalid || operation == NULL ||
        operation->combine == NULL || operation->reduce == NULL ||
        operation->identity == mtbdd_invalid ||
        !mtbdd_is_leaf(operation->identity)) {
        return SYLVAN_ERR_INVALID;
    }
    if (operation->cache_id != 0 &&
        (operation->cache_id < (UINT64_C(512) << 40) ||
         (operation->cache_id & UINT64_C(0x000000ffffffffff)) != 0)) {
        return SYLVAN_ERR_INVALID;
    }

    MTBDD computed = mtbdd_invalid;
    MTBDD identity = operation->identity;
    mtbdd_refs_pushptr(&computed);
    mtbdd_refs_pushptr(&identity);
    const int status = mtbdd_combine_reduce_rec_CALL(
        lace, &computed, a, b, variables, operation);
    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(2);
    return status;
}

int
mtbdd_neg_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    return mtbdd_apply_unary_CALL(
        lace, destination, dd, mtbdd_op_negate_CALL, 0);
}

static int
mtbdd_abs_leaf(lace_worker *lace, MTBDD *destination, MTBDD leaf,
               void *context)
{
    (void)lace;
    (void)context;

    if (leaf == mtbdd_undefined) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }
    if (leaf == bdd_true) return SYLVAN_ERR_INVALID;

    const uint32_t type = mtbdd_leaf_type(leaf);
    if (mtbdd_is_nan(leaf)) {
        *destination = mtbdd_nan(type);
        return SYLVAN_OK;
    }

    if (type == 0) {
        const int64_t value = mtbdd_leaf_int64(leaf);
        *destination = value == INT64_MIN
            ? mtbdd_nan(0) : mtbdd_int64(value < 0 ? -value : value);
        return SYLVAN_OK;
    }
    if (type == 1) {
        *destination = mtbdd_double(fabs(mtbdd_leaf_double(leaf)));
        return SYLVAN_OK;
    }
    if (type == 2) {
        const int32_t numerator = mtbdd_fraction_numerator(leaf);
        *destination = numerator < 0
            ? mtbdd_fraction(-(int64_t)numerator,
                             mtbdd_fraction_denominator(leaf))
            : leaf;
        return SYLVAN_OK;
    }
    return SYLVAN_ERR_INVALID;
}

static int
mtbdd_floor_leaf(lace_worker *lace, MTBDD *destination, MTBDD leaf,
                 void *context)
{
    (void)lace;
    (void)context;

    if (leaf == mtbdd_undefined) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }
    if (leaf == bdd_true) return SYLVAN_ERR_INVALID;

    const uint32_t type = mtbdd_leaf_type(leaf);
    if (mtbdd_is_nan(leaf)) {
        *destination = mtbdd_nan(type);
        return SYLVAN_OK;
    }
    if (type == 0) {
        *destination = leaf;
        return SYLVAN_OK;
    }
    if (type == 1) {
        *destination = mtbdd_double(floor(mtbdd_leaf_double(leaf)));
        return SYLVAN_OK;
    }
    if (type == 2) {
        const int64_t numerator = mtbdd_fraction_numerator(leaf);
        const int64_t denominator = mtbdd_fraction_denominator(leaf);
        int64_t integral = numerator / denominator;
        if (numerator % denominator < 0) integral--;
        *destination = mtbdd_fraction(integral, 1);
        return SYLVAN_OK;
    }
    return SYLVAN_ERR_INVALID;
}

static int
mtbdd_ceil_leaf(lace_worker *lace, MTBDD *destination, MTBDD leaf,
                void *context)
{
    (void)lace;
    (void)context;

    if (leaf == mtbdd_undefined) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }
    if (leaf == bdd_true) return SYLVAN_ERR_INVALID;

    const uint32_t type = mtbdd_leaf_type(leaf);
    if (mtbdd_is_nan(leaf)) {
        *destination = mtbdd_nan(type);
        return SYLVAN_OK;
    }
    if (type == 0) {
        *destination = leaf;
        return SYLVAN_OK;
    }
    if (type == 1) {
        *destination = mtbdd_double(ceil(mtbdd_leaf_double(leaf)));
        return SYLVAN_OK;
    }
    if (type == 2) {
        const int64_t numerator = mtbdd_fraction_numerator(leaf);
        const int64_t denominator = mtbdd_fraction_denominator(leaf);
        int64_t integral = numerator / denominator;
        if (numerator % denominator > 0) integral++;
        *destination = mtbdd_fraction(integral, 1);
        return SYLVAN_OK;
    }
    return SYLVAN_ERR_INVALID;
}

static int
mtbdd_log_leaf(lace_worker *lace, MTBDD *destination, MTBDD leaf,
               void *context)
{
    (void)lace;
    (void)context;

    if (leaf == mtbdd_undefined) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }
    if (leaf == bdd_true || mtbdd_leaf_type(leaf) != 1) {
        return SYLVAN_ERR_INVALID;
    }
    if (mtbdd_is_nan(leaf)) {
        *destination = mtbdd_nan(1);
        return SYLVAN_OK;
    }

    const double value = mtbdd_leaf_double(leaf);
    *destination = value < 0.0
        ? mtbdd_nan(1) : mtbdd_double(log(value));
    return SYLVAN_OK;
}

int
mtbdd_abs_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    return mtbdd_map_CALL(lace, destination, dd, &mtbdd_abs_operation);
}

int
mtbdd_floor_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    return mtbdd_map_CALL(lace, destination, dd, &mtbdd_floor_operation);
}

int
mtbdd_ceil_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    return mtbdd_map_CALL(lace, destination, dd, &mtbdd_ceil_operation);
}

int
mtbdd_log_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    return mtbdd_map_CALL(lace, destination, dd, &mtbdd_log_operation);
}

int
mtbdd_zero_indicator_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    return mtbdd_apply_unary_CALL(
        lace, destination, dd, mtbdd_op_cmpl_CALL, 0);
}

int
mtbdd_add_CALL(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_plus_CALL);
}

int
mtbdd_sub_CALL(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_minus_CALL);
}

int
mtbdd_mul_CALL(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_times_CALL);
}

int
mtbdd_div_CALL(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_divide_CALL);
}

int
mtbdd_min_CALL(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_min_CALL);
}

int
mtbdd_max_CALL(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_max_CALL);
}

int
mtbdd_abstract_add_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd,
                        BDDSET variables)
{
    return mtbdd_abstract_CALL(
        lace, destination, dd, variables, mtbdd_abstract_op_plus_CALL);
}

int
mtbdd_abstract_mul_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd,
                        BDDSET variables)
{
    return mtbdd_abstract_CALL(
        lace, destination, dd, variables, mtbdd_abstract_op_times_CALL);
}

int
mtbdd_abstract_min_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd,
                        BDDSET variables)
{
    return mtbdd_abstract_CALL(
        lace, destination, dd, variables, mtbdd_abstract_op_min_CALL);
}

int
mtbdd_abstract_max_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd,
                        BDDSET variables)
{
    return mtbdd_abstract_CALL(
        lace, destination, dd, variables, mtbdd_abstract_op_max_CALL);
}

static int
mtbdd_arg_extremum_CALL(lace_worker *lace, BDD *destination, MTBDD dd,
                        BDDSET variables, int maximum)
{
    if (destination == NULL || dd == mtbdd_invalid ||
        variables == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    MTBDD extremum = mtbdd_invalid;
    BDD candidates = mtbdd_invalid;
    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&extremum);
    mtbdd_refs_pushptr(&candidates);
    mtbdd_refs_pushptr(&computed);

    int status = maximum
        ? mtbdd_abstract_max_CALL(lace, &extremum, dd, variables)
        : mtbdd_abstract_min_CALL(lace, &extremum, dd, variables);
    if (status == SYLVAN_OK) {
        status = maximum
            ? mtbdd_compare_geq_CALL(lace, &candidates, dd, extremum)
            : mtbdd_compare_leq_CALL(lace, &candidates, dd, extremum);
    }
    if (status == SYLVAN_OK) {
        status = bdd_pick_representatives_CALL(
            lace, &computed, candidates, variables);
    }
    if (status == SYLVAN_OK) *destination = computed;

    mtbdd_refs_popptr(3);
    return status;
}

int
mtbdd_argmin_CALL(lace_worker *lace, BDD *destination, MTBDD dd,
                  BDDSET variables)
{
    return mtbdd_arg_extremum_CALL(
        lace, destination, dd, variables, 0);
}

int
mtbdd_argmax_CALL(lace_worker *lace, BDD *destination, MTBDD dd,
                  BDDSET variables)
{
    return mtbdd_arg_extremum_CALL(
        lace, destination, dd, variables, 1);
}

static int
_mtbdd_apply_callback_result(MTBDD *destination, MTBDD result)
{
    if (result == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    *destination = result;
    return SYLVAN_OK;
}

TASK(int, mtbdd_uop_times_uint, MTBDD*, result, MTBDD, a, size_t, k)

int mtbdd_uop_times_uint_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, size_t k)
{
    (void)lace;

    if (a == mtbdd_undefined) return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
    if (a == bdd_true) return _mtbdd_apply_callback_result(destination, bdd_true);

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_isnan(na)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(mtbddnode_gettype(na)));
        }
        if (mtbddnode_gettype(na) == 0) {
            int64_t v = mtbdd_leaf_int64(a);
            int64_t result;
            if (v == 0) return _mtbdd_apply_callback_result(destination, a);
            if (k > INT64_MAX || !int64_mul_checked(v, (int64_t)k, &result)) {
                return _mtbdd_apply_callback_result(destination, mtbdd_nan(0));
            }
            return _mtbdd_apply_callback_result(destination, mtbdd_int64(result));
        } else if (mtbddnode_gettype(na) == 1) {
            double d = mtbdd_leaf_double(a);
            return _mtbdd_apply_callback_result(destination, mtbdd_double(d*k));
        } else if (mtbddnode_gettype(na) == 2) {
            int64_t numerator = mtbdd_fraction_numerator(a);
            uint64_t denominator = mtbdd_fraction_denominator(a);
            if (numerator == 0) return _mtbdd_apply_callback_result(destination, a);
            const uint64_t divisor = gcd64(denominator, (uint64_t)k);
            int64_t scaled;
            const uint64_t factor = (uint64_t)k / divisor;
            if (factor > INT64_MAX ||
                !int64_mul_checked(numerator, (int64_t)factor, &scaled)) {
                return _mtbdd_apply_callback_result(destination, mtbdd_nan(2));
            }
            return _mtbdd_apply_callback_result(
                destination, mtbdd_fraction_result(scaled, denominator / divisor));
        } else {
            return SYLVAN_ERR_INVALID;
        }
    }

    return SYLVAN_APPLY_RECURSE;
}

TASK(int, mtbdd_uop_pow_uint, MTBDD*, result, MTBDD, a, size_t, k)

int mtbdd_uop_pow_uint_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, size_t k)
{
    (void)lace;

    if (a == mtbdd_undefined) return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
    if (a == bdd_true) return _mtbdd_apply_callback_result(destination, bdd_true);

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_isnan(na)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(mtbddnode_gettype(na)));
        }
        if (mtbddnode_gettype(na) == 0) {
            int64_t v = mtbdd_leaf_int64(a);
            int64_t result;
            return _mtbdd_apply_callback_result(destination,
                int64_pow_checked(v, k, &result) ? mtbdd_int64(result) : mtbdd_nan(0));
        } else if (mtbddnode_gettype(na) == 1) {
            double d = mtbdd_leaf_double(a);
            return _mtbdd_apply_callback_result(destination, mtbdd_double(pow(d, k)));
        } else if (mtbddnode_gettype(na) == 2) {
            int64_t numerator;
            uint64_t denominator;
            if (!int64_pow_checked(mtbdd_fraction_numerator(a), k, &numerator) ||
                !uint64_pow_checked(mtbdd_fraction_denominator(a), k, &denominator)) {
                return _mtbdd_apply_callback_result(destination, mtbdd_nan(2));
            }
            return _mtbdd_apply_callback_result(
                destination, mtbdd_fraction_result(numerator, denominator));
        } else {
            return SYLVAN_ERR_INVALID;
        }
    }

    return SYLVAN_APPLY_RECURSE;
}

static int
mtbdd_uapply_power_of_two(MTBDD *destination, MTBDD a, mtbdd_apply_unary_cb op, unsigned int k)
{
    const unsigned int max_shift = (unsigned int)(sizeof(size_t) * CHAR_BIT - 1);
    const size_t max_factor = (size_t)1 << max_shift;
    MTBDD result = a;
    mtbdd_refs_pushptr(&result);

    while (k > max_shift) {
        int status = mtbdd_apply_unary(&result, result, op, max_factor);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(1);
            return status;
        }
        k -= max_shift;
    }

    int status = mtbdd_apply_unary(&result, result, op, (size_t)1 << k);
    if (status == SYLVAN_OK) *destination = result;
    mtbdd_refs_popptr(1);
    return status;
}

int mtbdd_abstract_op_plus_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || k < 0) return SYLVAN_ERR_INVALID;
    if (k == 0) return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_plus_CALL);
    return mtbdd_uapply_power_of_two(destination, a, mtbdd_uop_times_uint_CALL, (unsigned int)k);
}

int mtbdd_abstract_op_times_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || k < 0) return SYLVAN_ERR_INVALID;
    if (k == 0) return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_times_CALL);
    return mtbdd_uapply_power_of_two(destination, a, mtbdd_uop_pow_uint_CALL, (unsigned int)k);
}

int mtbdd_abstract_op_min_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || k < 0) return SYLVAN_ERR_INVALID;
    if (k == 0) return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_min_CALL);
    *destination = a;
    return SYLVAN_OK;
}

int mtbdd_abstract_op_max_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || k < 0) return SYLVAN_ERR_INVALID;
    if (k == 0) return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_max_CALL);
    *destination = a;
    return SYLVAN_OK;
}

static int
_mtbdd_abstract_callback_CALL(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b, int k, mtbdd_abstract_cb op)
{
    MTBDD result = mtbdd_invalid;
    mtbdd_refs_pushptr(&result);
    int status = op(lace, &result, a, b, k);
    if (status == SYLVAN_OK) {
        if (result == mtbdd_invalid) status = SYLVAN_ERR_CALLBACK;
        else *destination = result;
    } else if (status > 0) {
        status = SYLVAN_ERR_CALLBACK;
    }
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * Abstract the variables in <v> from <a> using the operation <op>
 */
int mtbdd_abstract_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD v, mtbdd_abstract_cb op)
{
    if (destination == NULL || a == mtbdd_invalid || v == mtbdd_invalid || op == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check terminal case */
    if (a == mtbdd_undefined) { *destination = mtbdd_undefined; return SYLVAN_OK; }
    if (a == bdd_true) { *destination = bdd_true; return SYLVAN_OK; }
    if (v == bdd_true) { *destination = a; return SYLVAN_OK; }

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_ABSTRACT);

    /* a != constant, v != constant */
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        /* Count number of variables */
        int k = 0;
        while (v != bdd_true) {
            if (k == INT_MAX) {
                mtbdd_refs_popptr(1);
                return SYLVAN_ERR_INVALID;
            }
            k++;
            v = node_gethigh(v, MTBDD_GETNODE(v));
        }

        /* Check cache */
        const int cacheable = (unsigned int)k <= UINT32_C(0xffffff);
        const uint64_t cache_key = cacheable
            ? (v & UINT64_C(0x000000ffffffffff)) | ((uint64_t)(unsigned int)k << 40)
            : 0;
        if (cacheable && cache_get3(CACHE_MTBDD_ABSTRACT, a, cache_key, (size_t)op, &computed)) {
            sylvan_stats_count(MTBDD_ABSTRACT_CACHED);
            *destination = computed;
            mtbdd_refs_popptr(1);
            return SYLVAN_OK;
        }

        /* Compute result */
        int status = _mtbdd_abstract_callback_CALL(lace, &computed, a, a, k, op);

        /* Store in cache */
        if (status == SYLVAN_OK && cacheable && cache_put3(CACHE_MTBDD_ABSTRACT, a, cache_key, (size_t)op, computed)) {
            sylvan_stats_count(MTBDD_ABSTRACT_CACHEDPUT);
        }

        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(1);
        return status;
    }

    /* Possibly skip k variables */
    mtbddnode* nv = MTBDD_GETNODE(v);
    uint32_t var_a = mtbddnode_getvariable(na);
    uint32_t var_v = mtbddnode_getvariable(nv);
    int k = 0;
    while (var_v < var_a) {
        if (k == INT_MAX) {
            mtbdd_refs_popptr(1);
            return SYLVAN_ERR_INVALID;
        }
        k++;
        v = node_gethigh(v, nv);
        if (v == bdd_true) break;
        nv = MTBDD_GETNODE(v);
        var_v = mtbddnode_getvariable(nv);
    }

    /* Check cache */
    const int cacheable = (unsigned int)k <= UINT32_C(0xffffff);
    const uint64_t cache_key = cacheable
        ? (v & UINT64_C(0x000000ffffffffff)) | ((uint64_t)(unsigned int)k << 40)
        : 0;
    if (cacheable && cache_get3(CACHE_MTBDD_ABSTRACT, a, cache_key, (size_t)op, &computed)) {
        sylvan_stats_count(MTBDD_ABSTRACT_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Recursive */
    int status = SYLVAN_OK;
    if (v == bdd_true) {
        computed = a;
    } else {
        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        if (var_a < var_v) {
            mtbdd_abstract_SPAWN(lace, &high, node_gethigh(a, na), v, op);
            status = mtbdd_abstract_CALL(lace, &low, node_getlow(a, na), v, op);
            int high_status = mtbdd_abstract_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
            if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, var_a, low, high);
        } else /* var_a == var_v */ {
            MTBDD next_v = node_gethigh(v, nv);
            mtbdd_abstract_SPAWN(lace, &high, node_gethigh(a, na), next_v, op);
            status = mtbdd_abstract_CALL(lace, &low, node_getlow(a, na), next_v, op);
            int high_status = mtbdd_abstract_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
            if (status == SYLVAN_OK) status = _mtbdd_abstract_callback_CALL(lace, &computed, low, high, 0, op);
        }
        mtbdd_refs_popptr(2);
    }

    if (status == SYLVAN_OK && k) {
        MTBDD with_skipped = mtbdd_invalid;
        mtbdd_refs_pushptr(&with_skipped);
        status = _mtbdd_abstract_callback_CALL(lace, &with_skipped, computed, computed, k, op);
        if (status == SYLVAN_OK) computed = with_skipped;
        mtbdd_refs_popptr(1);
    }

    /* Store in cache */
    if (status == SYLVAN_OK && cacheable && cache_put3(CACHE_MTBDD_ABSTRACT, a, cache_key, (size_t)op, computed)) {
        sylvan_stats_count(MTBDD_ABSTRACT_CACHEDPUT);
    }

    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * Binary operation Plus (for MTBDDs of same type)
 * Numeric operands must have the same built-in type.
 * Undefined values propagate for numeric MTBDDs.
 */
int mtbdd_op_plus_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;

    // Handle Boolean MTBDDs: interpret as Or
    if (a == bdd_true) return _mtbdd_apply_callback_result(destination, bdd_true);
    if (b == bdd_true) return _mtbdd_apply_callback_result(destination, bdd_true);
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        const uint32_t type = mtbddnode_gettype(na);
        if (type != mtbddnode_gettype(nb)) return SYLVAN_ERR_INVALID;
        if (mtbddnode_isnan(na) || mtbddnode_isnan(nb)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(type));
        }
        if (type == 0) {
            int64_t value;
            return _mtbdd_apply_callback_result(destination,
                int64_add_checked(mtbdd_leaf_int64(a), mtbdd_leaf_int64(b), &value)
                    ? mtbdd_int64(value) : mtbdd_nan(0));
        }
        if (type == 1) {
            return _mtbdd_apply_callback_result(
                destination, mtbdd_double(mtbdd_leaf_double(a) + mtbdd_leaf_double(b)));
        }
        if (type == 2) {
            return _mtbdd_apply_callback_result(destination, mtbdd_fraction_add_result(a, b, 0));
        }
        return SYLVAN_ERR_INVALID;
    }

    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Binary operation Minus (for MTBDDs of same type)
 * Numeric operands must have the same built-in type.
 * Undefined values propagate.
 */
int mtbdd_op_minus_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        const uint32_t type = mtbddnode_gettype(na);
        if (type != mtbddnode_gettype(nb)) return SYLVAN_ERR_INVALID;
        if (mtbddnode_isnan(na) || mtbddnode_isnan(nb)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(type));
        }
        if (type == 0) {
            int64_t value;
            return _mtbdd_apply_callback_result(destination,
                int64_sub_checked(mtbdd_leaf_int64(a), mtbdd_leaf_int64(b), &value)
                    ? mtbdd_int64(value) : mtbdd_nan(0));
        }
        if (type == 1) {
            return _mtbdd_apply_callback_result(
                destination, mtbdd_double(mtbdd_leaf_double(a) - mtbdd_leaf_double(b)));
        }
        if (type == 2) {
            return _mtbdd_apply_callback_result(destination, mtbdd_fraction_add_result(a, b, 1));
        }
        return SYLVAN_ERR_INVALID;
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Binary operation Times (for MTBDDs of same type)
 * Numeric operands must have the same built-in type.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is mtbdd_undefined (i.e. not defined).
 */
int mtbdd_op_times_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;
    if (a == mtbdd_undefined || b == mtbdd_undefined) return _mtbdd_apply_callback_result(destination, mtbdd_undefined);

    // Handle Boolean MTBDDs: interpret as And
    if (a == bdd_true) return _mtbdd_apply_callback_result(destination, b);
    if (b == bdd_true) return _mtbdd_apply_callback_result(destination, a);

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        const uint32_t type = mtbddnode_gettype(na);
        if (type != mtbddnode_gettype(nb)) return SYLVAN_ERR_INVALID;
        if (mtbddnode_isnan(na) || mtbddnode_isnan(nb)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(type));
        }
        if (type == 0) {
            int64_t value;
            return _mtbdd_apply_callback_result(destination,
                int64_mul_checked(mtbdd_leaf_int64(a), mtbdd_leaf_int64(b), &value)
                    ? mtbdd_int64(value) : mtbdd_nan(0));
        }
        if (type == 1) {
            return _mtbdd_apply_callback_result(
                destination, mtbdd_double(mtbdd_leaf_double(a) * mtbdd_leaf_double(b)));
        }
        if (type == 2) {
            return _mtbdd_apply_callback_result(destination, mtbdd_fraction_mul_result(a, b));
        }
        return SYLVAN_ERR_INVALID;
    }

    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Binary operation Divide (for numeric MTBDDs of the same type).
 * Undefined and NaN values propagate. Integer and fraction division by zero
 * and fixed-width overflow produce a typed NaN.
 */
int mtbdd_op_divide_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    const MTBDD a = *pa, b = *pb;
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        const uint32_t type = mtbddnode_gettype(na);
        if (type != mtbddnode_gettype(nb)) return SYLVAN_ERR_INVALID;
        if (mtbddnode_isnan(na) || mtbddnode_isnan(nb)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(type));
        }
        if (type == 0) {
            const int64_t numerator = mtbdd_leaf_int64(a);
            const int64_t denominator = mtbdd_leaf_int64(b);
            if (denominator == 0 || (numerator == INT64_MIN && denominator == -1)) {
                return _mtbdd_apply_callback_result(destination, mtbdd_nan(0));
            }
            return _mtbdd_apply_callback_result(
                destination, mtbdd_int64(numerator / denominator));
        }
        if (type == 1) {
            return _mtbdd_apply_callback_result(
                destination, mtbdd_double(mtbdd_leaf_double(a) / mtbdd_leaf_double(b)));
        }
        if (type == 2) {
            return _mtbdd_apply_callback_result(destination, mtbdd_fraction_div_result(a, b));
        }
        return SYLVAN_ERR_INVALID;
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Binary operation Minimum (for MTBDDs of same type)
 * Numeric operands must have the same built-in type. Undefined values
 * propagate.
 */
int mtbdd_op_min_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
    }
    if (a == bdd_true || b == bdd_true) return SYLVAN_ERR_INVALID;
    if (a == b) return _mtbdd_apply_callback_result(destination, a);

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        if (mtbddnode_gettype(na) != mtbddnode_gettype(nb)) return SYLVAN_ERR_INVALID;
        if (mtbddnode_isnan(na) || mtbddnode_isnan(nb)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(mtbddnode_gettype(na)));
        }
        uint64_t val_a = mtbddnode_getvalue(na);
        uint64_t val_b = mtbddnode_getvalue(nb);
        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // both integer
            int64_t va = *(int64_t*)(&val_a);
            int64_t vb = *(int64_t*)(&val_b);
            return _mtbdd_apply_callback_result(destination, va < vb ? a : b);
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // both double
            double va = *(double*)&val_a;
            double vb = *(double*)&val_b;
            return _mtbdd_apply_callback_result(destination, va < vb ? a : b);
        } else if (mtbddnode_gettype(na) == 2 && mtbddnode_gettype(nb) == 2) {
            // both fraction
            int64_t nom_a = (int32_t)(val_a>>32);
            int64_t nom_b = (int32_t)(val_b>>32);
            uint64_t denom_a = val_a&0xffffffff;
            uint64_t denom_b = val_b&0xffffffff;
            // equalize denominators
            uint32_t c = gcd((uint32_t)denom_a, (uint32_t)denom_b);
            nom_a *= denom_b/c;
            nom_b *= denom_a/c;
            // compute lowest
            return _mtbdd_apply_callback_result(destination, nom_a < nom_b ? a : b);
        } else {
            return SYLVAN_ERR_INVALID;
        }
    }

    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Binary operation Maximum (for MTBDDs of same type)
 * Numeric operands must have the same built-in type. Undefined values
 * propagate.
 */
int mtbdd_op_max_CALL(lace_worker* lace, MTBDD *destination, MTBDD* pa, MTBDD* pb)
{
    (void)lace;

    MTBDD a = *pa, b = *pb;
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
    }
    if (a == bdd_true || b == bdd_true) return SYLVAN_ERR_INVALID;
    if (a == b) return _mtbdd_apply_callback_result(destination, a);

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        if (mtbddnode_gettype(na) != mtbddnode_gettype(nb)) return SYLVAN_ERR_INVALID;
        if (mtbddnode_isnan(na) || mtbddnode_isnan(nb)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(mtbddnode_gettype(na)));
        }
        uint64_t val_a = mtbddnode_getvalue(na);
        uint64_t val_b = mtbddnode_getvalue(nb);
        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // both integer
            int64_t va = *(int64_t*)(&val_a);
            int64_t vb = *(int64_t*)(&val_b);
            return _mtbdd_apply_callback_result(destination, va > vb ? a : b);
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // both double
            double vval_a = *(double*)&val_a;
            double vval_b = *(double*)&val_b;
            return _mtbdd_apply_callback_result(destination, vval_a > vval_b ? a : b);
        } else if (mtbddnode_gettype(na) == 2 && mtbddnode_gettype(nb) == 2) {
            // both fraction
            int64_t nom_a = (int32_t)(val_a>>32);
            int64_t nom_b = (int32_t)(val_b>>32);
            uint64_t denom_a = val_a&0xffffffff;
            uint64_t denom_b = val_b&0xffffffff;
            // equalize denominators
            uint32_t c = gcd((uint32_t)denom_a, (uint32_t)denom_b);
            nom_a *= denom_b/c;
            nom_b *= denom_a/c;
            // compute highest
            return _mtbdd_apply_callback_result(destination, nom_a > nom_b ? a : b);
        } else {
            return SYLVAN_ERR_INVALID;
        }
    }

    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return SYLVAN_APPLY_RECURSE;
}

int mtbdd_op_cmpl_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, size_t k)
{
    (void)lace;
    (void)k;

    // if a is false, then it is a partial function. Keep partial!
    if (a == mtbdd_undefined) return _mtbdd_apply_callback_result(destination, mtbdd_undefined);

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_isnan(na)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(mtbddnode_gettype(na)));
        }
        if (mtbddnode_gettype(na) == 0) {
            int64_t v = mtbdd_leaf_int64(a);
            if (v == 0) return _mtbdd_apply_callback_result(destination, mtbdd_int64(1));
            else return _mtbdd_apply_callback_result(destination, mtbdd_int64(0));
        } else if (mtbddnode_gettype(na) == 1) {
            double d = mtbdd_leaf_double(a);
            if (d == 0.0) return _mtbdd_apply_callback_result(destination, mtbdd_double(1.0));
            else return _mtbdd_apply_callback_result(destination, mtbdd_double(0.0));
        } else if (mtbddnode_gettype(na) == 2) {
            uint64_t v = mtbddnode_getvalue(na);
            if (v == 1) return _mtbdd_apply_callback_result(destination, mtbdd_fraction(1, 1));
            else return _mtbdd_apply_callback_result(destination, mtbdd_fraction(0, 1));
        } else {
            return SYLVAN_ERR_INVALID;
        }
    }

    return SYLVAN_APPLY_RECURSE;
}

int mtbdd_op_negate_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, size_t k)
{
    (void)lace;
    (void)k;

    // if a is false, then it is a partial function. Keep partial!
    if (a == mtbdd_undefined) return _mtbdd_apply_callback_result(destination, mtbdd_undefined);

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_isnan(na)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_nan(mtbddnode_gettype(na)));
        }
        if (mtbddnode_gettype(na) == 0) {
            int64_t v = mtbdd_leaf_int64(a);
            return _mtbdd_apply_callback_result(
                destination, v == INT64_MIN ? mtbdd_nan(0) : mtbdd_int64(-v));
        } else if (mtbddnode_gettype(na) == 1) {
            double d = mtbdd_leaf_double(a);
            return _mtbdd_apply_callback_result(destination, mtbdd_double(-d));
        } else if (mtbddnode_gettype(na) == 2) {
            uint64_t v = mtbddnode_getvalue(na);
            return _mtbdd_apply_callback_result(destination, mtbdd_fraction(-(int32_t)(v>>32), (uint32_t)v));
        } else {
            return SYLVAN_ERR_INVALID;
        }
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Compute IF <f> THEN <g> ELSE <h>.
 * <f> must be a Boolean MTBDD (or standard BDD).
 */
int mtbdd_ite_CALL(lace_worker* lace, MTBDD *destination, BDD f, MTBDD g, MTBDD h)
{
    if (destination == NULL || f == mtbdd_invalid || g == mtbdd_invalid || h == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Terminal cases */
    if (f == bdd_true) { *destination = g; return SYLVAN_OK; }
    if (f == mtbdd_undefined) { *destination = h; return SYLVAN_OK; }
    if (g == h) { *destination = g; return SYLVAN_OK; }
    if (g == bdd_true && h == mtbdd_undefined) { *destination = f; return SYLVAN_OK; }
    if (h == bdd_true && g == mtbdd_undefined) {
        *destination = MTBDD_TOGGLEMARK(f);
        return SYLVAN_OK;
    }

    // If all MTBDD's are Boolean, then there could be further optimizations (see sylvan_bdd.c)

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_ITE);

    /* Check cache */
    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_MTBDD_ITE, f, g, h, &computed)) {
        sylvan_stats_count(MTBDD_ITE_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Get top variable */
    int lg = mtbdd_is_leaf(g);
    int lh = mtbdd_is_leaf(h);
    mtbddnode* nf = MTBDD_GETNODE(f);
    mtbddnode* ng = lg ? 0 : MTBDD_GETNODE(g);
    mtbddnode* nh = lh ? 0 : MTBDD_GETNODE(h);
    uint32_t vf = mtbddnode_getvariable(nf);
    uint32_t vg = lg ? 0 : mtbddnode_getvariable(ng);
    uint32_t vh = lh ? 0 : mtbddnode_getvariable(nh);
    uint32_t v = vf;
    if (!lg && vg < v) v = vg;
    if (!lh && vh < v) v = vh;

    /* Get cofactors */
    MTBDD flow, fhigh, glow, ghigh, hlow, hhigh;
    flow = (vf == v) ? node_getlow(f, nf) : f;
    fhigh = (vf == v) ? node_gethigh(f, nf) : f;
    glow = (!lg && vg == v) ? node_getlow(g, ng) : g;
    ghigh = (!lg && vg == v) ? node_gethigh(g, ng) : g;
    hlow = (!lh && vh == v) ? node_getlow(h, nh) : h;
    hhigh = (!lh && vh == v) ? node_gethigh(h, nh) : h;

    /* Recursive calls */
    MTBDD low = mtbdd_invalid;
    MTBDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_ite_SPAWN(lace, &high, fhigh, ghigh, hhigh);
    int status = mtbdd_ite_CALL(lace, &low, flow, glow, hlow);
    int high_status = mtbdd_ite_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    status = _mtbdd_try_make_node(&computed, v, low, high);
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_ITE, f, g, h, computed)) {
        sylvan_stats_count(MTBDD_ITE_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

/**
 * Monad that converts double/fraction to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 */
int mtbdd_op_threshold_double_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, size_t svalue)
{
    (void)lace;

    /* We only expect "double" terminals, or false */
    if (a == mtbdd_undefined) return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
    if (a == bdd_true) return SYLVAN_ERR_INVALID;

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_isnan(na)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
        }
        double value = mtbdd_parameter_double(svalue);
        if (mtbddnode_gettype(na) == 1) {
            return _mtbdd_apply_callback_result(destination, mtbdd_leaf_double(a) >= value ? bdd_true : mtbdd_undefined);
        } else if (mtbddnode_gettype(na) == 2) {
            double d = (double)mtbdd_fraction_numerator(a);
            d /= mtbdd_fraction_denominator(a);
            return _mtbdd_apply_callback_result(destination, d >= value ? bdd_true : mtbdd_undefined);
        } else {
            return SYLVAN_ERR_INVALID;
        }
    }

    return SYLVAN_APPLY_RECURSE;
}

/**
 * Monad that converts double/fraction to a Boolean BDD, translate terminals > value to 1 and to 0 otherwise;
 */
int mtbdd_op_strict_threshold_double_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, size_t svalue)
{
    (void)lace;

    /* We only expect "double" terminals, or false */
    if (a == mtbdd_undefined) return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
    if (a == bdd_true) return SYLVAN_ERR_INVALID;

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_isnan(na)) {
            return _mtbdd_apply_callback_result(destination, mtbdd_undefined);
        }
        double value = mtbdd_parameter_double(svalue);
        if (mtbddnode_gettype(na) == 1) {
            return _mtbdd_apply_callback_result(destination, mtbdd_leaf_double(a) > value ? bdd_true : mtbdd_undefined);
        } else if (mtbddnode_gettype(na) == 2) {
            double d = (double)mtbdd_fraction_numerator(a);
            d /= mtbdd_fraction_denominator(a);
            return _mtbdd_apply_callback_result(destination, d > value ? bdd_true : mtbdd_undefined);
        } else {
            return SYLVAN_ERR_INVALID;
        }
    }

    return SYLVAN_APPLY_RECURSE;
}

int mtbdd_threshold_double_CALL(lace_worker* lace, MTBDD *destination, MTBDD dd, double d)
{
    return mtbdd_apply_unary_CALL(lace, destination, dd, mtbdd_op_threshold_double_CALL, mtbdd_double_parameter(d));
}

int mtbdd_strict_threshold_double_CALL(lace_worker* lace, MTBDD *destination, MTBDD dd, double d)
{
    return mtbdd_apply_unary_CALL(lace, destination, dd, mtbdd_op_strict_threshold_double_CALL, mtbdd_double_parameter(d));
}

/**
 * Compare two Double MTBDDs, returns Boolean True if they are equal within some value epsilon
 */
TASK(int, mtbdd_equal_norm_d2, MTBDD*, result, MTBDD, a, MTBDD, b, size_t, svalue, atomic_int*, shortcircuit)

int mtbdd_equal_norm_d2_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, size_t svalue, atomic_int* shortcircuit)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || shortcircuit == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check short circuit */
    if (atomic_load_explicit(shortcircuit, memory_order_relaxed)) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }

    /* Check terminal case */
    if (a == b) {
        *destination = mtbdd_is_nan(a) ? mtbdd_undefined : bdd_true;
        return SYLVAN_OK;
    }
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        *destination = bdd_true;
        return SYLVAN_OK;
    }

    if (b < a) {
        MTBDD t = a;
        a = b;
        b = t;
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    int la = mtbddnode_isleaf(na);
    int lb = mtbddnode_isleaf(nb);

    if (la && lb) {
        if (mtbddnode_gettype(na) != 1 || mtbddnode_gettype(nb) != 1) return SYLVAN_ERR_INVALID;
        double va = mtbdd_leaf_double(a);
        double vb = mtbdd_leaf_double(b);
        va -= vb;
        if (va < 0) va = -va;
        *destination = (va < mtbdd_parameter_double(svalue)) ? bdd_true : mtbdd_undefined;
        return SYLVAN_OK;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_ALL_EQUAL_ABS);

    /* Check cache */
    MTBDD computed = mtbdd_invalid;
    if (cache_get3(CACHE_MTBDD_ALL_EQUAL_ABS, a, b, svalue, &computed)) {
        sylvan_stats_count(MTBDD_ALL_EQUAL_ABS_CACHED);
        *destination = computed;
        return SYLVAN_OK;
    }

    /* Get top variable */
    uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
    uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
    uint32_t var = va < vb ? va : vb;

    /* Get cofactors */
    MTBDD alow, ahigh, blow, bhigh;
    alow  = va == var ? node_getlow(a, na)  : a;
    ahigh = va == var ? node_gethigh(a, na) : a;
    blow  = vb == var ? node_getlow(b, nb)  : b;
    bhigh = vb == var ? node_gethigh(b, nb) : b;

    MTBDD low = mtbdd_invalid;
    MTBDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_equal_norm_d2_SPAWN(lace, &high, ahigh, bhigh, svalue, shortcircuit);
    int status = mtbdd_equal_norm_d2_CALL(lace, &low, alow, blow, svalue, shortcircuit);
    if (status == SYLVAN_OK && low == mtbdd_undefined) {
        atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
    }
    int high_status = mtbdd_equal_norm_d2_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(2);
        return status;
    }
    computed = low == bdd_true && high == bdd_true ? bdd_true : mtbdd_undefined;
    if (computed == mtbdd_undefined) atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_ALL_EQUAL_ABS, a, b, svalue, computed)) {
        sylvan_stats_count(MTBDD_ALL_EQUAL_ABS_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(2);
    return SYLVAN_OK;
}

int mtbdd_all_equal_abs_double_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b, double d)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;

    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    atomic_int shortcircuit = 0;
    MTBDD comparison = mtbdd_invalid;
    mtbdd_refs_pushptr(&comparison);
    int status = mtbdd_equal_norm_d2_CALL(lace, &comparison, a, b, mtbdd_double_parameter(d), &shortcircuit);
    if (status == SYLVAN_OK) *destination = comparison == bdd_true;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * Compare two Double MTBDDs, returns Boolean True if they are equal within some value epsilon
 * This version computes the relative difference vs the value in a.
 */
TASK(int, mtbdd_equal_norm_rel_d2, MTBDD*, result, MTBDD, a, MTBDD, b, size_t, svalue, atomic_int*, shortcircuit)
int mtbdd_equal_norm_rel_d2_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, size_t svalue, atomic_int* shortcircuit)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || shortcircuit == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check short circuit */
    if (atomic_load_explicit(shortcircuit, memory_order_relaxed)) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }

    /* Check terminal case */
    if (a == b) {
        *destination = mtbdd_is_nan(a) ? mtbdd_undefined : bdd_true;
        return SYLVAN_OK;
    }
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        *destination = bdd_true;
        return SYLVAN_OK;
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    int la = mtbddnode_isleaf(na);
    int lb = mtbddnode_isleaf(nb);

    if (la && lb) {
        if (mtbddnode_gettype(na) != 1 || mtbddnode_gettype(nb) != 1) return SYLVAN_ERR_INVALID;
        double va = mtbdd_leaf_double(a);
        double vb = mtbdd_leaf_double(b);
        if (va == 0) { *destination = mtbdd_undefined; return SYLVAN_OK; }
        va = (va - vb) / va;
        if (va < 0) va = -va;
        *destination = (va < mtbdd_parameter_double(svalue)) ? bdd_true : mtbdd_undefined;
        return SYLVAN_OK;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_ALL_EQUAL_REL);

    /* Check cache */
    MTBDD computed = mtbdd_invalid;
    if (cache_get3(CACHE_MTBDD_ALL_EQUAL_REL, a, b, svalue, &computed)) {
        sylvan_stats_count(MTBDD_ALL_EQUAL_REL_CACHED);
        *destination = computed;
        return SYLVAN_OK;
    }

    /* Get top variable */
    uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
    uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
    uint32_t var = va < vb ? va : vb;

    /* Get cofactors */
    MTBDD alow, ahigh, blow, bhigh;
    alow  = va == var ? node_getlow(a, na)  : a;
    ahigh = va == var ? node_gethigh(a, na) : a;
    blow  = vb == var ? node_getlow(b, nb)  : b;
    bhigh = vb == var ? node_gethigh(b, nb) : b;

    MTBDD low = mtbdd_invalid;
    MTBDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_equal_norm_rel_d2_SPAWN(lace, &high, ahigh, bhigh, svalue, shortcircuit);
    int status = mtbdd_equal_norm_rel_d2_CALL(lace, &low, alow, blow, svalue, shortcircuit);
    if (status == SYLVAN_OK && low == mtbdd_undefined) {
        atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
    }
    int high_status = mtbdd_equal_norm_rel_d2_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(2);
        return status;
    }
    computed = low == bdd_true && high == bdd_true ? bdd_true : mtbdd_undefined;
    if (computed == mtbdd_undefined) atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_ALL_EQUAL_REL, a, b, svalue, computed)) {
        sylvan_stats_count(MTBDD_ALL_EQUAL_REL_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(2);
    return SYLVAN_OK;
}

int mtbdd_all_equal_rel_double_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b, double d)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;

    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    atomic_int shortcircuit = 0;
    MTBDD comparison = mtbdd_invalid;
    mtbdd_refs_pushptr(&comparison);
    int status = mtbdd_equal_norm_rel_d2_CALL(lace, &comparison, a, b, mtbdd_double_parameter(d), &shortcircuit);
    if (status == SYLVAN_OK) *destination = comparison == bdd_true;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) <= b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(int, mtbdd_leq_rec, MTBDD*, result, MTBDD, a, MTBDD, b, atomic_int*, shortcircuit)

int mtbdd_leq_rec_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, atomic_int* shortcircuit)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || shortcircuit == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check short circuit */
    if (atomic_load_explicit(shortcircuit, memory_order_relaxed)) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }

    /* Check terminal case */
    if (a == b) {
        *destination = mtbdd_is_nan(a) ? mtbdd_undefined : bdd_true;
        return SYLVAN_OK;
    }

    /* For partial functions, just return true */
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        *destination = bdd_true;
        return SYLVAN_OK;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_ALL_LEQ);

    /* Check cache */
    MTBDD computed = mtbdd_invalid;
    if (cache_get3(CACHE_MTBDD_ALL_LEQ, a, b, 0, &computed)) {
        sylvan_stats_count(MTBDD_ALL_LEQ_CACHED);
        *destination = computed;
        return SYLVAN_OK;
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    int la = mtbddnode_isleaf(na);
    int lb = mtbddnode_isleaf(nb);

    if (la && lb) {
        uint64_t va = mtbddnode_getvalue(na);
        uint64_t vb = mtbddnode_getvalue(nb);

        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // type 0 = integer
            computed = mtbdd_leaf_int64(a) <= mtbdd_leaf_int64(b) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // type 1 = double
            computed = mtbdd_leaf_double(a) <= mtbdd_leaf_double(b) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 2 && mtbddnode_gettype(nb) == 2) {
            // type 2 = fraction
            int64_t nom_a = (int32_t)(va>>32);
            int64_t nom_b = (int32_t)(vb>>32);
            uint64_t da = va&0xffffffff;
            uint64_t db = vb&0xffffffff;
            // equalize denominators
            uint32_t c = gcd((uint32_t)da, (uint32_t)db);
            nom_a *= db/c;
            nom_b *= da/c;
            computed = nom_a <= nom_b ? bdd_true : mtbdd_undefined;
        } else {
            return SYLVAN_ERR_INVALID;
        }
    } else {
        /* Get top variable */
        uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
        uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
        uint32_t var = va < vb ? va : vb;

        /* Get cofactors */
        MTBDD alow, ahigh, blow, bhigh;
        alow  = va == var ? node_getlow(a, na)  : a;
        ahigh = va == var ? node_gethigh(a, na) : a;
        blow  = vb == var ? node_getlow(b, nb)  : b;
        bhigh = vb == var ? node_gethigh(b, nb) : b;

        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        mtbdd_leq_rec_SPAWN(lace, &high, ahigh, bhigh, shortcircuit);
        int status = mtbdd_leq_rec_CALL(lace, &low, alow, blow, shortcircuit);
        if (status == SYLVAN_OK && low == mtbdd_undefined) {
            atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
        }
        int high_status = mtbdd_leq_rec_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(2);
            return status;
        }
        computed = low == bdd_true && high == bdd_true ? bdd_true : mtbdd_undefined;
        mtbdd_refs_popptr(2);
    }

    if (computed == mtbdd_undefined) atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_ALL_LEQ, a, b, 0, computed)) {
        sylvan_stats_count(MTBDD_ALL_LEQ_CACHEDPUT);
    }

    *destination = computed;
    return SYLVAN_OK;
}

int mtbdd_all_leq_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;

    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    atomic_int shortcircuit = 0;
    MTBDD comparison = mtbdd_invalid;
    mtbdd_refs_pushptr(&comparison);
    int status = mtbdd_leq_rec_CALL(lace, &comparison, a, b, &shortcircuit);
    if (status == SYLVAN_OK) *destination = comparison == bdd_true;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) < b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(int, mtbdd_less_rec, MTBDD*, result, MTBDD, a, MTBDD, b, atomic_int*, shortcircuit)

int mtbdd_less_rec_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, atomic_int* shortcircuit)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || shortcircuit == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check short circuit */
    if (atomic_load_explicit(shortcircuit, memory_order_relaxed)) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }

    /* Check terminal case */
    if (a == b) { *destination = mtbdd_undefined; return SYLVAN_OK; }

    /* For partial functions, just return true */
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        *destination = bdd_true;
        return SYLVAN_OK;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_ALL_LT);

    /* Check cache */
    MTBDD computed = mtbdd_invalid;
    if (cache_get3(CACHE_MTBDD_ALL_LT, a, b, 0, &computed)) {
        sylvan_stats_count(MTBDD_ALL_LT_CACHED);
        *destination = computed;
        return SYLVAN_OK;
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    int la = mtbddnode_isleaf(na);
    int lb = mtbddnode_isleaf(nb);

    if (la && lb) {
        uint64_t va = mtbddnode_getvalue(na);
        uint64_t vb = mtbddnode_getvalue(nb);

        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // type 0 = integer
            computed = mtbdd_leaf_int64(a) < mtbdd_leaf_int64(b) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // type 1 = double
            computed = mtbdd_leaf_double(a) < mtbdd_leaf_double(b) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 2 && mtbddnode_gettype(nb) == 2) {
            // type 2 = fraction
            int64_t nom_a = (int32_t)(va>>32);
            int64_t nom_b = (int32_t)(vb>>32);
            uint64_t da = va&0xffffffff;
            uint64_t db = vb&0xffffffff;
            // equalize denominators
            uint32_t c = gcd((uint32_t)da, (uint32_t)db);
            nom_a *= db/c;
            nom_b *= da/c;
            computed = nom_a < nom_b ? bdd_true : mtbdd_undefined;
        } else {
            return SYLVAN_ERR_INVALID;
        }
    } else {
        /* Get top variable */
        uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
        uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
        uint32_t var = va < vb ? va : vb;

        /* Get cofactors */
        MTBDD alow, ahigh, blow, bhigh;
        alow  = va == var ? node_getlow(a, na)  : a;
        ahigh = va == var ? node_gethigh(a, na) : a;
        blow  = vb == var ? node_getlow(b, nb)  : b;
        bhigh = vb == var ? node_gethigh(b, nb) : b;

        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        mtbdd_less_rec_SPAWN(lace, &high, ahigh, bhigh, shortcircuit);
        int status = mtbdd_less_rec_CALL(lace, &low, alow, blow, shortcircuit);
        if (status == SYLVAN_OK && low == mtbdd_undefined) {
            atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
        }
        int high_status = mtbdd_less_rec_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(2);
            return status;
        }
        computed = low == bdd_true && high == bdd_true ? bdd_true : mtbdd_undefined;
        mtbdd_refs_popptr(2);
    }

    if (computed == mtbdd_undefined) atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_ALL_LT, a, b, 0, computed)) {
        sylvan_stats_count(MTBDD_ALL_LT_CACHEDPUT);
    }

    *destination = computed;
    return SYLVAN_OK;
}

int mtbdd_all_lt_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;

    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    atomic_int shortcircuit = 0;
    MTBDD comparison = mtbdd_invalid;
    mtbdd_refs_pushptr(&comparison);
    int status = mtbdd_less_rec_CALL(lace, &comparison, a, b, &shortcircuit);
    if (status == SYLVAN_OK) *destination = comparison == bdd_true;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) >= b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(int, mtbdd_geq_rec, MTBDD*, result, MTBDD, a, MTBDD, b, atomic_int*, shortcircuit)

int mtbdd_geq_rec_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, atomic_int* shortcircuit)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || shortcircuit == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check short circuit */
    if (atomic_load_explicit(shortcircuit, memory_order_relaxed)) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }

    /* Check terminal case */
    if (a == b) {
        *destination = mtbdd_is_nan(a) ? mtbdd_undefined : bdd_true;
        return SYLVAN_OK;
    }

    /* For partial functions, just return true */
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        *destination = bdd_true;
        return SYLVAN_OK;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_ALL_GEQ);

    /* Check cache */
    MTBDD computed = mtbdd_invalid;
    if (cache_get3(CACHE_MTBDD_ALL_GEQ, a, b, 0, &computed)) {
        sylvan_stats_count(MTBDD_ALL_GEQ_CACHED);
        *destination = computed;
        return SYLVAN_OK;
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    int la = mtbddnode_isleaf(na);
    int lb = mtbddnode_isleaf(nb);

    if (la && lb) {
        uint64_t va = mtbddnode_getvalue(na);
        uint64_t vb = mtbddnode_getvalue(nb);

        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // type 0 = integer
            computed = mtbdd_leaf_int64(a) >= mtbdd_leaf_int64(b) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // type 1 = double
            computed = mtbdd_leaf_double(a) >= mtbdd_leaf_double(b) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 2 && mtbddnode_gettype(nb) == 2) {
            // type 2 = fraction
            int64_t nom_a = (int32_t)(va>>32);
            int64_t nom_b = (int32_t)(vb>>32);
            uint64_t da = va&0xffffffff;
            uint64_t db = vb&0xffffffff;
            // equalize denominators
            uint32_t c = gcd((uint32_t)da, (uint32_t)db);
            nom_a *= db/c;
            nom_b *= da/c;
            computed = nom_a >= nom_b ? bdd_true : mtbdd_undefined;
        } else {
            return SYLVAN_ERR_INVALID;
        }
    } else {
        /* Get top variable */
        uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
        uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
        uint32_t var = va < vb ? va : vb;

        /* Get cofactors */
        MTBDD alow, ahigh, blow, bhigh;
        alow  = va == var ? node_getlow(a, na)  : a;
        ahigh = va == var ? node_gethigh(a, na) : a;
        blow  = vb == var ? node_getlow(b, nb)  : b;
        bhigh = vb == var ? node_gethigh(b, nb) : b;

        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        mtbdd_geq_rec_SPAWN(lace, &high, ahigh, bhigh, shortcircuit);
        int status = mtbdd_geq_rec_CALL(lace, &low, alow, blow, shortcircuit);
        if (status == SYLVAN_OK && low == mtbdd_undefined) {
            atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
        }
        int high_status = mtbdd_geq_rec_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(2);
            return status;
        }
        computed = low == bdd_true && high == bdd_true ? bdd_true : mtbdd_undefined;
        mtbdd_refs_popptr(2);
    }

    if (computed == mtbdd_undefined) atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_ALL_GEQ, a, b, 0, computed)) {
        sylvan_stats_count(MTBDD_ALL_GEQ_CACHEDPUT);
    }

    *destination = computed;
    return SYLVAN_OK;
}

int mtbdd_all_geq_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;

    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    atomic_int shortcircuit = 0;
    MTBDD comparison = mtbdd_invalid;
    mtbdd_refs_pushptr(&comparison);
    int status = mtbdd_geq_rec_CALL(lace, &comparison, a, b, &shortcircuit);
    if (status == SYLVAN_OK) *destination = comparison == bdd_true;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) > b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(int, mtbdd_greater_rec, MTBDD*, result, MTBDD, a, MTBDD, b, atomic_int*, shortcircuit)

int mtbdd_greater_rec_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, atomic_int* shortcircuit)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || shortcircuit == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check short circuit */
    if (atomic_load_explicit(shortcircuit, memory_order_relaxed)) {
        *destination = mtbdd_undefined;
        return SYLVAN_OK;
    }

    /* Check terminal case */
    if (a == b) { *destination = mtbdd_undefined; return SYLVAN_OK; }

    /* For partial functions, just return true */
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        *destination = bdd_true;
        return SYLVAN_OK;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_ALL_GT);

    /* Check cache */
    MTBDD computed = mtbdd_invalid;
    if (cache_get3(CACHE_MTBDD_ALL_GT, a, b, 0, &computed)) {
        sylvan_stats_count(MTBDD_ALL_GT_CACHED);
        *destination = computed;
        return SYLVAN_OK;
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    int la = mtbddnode_isleaf(na);
    int lb = mtbddnode_isleaf(nb);

    if (la && lb) {
        uint64_t va = mtbddnode_getvalue(na);
        uint64_t vb = mtbddnode_getvalue(nb);

        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // type 0 = integer
            computed = mtbdd_leaf_int64(a) > mtbdd_leaf_int64(b) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // type 1 = double
            computed = mtbdd_leaf_double(a) > mtbdd_leaf_double(b) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 2 && mtbddnode_gettype(nb) == 2) {
            // type 2 = fraction
            int64_t nom_a = (int32_t)(va>>32);
            int64_t nom_b = (int32_t)(vb>>32);
            uint64_t da = va&0xffffffff;
            uint64_t db = vb&0xffffffff;
            // equalize denominators
            uint32_t c = gcd((uint32_t)da, (uint32_t)db);
            nom_a *= db/c;
            nom_b *= da/c;
            computed = nom_a > nom_b ? bdd_true : mtbdd_undefined;
        } else {
            return SYLVAN_ERR_INVALID;
        }
    } else {
        /* Get top variable */
        uint32_t va = la ? 0xffffffff : mtbddnode_getvariable(na);
        uint32_t vb = lb ? 0xffffffff : mtbddnode_getvariable(nb);
        uint32_t var = va < vb ? va : vb;

        /* Get cofactors */
        MTBDD alow, ahigh, blow, bhigh;
        alow  = va == var ? node_getlow(a, na)  : a;
        ahigh = va == var ? node_gethigh(a, na) : a;
        blow  = vb == var ? node_getlow(b, nb)  : b;
        bhigh = vb == var ? node_gethigh(b, nb) : b;

        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        mtbdd_greater_rec_SPAWN(lace, &high, ahigh, bhigh, shortcircuit);
        int status = mtbdd_greater_rec_CALL(lace, &low, alow, blow, shortcircuit);
        if (status == SYLVAN_OK && low == mtbdd_undefined) {
            atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
        }
        int high_status = mtbdd_greater_rec_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(2);
            return status;
        }
        computed = low == bdd_true && high == bdd_true ? bdd_true : mtbdd_undefined;
        mtbdd_refs_popptr(2);
    }

    if (computed == mtbdd_undefined) atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_ALL_GT, a, b, 0, computed)) {
        sylvan_stats_count(MTBDD_ALL_GT_CACHEDPUT);
    }

    *destination = computed;
    return SYLVAN_OK;
}

int mtbdd_all_gt_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;

    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    atomic_int shortcircuit = 0;
    MTBDD comparison = mtbdd_invalid;
    mtbdd_refs_pushptr(&comparison);
    int status = mtbdd_greater_rec_CALL(lace, &comparison, a, b, &shortcircuit);
    if (status == SYLVAN_OK) *destination = comparison == bdd_true;
    mtbdd_refs_popptr(1);
    return status;
}

enum mtbdd_compare_relation {
    MTBDD_COMPARE_REL_LEQ,
    MTBDD_COMPARE_REL_LT,
    MTBDD_COMPARE_REL_GEQ,
    MTBDD_COMPARE_REL_GT,
    MTBDD_COMPARE_REL_EQUAL_ABS,
    MTBDD_COMPARE_REL_EQUAL_REL
};

static int
mtbdd_compare_leaf_relation(MTBDD a, MTBDD b, size_t parameter,
                            enum mtbdd_compare_relation relation, int *result)
{
    if (a == mtbdd_undefined || b == mtbdd_undefined || a == bdd_true || b == bdd_true) {
        return SYLVAN_ERR_INVALID;
    }

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    uint32_t type_a = mtbddnode_gettype(na);
    uint32_t type_b = mtbddnode_gettype(nb);
    if (type_a != type_b) return SYLVAN_ERR_INVALID;
    if (mtbddnode_isnan(na) || mtbddnode_isnan(nb)) {
        *result = 0;
        return SYLVAN_OK;
    }

    if (relation == MTBDD_COMPARE_REL_EQUAL_ABS || relation == MTBDD_COMPARE_REL_EQUAL_REL) {
        if (type_a != 1) return SYLVAN_ERR_INVALID;

        double value_a = mtbdd_leaf_double(a);
        double value_b = mtbdd_leaf_double(b);
        double tolerance = mtbdd_parameter_double(parameter);
        if (relation == MTBDD_COMPARE_REL_EQUAL_ABS) {
            *result = fabs(value_a - value_b) < tolerance;
        } else if (value_a == value_b) {
            *result = 1;
        } else if (value_a == 0.0) {
            *result = 0;
        } else {
            *result = fabs((value_a - value_b) / value_a) < tolerance;
        }
        return SYLVAN_OK;
    }

    int comparison;
    if (type_a == 0) {
        int64_t value_a = mtbdd_leaf_int64(a);
        int64_t value_b = mtbdd_leaf_int64(b);
        comparison = value_a < value_b ? -1 : value_a > value_b;
    } else if (type_a == 1) {
        double value_a = mtbdd_leaf_double(a);
        double value_b = mtbdd_leaf_double(b);
        comparison = value_a < value_b ? -1 : value_a > value_b;
    } else if (type_a == 2) {
        int64_t numerator_a = mtbdd_fraction_numerator(a);
        int64_t numerator_b = mtbdd_fraction_numerator(b);
        uint64_t denominator_a = mtbdd_fraction_denominator(a);
        uint64_t denominator_b = mtbdd_fraction_denominator(b);
        uint32_t divisor = gcd((uint32_t)denominator_a, (uint32_t)denominator_b);
        numerator_a *= denominator_b / divisor;
        numerator_b *= denominator_a / divisor;
        comparison = numerator_a < numerator_b ? -1 : numerator_a > numerator_b;
    } else {
        return SYLVAN_ERR_INVALID;
    }

    switch (relation) {
    case MTBDD_COMPARE_REL_LEQ: *result = comparison <= 0; break;
    case MTBDD_COMPARE_REL_LT:  *result = comparison < 0; break;
    case MTBDD_COMPARE_REL_GEQ: *result = comparison >= 0; break;
    case MTBDD_COMPARE_REL_GT:  *result = comparison > 0; break;
    default: return SYLVAN_ERR_INVALID;
    }
    return SYLVAN_OK;
}

struct mtbdd_compare_config {
    size_t parameter;
    uint64_t cache_id;
    size_t counter;
    uint32_t relation;
};

TASK(int, mtbdd_any_relation_rec, int*, result, MTBDD, a, MTBDD, b,
     struct mtbdd_compare_config*, config, atomic_int*, shortcircuit)

int
mtbdd_any_relation_rec_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b,
                            struct mtbdd_compare_config *config, atomic_int *shortcircuit)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid ||
        config == NULL || shortcircuit == NULL) {
        return SYLVAN_ERR_INVALID;
    }
    if (atomic_load_explicit(shortcircuit, memory_order_relaxed)) {
        *destination = 1;
        return SYLVAN_OK;
    }
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        *destination = 0;
        return SYLVAN_OK;
    }

    int leaf_a = mtbdd_is_leaf(a);
    int leaf_b = mtbdd_is_leaf(b);
    if (leaf_a && leaf_b) {
        int status = mtbdd_compare_leaf_relation(a, b, config->parameter,
                                                 (enum mtbdd_compare_relation)config->relation,
                                                 destination);
        if (status == SYLVAN_OK && *destination) {
            atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
        }
        return status;
    }

    sylvan_stats_count(config->counter);

    uint64_t cached;
    if (cache_get3(config->cache_id, a, b, config->parameter, &cached)) {
        sylvan_stats_count(config->counter + 2);
        *destination = (int)cached;
        if (*destination) atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
        return SYLVAN_OK;
    }

    mtbddnode* node_a = leaf_a ? NULL : MTBDD_GETNODE(a);
    mtbddnode* node_b = leaf_b ? NULL : MTBDD_GETNODE(b);
    uint32_t level_a = leaf_a ? UINT32_MAX : mtbddnode_getvariable(node_a);
    uint32_t level_b = leaf_b ? UINT32_MAX : mtbddnode_getvariable(node_b);
    uint32_t level = level_a < level_b ? level_a : level_b;

    MTBDD low_a = level_a == level ? node_getlow(a, node_a) : a;
    MTBDD high_a = level_a == level ? node_gethigh(a, node_a) : a;
    MTBDD low_b = level_b == level ? node_getlow(b, node_b) : b;
    MTBDD high_b = level_b == level ? node_gethigh(b, node_b) : b;

    int low = 0;
    int high = 0;
    mtbdd_any_relation_rec_SPAWN(lace, &high, high_a, high_b, config, shortcircuit);
    int status = mtbdd_any_relation_rec_CALL(lace, &low, low_a, low_b, config, shortcircuit);
    if (status == SYLVAN_OK && low) {
        atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
    }
    int high_status = mtbdd_any_relation_rec_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;
    if (status != SYLVAN_OK) return status;

    *destination = low || high;
    if (*destination) atomic_store_explicit(shortcircuit, 1, memory_order_relaxed);
    if (cache_put3(config->cache_id, a, b, config->parameter, (uint64_t)*destination)) {
        sylvan_stats_count(config->counter + 1);
    }
    return SYLVAN_OK;
}

static int
mtbdd_any_relation(lace_worker* lace, int *destination, MTBDD a, MTBDD b,
                   size_t parameter, enum mtbdd_compare_relation relation,
                   uint64_t cache_id, size_t counter)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;
    atomic_int shortcircuit = 0;
    struct mtbdd_compare_config config = {parameter, cache_id, counter, (uint32_t)relation};
    return mtbdd_any_relation_rec_CALL(lace, destination, a, b, &config, &shortcircuit);
}

int mtbdd_any_leq_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b)
{
    return mtbdd_any_relation(lace, destination, a, b, 0, MTBDD_COMPARE_REL_LEQ,
                              CACHE_MTBDD_ANY_LEQ, MTBDD_ANY_LEQ);
}

int mtbdd_any_lt_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b)
{
    return mtbdd_any_relation(lace, destination, a, b, 0, MTBDD_COMPARE_REL_LT,
                              CACHE_MTBDD_ANY_LT, MTBDD_ANY_LT);
}

int mtbdd_any_geq_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b)
{
    return mtbdd_any_relation(lace, destination, a, b, 0, MTBDD_COMPARE_REL_GEQ,
                              CACHE_MTBDD_ANY_GEQ, MTBDD_ANY_GEQ);
}

int mtbdd_any_gt_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b)
{
    return mtbdd_any_relation(lace, destination, a, b, 0, MTBDD_COMPARE_REL_GT,
                              CACHE_MTBDD_ANY_GT, MTBDD_ANY_GT);
}

int mtbdd_any_equal_abs_double_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b, double tolerance)
{
    return mtbdd_any_relation(lace, destination, a, b, mtbdd_double_parameter(tolerance),
                              MTBDD_COMPARE_REL_EQUAL_ABS,
                              CACHE_MTBDD_ANY_EQUAL_ABS, MTBDD_ANY_EQUAL_ABS);
}

int mtbdd_any_equal_rel_double_CALL(lace_worker* lace, int *destination, MTBDD a, MTBDD b, double tolerance)
{
    return mtbdd_any_relation(lace, destination, a, b, mtbdd_double_parameter(tolerance),
                              MTBDD_COMPARE_REL_EQUAL_REL,
                              CACHE_MTBDD_ANY_EQUAL_REL, MTBDD_ANY_EQUAL_REL);
}

TASK(int, mtbdd_compare_relation_rec, BDD*, result, MTBDD, a, MTBDD, b,
     struct mtbdd_compare_config*, config)

int
mtbdd_compare_relation_rec_CALL(lace_worker* lace, BDD *destination, MTBDD a, MTBDD b,
                                struct mtbdd_compare_config *config)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || config == NULL) {
        return SYLVAN_ERR_INVALID;
    }
    if (a == mtbdd_undefined || b == mtbdd_undefined) {
        *destination = bdd_false;
        return SYLVAN_OK;
    }

    int leaf_a = mtbdd_is_leaf(a);
    int leaf_b = mtbdd_is_leaf(b);
    if (leaf_a && leaf_b) {
        int comparison;
        int status = mtbdd_compare_leaf_relation(a, b, config->parameter,
                                                 (enum mtbdd_compare_relation)config->relation,
                                                 &comparison);
        if (status == SYLVAN_OK) *destination = comparison ? bdd_true : bdd_false;
        return status;
    }

    sylvan_gc_test(lace);
    sylvan_stats_count(config->counter);

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(config->cache_id, a, b, config->parameter, &computed)) {
        sylvan_stats_count(config->counter + 2);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    mtbddnode* node_a = leaf_a ? NULL : MTBDD_GETNODE(a);
    mtbddnode* node_b = leaf_b ? NULL : MTBDD_GETNODE(b);
    uint32_t level_a = leaf_a ? UINT32_MAX : mtbddnode_getvariable(node_a);
    uint32_t level_b = leaf_b ? UINT32_MAX : mtbddnode_getvariable(node_b);
    uint32_t level = level_a < level_b ? level_a : level_b;

    MTBDD low_a = level_a == level ? node_getlow(a, node_a) : a;
    MTBDD high_a = level_a == level ? node_gethigh(a, node_a) : a;
    MTBDD low_b = level_b == level ? node_getlow(b, node_b) : b;
    MTBDD high_b = level_b == level ? node_gethigh(b, node_b) : b;

    BDD low = mtbdd_invalid;
    BDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_compare_relation_rec_SPAWN(lace, &high, high_a, high_b, config);
    int status = mtbdd_compare_relation_rec_CALL(lace, &low, low_a, low_b, config);
    int high_status = mtbdd_compare_relation_rec_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;
    if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, level, low, high);
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    if (cache_put3(config->cache_id, a, b, config->parameter, computed)) {
        sylvan_stats_count(config->counter + 1);
    }
    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

static int
mtbdd_compare_relation(lace_worker* lace, BDD *destination, MTBDD a, MTBDD b,
                       size_t parameter, enum mtbdd_compare_relation relation,
                       uint64_t cache_id, size_t counter)
{
    struct mtbdd_compare_config config = {parameter, cache_id, counter, (uint32_t)relation};
    return mtbdd_compare_relation_rec_CALL(lace, destination, a, b, &config);
}

int mtbdd_compare_leq_CALL(lace_worker* lace, BDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_compare_relation(lace, destination, a, b, 0, MTBDD_COMPARE_REL_LEQ,
                                  CACHE_MTBDD_COMPARE_LEQ, MTBDD_COMPARE_LEQ);
}

int mtbdd_compare_lt_CALL(lace_worker* lace, BDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_compare_relation(lace, destination, a, b, 0, MTBDD_COMPARE_REL_LT,
                                  CACHE_MTBDD_COMPARE_LT, MTBDD_COMPARE_LT);
}

int mtbdd_compare_geq_CALL(lace_worker* lace, BDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_compare_relation(lace, destination, a, b, 0, MTBDD_COMPARE_REL_GEQ,
                                  CACHE_MTBDD_COMPARE_GEQ, MTBDD_COMPARE_GEQ);
}

int mtbdd_compare_gt_CALL(lace_worker* lace, BDD *destination, MTBDD a, MTBDD b)
{
    return mtbdd_compare_relation(lace, destination, a, b, 0, MTBDD_COMPARE_REL_GT,
                                  CACHE_MTBDD_COMPARE_GT, MTBDD_COMPARE_GT);
}

int
mtbdd_compare_equal_abs_double_CALL(lace_worker* lace, BDD *destination,
                                    MTBDD a, MTBDD b, double tolerance)
{
    return mtbdd_compare_relation(lace, destination, a, b, mtbdd_double_parameter(tolerance),
                                  MTBDD_COMPARE_REL_EQUAL_ABS,
                                  CACHE_MTBDD_COMPARE_EQUAL_ABS, MTBDD_COMPARE_EQUAL_ABS);
}

int
mtbdd_compare_equal_rel_double_CALL(lace_worker* lace, BDD *destination,
                                    MTBDD a, MTBDD b, double tolerance)
{
    return mtbdd_compare_relation(lace, destination, a, b, mtbdd_double_parameter(tolerance),
                                  MTBDD_COMPARE_REL_EQUAL_REL,
                                  CACHE_MTBDD_COMPARE_EQUAL_REL, MTBDD_COMPARE_EQUAL_REL);
}

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 */
int mtbdd_mul_abstract_add_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, MTBDD v)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || v == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check terminal case */
    if (v == bdd_true) return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_times_CALL);

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = mtbdd_op_times_CALL(lace, &computed, &a, &b);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) status = SYLVAN_ERR_CALLBACK;
        else status = mtbdd_abstract_CALL(lace, &computed, computed, v, mtbdd_abstract_op_plus_CALL);
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

    /* Check cache */
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
        status = mtbdd_mul_abstract_add_CALL(lace, &computed, a, b, node_gethigh(v, nv));
        if (status == SYLVAN_OK) status = mtbdd_apply_CALL(lace, &computed, computed, computed, mtbdd_op_plus_CALL);
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
            mtbdd_mul_abstract_add_SPAWN(lace, &high, ahigh, bhigh, next_v);
            status = mtbdd_mul_abstract_add_CALL(lace, &low, alow, blow, next_v);
            int high_status = mtbdd_mul_abstract_add_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
            if (status == SYLVAN_OK) status = mtbdd_apply_CALL(lace, &computed, low, high, mtbdd_op_plus_CALL);
            mtbdd_refs_popptr(2);
        } else /* vv > v */ {
            /* Recursive, then create node */
            MTBDD low = mtbdd_invalid;
            MTBDD high = mtbdd_invalid;
            mtbdd_refs_pushptr(&low);
            mtbdd_refs_pushptr(&high);
            mtbdd_mul_abstract_add_SPAWN(lace, &high, ahigh, bhigh, v);
            status = mtbdd_mul_abstract_add_CALL(lace, &low, alow, blow, v);
            int high_status = mtbdd_mul_abstract_add_SYNC(lace);
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
int mtbdd_mul_abstract_max_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDD b, MTBDD v)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || v == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Check terminal case */
    if (v == bdd_true) return mtbdd_apply_CALL(lace, destination, a, b, mtbdd_op_times_CALL);

    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = mtbdd_op_times_CALL(lace, &computed, &a, &b);
    if (status == SYLVAN_OK) {
        if (computed == mtbdd_invalid) status = SYLVAN_ERR_CALLBACK;
        else status = mtbdd_abstract_CALL(lace, &computed, computed, v, mtbdd_abstract_op_max_CALL);
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
        v = node_gethigh(v, nv);
        if (v == bdd_true) {
            status = mtbdd_apply_CALL(lace, &computed, a, b, mtbdd_op_times_CALL);
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

    /* Check cache */
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
        mtbdd_mul_abstract_max_SPAWN(lace, &high, ahigh, bhigh, next_v);
        status = mtbdd_mul_abstract_max_CALL(lace, &low, alow, blow, next_v);
        int high_status = mtbdd_mul_abstract_max_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status == SYLVAN_OK) status = mtbdd_apply_CALL(lace, &computed, low, high, mtbdd_op_max_CALL);
        mtbdd_refs_popptr(2);
    } else /* vv > v */ {
        /* Recursive, then create node */
        MTBDD low = mtbdd_invalid;
        MTBDD high = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        mtbdd_mul_abstract_max_SPAWN(lace, &high, ahigh, bhigh, v);
        status = mtbdd_mul_abstract_max_CALL(lace, &low, alow, blow, v);
        int high_status = mtbdd_mul_abstract_max_SYNC(lace);
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

/**
 * Calculate the support of a MTBDD, i.e. the cube of all variables that appear in the MTBDD nodes.
 */
int mtbdd_support_CALL(lace_worker* lace, BDDSET *destination, MTBDD dd)
{
    if (destination == NULL || dd == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminal case */
    if (mtbdd_is_leaf(dd)) { *destination = bdd_true; return SYLVAN_OK; }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_SUPPORT);

    /* Check cache */
    BDDSET computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_MTBDD_SUPPORT, dd, 0, 0, &computed)) {
        sylvan_stats_count(BDD_SUPPORT_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Recursive calls */
    mtbddnode* n = MTBDD_GETNODE(dd);
    BDDSET low = mtbdd_invalid;
    BDDSET high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_support_SPAWN(lace, &low, node_getlow(dd, n));
    int status = mtbdd_support_CALL(lace, &high, node_gethigh(dd, n));
    int low_status = mtbdd_support_SYNC(lace);
    if (status == SYLVAN_OK) status = low_status;
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    /* Compute result */
    status = bdd_and_CALL(lace, &computed, low, high);
    if (status == SYLVAN_OK) {
        status = _mtbdd_try_make_node(&computed, mtbddnode_getvariable(n), mtbdd_undefined, computed);
    }
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    /* Write to cache */
    if (cache_put3(CACHE_MTBDD_SUPPORT, dd, 0, 0, computed)) {
        sylvan_stats_count(BDD_SUPPORT_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

/**
 * Function composition, for each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of mtbdd_ite(<value>, <high>, <low>).
 * Each <value> in <map> must be a Boolean MTBDD.
 */
int mtbdd_compose_CALL(lace_worker* lace, MTBDD *destination, MTBDD a, MTBDDMAP map)
{
    if (destination == NULL || a == mtbdd_invalid || map == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminal case */
    if (mtbdd_is_leaf(a) || mtbdd_map_is_empty(map)) { *destination = a; return SYLVAN_OK; }

    /* Determine top level */
    mtbddnode* n = MTBDD_GETNODE(a);
    uint32_t v = mtbddnode_getvariable(n);

    /* Find in map */
    while (mtbdd_map_key(map) < v) {
        map = mtbdd_map_next(map);
        if (mtbdd_map_is_empty(map)) { *destination = a; return SYLVAN_OK; }
    }

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_COMPOSE);

    /* Check cache */
    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_MTBDD_COMPOSE, a, map, 0, &computed)) {
        sylvan_stats_count(MTBDD_COMPOSE_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Recursive calls */
    MTBDD low = mtbdd_invalid;
    MTBDD high = mtbdd_invalid;
    MTBDD condition = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_refs_pushptr(&condition);
    mtbdd_compose_SPAWN(lace, &low, node_getlow(a, n), map);
    int status = mtbdd_compose_CALL(lace, &high, node_gethigh(a, n), map);
    int low_status = mtbdd_compose_SYNC(lace);
    if (status == SYLVAN_OK) status = low_status;
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(4);
        return status;
    }

    /* Calculate result */
    if (mtbdd_map_key(map) == v) condition = mtbdd_map_value(map);
    else status = _mtbdd_try_make_node(&condition, v, mtbdd_undefined, bdd_true);
    if (status == SYLVAN_OK) status = mtbdd_ite_CALL(lace, &computed, condition, high, low);
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(4);
        return status;
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_COMPOSE, a, map, 0, computed)) {
        sylvan_stats_count(MTBDD_COMPOSE_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(4);
    return SYLVAN_OK;
}

/**
 * Compute minimum leaf in the MTBDD (for Integer, Double, Rational MTBDDs)
 */
int mtbdd_find_min_CALL(lace_worker* lace, MTBDD *destination, MTBDD a)
{
    if (destination == NULL || a == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Check terminal case */
    if (a == mtbdd_undefined) { *destination = mtbdd_undefined; return SYLVAN_OK; }
    mtbddnode* na = MTBDD_GETNODE(a);
    if (mtbddnode_isleaf(na)) { *destination = a; return SYLVAN_OK; }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_MINIMUM);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_MINIMUM, a, 0, 0, &result)) {
        sylvan_stats_count(MTBDD_MINIMUM_CACHED);
        *destination = result;
        return SYLVAN_OK;
    }

    /* Call recursive */
    MTBDD low = mtbdd_invalid;
    MTBDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_find_min_SPAWN(lace, &low, node_getlow(a, na));
    int status = mtbdd_find_min_CALL(lace, &high, node_gethigh(a, na));
    int low_status = mtbdd_find_min_SYNC(lace);
    if (status == SYLVAN_OK) status = low_status;
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(2);
        return status;
    }

    /* Determine lowest; undefined is not a numeric leaf. */
    mtbddnode* nl = low == mtbdd_undefined ? NULL : MTBDD_GETNODE(low);
    mtbddnode* nh = high == mtbdd_undefined ? NULL : MTBDD_GETNODE(high);

    if (low == mtbdd_undefined) {
        result = high;
    } else if (high == mtbdd_undefined) {
        result = low;
    } else if (mtbddnode_gettype(nl) != mtbddnode_gettype(nh)) {
        mtbdd_refs_popptr(2);
        return SYLVAN_ERR_INVALID;
    } else if (mtbddnode_isnan(nl) || mtbddnode_isnan(nh)) {
        result = mtbdd_nan(mtbddnode_gettype(nl));
    } else if (mtbddnode_gettype(nl) == 0 && mtbddnode_gettype(nh) == 0) {
        result = mtbdd_leaf_int64(low) < mtbdd_leaf_int64(high) ? low : high;
    } else if (mtbddnode_gettype(nl) == 1 && mtbddnode_gettype(nh) == 1) {
        result = mtbdd_leaf_double(low) < mtbdd_leaf_double(high) ? low : high;
    } else if (mtbddnode_gettype(nl) == 2 && mtbddnode_gettype(nh) == 2) {
        // type 2 = fraction
        int64_t nom_l = mtbdd_fraction_numerator(low);
        int64_t nom_h = mtbdd_fraction_numerator(high);
        uint64_t denom_l = mtbdd_fraction_denominator(low);
        uint64_t denom_h = mtbdd_fraction_denominator(high);
        // equalize denominators
        uint32_t c = gcd((uint32_t)denom_l, (uint32_t)denom_h);
        nom_l *= denom_h/c;
        nom_h *= denom_l/c;
        result = nom_l < nom_h ? low : high;
    } else {
        mtbdd_refs_popptr(2);
        return SYLVAN_ERR_INVALID;
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_MINIMUM, a, 0, 0, result)) {
        sylvan_stats_count(MTBDD_MINIMUM_CACHEDPUT);
    }

    *destination = result;
    mtbdd_refs_popptr(2);
    return SYLVAN_OK;
}

/**
 * Compute maximum leaf in the MTBDD (for Integer, Double, Rational MTBDDs)
 */
int mtbdd_find_max_CALL(lace_worker* lace, MTBDD *destination, MTBDD a)
{
    if (destination == NULL || a == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Check terminal case */
    if (a == mtbdd_undefined) { *destination = mtbdd_undefined; return SYLVAN_OK; }
    mtbddnode* na = MTBDD_GETNODE(a);
    if (mtbddnode_isleaf(na)) { *destination = a; return SYLVAN_OK; }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_MAXIMUM);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_MAXIMUM, a, 0, 0, &result)) {
        sylvan_stats_count(MTBDD_MAXIMUM_CACHED);
        *destination = result;
        return SYLVAN_OK;
    }

    /* Call recursive */
    MTBDD low = mtbdd_invalid;
    MTBDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    mtbdd_find_max_SPAWN(lace, &low, node_getlow(a, na));
    int status = mtbdd_find_max_CALL(lace, &high, node_gethigh(a, na));
    int low_status = mtbdd_find_max_SYNC(lace);
    if (status == SYLVAN_OK) status = low_status;
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(2);
        return status;
    }

    /* Determine highest; undefined is not a numeric leaf. */
    mtbddnode* nl = low == mtbdd_undefined ? NULL : MTBDD_GETNODE(low);
    mtbddnode* nh = high == mtbdd_undefined ? NULL : MTBDD_GETNODE(high);

    if (low == mtbdd_undefined) {
        result = high;
    } else if (high == mtbdd_undefined) {
        result = low;
    } else if (mtbddnode_gettype(nl) != mtbddnode_gettype(nh)) {
        mtbdd_refs_popptr(2);
        return SYLVAN_ERR_INVALID;
    } else if (mtbddnode_isnan(nl) || mtbddnode_isnan(nh)) {
        result = mtbdd_nan(mtbddnode_gettype(nl));
    } else if (mtbddnode_gettype(nl) == 0 && mtbddnode_gettype(nh) == 0) {
        result = mtbdd_leaf_int64(low) > mtbdd_leaf_int64(high) ? low : high;
    } else if (mtbddnode_gettype(nl) == 1 && mtbddnode_gettype(nh) == 1) {
        result = mtbdd_leaf_double(low) > mtbdd_leaf_double(high) ? low : high;
    } else if (mtbddnode_gettype(nl) == 2 && mtbddnode_gettype(nh) == 2) {
        // type 2 = fraction
        int64_t nom_l = mtbdd_fraction_numerator(low);
        int64_t nom_h = mtbdd_fraction_numerator(high);
        uint64_t denom_l = mtbdd_fraction_denominator(low);
        uint64_t denom_h = mtbdd_fraction_denominator(high);
        // equalize denominators
        uint32_t c = gcd((uint32_t)denom_l, (uint32_t)denom_h);
        nom_l *= denom_h/c;
        nom_h *= denom_l/c;
        result = nom_l > nom_h ? low : high;
    } else {
        mtbdd_refs_popptr(2);
        return SYLVAN_ERR_INVALID;
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_MAXIMUM, a, 0, 0, result)) {
        sylvan_stats_count(MTBDD_MAXIMUM_CACHEDPUT);
    }

    *destination = result;
    mtbdd_refs_popptr(2);
    return SYLVAN_OK;
}

static int
mtbdd_count_leaf_is_nonzero(MTBDD dd)
{
    if (dd == mtbdd_undefined) return 0;
    if (dd == bdd_true) return 1;
    if (mtbdd_is_nan(dd)) return 1;

    mtbddnode *node = MTBDD_GETNODE(dd);
    switch (mtbddnode_gettype(node)) {
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

static int
mtbdd_count_shift_u64(uint64_t *result, uint64_t value, size_t shift)
{
    if (value == 0) {
        *result = 0;
        return SYLVAN_OK;
    }
    if (shift >= 64 || value > (UINT64_MAX >> shift)) return SYLVAN_ERR_OVERFLOW;
    *result = value << shift;
    return SYLVAN_OK;
}

int
mtbdd_sat_count_u64_CALL(lace_worker* lace, uint64_t *destination, MTBDD dd, BDDSET variables)
{
    if (destination == NULL || dd == mtbdd_invalid || variables == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    if (mtbdd_is_leaf(dd)) {
        uint64_t result;
        int status = mtbdd_count_shift_u64(
            &result, mtbdd_count_leaf_is_nonzero(dd) ? 1 : 0, bdd_set_count(variables));
        if (status == SYLVAN_OK) *destination = result;
        return status;
    }

    sylvan_stats_count(MTBDD_SAT_COUNT_U64);

    size_t skipped = 0;
    const uint32_t variable = mtbdd_node_variable(dd);
    while (!bdd_set_is_empty(variables) && bdd_set_first(variables) < variable) {
        skipped++;
        variables = bdd_set_next(variables);
    }
    if (bdd_set_is_empty(variables) || bdd_set_first(variables) != variable) {
        return SYLVAN_ERR_INVALID;
    }

    uint64_t cached;
    if (cache_get3(CACHE_MTBDD_SAT_COUNT_U64, dd, variables, 0, &cached)) {
        sylvan_stats_count(MTBDD_SAT_COUNT_U64_CACHED);
        uint64_t result;
        int status = mtbdd_count_shift_u64(&result, cached, skipped);
        if (status == SYLVAN_OK) *destination = result;
        return status;
    }

    const BDDSET next = bdd_set_next(variables);
    uint64_t low, high;
    mtbdd_sat_count_u64_SPAWN(lace, &high, mtbdd_node_high(dd), next);
    int low_status = mtbdd_sat_count_u64_CALL(lace, &low, mtbdd_node_low(dd), next);
    int high_status = mtbdd_sat_count_u64_SYNC(lace);
    if (low_status != SYLVAN_OK) return low_status;
    if (high_status != SYLVAN_OK) return high_status;
    if (UINT64_MAX - low < high) return SYLVAN_ERR_OVERFLOW;

    const uint64_t sum = low + high;
    if (cache_put3(CACHE_MTBDD_SAT_COUNT_U64, dd, variables, 0, sum)) {
        sylvan_stats_count(MTBDD_SAT_COUNT_U64_CACHEDPUT);
    }

    uint64_t result;
    int status = mtbdd_count_shift_u64(&result, sum, skipped);
    if (status == SYLVAN_OK) *destination = result;
    return status;
}

/**
 * Calculate an approximate number of satisfying variable assignments according
 * to <variables>.
 */
double
mtbdd_sat_count_double_CALL(lace_worker* lace, MTBDD dd, BDDSET variables)
{
    if (dd == mtbdd_invalid || variables == mtbdd_invalid) return NAN;

    /* Trivial cases */
    if (mtbdd_is_leaf(dd)) {
        return mtbdd_count_leaf_is_nonzero(dd)
            ? (double)powl(2.0L, (long double)bdd_set_count(variables))
            : 0.0;
    }

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    sylvan_stats_count(MTBDD_SAT_COUNT_DOUBLE);

    size_t skipped = 0;
    const uint32_t variable = mtbdd_node_variable(dd);
    while (!bdd_set_is_empty(variables) && bdd_set_first(variables) < variable) {
        skipped++;
        variables = bdd_set_next(variables);
    }
    if (bdd_set_is_empty(variables) || bdd_set_first(variables) != variable) return NAN;

    union {
        double d;
        uint64_t s;
    } hack;

    /* Consult cache */
    if (cache_get3(CACHE_MTBDD_SAT_COUNT_DOUBLE, dd, variables, 0, &hack.s)) {
        sylvan_stats_count(MTBDD_SAT_COUNT_DOUBLE_CACHED);
        return (double)((long double)hack.d * powl(2.0L, (long double)skipped));
    }

    const BDDSET next = bdd_set_next(variables);
    mtbdd_sat_count_double_SPAWN(lace, mtbdd_node_high(dd), next);
    double low = mtbdd_sat_count_double_CALL(lace, mtbdd_node_low(dd), next);
    hack.d = low + mtbdd_sat_count_double_SYNC(lace);

    if (cache_put3(CACHE_MTBDD_SAT_COUNT_DOUBLE, dd, variables, 0, hack.s)) {
        sylvan_stats_count(MTBDD_SAT_COUNT_DOUBLE_CACHEDPUT);
    }

    return (double)((long double)hack.d * powl(2.0L, (long double)skipped));
}

/**
 * Given a MTBDD <dd>, call <cb> with context <context> for every unique path in <dd> ending in leaf <leaf>.
 *
 * Usage:
 * TASK(void, cb, mtbdd_enum_trace*, trace, MTBDD, leaf, void*, context) { ... do something ... }
 * mtbdd_enumerate_parallel(dd, cb, context);
 */
TASK(void, mtbdd_enum_par_do, MTBDD, dd, mtbdd_enumerate_cb, cb, void*, context, mtbdd_enum_trace*, trace)

void mtbdd_enum_par_do_CALL(lace_worker* lace, MTBDD dd, mtbdd_enumerate_cb cb, void* context, mtbdd_enum_trace* trace)
{
    if (mtbdd_is_leaf(dd)) {
        cb(trace, dd, context);
        return;
    }

    mtbddnode* ndd = MTBDD_GETNODE(dd);
    uint32_t var = mtbddnode_getvariable(ndd);

    struct mtbdd_enum_trace t0 = (struct mtbdd_enum_trace){trace, var, 0};
    struct mtbdd_enum_trace t1 = (struct mtbdd_enum_trace){trace, var, 1};
    mtbdd_enum_par_do_SPAWN(lace, node_getlow(dd, ndd), cb, context, &t0);
    mtbdd_enum_par_do_CALL(lace, node_gethigh(dd, ndd), cb, context, &t1);
    mtbdd_enum_par_do_SYNC(lace);
}

void mtbdd_enumerate_parallel_CALL(lace_worker* lace, MTBDD dd, mtbdd_enumerate_cb cb, void* context)
{
    mtbdd_enum_par_do_CALL(lace, dd, cb, context, NULL);
}

/**
 * Function composition after partial evaluation.
 *
 * Given a function F(X) = f, compute the composition F'(X) = g(f) for every assignment to X.
 * All variables X in <vars> must appear before all variables in f and g(f).
 *
 * Usage:
 * TASK(int, g, MTBDD*, result, MTBDD, in) { ... write g of <in> to result ... }
 * MTBDD x_vars = ...;  // the cube of variables x
 * MTBDD result = mtbdd_invalid;
 * int status = mtbdd_eval_compose(&result, dd, x_vars, TASK(g));
 */
static int
_mtbdd_eval_compose_callback_CALL(lace_worker *lace, MTBDD *destination, MTBDD dd, mtbdd_eval_compose_cb cb)
{
    MTBDD result = mtbdd_invalid;
    mtbdd_refs_pushptr(&result);
    int status = cb(lace, &result, dd);
    if (status == SYLVAN_OK) {
        if (result == mtbdd_invalid) status = SYLVAN_ERR_CALLBACK;
        else *destination = result;
    } else if (status > 0) {
        status = SYLVAN_ERR_CALLBACK;
    }
    mtbdd_refs_popptr(1);
    return status;
}

int mtbdd_eval_compose_CALL(lace_worker* lace, MTBDD *destination, MTBDD dd, MTBDD vars, mtbdd_eval_compose_cb cb)
{
    if (destination == NULL || dd == mtbdd_invalid || vars == mtbdd_invalid || cb == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_EVAL_COMPOSE);

    /* Check cache */
    MTBDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_MTBDD_EVAL_COMPOSE, dd, vars, (size_t)cb, &computed)) {
        sylvan_stats_count(MTBDD_EVAL_COMPOSE_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    int status = SYLVAN_OK;
    if (mtbdd_is_leaf(dd) || vars == bdd_true) {
        /* Apply */
        status = _mtbdd_eval_compose_callback_CALL(lace, &computed, dd, cb);
    } else {
        /* Get top variable in dd */
        mtbddnode* ndd = MTBDD_GETNODE(dd);
        uint32_t var = mtbddnode_getvariable(ndd);

        /* Check if <var> is in <vars> */
        mtbddnode* nvars = MTBDD_GETNODE(vars);
        uint32_t vv = mtbddnode_getvariable(nvars);

        /* Search/forward <vars> */
        MTBDD _vars = vars;
        while (vv < var) {
            _vars = node_gethigh(_vars, nvars);
            if (_vars == bdd_true) break;
            nvars = MTBDD_GETNODE(_vars);
            vv = mtbddnode_getvariable(nvars);
        }

        if (_vars == bdd_true) {
            /* Apply */
            status = _mtbdd_eval_compose_callback_CALL(lace, &computed, dd, cb);
        } else {
            /* If this fails, then there are variables in f/g BEFORE vars, which breaks functionality. */
            if (vv != var) {
                mtbdd_refs_popptr(1);
                return SYLVAN_ERR_INVALID;
            }

            /* Get cofactors */
            MTBDD ddlow = node_getlow(dd, ndd);
            MTBDD ddhigh = node_gethigh(dd, ndd);

            /* Recursive */
            _vars = node_gethigh(_vars, nvars);
            MTBDD low = mtbdd_invalid;
            MTBDD high = mtbdd_invalid;
            mtbdd_refs_pushptr(&low);
            mtbdd_refs_pushptr(&high);
            mtbdd_eval_compose_SPAWN(lace, &high, ddhigh, _vars, cb);
            status = mtbdd_eval_compose_CALL(lace, &low, ddlow, _vars, cb);
            int high_status = mtbdd_eval_compose_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
            if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, var, low, high);
            mtbdd_refs_popptr(2);
        }
    }

    /* Store in cache */
    if (status == SYLVAN_OK && cache_put3(CACHE_MTBDD_EVAL_COMPOSE, dd, vars, (size_t)cb, computed)) {
        sylvan_stats_count(MTBDD_EVAL_COMPOSE_CACHEDPUT);
    }

    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * Helper function for recursive unmarking
 */
static void
mtbdd_unmark_rec(MTBDD mtbdd)
{
    mtbddnode* n = MTBDD_GETNODE(mtbdd);
    if (!mtbddnode_getmark(n)) return;
    mtbddnode_setmark(n, 0);
    if (mtbddnode_isleaf(n)) return;
    mtbdd_unmark_rec(mtbddnode_getlow(n));
    mtbdd_unmark_rec(mtbddnode_gethigh(n));
}

/**
 * Count number of leaves in MTBDD
 */

static size_t
mtbdd_leafcount_mark(MTBDD mtbdd)
{
    if (mtbdd == bdd_true) return 0; // do not count true/false leaf
    if (mtbdd == mtbdd_undefined) return 0; // do not count true/false leaf
    mtbddnode* n = MTBDD_GETNODE(mtbdd);
    if (mtbddnode_getmark(n)) return 0;
    mtbddnode_setmark(n, 1);
    if (mtbddnode_isleaf(n)) return 1; // count leaf as 1
    return mtbdd_leafcount_mark(mtbddnode_getlow(n)) + mtbdd_leafcount_mark(mtbddnode_gethigh(n));
}

size_t
mtbdd_shared_leaf_count(const MTBDD *mtbdds, size_t count)
{
    size_t result = 0, i;
    for (i=0; i<count; i++) result += mtbdd_leafcount_mark(mtbdds[i]);
    for (i=0; i<count; i++) mtbdd_unmark_rec(mtbdds[i]);
    return result;
}

/**
 * Count number of nodes in MTBDD
 */

static size_t
mtbdd_nodecount_mark(MTBDD mtbdd)
{
    mtbddnode* n = MTBDD_GETNODE(mtbdd);
    if (mtbddnode_getmark(n)) return 0;
    mtbddnode_setmark(n, 1);
    if (mtbddnode_isleaf(n)) return 1; // count leaf as 1
    return 1 + mtbdd_nodecount_mark(mtbddnode_getlow(n)) + mtbdd_nodecount_mark(mtbddnode_gethigh(n));
}

size_t
mtbdd_shared_node_count(const MTBDD *mtbdds, size_t count)
{
    size_t result = 0, i;
    for (i=0; i<count; i++) result += mtbdd_nodecount_mark(mtbdds[i]);
    for (i=0; i<count; i++) mtbdd_unmark_rec(mtbdds[i]);
    return result;
}

TASK(int, mtbdd_test_isvalid_rec, MTBDD, dd, uint32_t, parent_var)

int mtbdd_test_isvalid_rec_CALL(lace_worker* lace, MTBDD dd, uint32_t parent_var)
{
    // check if True/False leaf
    if (dd == bdd_true || dd == mtbdd_undefined) return 1;

    // check if index is in array
    uint64_t index = dd & (~bdd_complement);
    assert(index > 1 && index < nodes_get_size(nodes));
    if (index <= 1 || index >= nodes_get_size(nodes)) return 0;

    // check if marked
    int marked = nodes_is_marked(nodes, index);
    assert(marked);
    if (marked == 0) return 0;

    // check if leaf
    mtbddnode* n = MTBDD_GETNODE(dd);
    if (mtbddnode_isleaf(n)) return 1; // we're fine

    // check variable order
    uint32_t var = mtbddnode_getvariable(n);
    assert(var > parent_var);
    if (var <= parent_var) return 0;

    // check cache
    uint64_t result;
    if (cache_get3(CACHE_BDD_ISBDD, dd, 0, 0, &result)) {
        sylvan_stats_count(BDD_ISBDD_CACHED);
        return (int)result;
    }

    // check recursively
    mtbdd_test_isvalid_rec_SPAWN(lace, node_getlow(dd, n), var);
    result = (uint64_t)mtbdd_test_isvalid_rec_CALL(lace, node_gethigh(dd, n), var);
    if (!mtbdd_test_isvalid_rec_SYNC(lace)) result = 0;

    // put in cache and return result
    if (cache_put3(CACHE_BDD_ISBDD, dd, 0, 0, result)) {
        sylvan_stats_count(BDD_ISBDD_CACHEDPUT);
    }

    return (int)result;
}

int mtbdd_is_valid_CALL(lace_worker* lace, MTBDD dd)
{
    // check if True/False leaf
    if (dd == bdd_true || dd == mtbdd_undefined) return 1;

    // check if index is in array
    uint64_t index = dd & (~bdd_complement);
    assert(index > 1 && index < nodes_get_size(nodes));
    if (index <= 1 || index >= nodes_get_size(nodes)) return 0;

    // check if marked
    int marked = nodes_is_marked(nodes, index);
    assert(marked);
    if (marked == 0) return 0;

    // check if leaf
    mtbddnode* n = MTBDD_GETNODE(dd);
    if (mtbddnode_isleaf(n)) return 1; // we're fine

    // check recursively
    uint32_t var = mtbddnode_getvariable(n);
    mtbdd_test_isvalid_rec_SPAWN(lace, node_getlow(dd, n), var);
    int result = mtbdd_test_isvalid_rec_CALL(lace, node_gethigh(dd, n), var);
    if (!mtbdd_test_isvalid_rec_SYNC(lace)) result = 0;
    return result;
}

/**
 * Write a text representation of a leaf to the given file.
 */
void
mtbdd_fprint_leaf(FILE *out, MTBDD leaf)
{
    char buf[64];
    char *ptr = mtbdd_leaf_to_string(leaf, buf, 64);
    if (ptr != NULL) {
        fputs(ptr, out);
        if (ptr != buf) free(ptr);
    }
}

/**
 * Write a text representation of a leaf to stdout.
 */
void
mtbdd_print_leaf(MTBDD leaf)
{
    mtbdd_fprint_leaf(stdout, leaf);
}

/**
 * Obtain the textual representation of a leaf.
 * The returned result is either equal to the given <buf> (if the results fits)
 * or to a newly allocated array (with malloc).
 */
char *
mtbdd_leaf_to_string(MTBDD leaf, char *buf, size_t buflen)
{
    mtbddnode* n = MTBDD_GETNODE(leaf);
    if (mtbddnode_isnan(n)) {
        if (buflen >= 4) {
            memcpy(buf, "nan", 4);
            return buf;
        }
        char *result = (char*)malloc(4);
        if (result != NULL) memcpy(result, "nan", 4);
        return result;
    }
    uint32_t type = mtbddnode_gettype(n);
    uint64_t value = mtbddnode_getvalue(n);
    int complement = MTBDD_HASMARK(leaf) ? 1 : 0;

    return sylvan_mt_to_str(complement, type, value, buf, buflen);
}

/**
 * Export to .dot file
 */

static void
mtbdd_fprintdot_rec(FILE *out, MTBDD mtbdd)
{
    mtbddnode* n = MTBDD_GETNODE(mtbdd); // also works for mtbdd_undefined
    if (mtbddnode_getmark(n)) return;
    mtbddnode_setmark(n, 1);

    if (mtbdd == bdd_true || mtbdd == mtbdd_undefined) {
        fprintf(out, "0 [shape=box, style=filled, label=\"F\"];\n");
    } else if (mtbddnode_isleaf(n)) {
        fprintf(out, "%" PRIu64 " [shape=box, style=filled, label=\"", MTBDD_STRIPMARK(mtbdd));
        mtbdd_fprint_leaf(out, mtbdd);
        fprintf(out, "\"];\n");
    } else {
        fprintf(out, "%" PRIu64 " [label=\"%" PRIu32 "\"];\n",
                MTBDD_STRIPMARK(mtbdd), mtbddnode_getvariable(n));

        mtbdd_fprintdot_rec(out, mtbddnode_getlow(n));
        mtbdd_fprintdot_rec(out, mtbddnode_gethigh(n));

        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=dashed];\n",
                MTBDD_STRIPMARK(mtbdd), mtbddnode_getlow(n));
        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n",
                MTBDD_STRIPMARK(mtbdd), MTBDD_STRIPMARK(mtbddnode_gethigh(n)),
                mtbddnode_getcomp(n) ? "dot" : "none");
    }
}

void
mtbdd_fprint_dot(FILE *out, MTBDD mtbdd)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    fprintf(out, "root [style=invis];\n");
    fprintf(out, "root -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n",
            MTBDD_STRIPMARK(mtbdd), MTBDD_HASMARK(mtbdd) ? "dot" : "none");

    mtbdd_fprintdot_rec(out, mtbdd);
    mtbdd_unmark_rec(mtbdd);

    fprintf(out, "}\n");
}

/**
 * Export to .dot file, but do not display complement edges. Expand instead.
 */

static void
mtbdd_fprintdot_nc_rec(FILE *out, MTBDD mtbdd)
{
    mtbddnode* n = MTBDD_GETNODE(mtbdd); // also works for mtbdd_undefined
    if (mtbddnode_getmark(n)) return;
    mtbddnode_setmark(n, 1);

    if (mtbdd == bdd_true) {
        fprintf(out, "%" PRIu64 " [shape=box, style=filled, label=\"T\"];\n", mtbdd);
    } else if (mtbdd == mtbdd_undefined) {
        fprintf(out, "0 [shape=box, style=filled, label=\"F\"];\n");
    } else if (mtbddnode_isleaf(n)) {
        fprintf(out, "%" PRIu64 " [shape=box, style=filled, label=\"", mtbdd);
        mtbdd_fprint_leaf(out, mtbdd);
        fprintf(out, "\"];\n");
    } else {
        fprintf(out, "%" PRIu64 " [label=\"%" PRIu32 "\"];\n", mtbdd, mtbddnode_getvariable(n));

        mtbdd_fprintdot_nc_rec(out, mtbddnode_getlow(n));
        mtbdd_fprintdot_nc_rec(out, mtbddnode_gethigh(n));

        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=dashed];\n", mtbdd, node_getlow(mtbdd, n));
        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=solid];\n", mtbdd, node_gethigh(mtbdd, n));
    }
}

void
mtbdd_fprint_dot_no_complement(FILE *out, MTBDD mtbdd)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    fprintf(out, "root [style=invis];\n");
    fprintf(out, "root -> %" PRIu64 " [style=solid];\n", mtbdd);

    mtbdd_fprintdot_nc_rec(out, mtbdd);
    mtbdd_unmark_rec(mtbdd);

    fprintf(out, "}\n");
}

/**
 * Generate SHA2 structural hashes.
 * Hashes are independent of location.
 * Mainly useful for debugging purposes.
 */
static void
mtbdd_sha2_rec(MTBDD dd, SHA256_CTX *ctx)
{
    if (dd == bdd_true || dd == mtbdd_undefined) {
        SHA256_Update(ctx, (void*)&dd, sizeof(MTBDD));
        return;
    }

    mtbddnode* node = MTBDD_GETNODE(dd);
    if (mtbddnode_getmark(node) == 0) {
        mtbddnode_setmark(node, 1);
        if (mtbddnode_isleaf(node)) {
            uint32_t type = mtbddnode_gettype(node);
            SHA256_Update(ctx, (void*)&type, sizeof(uint32_t));
            uint64_t value;
            if (mtbddnode_isnan(node)) {
                const uint32_t kind = mtbddnode_getleafkind(node);
                SHA256_Update(ctx, (void*)&kind, sizeof(uint32_t));
                value = 0;
            } else {
                value = mtbddnode_getvalue(node);
                value = sylvan_mt_hash(type, value, value);
            }
            SHA256_Update(ctx, (void*)&value, sizeof(uint64_t));
        } else {
            uint32_t level = mtbddnode_getvariable(node);
            if (MTBDD_STRIPMARK(mtbddnode_gethigh(node))) level |= 0x80000000;
            SHA256_Update(ctx, (void*)&level, sizeof(uint32_t));
            mtbdd_sha2_rec(mtbddnode_gethigh(node), ctx);
            mtbdd_sha2_rec(mtbddnode_getlow(node), ctx);
        }
    }
}

void
mtbdd_print_sha256(MTBDD dd)
{
    mtbdd_fprint_sha256(stdout, dd);
}

void
mtbdd_fprint_sha256(FILE *f, MTBDD dd)
{
    char buf[80];
    mtbdd_sha256(dd, buf);
    fprintf(f, "%s", buf);
}

void
mtbdd_sha256(MTBDD dd, char *target)
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    mtbdd_sha2_rec(dd, &ctx);
    if (dd != bdd_true && dd != mtbdd_undefined) mtbdd_unmark_rec(dd);
    SHA256_End(&ctx, target);
}

/**
 * Implementation of visitor operations
 */

void mtbdd_visit_CALL(lace_worker* lace, MTBDD dd, mtbdd_visit_pre_cb pre_cb, mtbdd_visit_post_cb post_cb, void* ctx)
{
    int children = 1;
    if (pre_cb != NULL) children = pre_cb(dd, ctx);
    if (children && !mtbdd_is_leaf(dd)) {
        mtbdd_visit_CALL(lace, mtbdd_node_low(dd), pre_cb, post_cb, ctx);
        mtbdd_visit_CALL(lace, mtbdd_node_high(dd), pre_cb, post_cb, ctx);
    }
    if (post_cb != NULL) post_cb(dd, ctx);
}

void mtbdd_visit_parallel_CALL(lace_worker* lace, MTBDD dd, mtbdd_visit_pre_cb pre_cb, mtbdd_visit_post_cb post_cb, void* ctx)
{
    int children = 1;
    if (pre_cb != NULL) children = pre_cb(dd, ctx);
    if (children && !mtbdd_is_leaf(dd)) {
        mtbdd_visit_parallel_SPAWN(lace, mtbdd_node_low(dd), pre_cb, post_cb, ctx);
        mtbdd_visit_parallel_CALL(lace, mtbdd_node_high(dd), pre_cb, post_cb, ctx);
        mtbdd_visit_parallel_SYNC(lace);
    }
    if (post_cb != NULL) post_cb(dd, ctx);
}

/**
 * Writing MTBDD files using a skiplist as a backend
 */
int mtbdd_writer_add_visitor_pre(MTBDD dd, sylvan_skiplist_t sl)
{
    if (mtbdd_is_leaf(dd)) return 0;
    return sylvan_skiplist_get(sl, MTBDD_STRIPMARK(dd)) == 0 ? 1 : 0;
}

void mtbdd_writer_add_visitor_post(MTBDD dd, sylvan_skiplist_t sl)
{
    if (dd == bdd_true || dd == mtbdd_undefined) return;
    sylvan_skiplist_assign_next(sl, MTBDD_STRIPMARK(dd));
}

sylvan_skiplist_t
mtbdd_writer_start(void)
{
    size_t sl_size = nodes_get_size(nodes) > 0x7fffffff ? 0x7fffffff : nodes_get_size(nodes);
    return sylvan_skiplist_alloc(sl_size);
}

void mtbdd_writer_add_CALL(lace_worker* lace, sylvan_skiplist_t sl, MTBDD dd)
{
    (void)lace;
    mtbdd_visit(dd, (mtbdd_visit_pre_cb)mtbdd_writer_add_visitor_pre, (mtbdd_visit_post_cb)mtbdd_writer_add_visitor_post, (void*)sl);
}

void
mtbdd_writer_writebinary(FILE *out, sylvan_skiplist_t sl)
{
    size_t nodecount = sylvan_skiplist_count(sl);
    fwrite(&nodecount, sizeof(size_t), 1, out);
    for (size_t i=1; i<=nodecount; i++) {
        MTBDD dd = sylvan_skiplist_getr(sl, i);

        mtbddnode* n = MTBDD_GETNODE(dd);
        if (mtbddnode_isleaf(n)) {
            /* write leaf */
            fwrite(n, sizeof(struct mtbddnode), 1, out);
            if (!mtbddnode_isnan(n)) {
                uint32_t type = mtbddnode_gettype(n);
                uint64_t value = mtbddnode_getvalue(n);
                sylvan_mt_write_binary(type, value, out);
            }
        } else {
            struct mtbddnode node;
            MTBDD low = sylvan_skiplist_get(sl, mtbddnode_getlow(n));
            MTBDD high = mtbddnode_gethigh(n);
            high = MTBDD_TRANSFERMARK(high, sylvan_skiplist_get(sl, MTBDD_STRIPMARK(high)));
            mtbddnode_makenode(&node, mtbddnode_getvariable(n), low, high);
            fwrite(&node, sizeof(struct mtbddnode), 1, out);
        }
    }
}

uint64_t
mtbdd_writer_get(sylvan_skiplist_t sl, MTBDD dd)
{
    return MTBDD_TRANSFERMARK(dd, sylvan_skiplist_get(sl, MTBDD_STRIPMARK(dd)));
}

void
mtbdd_writer_end(sylvan_skiplist_t sl)
{
    sylvan_skiplist_free(sl);
}

void mtbdd_writer_tobinary_CALL(lace_worker* lace, FILE * out, MTBDD * dds, int count)
{
    sylvan_skiplist_t sl = mtbdd_writer_start();

    for (int i=0; i<count; i++) {
        mtbdd_writer_add_CALL(lace, sl, dds[i]);
    }

    mtbdd_writer_writebinary(out, sl);

    fwrite(&count, sizeof(int), 1, out);
    
    for (int i=0; i<count; i++) {
        uint64_t v = mtbdd_writer_get(sl, dds[i]);
        fwrite(&v, sizeof(uint64_t), 1, out);
    }

    mtbdd_writer_end(sl);
}

void
mtbdd_writer_writetext(FILE *out, sylvan_skiplist_t sl)
{
    fprintf(out, "[\n");
    size_t nodecount = sylvan_skiplist_count(sl);
    for (size_t i=1; i<=nodecount; i++) {
        MTBDD dd = sylvan_skiplist_getr(sl, i);

        mtbddnode* n = MTBDD_GETNODE(dd);
        if (mtbddnode_isleaf(n)) {
            /* serialize leaf, does not support customs yet */
            fprintf(out, "  leaf(%zu,%u,\"", i, mtbddnode_gettype(n));
            mtbdd_fprint_leaf(out, MTBDD_STRIPMARK(dd));
            fprintf(out, "\"),\n");
        } else {
            MTBDD low = sylvan_skiplist_get(sl, mtbddnode_getlow(n));
            MTBDD high = mtbddnode_gethigh(n);
            high = MTBDD_TRANSFERMARK(high, sylvan_skiplist_get(sl, MTBDD_STRIPMARK(high)));
            fprintf(out, "  node(%zu,%u,%zu,%s%zu),\n", i, mtbddnode_getvariable(n), (size_t)low, MTBDD_HASMARK(high)?"~":"", (size_t)MTBDD_STRIPMARK(high));
        }
    }

    fprintf(out, "]");
}

void mtbdd_writer_totext_CALL(lace_worker* lace, FILE * out, MTBDD * dds, int count)
{
    sylvan_skiplist_t sl = mtbdd_writer_start();

    for (int i=0; i<count; i++) {
        mtbdd_writer_add_CALL(lace, sl, dds[i]);
    }

    mtbdd_writer_writetext(out, sl);

    fprintf(out, ",[");
    
    for (int i=0; i<count; i++) {
        uint64_t v = mtbdd_writer_get(sl, dds[i]);
        fprintf(out, "%s%zu,", MTBDD_HASMARK(v)?"~":"", (size_t)MTBDD_STRIPMARK(v));
    }

    fprintf(out, "]\n");

    mtbdd_writer_end(sl);
}

/**
 * Reading a file earlier written with mtbdd_writer_writebinary
 * Returns an array with the conversion from stored identifier to MTBDD
 * This array is allocated with malloc and must be freed afterwards.
 * This method does not support custom leaves.
 */
uint64_t* mtbdd_reader_readbinary_CALL(lace_worker* lace, FILE* in)
{
    (void)lace;
    size_t nodecount;
    if (fread(&nodecount, sizeof(size_t), 1, in) != 1) {
        return NULL;
    }

    uint64_t *arr = malloc(sizeof(uint64_t)*(nodecount+1));
    arr[0] = 0;
    for (size_t i=1; i<=nodecount; i++) {
        struct mtbddnode node;
        if (fread(&node, sizeof(struct mtbddnode), 1, in) != 1) {
            free(arr);
            return NULL;
        }

        if (mtbddnode_isleaf(&node)) {
            /* serialize leaf */
            uint32_t type = mtbddnode_gettype(&node);
            if (mtbddnode_isnan(&node)) {
                arr[i] = mtbdd_nan(type);
            } else {
                uint64_t value = mtbddnode_getvalue(&node);
                sylvan_mt_read_binary(type, &value, in);
                arr[i] = mtbdd_leaf(type, value);
            }
        } else {
            MTBDD low = arr[mtbddnode_getlow(&node)];
            MTBDD high = mtbddnode_gethigh(&node);
            high = MTBDD_TRANSFERMARK(high, arr[MTBDD_STRIPMARK(high)]);
            arr[i] = mtbdd_make_node(mtbddnode_getvariable(&node), low, high);
        }
    }

    return arr;
}

/**
 * Retrieve the MTBDD of the given stored identifier.
 */
MTBDD
mtbdd_reader_get(uint64_t* arr, uint64_t identifier)
{
    return MTBDD_TRANSFERMARK(identifier, arr[MTBDD_STRIPMARK(identifier)]);
}

/**
 * Free the allocated translation array
 */
void
mtbdd_reader_end(uint64_t *arr)
{
    free(arr);
}

/**
 * Reading a file earlier written with mtbdd_writer_tobinary
 */
int mtbdd_reader_frombinary_CALL(lace_worker* lace, FILE* in, MTBDD* dds, int count)
{
    uint64_t *arr = mtbdd_reader_readbinary_CALL(lace, in);
    if (arr == NULL) return -1;

    /* Read stored count */
    int actual_count;
    if (fread(&actual_count, sizeof(int), 1, in) != 1) {
        mtbdd_reader_end(arr);
        return -1;
    }

    /* If actual count does not agree with given count, abort */
    if (actual_count != count) {
        mtbdd_reader_end(arr);
        return -1;
    }
    
    /* Read every stored identifier, and translate to MTBDD */
    for (int i=0; i<count; i++) {
        uint64_t v;
        if (fread(&v, sizeof(uint64_t), 1, in) != 1) {
            mtbdd_reader_end(arr);
            return -1;
        }
        dds[i] = mtbdd_reader_get(arr, v);
    }

    mtbdd_reader_end(arr);
    return 0;
}

/**
 * Implementation of variable sets, i.e., cubes of (positive) variables.
 */

/**
 * Create a set of variables, represented as the conjunction of (positive) variables.
 */
int
bdd_set_from_array(BDDSET *destination, const uint32_t *arr, size_t length)
{
    if (destination == NULL || (length != 0 && arr == NULL)) return SYLVAN_ERR_INVALID;

    BDDSET computed = bdd_true;
    mtbdd_refs_pushptr(&computed);
    int status = SYLVAN_OK;
    for (size_t i = length; i > 0 && status == SYLVAN_OK; i--) {
        status = bdd_set_add(&computed, computed, arr[i-1]);
    }
    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * Write all variables in a variable set to the given array.
 * The array must be sufficiently large.
 */
void
bdd_set_to_array(MTBDD set, uint32_t *arr)
{
    while (set != bdd_true) {
        mtbddnode* n = MTBDD_GETNODE(set);
        *arr++ = mtbddnode_getvariable(n);
        set = node_gethigh(set, n);
    }
}

/**
 * Add the variable <var> to <set>.
 */
int
bdd_set_add(BDDSET *destination, BDDSET set, uint32_t var)
{
    if (destination == NULL || set == mtbdd_invalid || var > UINT32_C(0x00ffffff)) {
        return SYLVAN_ERR_INVALID;
    }
    if (set == bdd_true) return _mtbdd_try_make_node(destination, var, mtbdd_undefined, bdd_true);

    mtbddnode* set_node = MTBDD_GETNODE(set);
    uint32_t set_var = mtbddnode_getvariable(set_node);
    if (var < set_var) return _mtbdd_try_make_node(destination, var, mtbdd_undefined, set);
    else if (set_var == var) {
        *destination = set;
        return SYLVAN_OK;
    }
    else {
        MTBDD sub = mtbddnode_followhigh(set, set_node);
        BDDSET computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&computed);
        int status = bdd_set_add(&computed, sub, var);
        if (status == SYLVAN_OK && sub == computed) computed = set;
        else if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, set_var, mtbdd_undefined, computed);
        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(1);
        return status;
    }
}

/**
 * Remove the variable <var> from <set>.
 */
int
bdd_set_remove(BDDSET *destination, BDDSET set, uint32_t var)
{
    if (destination == NULL || set == mtbdd_invalid || var > UINT32_C(0x00ffffff)) {
        return SYLVAN_ERR_INVALID;
    }
    if (set == bdd_true) {
        *destination = bdd_true;
        return SYLVAN_OK;
    }

    mtbddnode* set_node = MTBDD_GETNODE(set);
    uint32_t set_var = mtbddnode_getvariable(set_node);
    if (var < set_var) {
        *destination = set;
        return SYLVAN_OK;
    }
    else if (set_var == var) {
        *destination = mtbddnode_followhigh(set, set_node);
        return SYLVAN_OK;
    }
    else {
        MTBDD sub = mtbddnode_followhigh(set, set_node);
        BDDSET computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&computed);
        int status = bdd_set_remove(&computed, sub, var);
        if (status == SYLVAN_OK && sub == computed) computed = set;
        else if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, set_var, mtbdd_undefined, computed);
        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(1);
        return status;
    }
}

/**
 * Remove variables in <set2> from <set1>.
 */
int
bdd_set_union_CALL(lace_worker *lace, BDDSET *destination,
                   BDDSET set1, BDDSET set2)
{
    return bdd_and_CALL(lace, destination, set1, set2);
}

int bdd_set_difference_CALL(lace_worker* lace, BDDSET *destination, BDDSET set1, BDDSET set2)
{
    if (destination == NULL || set1 == mtbdd_invalid || set2 == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    if (set1 == bdd_true || set1 == set2) { *destination = bdd_true; return SYLVAN_OK; }
    if (set2 == bdd_true) { *destination = set1; return SYLVAN_OK; }

    mtbddnode* set1_node = MTBDD_GETNODE(set1);
    mtbddnode* set2_node = MTBDD_GETNODE(set2);
    uint32_t set1_var = mtbddnode_getvariable(set1_node);
    uint32_t set2_var = mtbddnode_getvariable(set2_node);

    if (set1_var == set2_var) {
        return bdd_set_difference_CALL(lace, destination, mtbddnode_followhigh(set1, set1_node), mtbddnode_followhigh(set2, set2_node));
    }

    if (set1_var > set2_var) {
        return bdd_set_difference_CALL(lace, destination, set1, mtbddnode_followhigh(set2, set2_node));
    }

    /* set1_var < set2_var */
    MTBDD sub = mtbddnode_followhigh(set1, set1_node);
    BDDSET computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = bdd_set_difference_CALL(lace, &computed, sub, set2);
    if (status == SYLVAN_OK && computed == sub) computed = set1;
    else if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, set1_var, mtbdd_undefined, computed);
    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * Return 1 if <set> contains <var>, 0 otherwise.
 */
int
bdd_set_contains(MTBDD set, uint32_t var)
{
    while (set != bdd_true) {
        mtbddnode* n = MTBDD_GETNODE(set);
        uint32_t v = mtbddnode_getvariable(n);
        if (v == var) return 1;
        if (v > var) return 0;
        set = node_gethigh(set, n);
    }
    return 0;
}

/**
 * Compute the number of variables in a given set of variables.
 */
size_t
bdd_set_count(MTBDD set)
{
    size_t result = 0;
    while (set != bdd_true) {
        result++;
        set = mtbdd_node_high(set);
    }
    return result;
}

/**
 * Sanity check if the given MTBDD is a conjunction of positive variables,
 * and if all nodes are marked in the nodes table (detects violations after garbage collection).
 */
void
bdd_set_assert_valid(MTBDD set)
{
    while (set != bdd_true) {
        assert(set != mtbdd_undefined);
        assert(nodes_is_marked(nodes, set));
        mtbddnode* n = MTBDD_GETNODE(set);
        assert(node_getlow(set, n) == mtbdd_undefined);
        set = node_gethigh(set, n);
    }
}

/**
 * Return 1 if the map contains the key, 0 otherwise.
 */
int
mtbdd_map_contains(MTBDDMAP map, uint32_t key)
{
    while (!mtbdd_map_is_empty(map)) {
        mtbddnode* n = MTBDD_GETNODE(map);
        uint32_t k = mtbddnode_getvariable(n);
        if (k == key) return 1;
        if (k > key) return 0;
        map = node_getlow(map, n);
    }

    return 0;
}

/**
 * Retrieve the number of keys in the map.
 */
size_t
mtbdd_map_count(MTBDDMAP map)
{
    size_t r = 0;

    while (!mtbdd_map_is_empty(map)) {
        r++;
        map = mtbdd_map_next(map);
    }

    return r;
}

/**
 * Add the pair <key,value> to the map, overwrites if key already in map.
 */
int
mtbdd_map_set(MTBDDMAP *destination, MTBDDMAP map, uint32_t key, MTBDD value)
{
    if (destination == NULL || map == mtbdd_invalid || value == mtbdd_invalid ||
        key > UINT32_C(0x00ffffff)) {
        return SYLVAN_ERR_INVALID;
    }

    MTBDDMAP computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = SYLVAN_OK;

    if (mtbdd_map_is_empty(map)) {
        status = _mtbdd_try_make_map_node(&computed, key, mtbdd_map_empty(), value);
    } else {
        mtbddnode* n = MTBDD_GETNODE(map);
        uint32_t k = mtbddnode_getvariable(n);

        if (k < key) {
            // add recursively and rebuild tree
            MTBDDMAP next = mtbdd_invalid;
            mtbdd_refs_pushptr(&next);
            status = mtbdd_map_set(&next, node_getlow(map, n), key, value);
            if (status == SYLVAN_OK) {
                status = _mtbdd_try_make_map_node(&computed, k, next, node_gethigh(map, n));
            }
            mtbdd_refs_popptr(1);
        } else if (k > key) {
            status = _mtbdd_try_make_map_node(&computed, key, map, value);
        } else {
            // replace old
            status = _mtbdd_try_make_map_node(&computed, key, node_getlow(map, n), value);
        }
    }

    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
}

/**
 * Add all values from map2 to map1, overwrites if key already in map1.
 */
int
mtbdd_map_update(MTBDDMAP *destination, MTBDDMAP map1, MTBDDMAP map2)
{
    if (destination == NULL || map1 == mtbdd_invalid || map2 == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    if (mtbdd_map_is_empty(map1)) { *destination = map2; return SYLVAN_OK; }
    if (mtbdd_map_is_empty(map2)) { *destination = map1; return SYLVAN_OK; }

    mtbddnode* n1 = MTBDD_GETNODE(map1);
    mtbddnode* n2 = MTBDD_GETNODE(map2);
    uint32_t k1 = mtbddnode_getvariable(n1);
    uint32_t k2 = mtbddnode_getvariable(n2);

    MTBDDMAP next = mtbdd_invalid;
    MTBDDMAP computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&next);
    mtbdd_refs_pushptr(&computed);
    int status;
    if (k1 < k2) {
        status = mtbdd_map_update(&next, node_getlow(map1, n1), map2);
        if (status == SYLVAN_OK) {
            status = _mtbdd_try_make_map_node(&computed, k1, next, node_gethigh(map1, n1));
        }
    } else if (k1 > k2) {
        status = mtbdd_map_update(&next, map1, node_getlow(map2, n2));
        if (status == SYLVAN_OK) {
            status = _mtbdd_try_make_map_node(&computed, k2, next, node_gethigh(map2, n2));
        }
    } else {
        status = mtbdd_map_update(&next, node_getlow(map1, n1), node_getlow(map2, n2));
        if (status == SYLVAN_OK) {
            status = _mtbdd_try_make_map_node(&computed, k2, next, node_gethigh(map2, n2));
        }
    }

    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(2);
    return status;
}

/**
 * Remove the key <key> from the map and return the result
 */
int
mtbdd_map_remove(MTBDDMAP *destination, MTBDDMAP map, uint32_t key)
{
    if (destination == NULL || map == mtbdd_invalid || key > UINT32_C(0x00ffffff)) {
        return SYLVAN_ERR_INVALID;
    }
    if (mtbdd_map_is_empty(map)) { *destination = map; return SYLVAN_OK; }

    mtbddnode* n = MTBDD_GETNODE(map);
    uint32_t k = mtbddnode_getvariable(n);

    if (k < key) {
        MTBDDMAP next = mtbdd_invalid;
        MTBDDMAP computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&next);
        mtbdd_refs_pushptr(&computed);
        int status = mtbdd_map_remove(&next, node_getlow(map, n), key);
        if (status == SYLVAN_OK) {
            status = _mtbdd_try_make_map_node(&computed, k, next, node_gethigh(map, n));
        }
        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(2);
        return status;
    } else if (k > key) {
        *destination = map;
        return SYLVAN_OK;
    } else {
        *destination = node_getlow(map, n);
        return SYLVAN_OK;
    }
}

/**
 * Remove all keys in the variable set <variables> from the map.
 */
int
mtbdd_map_remove_all(MTBDDMAP *destination, MTBDDMAP map, BDDSET variables)
{
    if (destination == NULL || map == mtbdd_invalid || variables == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    if (mtbdd_map_is_empty(map) || variables == bdd_true) { *destination = map; return SYLVAN_OK; }

    mtbddnode* n1 = MTBDD_GETNODE(map);
    mtbddnode* n2 = MTBDD_GETNODE(variables);
    uint32_t k1 = mtbddnode_getvariable(n1);
    uint32_t k2 = mtbddnode_getvariable(n2);

    if (k1 < k2) {
        MTBDDMAP next = mtbdd_invalid;
        MTBDDMAP computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&next);
        mtbdd_refs_pushptr(&computed);
        int status = mtbdd_map_remove_all(&next, node_getlow(map, n1), variables);
        if (status == SYLVAN_OK) {
            status = _mtbdd_try_make_map_node(&computed, k1, next, node_gethigh(map, n1));
        }
        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(2);
        return status;
    } else if (k1 > k2) {
        return mtbdd_map_remove_all(destination, map, node_gethigh(variables, n2));
    } else {
        return mtbdd_map_remove_all(destination, node_getlow(map, n1), node_gethigh(variables, n2));
    }
}
