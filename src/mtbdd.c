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

/* Primitives */
int
mtbdd_is_leaf(MTBDD bdd)
{
    if (bdd == bdd_true || bdd == mtbdd_undefined) return 1;
    return mtbddnode_isleaf(MTBDD_GETNODE(bdd));
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
typedef struct mtbdd_refs_task
{
    lace_task* t;
    void* f;
} *mtbdd_refs_task_t;

typedef struct mtbdd_refs_internal
{
    const MTBDD **pbegin, **pend, **pcur;
    MTBDD *rbegin, *rend, *rcur;
    mtbdd_refs_task_t sbegin, send, scur;
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

TASK(void, mtbdd_refs_mark_s_par, mtbdd_refs_task_t, begin, size_t, count)
void mtbdd_refs_mark_s_par_CALL(lace_worker* lace, mtbdd_refs_task_t begin, size_t count)
{
    if (count < 32) {
        while (count > 0) {
            lace_task* t = begin->t;
            if (!lace_is_stolen_task(t)) return;
            if (t->f == begin->f && lace_is_completed_task(t)) {
                mtbdd_gc_mark(*(MTBDD*)lace_task_result(t));
            }
            begin += 1;
            count -= 1;
        }
    } else {
        if (!lace_is_stolen_task(begin->t)) return;
        mtbdd_refs_mark_s_par_SPAWN(lace, begin, count / 2);
        mtbdd_refs_mark_s_par_CALL(lace, begin + (count / 2), count - count / 2);
        mtbdd_refs_mark_s_par_SYNC(lace);
    }
}

TASK(void, mtbdd_refs_mark_task)

void mtbdd_refs_mark_task_CALL(lace_worker* lace)
{
    mtbdd_refs_mark_p_par_SPAWN(lace, mtbdd_refs_key->pbegin, (size_t)(mtbdd_refs_key->pcur-mtbdd_refs_key->pbegin));
    mtbdd_refs_mark_r_par_SPAWN(lace, mtbdd_refs_key->rbegin, (size_t)(mtbdd_refs_key->rcur-mtbdd_refs_key->rbegin));
    mtbdd_refs_mark_s_par_CALL(lace, mtbdd_refs_key->sbegin, (size_t)(mtbdd_refs_key->scur-mtbdd_refs_key->sbegin));
    mtbdd_refs_mark_r_par_SYNC(lace);
    mtbdd_refs_mark_p_par_SYNC(lace);
}

TASK(void, mtbdd_refs_mark)

void mtbdd_refs_mark_CALL(lace_worker* lace)
{
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
    s->scur = s->sbegin = (mtbdd_refs_task_t)malloc(sizeof(struct mtbdd_refs_task) * 1024);
    s->send = s->sbegin + 1024;
    mtbdd_refs_key = s;
}

TASK(void, mtbdd_refs_free)

void mtbdd_refs_free_CALL(lace_worker* lace)
{
    free(mtbdd_refs_key->pbegin);
    free(mtbdd_refs_key->rbegin);
    free(mtbdd_refs_key->sbegin);
    free(mtbdd_refs_key);
}

TASK(void, mtbdd_refs_init_task)

void mtbdd_refs_init_task_CALL(lace_worker* lace)
{
    mtbdd_refs_init_key();
}

TASK(void, mtbdd_refs_init)

void mtbdd_refs_init_CALL(lace_worker* lace)
{
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

void SYLVAN_NOINLINE
mtbdd_refs_tasks_up(mtbdd_refs_internal_t refs)
{
    size_t size = (size_t)(refs->send - refs->sbegin);
    refs->sbegin = (mtbdd_refs_task_t)realloc(refs->sbegin, sizeof(struct mtbdd_refs_task) * size * 2);
    refs->scur = refs->sbegin + size;
    refs->send = refs->sbegin + (size * 2);
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

void
mtbdd_refs_spawn(lace_task* t)
{
    mtbdd_refs_key->scur->t = t;
    mtbdd_refs_key->scur->f = t->f;
    mtbdd_refs_key->scur += 1;
    if (mtbdd_refs_key->scur == mtbdd_refs_key->send) mtbdd_refs_tasks_up(mtbdd_refs_key);
}

MTBDD
mtbdd_refs_sync(MTBDD result)
{
    mtbdd_refs_key->scur -= 1;
    return result;
}

/**
 * Initialize and quit functions
 */

static int mtbdd_initialized = 0;
static _Atomic(uint32_t) bdd_next_level;

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

    int created;
    uint64_t index = custom ? nodes_lookupc(nodes, n.a, n.b, &created) : nodes_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        sylvan_gc(); // FIXME ?

        index = custom ? nodes_lookupc(nodes, n.a, n.b, &created) : nodes_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", nodes_count_nodes(nodes), nodes_get_size(nodes));
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

MTBDD
mtbdd_makemapnode(uint32_t var, MTBDD low, MTBDD high)
{
    struct mtbddnode n;
    uint64_t index;
    int created;

    // in an MTBDDMAP, the low edges eventually lead to 0 and cannot have a low mark
    assert(!MTBDD_HASMARK(low));

    mtbddnode_makemapnode(&n, var, low, high);
    index = nodes_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        mtbdd_refs_push(low);
        mtbdd_refs_push(high);
        sylvan_gc(); //FIXME
        mtbdd_refs_pop(2);

        index = nodes_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", nodes_count_nodes(nodes), nodes_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return index;
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
        leaf == mtbdd_undefined || leaf == bdd_true || mtbdd_leaf_type(leaf) != 2) {
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

/**
 * Create a MTBDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * Use cube[idx]==3 for "s=s'" in interleaved variables (matches with next variable)
 * <variables> is the cube of variables
 */
MTBDD
mtbdd_cube(MTBDD variables, uint8_t *cube, MTBDD terminal)
{
    if (variables == bdd_true) return terminal;
    mtbddnode* n = MTBDD_GETNODE(variables);

    BDD result;
    switch (*cube) {
    case 0:
        result = mtbdd_cube(node_gethigh(variables, n), cube+1, terminal);
        result = mtbdd_make_node(mtbddnode_getvariable(n), result, mtbdd_undefined);
        return result;
    case 1:
        result = mtbdd_cube(node_gethigh(variables, n), cube+1, terminal);
        result = mtbdd_make_node(mtbddnode_getvariable(n), mtbdd_undefined, result);
        return result;
    case 2:
        return mtbdd_cube(node_gethigh(variables, n), cube+1, terminal);
    case 3:
    {
        MTBDD variables2 = node_gethigh(variables, n);
        mtbddnode* n2 = MTBDD_GETNODE(variables2);
        uint32_t var2 = mtbddnode_getvariable(n2);
        result = mtbdd_cube(node_gethigh(variables2, n2), cube+2, terminal);
        BDD low = mtbdd_make_node(var2, result, mtbdd_undefined);
        mtbdd_refs_push(low);
        BDD high = mtbdd_make_node(var2, mtbdd_undefined, result);
        mtbdd_refs_pop(1);
        result = mtbdd_make_node(mtbddnode_getvariable(n), low, high);
        return result;
    }
    default:
        return mtbdd_undefined; // ?
    }
}

/**
 * Same as mtbdd_cube, but also performs "or" with existing MTBDD,
 * effectively adding an item to the set
 */
MTBDD mtbdd_set_cube_CALL(lace_worker* lace, MTBDD mtbdd, MTBDD vars, uint8_t* cube, MTBDD terminal)
{
    /* Terminal cases */
    if (mtbdd == terminal) return terminal;
    if (mtbdd == mtbdd_undefined) return mtbdd_cube(vars, cube, terminal);
    if (vars == bdd_true) return terminal;

    sylvan_gc_test(lace);

    mtbddnode* nv = MTBDD_GETNODE(vars);
    uint32_t v = mtbddnode_getvariable(nv);

    const int is_leaf = mtbdd_is_leaf(mtbdd);
    mtbddnode* na = is_leaf ? NULL : MTBDD_GETNODE(mtbdd);
    uint32_t va = is_leaf ? UINT32_MAX : mtbddnode_getvariable(na);

    if (va < v) {
        MTBDD low = node_getlow(mtbdd, na);
        MTBDD high = node_gethigh(mtbdd, na);
        mtbdd_refs_spawn(mtbdd_set_cube_SPAWN(lace, high, vars, cube, terminal));
        BDD new_low = mtbdd_set_cube_CALL(lace, low, vars, cube, terminal);
        mtbdd_refs_push(new_low);
        BDD new_high = mtbdd_refs_sync(mtbdd_set_cube_SYNC(lace));
        mtbdd_refs_pop(1);
        if (new_low != low || new_high != high) return mtbdd_make_node(va, new_low, new_high);
        else return mtbdd;
    } else if (va == v) {
        MTBDD low = node_getlow(mtbdd, na);
        MTBDD high = node_gethigh(mtbdd, na);
        switch (*cube) {
        case 0:
        {
            MTBDD new_low = mtbdd_set_cube_CALL(lace, low, node_gethigh(vars, nv), cube+1, terminal);
            if (new_low != low) return mtbdd_make_node(v, new_low, high);
            else return mtbdd;
        }
        case 1:
        {
            MTBDD new_high = mtbdd_set_cube(high, node_gethigh(vars, nv), cube+1, terminal);
            if (new_high != high) return mtbdd_make_node(v, low, new_high);
            return mtbdd;
        }
        case 2:
        {
            mtbdd_refs_spawn(mtbdd_set_cube_SPAWN(lace, high, node_gethigh(vars, nv), cube+1, terminal));
            MTBDD new_low = mtbdd_set_cube(low, node_gethigh(vars, nv), cube+1, terminal);
            mtbdd_refs_push(new_low);
            MTBDD new_high = mtbdd_refs_sync(mtbdd_set_cube_SYNC(lace));
            mtbdd_refs_pop(1);
            if (new_low != low || new_high != high) return mtbdd_make_node(v, new_low, new_high);
            return mtbdd;
        }
        case 3:
        {
            return mtbdd_undefined; // currently not implemented
        }
        default:
            return mtbdd_undefined;
        }
    } else /* va > v */ {
        switch (*cube) {
        case 0:
        {
            MTBDD new_low = mtbdd_set_cube(mtbdd, node_gethigh(vars, nv), cube+1, terminal);
            return mtbdd_make_node(v, new_low, mtbdd);
        }
        case 1:
        {
            MTBDD new_high = mtbdd_set_cube(mtbdd, node_gethigh(vars, nv), cube+1, terminal);
            return mtbdd_make_node(v, mtbdd, new_high);
        }
        case 2:
            return mtbdd_set_cube(mtbdd, node_gethigh(vars, nv), cube+1, terminal);
        case 3:
        {
            return mtbdd_undefined; // currently not implemented
        }
        default:
            return mtbdd_undefined;
        }
    }
}

/**
 * Apply a binary operation <op> to <a> and <b>.
 */
MTBDD mtbdd_apply_CALL(lace_worker* lace, MTBDD a, MTBDD b, mtbdd_apply_cb op)
{
    /* Check terminal case */
    MTBDD result = op(lace, &a, &b);
    if (result != mtbdd_invalid) return result;

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_APPLY);

    /* Check cache */
    if (cache_get3(CACHE_MTBDD_APPLY, a, b, (size_t)op, &result)) {
        sylvan_stats_count(MTBDD_APPLY_CACHED);
        return result;
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
    mtbdd_refs_spawn(mtbdd_apply_SPAWN(lace, ahigh, bhigh, op));
    MTBDD low = mtbdd_refs_push(mtbdd_apply_CALL(lace, alow, blow, op));
    MTBDD high = mtbdd_refs_sync(mtbdd_apply_SYNC(lace));
    mtbdd_refs_pop(1);
    result = mtbdd_make_node(v, low, high);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_APPLY, a, b, (size_t)op, result)) {
        sylvan_stats_count(MTBDD_APPLY_CACHEDPUT);
    }

    return result;
}

/**
 * Apply a binary operation <op> to <a> and <b> with parameter <p>
 */
MTBDD mtbdd_apply_param_CALL(lace_worker* lace, MTBDD a, MTBDD b, size_t p, mtbdd_apply_param_cb op, uint64_t opid)
{
    /* Check terminal case */
    MTBDD result = op(lace, &a, &b, p);
    if (result != mtbdd_invalid) return result;

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_APPLY);

    /* Check cache */
    if (cache_get3(opid, a, b, p, &result)) {
        sylvan_stats_count(MTBDD_APPLY_CACHED);
        return result;
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
    mtbdd_refs_spawn(mtbdd_apply_param_SPAWN(lace, ahigh, bhigh, p, op, opid));
    MTBDD low = mtbdd_refs_push(mtbdd_apply_param_CALL(lace, alow, blow, p, op, opid));
    MTBDD high = mtbdd_refs_sync(mtbdd_apply_param_SYNC(lace));
    mtbdd_refs_pop(1);
    result = mtbdd_make_node(v, low, high);

    /* Store in cache */
    if (cache_put3(opid, a, b, p, result)) {
        sylvan_stats_count(MTBDD_APPLY_CACHEDPUT);
    }

    return result;
}

/**
 * Apply a unary operation <op> to <dd>.
 */
MTBDD mtbdd_apply_unary_CALL(lace_worker* lace, MTBDD dd, mtbdd_apply_unary_cb op, size_t param)
{
    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_UAPPLY);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_UAPPLY, dd, (size_t)op, param, &result)) {
        sylvan_stats_count(MTBDD_UAPPLY_CACHED);
        return result;
    }

    /* Check terminal case */
    result = op(lace, dd, param);
    if (result != mtbdd_invalid) {
        /* Store in cache */
        if (cache_put3(CACHE_MTBDD_UAPPLY, dd, (size_t)op, param, result)) {
            sylvan_stats_count(MTBDD_UAPPLY_CACHEDPUT);
        }

        return result;
    }

    /* Get cofactors */
    mtbddnode* ndd = MTBDD_GETNODE(dd);
    MTBDD ddlow = node_getlow(dd, ndd);
    MTBDD ddhigh = node_gethigh(dd, ndd);

    /* Recursive */
    mtbdd_refs_spawn(mtbdd_apply_unary_SPAWN(lace, ddhigh, op, param));
    MTBDD low = mtbdd_refs_push(mtbdd_apply_unary_CALL(lace, ddlow, op, param));
    MTBDD high = mtbdd_refs_sync(mtbdd_apply_unary_SYNC(lace));
    mtbdd_refs_pop(1);
    result = mtbdd_make_node(mtbddnode_getvariable(ndd), low, high);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_UAPPLY, dd, (size_t)op, param, result)) {
        sylvan_stats_count(MTBDD_UAPPLY_CACHEDPUT);
    }

    return result;
}

TASK(MTBDD, mtbdd_uop_times_uint, MTBDD, a, size_t, k)

MTBDD mtbdd_uop_times_uint_CALL(lace_worker* lace, MTBDD a, size_t k)
{
    if (a == mtbdd_undefined) return mtbdd_undefined;
    if (a == bdd_true) return bdd_true;

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_gettype(na) == 0) {
            int64_t v = mtbdd_leaf_int64(a);
            return mtbdd_int64(v*k);
        } else if (mtbddnode_gettype(na) == 1) {
            double d = mtbdd_leaf_double(a);
            return mtbdd_double(d*k);
        } else if (mtbddnode_gettype(na) == 2) {
            uint64_t v = mtbddnode_getvalue(na);
            int64_t n = (int32_t)(v>>32);
            uint32_t d = (uint32_t)v;
            uint32_t c = gcd(d, (uint32_t)k);
            return mtbdd_fraction(n*(k/c), d/c);
        } else {
            assert(0); // failure
        }
    }

    return mtbdd_invalid;
}

TASK(MTBDD, mtbdd_uop_pow_uint, MTBDD, a, size_t, k)

MTBDD mtbdd_uop_pow_uint_CALL(lace_worker* lace, MTBDD a, size_t k)
{
    if (a == mtbdd_undefined) return mtbdd_undefined;
    if (a == bdd_true) return bdd_true;

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_gettype(na) == 0) {
            int64_t v = mtbdd_leaf_int64(a);
            return mtbdd_int64(pow(v, k));
        } else if (mtbddnode_gettype(na) == 1) {
            double d = mtbdd_leaf_double(a);
            return mtbdd_double(pow(d, k));
        } else if (mtbddnode_gettype(na) == 2) {
            uint64_t v = mtbddnode_getvalue(na);
            return mtbdd_fraction(pow((int32_t)(v>>32), k), (uint32_t)v);
        } else {
            assert(0); // failure
        }
    }

    return mtbdd_invalid;
}

static MTBDD
mtbdd_uapply_power_of_two(MTBDD a, mtbdd_apply_unary_cb op, unsigned int k)
{
    const unsigned int max_shift = (unsigned int)(sizeof(size_t) * CHAR_BIT - 1);
    const size_t max_factor = (size_t)1 << max_shift;
    MTBDD result = a;

    while (k > max_shift) {
        mtbdd_refs_push(result);
        result = mtbdd_apply_unary(result, op, max_factor);
        mtbdd_refs_pop(1);
        if (result == mtbdd_invalid) return mtbdd_invalid;
        k -= max_shift;
    }

    mtbdd_refs_push(result);
    result = mtbdd_apply_unary(result, op, (size_t)1 << k);
    mtbdd_refs_pop(1);
    return result;
}

MTBDD mtbdd_abstract_op_plus_CALL(lace_worker* lace, MTBDD a, MTBDD b, int k)
{
    if (k < 0) {
        return mtbdd_invalid;
    } else if (k == 0) {
        return mtbdd_apply(a, b, mtbdd_op_plus_CALL);
    } else {
        return mtbdd_uapply_power_of_two(a, mtbdd_uop_times_uint_CALL, (unsigned int)k);
    }
}

MTBDD mtbdd_abstract_op_times_CALL(lace_worker* lace, MTBDD a, MTBDD b, int k)
{
    if (k < 0) {
        return mtbdd_invalid;
    } else if (k == 0) {
        return mtbdd_apply(a, b, mtbdd_op_times_CALL);
    } else {
        return mtbdd_uapply_power_of_two(a, mtbdd_uop_pow_uint_CALL, (unsigned int)k);
    }
}

MTBDD mtbdd_abstract_op_min_CALL(lace_worker* lace, MTBDD a, MTBDD b, int k)
{
    return k == 0 ? mtbdd_apply(a, b, mtbdd_op_min_CALL) : a;
}

MTBDD mtbdd_abstract_op_max_CALL(lace_worker* lace, MTBDD a, MTBDD b, int k)
{
    return k == 0 ? mtbdd_apply(a, b, mtbdd_op_max_CALL) : a;
}

/**
 * Abstract the variables in <v> from <a> using the operation <op>
 */
MTBDD mtbdd_abstract_CALL(lace_worker* lace, MTBDD a, MTBDD v, mtbdd_abstract_cb op)
{
    /* Check terminal case */
    if (a == mtbdd_undefined) return mtbdd_undefined;
    if (a == bdd_true) return bdd_true;
    if (v == bdd_true) return a;

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
                fprintf(stderr, "mtbdd_abstract: variable count exceeds INT_MAX\n");
                return mtbdd_invalid;
            }
            k++;
            v = node_gethigh(v, MTBDD_GETNODE(v));
        }

        /* Check cache */
        MTBDD result;
        const int cacheable = (unsigned int)k <= UINT32_C(0xffffff);
        const uint64_t cache_key = cacheable
            ? (v & UINT64_C(0x000000ffffffffff)) | ((uint64_t)(unsigned int)k << 40)
            : 0;
        if (cacheable && cache_get3(CACHE_MTBDD_ABSTRACT, a, cache_key, (size_t)op, &result)) {
            sylvan_stats_count(MTBDD_ABSTRACT_CACHED);
            return result;
        }

        /* Compute result */
        result = op(lace, a, a, k);

        /* Store in cache */
        if (cacheable && cache_put3(CACHE_MTBDD_ABSTRACT, a, cache_key, (size_t)op, result)) {
            sylvan_stats_count(MTBDD_ABSTRACT_CACHEDPUT);
        }

        return result;
    }

    /* Possibly skip k variables */
    mtbddnode* nv = MTBDD_GETNODE(v);
    uint32_t var_a = mtbddnode_getvariable(na);
    uint32_t var_v = mtbddnode_getvariable(nv);
    int k = 0;
    while (var_v < var_a) {
        if (k == INT_MAX) {
            fprintf(stderr, "mtbdd_abstract: variable count exceeds INT_MAX\n");
            return mtbdd_invalid;
        }
        k++;
        v = node_gethigh(v, nv);
        if (v == bdd_true) break;
        nv = MTBDD_GETNODE(v);
        var_v = mtbddnode_getvariable(nv);
    }

    /* Check cache */
    MTBDD result;
    const int cacheable = (unsigned int)k <= UINT32_C(0xffffff);
    const uint64_t cache_key = cacheable
        ? (v & UINT64_C(0x000000ffffffffff)) | ((uint64_t)(unsigned int)k << 40)
        : 0;
    if (cacheable && cache_get3(CACHE_MTBDD_ABSTRACT, a, cache_key, (size_t)op, &result)) {
        sylvan_stats_count(MTBDD_ABSTRACT_CACHED);
        return result;
    }

    /* Recursive */
    if (v == bdd_true) {
        result = a;
    } else if (var_a < var_v) {
        mtbdd_refs_spawn(mtbdd_abstract_SPAWN(lace, node_gethigh(a, na), v, op));
        MTBDD low = mtbdd_refs_push(mtbdd_abstract_CALL(lace, node_getlow(a, na), v, op));
        MTBDD high = mtbdd_refs_sync(mtbdd_abstract_SYNC(lace));
        mtbdd_refs_pop(1);
        result = mtbdd_make_node(var_a, low, high);
    } else /* var_a == var_v */ {
        mtbdd_refs_spawn(mtbdd_abstract_SPAWN(lace, node_gethigh(a, na), node_gethigh(v, nv), op));
        MTBDD low = mtbdd_refs_push(mtbdd_abstract_CALL(lace, node_getlow(a, na), node_gethigh(v, nv), op));
        MTBDD high = mtbdd_refs_push(mtbdd_refs_sync(mtbdd_abstract_SYNC(lace)));
        result = op(lace, low, high, 0);
        mtbdd_refs_pop(2);
    }

    if (k) {
        mtbdd_refs_push(result);
        result = op(lace, result, result, k);
        mtbdd_refs_pop(1);
    }

    /* Store in cache */
    if (cacheable && cache_put3(CACHE_MTBDD_ABSTRACT, a, cache_key, (size_t)op, result)) {
        sylvan_stats_count(MTBDD_ABSTRACT_CACHEDPUT);
    }

    return result;
}

/**
 * Binary operation Plus (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDDs, mtbdd_undefined is interpreted as "0" or "0.0".
 */
MTBDD mtbdd_op_plus_CALL(lace_worker* lace, MTBDD* pa, MTBDD* pb)
{
    MTBDD a = *pa, b = *pb;
    if (a == mtbdd_undefined) return b;
    if (b == mtbdd_undefined) return a;

    // Handle Boolean MTBDDs: interpret as Or
    if (a == bdd_true) return bdd_true;
    if (b == bdd_true) return bdd_true;

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        uint64_t val_a = mtbddnode_getvalue(na);
        uint64_t val_b = mtbddnode_getvalue(nb);
        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // both integer
            return mtbdd_int64(*(int64_t*)(&val_a) + *(int64_t*)(&val_b));
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // both double
            return mtbdd_double(*(double*)(&val_a) + *(double*)(&val_b));
        } else if (mtbddnode_gettype(na) == 2 && mtbddnode_gettype(nb) == 2) {
            // both fraction
            int64_t nom_a = (int32_t)(val_a>>32);
            int64_t nom_b = (int32_t)(val_b>>32);
            uint64_t denom_a = val_a&0xffffffff;
            uint64_t denom_b = val_b&0xffffffff;
            // common cases
            if (nom_a == 0) return b;
            if (nom_b == 0) return a;
            // equalize denominators
            uint32_t c = gcd((uint32_t)denom_a, (uint32_t)denom_b);
            nom_a *= denom_b/c;
            nom_b *= denom_a/c;
            denom_a *= denom_b/c;
            // add
            return mtbdd_fraction(nom_a + nom_b, denom_a);
        } else {
            assert(0); // failure
        }
    }

    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return mtbdd_invalid;
}

/**
 * Binary operation Minus (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDDs, mtbdd_undefined is interpreted as "0" or "0.0".
 */
MTBDD mtbdd_op_minus_CALL(lace_worker* lace, MTBDD* pa, MTBDD* pb)
{
    MTBDD a = *pa, b = *pb;
    if (a == mtbdd_undefined) return mtbdd_neg(b);
    if (b == mtbdd_undefined) return a;

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        uint64_t val_a = mtbddnode_getvalue(na);
        uint64_t val_b = mtbddnode_getvalue(nb);
        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // both integer
            return mtbdd_int64(*(int64_t*)(&val_a) - *(int64_t*)(&val_b));
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // both double
            return mtbdd_double(*(double*)(&val_a) - *(double*)(&val_b));
        } else if (mtbddnode_gettype(na) == 2 && mtbddnode_gettype(nb) == 2) {
            // both fraction
            int64_t nom_a = (int32_t)(val_a>>32);
            int64_t nom_b = (int32_t)(val_b>>32);
            uint64_t denom_a = val_a&0xffffffff;
            uint64_t denom_b = val_b&0xffffffff;
            // common cases
            if (nom_b == 0) return a;
            // equalize denominators
            uint32_t c = gcd((uint32_t)denom_a, (uint32_t)denom_b);
            nom_a *= denom_b/c;
            nom_b *= denom_a/c;
            denom_a *= denom_b/c;
            // subtract
            return mtbdd_fraction(nom_a - nom_b, denom_a);
        } else {
            assert(0); // failure
        }
    }

    return mtbdd_invalid;
}

/**
 * Binary operation Times (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is mtbdd_undefined (i.e. not defined).
 */
MTBDD mtbdd_op_times_CALL(lace_worker* lace, MTBDD* pa, MTBDD* pb)
{
    MTBDD a = *pa, b = *pb;
    if (a == mtbdd_undefined || b == mtbdd_undefined) return mtbdd_undefined;

    // Handle Boolean MTBDDs: interpret as And
    if (a == bdd_true) return b;
    if (b == bdd_true) return a;

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        uint64_t val_a = mtbddnode_getvalue(na);
        uint64_t val_b = mtbddnode_getvalue(nb);
        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // both integer
            int64_t i_a = *(int64_t*)(&val_a);
            int64_t i_b = *(int64_t*)(&val_b);
            if (i_a == 0) return a;
            if (i_b == 0) return b;
            if (i_a == 1) return b;
            if (i_b == 1) return a;
            return mtbdd_int64(i_a * i_b);
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // both double
            double d_a = *(double*)(&val_a);
            double d_b = *(double*)(&val_b);
            if (d_a == 0.0) return a;
            if (d_a == 1.0) return b;
            if (d_b == 0.0) return b;
            if (d_b == 1.0) return a;
            return mtbdd_double(d_a * d_b);
        } else if (mtbddnode_gettype(na) == 2 && mtbddnode_gettype(nb) == 2) {
            // both fraction
            int64_t nom_a = (int32_t)(val_a>>32);
            int64_t nom_b = (int32_t)(val_b>>32);
            uint64_t denom_a = val_a&0xffffffff;
            uint64_t denom_b = val_b&0xffffffff;
            if (nom_a == 0) return a;
            if (nom_b == 0) return b;
            // multiply!
            uint32_t c = gcd((uint32_t)(nom_b < 0 ? -nom_b : nom_b), (uint32_t)denom_a);
            uint32_t d = gcd((uint32_t)(nom_a < 0 ? -nom_a : nom_a), (uint32_t)denom_b);
            nom_a /= d;
            denom_a /= c;
            nom_a *= (nom_b/c);
            denom_a *= (denom_b/d);
            return mtbdd_fraction(nom_a, denom_a);
        } else {
            assert(0); // failure
        }
    }

    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return mtbdd_invalid;
}

/**
 * Binary operation Minimum (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is the other operand.
 */
MTBDD mtbdd_op_min_CALL(lace_worker* lace, MTBDD* pa, MTBDD* pb)
{
    MTBDD a = *pa, b = *pb;
    if (a == bdd_true) return b;
    if (b == bdd_true) return a;
    if (a == b) return a;

    // Special case where "false" indicates a partial function
    if (a == mtbdd_undefined && b != mtbdd_undefined && mtbddnode_isleaf(MTBDD_GETNODE(b))) return b;
    if (b == mtbdd_undefined && a != mtbdd_undefined && mtbddnode_isleaf(MTBDD_GETNODE(a))) return a;

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        uint64_t val_a = mtbddnode_getvalue(na);
        uint64_t val_b = mtbddnode_getvalue(nb);
        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // both integer
            int64_t va = *(int64_t*)(&val_a);
            int64_t vb = *(int64_t*)(&val_b);
            return va < vb ? a : b;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // both double
            double va = *(double*)&val_a;
            double vb = *(double*)&val_b;
            return va < vb ? a : b;
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
            return nom_a < nom_b ? a : b;
        } else {
            assert(0); // failure
        }
    }

    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return mtbdd_invalid;
}

/**
 * Binary operation Maximum (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is the other operand.
 */
MTBDD mtbdd_op_max_CALL(lace_worker* lace, MTBDD* pa, MTBDD* pb)
{
    MTBDD a = *pa, b = *pb;
    if (a == bdd_true) return a;
    if (b == bdd_true) return b;
    if (a == mtbdd_undefined) return b;
    if (b == mtbdd_undefined) return a;
    if (a == b) return a;

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);

    if (mtbddnode_isleaf(na) && mtbddnode_isleaf(nb)) {
        uint64_t val_a = mtbddnode_getvalue(na);
        uint64_t val_b = mtbddnode_getvalue(nb);
        if (mtbddnode_gettype(na) == 0 && mtbddnode_gettype(nb) == 0) {
            // both integer
            int64_t va = *(int64_t*)(&val_a);
            int64_t vb = *(int64_t*)(&val_b);
            return va > vb ? a : b;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // both double
            double vval_a = *(double*)&val_a;
            double vval_b = *(double*)&val_b;
            return vval_a > vval_b ? a : b;
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
            return nom_a > nom_b ? a : b;
        } else {
            assert(0); // failure
        }
    }

    if (a < b) {
        *pa = b;
        *pb = a;
    }

    return mtbdd_invalid;
}

MTBDD mtbdd_op_cmpl_CALL(lace_worker* lace, MTBDD a, size_t k)
{
    // if a is false, then it is a partial function. Keep partial!
    if (a == mtbdd_undefined) return mtbdd_undefined;

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_gettype(na) == 0) {
            int64_t v = mtbdd_leaf_int64(a);
            if (v == 0) return mtbdd_int64(1);
            else return mtbdd_int64(0);
        } else if (mtbddnode_gettype(na) == 1) {
            double d = mtbdd_leaf_double(a);
            if (d == 0.0) return mtbdd_double(1.0);
            else return mtbdd_double(0.0);
        } else if (mtbddnode_gettype(na) == 2) {
            uint64_t v = mtbddnode_getvalue(na);
            if (v == 1) return mtbdd_fraction(1, 1);
            else return mtbdd_fraction(0, 1);
        } else {
            assert(0); // failure
        }
    }

    return mtbdd_invalid;
    (void)k; // unused variable
}

MTBDD mtbdd_op_negate_CALL(lace_worker* lace, MTBDD a, size_t k)
{
    // if a is false, then it is a partial function. Keep partial!
    if (a == mtbdd_undefined) return mtbdd_undefined;

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        if (mtbddnode_gettype(na) == 0) {
            int64_t v = mtbdd_leaf_int64(a);
            return mtbdd_int64(-v);
        } else if (mtbddnode_gettype(na) == 1) {
            double d = mtbdd_leaf_double(a);
            return mtbdd_double(-d);
        } else if (mtbddnode_gettype(na) == 2) {
            uint64_t v = mtbddnode_getvalue(na);
            return mtbdd_fraction(-(int32_t)(v>>32), (uint32_t)v);
        } else {
            assert(0); // failure
        }
    }

    return mtbdd_invalid;
    (void)k; // unused variable
}

/**
 * Compute IF <f> THEN <g> ELSE <h>.
 * <f> must be a Boolean MTBDD (or standard BDD).
 */
MTBDD mtbdd_ite_CALL(lace_worker* lace, BDD f, MTBDD g, MTBDD h)
{
    /* Terminal cases */
    if (f == bdd_true) return g;
    if (f == mtbdd_undefined) return h;
    if (g == h) return g;
    if (g == bdd_true && h == mtbdd_undefined) return f;
    if (h == bdd_true && g == mtbdd_undefined) return MTBDD_TOGGLEMARK(f);

    // If all MTBDD's are Boolean, then there could be further optimizations (see sylvan_bdd.c)

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_ITE);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_ITE, f, g, h, &result)) {
        sylvan_stats_count(MTBDD_ITE_CACHED);
        return result;
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
    mtbdd_refs_spawn(mtbdd_ite_SPAWN(lace, fhigh, ghigh, hhigh));
    MTBDD low = mtbdd_refs_push(mtbdd_ite_CALL(lace, flow, glow, hlow));
    MTBDD high = mtbdd_refs_sync(mtbdd_ite_SYNC(lace));
    mtbdd_refs_pop(1);
    result = mtbdd_make_node(v, low, high);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_ITE, f, g, h, result)) {
        sylvan_stats_count(MTBDD_ITE_CACHEDPUT);
    }

    return result;
}

/**
 * Monad that converts double/fraction to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 */
MTBDD mtbdd_op_threshold_double_CALL(lace_worker* lace, MTBDD a, size_t svalue)
{
    /* We only expect "double" terminals, or false */
    if (a == mtbdd_undefined) return mtbdd_undefined;
    if (a == bdd_true) return mtbdd_invalid;

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        double value = *(double*)&svalue;
        if (mtbddnode_gettype(na) == 1) {
            return mtbdd_leaf_double(a) >= value ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 2) {
            double d = (double)mtbdd_fraction_numerator(a);
            d /= mtbdd_fraction_denominator(a);
            return d >= value ? bdd_true : mtbdd_undefined;
        } else {
            assert(0); // failure
        }
    }

    return mtbdd_invalid;
}

/**
 * Monad that converts double/fraction to a Boolean BDD, translate terminals > value to 1 and to 0 otherwise;
 */
MTBDD mtbdd_op_strict_threshold_double_CALL(lace_worker* lace, MTBDD a, size_t svalue)
{
    /* We only expect "double" terminals, or false */
    if (a == mtbdd_undefined) return mtbdd_undefined;
    if (a == bdd_true) return mtbdd_invalid;

    // a != constant
    mtbddnode* na = MTBDD_GETNODE(a);

    if (mtbddnode_isleaf(na)) {
        double value = *(double*)&svalue;
        if (mtbddnode_gettype(na) == 1) {
            return mtbdd_leaf_double(a) > value ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 2) {
            double d = (double)mtbdd_fraction_numerator(a);
            d /= mtbdd_fraction_denominator(a);
            return d > value ? bdd_true : mtbdd_undefined;
        } else {
            assert(0); // failure
        }
    }

    return mtbdd_invalid;
}

MTBDD mtbdd_threshold_double_CALL(lace_worker* lace, MTBDD dd, double d)
{
    return mtbdd_apply_unary(dd, mtbdd_op_threshold_double_CALL, *(size_t*)&d);
}

MTBDD mtbdd_strict_threshold_double_CALL(lace_worker* lace, MTBDD dd, double d)
{
    return mtbdd_apply_unary(dd, mtbdd_op_strict_threshold_double_CALL, *(size_t*)&d);
}

/**
 * Compare two Double MTBDDs, returns Boolean True if they are equal within some value epsilon
 */
TASK(MTBDD, mtbdd_equal_norm_d2, MTBDD, a, MTBDD, b, size_t, svalue, int*, shortcircuit)

MTBDD mtbdd_equal_norm_d2_CALL(lace_worker* lace, MTBDD a, MTBDD b, size_t svalue, int* shortcircuit)
{
    /* Check short circuit */
    if (*shortcircuit) return mtbdd_undefined;

    /* Check terminal case */
    if (a == b) return bdd_true;
    if (a == mtbdd_undefined) return mtbdd_undefined;
    if (b == mtbdd_undefined) return mtbdd_undefined;

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    int la = mtbddnode_isleaf(na);
    int lb = mtbddnode_isleaf(nb);

    if (la && lb) {
        // assume Double MTBDD
        double va = mtbdd_leaf_double(a);
        double vb = mtbdd_leaf_double(b);
        va -= vb;
        if (va < 0) va = -va;
        return (va < *(double*)&svalue) ? bdd_true : mtbdd_undefined;
    }

    if (b < a) {
        MTBDD t = a;
        a = b;
        b = t;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_EQUAL_NORM);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_EQUAL_NORM, a, b, svalue, &result)) {
        sylvan_stats_count(MTBDD_EQUAL_NORM_CACHED);
        return result;
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

    mtbdd_equal_norm_d2_SPAWN(lace, ahigh, bhigh, svalue, shortcircuit);
    result = mtbdd_equal_norm_d2_CALL(lace, alow, blow, svalue, shortcircuit);
    if (result == mtbdd_undefined) *shortcircuit = 1;
    if (result != mtbdd_equal_norm_d2_SYNC(lace)) result = mtbdd_undefined;
    if (result == mtbdd_undefined) *shortcircuit = 1;

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_EQUAL_NORM, a, b, svalue, result)) {
        sylvan_stats_count(MTBDD_EQUAL_NORM_CACHEDPUT);
    }

    return result;
}

MTBDD mtbdd_equal_abs_double_CALL(lace_worker* lace, MTBDD a, MTBDD b, double d)
{
    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    int shortcircuit = 0;
    return mtbdd_equal_norm_d2_CALL(lace, a, b, *(size_t*)&d, &shortcircuit);
}

/**
 * Compare two Double MTBDDs, returns Boolean True if they are equal within some value epsilon
 * This version computes the relative difference vs the value in a.
 */
TASK(MTBDD, mtbdd_equal_norm_rel_d2, MTBDD, a, MTBDD, b, size_t, svalue, int*, shortcircuit)
MTBDD mtbdd_equal_norm_rel_d2_CALL(lace_worker* lace, MTBDD a, MTBDD b, size_t svalue, int* shortcircuit)
{
    /* Check short circuit */
    if (*shortcircuit) return mtbdd_undefined;

    /* Check terminal case */
    if (a == b) return bdd_true;
    if (a == mtbdd_undefined) return mtbdd_undefined;
    if (b == mtbdd_undefined) return mtbdd_undefined;

    mtbddnode* na = MTBDD_GETNODE(a);
    mtbddnode* nb = MTBDD_GETNODE(b);
    int la = mtbddnode_isleaf(na);
    int lb = mtbddnode_isleaf(nb);

    if (la && lb) {
        // assume Double MTBDD
        double va = mtbdd_leaf_double(a);
        double vb = mtbdd_leaf_double(b);
        if (va == 0) return mtbdd_undefined;
        va = (va - vb) / va;
        if (va < 0) va = -va;
        return (va < *(double*)&svalue) ? bdd_true : mtbdd_undefined;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_EQUAL_NORM_REL);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_EQUAL_NORM_REL, a, b, svalue, &result)) {
        sylvan_stats_count(MTBDD_EQUAL_NORM_REL_CACHED);
        return result;
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

    mtbdd_equal_norm_rel_d2_SPAWN(lace, ahigh, bhigh, svalue, shortcircuit);
    result = mtbdd_equal_norm_rel_d2_CALL(lace, alow, blow, svalue, shortcircuit);
    if (result == mtbdd_undefined) *shortcircuit = 1;
    if (result != mtbdd_equal_norm_rel_d2_SYNC(lace)) result = mtbdd_undefined;
    if (result == mtbdd_undefined) *shortcircuit = 1;

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_EQUAL_NORM_REL, a, b, svalue, result)) {
        sylvan_stats_count(MTBDD_EQUAL_NORM_REL_CACHEDPUT);
    }

    return result;
}

MTBDD mtbdd_equal_rel_double_CALL(lace_worker* lace, MTBDD a, MTBDD b, double d)
{
    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    int shortcircuit = 0;
    return mtbdd_equal_norm_rel_d2_CALL(lace, a, b, *(size_t*)&d, &shortcircuit);
}

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) <= b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(MTBDD, mtbdd_leq_rec, MTBDD, a, MTBDD, b, int*, shortcircuit)

MTBDD mtbdd_leq_rec_CALL(lace_worker* lace, MTBDD a, MTBDD b, int* shortcircuit)
{
    /* Check short circuit */
    if (*shortcircuit) return mtbdd_undefined;

    /* Check terminal case */
    if (a == b) return bdd_true;

    /* For partial functions, just return true */
    if (a == mtbdd_undefined) return bdd_true;
    if (b == mtbdd_undefined) return bdd_true;

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_LEQ);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_LEQ, a, b, 0, &result)) {
        sylvan_stats_count(MTBDD_LEQ_CACHED);
        return result;
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
            result = *(int64_t*)(&va) <= *(int64_t*)(&vb) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // type 1 = double
            double vva = *(double*)&va;
            double vvb = *(double*)&vb;
            result = vva <= vvb ? bdd_true : mtbdd_undefined;
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
            result = nom_a <= nom_b ? bdd_true : mtbdd_undefined;
        } else {
            assert(0); // failure
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

        mtbdd_leq_rec_SPAWN(lace, ahigh, bhigh, shortcircuit);
        result = mtbdd_leq_rec_CALL(lace, alow, blow, shortcircuit);
        if (result != mtbdd_leq_rec_SYNC(lace)) result = mtbdd_undefined;
    }

    if (result == mtbdd_undefined) *shortcircuit = 1;

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_LEQ, a, b, 0, result)) {
        sylvan_stats_count(MTBDD_LEQ_CACHEDPUT);
    }

    return result;
}

MTBDD mtbdd_leq_CALL(lace_worker* lace, MTBDD a, MTBDD b)
{
    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    int shortcircuit = 0;
    return mtbdd_leq_rec_CALL(lace, a, b, &shortcircuit);
}

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) < b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(MTBDD, mtbdd_less_rec, MTBDD, a, MTBDD, b, int*, shortcircuit)

MTBDD mtbdd_less_rec_CALL(lace_worker* lace, MTBDD a, MTBDD b, int* shortcircuit)
{
    /* Check short circuit */
    if (*shortcircuit) return mtbdd_undefined;

    /* Check terminal case */
    if (a == b) return mtbdd_undefined;

    /* For partial functions, just return true */
    if (a == mtbdd_undefined) return bdd_true;
    if (b == mtbdd_undefined) return bdd_true;

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_LESS);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_LESS, a, b, 0, &result)) {
        sylvan_stats_count(MTBDD_LESS_CACHED);
        return result;
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
            result = *(int64_t*)(&va) < *(int64_t*)(&vb) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // type 1 = double
            double vva = *(double*)&va;
            double vvb = *(double*)&vb;
            result = vva < vvb ? bdd_true : mtbdd_undefined;
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
            result = nom_a < nom_b ? bdd_true : mtbdd_undefined;
        } else {
            assert(0); // failure
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

        mtbdd_less_rec_SPAWN(lace, ahigh, bhigh, shortcircuit);
        result = mtbdd_less_rec_CALL(lace, alow, blow, shortcircuit);
        if (result != mtbdd_less_rec_SYNC(lace)) result = mtbdd_undefined;
    }

    if (result == mtbdd_undefined) *shortcircuit = 1;

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_LESS, a, b, 0, result)) {
        sylvan_stats_count(MTBDD_LESS_CACHEDPUT);
    }

    return result;
}

MTBDD mtbdd_lt_CALL(lace_worker* lace, MTBDD a, MTBDD b)
{
    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    int shortcircuit = 0;
    return mtbdd_less_rec_CALL(lace, a, b, &shortcircuit);
}

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) >= b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(MTBDD, mtbdd_geq_rec, MTBDD, a, MTBDD, b, int*, shortcircuit)

MTBDD mtbdd_geq_rec_CALL(lace_worker* lace, MTBDD a, MTBDD b, int* shortcircuit)
{
    /* Check short circuit */
    if (*shortcircuit) return mtbdd_undefined;

    /* Check terminal case */
    if (a == b) return bdd_true;

    /* For partial functions, just return true */
    if (a == mtbdd_undefined) return bdd_true;
    if (b == mtbdd_undefined) return bdd_true;

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_GEQ);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_GEQ, a, b, 0, &result)) {
        sylvan_stats_count(MTBDD_GEQ_CACHED);
        return result;
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
            result = *(int64_t*)(&va) >= *(int64_t*)(&vb) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // type 1 = double
            double vva = *(double*)&va;
            double vvb = *(double*)&vb;
            result = vva >= vvb ? bdd_true : mtbdd_undefined;
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
            result = nom_a >= nom_b ? bdd_true : mtbdd_undefined;
        } else {
            assert(0); // failure
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

        mtbdd_geq_rec_SPAWN(lace, ahigh, bhigh, shortcircuit);
        result = mtbdd_geq_rec_CALL(lace, alow, blow, shortcircuit);
        if (result != mtbdd_geq_rec_SYNC(lace)) result = mtbdd_undefined;
    }

    if (result == mtbdd_undefined) *shortcircuit = 1;

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_GEQ, a, b, 0, result)) {
        sylvan_stats_count(MTBDD_GEQ_CACHEDPUT);
    }

    return result;
}

MTBDD mtbdd_geq_CALL(lace_worker* lace, MTBDD a, MTBDD b)
{
    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    int shortcircuit = 0;
    return mtbdd_geq_rec_CALL(lace, a, b, &shortcircuit);
}

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) > b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(MTBDD, mtbdd_greater_rec, MTBDD, a, MTBDD, b, int*, shortcircuit)

MTBDD mtbdd_greater_rec_CALL(lace_worker* lace, MTBDD a, MTBDD b, int* shortcircuit)
{
    /* Check short circuit */
    if (*shortcircuit) return mtbdd_undefined;

    /* Check terminal case */
    if (a == b) return mtbdd_undefined;

    /* For partial functions, just return true */
    if (a == mtbdd_undefined) return bdd_true;
    if (b == mtbdd_undefined) return bdd_true;

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_GREATER);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_GREATER, a, b, 0, &result)) {
        sylvan_stats_count(MTBDD_GREATER_CACHED);
        return result;
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
            result = *(int64_t*)(&va) > *(int64_t*)(&vb) ? bdd_true : mtbdd_undefined;
        } else if (mtbddnode_gettype(na) == 1 && mtbddnode_gettype(nb) == 1) {
            // type 1 = double
            double vva = *(double*)&va;
            double vvb = *(double*)&vb;
            result = vva > vvb ? bdd_true : mtbdd_undefined;
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
            result = nom_a > nom_b ? bdd_true : mtbdd_undefined;
        } else {
            assert(0); // failure
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

        mtbdd_greater_rec_SPAWN(lace, ahigh, bhigh, shortcircuit);
        result = mtbdd_greater_rec_CALL(lace, alow, blow, shortcircuit);
        if (result != mtbdd_greater_rec_SYNC(lace)) result = mtbdd_undefined;
    }

    if (result == mtbdd_undefined) *shortcircuit = 1;

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_GREATER, a, b, 0, result)) {
        sylvan_stats_count(MTBDD_GREATER_CACHEDPUT);
    }

    return result;
}

MTBDD mtbdd_gt_CALL(lace_worker* lace, MTBDD a, MTBDD b)
{
    /* the implementation checks shortcircuit in every task and if the two
       MTBDDs are not equal module epsilon, then the computation tree quickly aborts */
    int shortcircuit = 0;
    return mtbdd_greater_rec_CALL(lace, a, b, &shortcircuit);
}

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 */
MTBDD mtbdd_mul_abstract_add_CALL(lace_worker* lace, MTBDD a, MTBDD b, MTBDD v)
{
    /* Check terminal case */
    if (v == bdd_true) return mtbdd_apply(a, b, mtbdd_op_times_CALL);
    MTBDD result = mtbdd_op_times_CALL(lace, &a, &b);
    if (result != mtbdd_invalid) {
        mtbdd_refs_push(result);
        result = mtbdd_abstract(result, v, mtbdd_abstract_op_plus_CALL);
        mtbdd_refs_pop(1);
        return result;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_AND_ABSTRACT_PLUS);

    /* Check cache */
    if (cache_get3(CACHE_MTBDD_AND_ABSTRACT_PLUS, a, b, v, &result)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_PLUS_CACHED);
        return result;
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
        result = mtbdd_mul_abstract_add_CALL(lace, a, b, node_gethigh(v, nv));
        mtbdd_refs_push(result);
        result = mtbdd_apply(result, result, mtbdd_op_plus_CALL);
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
            mtbdd_refs_spawn(mtbdd_mul_abstract_add_SPAWN(lace, ahigh, bhigh, node_gethigh(v, nv)));
            MTBDD low = mtbdd_refs_push(mtbdd_mul_abstract_add_CALL(lace, alow, blow, node_gethigh(v, nv)));
            MTBDD high = mtbdd_refs_push(mtbdd_refs_sync(mtbdd_mul_abstract_add_SYNC(lace)));
            result = mtbdd_apply_CALL(lace, low, high, mtbdd_op_plus_CALL);
            mtbdd_refs_pop(2);
        } else /* vv > v */ {
            /* Recursive, then create node */
            mtbdd_refs_spawn(mtbdd_mul_abstract_add_SPAWN(lace, ahigh, bhigh, v));
            MTBDD low = mtbdd_refs_push(mtbdd_mul_abstract_add_CALL(lace, alow, blow, v));
            MTBDD high = mtbdd_refs_sync(mtbdd_mul_abstract_add_SYNC(lace));
            mtbdd_refs_pop(1);
            result = mtbdd_make_node(var, low, high);
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
MTBDD mtbdd_mul_abstract_max_CALL(lace_worker* lace, MTBDD a, MTBDD b, MTBDD v)
{
    /* Check terminal case */
    if (v == bdd_true) return mtbdd_apply(a, b, mtbdd_op_times_CALL);
    MTBDD result = mtbdd_op_times_CALL(lace, &a, &b);
    if (result != mtbdd_invalid) {
        mtbdd_refs_push(result);
        result = mtbdd_abstract(result, v, mtbdd_abstract_op_max_CALL);
        mtbdd_refs_pop(1);
        return result;
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
        if (v == bdd_true) return mtbdd_apply(a, b, mtbdd_op_times_CALL);
        nv = MTBDD_GETNODE(v);
        vv = mtbddnode_getvariable(nv);
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_AND_ABSTRACT_MAX);

    /* Check cache */
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
        mtbdd_refs_spawn(mtbdd_mul_abstract_max_SPAWN(lace, ahigh, bhigh, node_gethigh(v, nv)));
        MTBDD low = mtbdd_refs_push(mtbdd_mul_abstract_max_CALL(lace, alow, blow, node_gethigh(v, nv)));
        MTBDD high = mtbdd_refs_push(mtbdd_refs_sync(mtbdd_mul_abstract_max_SYNC(lace)));
        result = mtbdd_apply_CALL(lace, low, high, mtbdd_op_max_CALL);
        mtbdd_refs_pop(2);
    } else /* vv > v */ {
        /* Recursive, then create node */
        mtbdd_refs_spawn(mtbdd_mul_abstract_max_SPAWN(lace, ahigh, bhigh, v));
        MTBDD low = mtbdd_refs_push(mtbdd_mul_abstract_max_CALL(lace, alow, blow, v));
        MTBDD high = mtbdd_refs_sync(mtbdd_mul_abstract_max_SYNC(lace));
        mtbdd_refs_pop(1);
        result = mtbdd_make_node(var, low, high);
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_AND_ABSTRACT_MAX, a, b, v, result)) {
        sylvan_stats_count(MTBDD_AND_ABSTRACT_MAX_CACHEDPUT);
    }

    return result;
}

/**
 * Calculate the support of a MTBDD, i.e. the cube of all variables that appear in the MTBDD nodes.
 */
MTBDD mtbdd_support_CALL(lace_worker* lace, MTBDD dd)
{
    /* Terminal case */
    if (mtbdd_is_leaf(dd)) return bdd_true;

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_SUPPORT);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_SUPPORT, dd, 0, 0, &result)) {
        sylvan_stats_count(BDD_SUPPORT_CACHED);
        return result;
    }

    /* Recursive calls */
    mtbddnode* n = MTBDD_GETNODE(dd);
    mtbdd_refs_spawn(mtbdd_support_SPAWN(lace, node_getlow(dd, n)));
    MTBDD high = mtbdd_refs_push(mtbdd_support_CALL(lace, node_gethigh(dd, n)));
    MTBDD low = mtbdd_refs_push(mtbdd_refs_sync(mtbdd_support_SYNC(lace)));

    /* Compute result */
    result = mtbdd_make_node(mtbddnode_getvariable(n), mtbdd_undefined, bdd_and_legacy_CALL(lace, low, high));
    mtbdd_refs_pop(2);

    /* Write to cache */
    if (cache_put3(CACHE_MTBDD_SUPPORT, dd, 0, 0, result)) {
        sylvan_stats_count(BDD_SUPPORT_CACHEDPUT);
    }

    return result;
}

/**
 * Function composition, for each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of mtbdd_ite(<value>, <high>, <low>).
 * Each <value> in <map> must be a Boolean MTBDD.
 */
MTBDD mtbdd_compose_CALL(lace_worker* lace, MTBDD a, MTBDDMAP map)
{
    /* Terminal case */
    if (mtbdd_is_leaf(a) || mtbdd_map_is_empty(map)) return a;

    /* Determine top level */
    mtbddnode* n = MTBDD_GETNODE(a);
    uint32_t v = mtbddnode_getvariable(n);

    /* Find in map */
    while (mtbdd_map_key(map) < v) {
        map = mtbdd_map_next(map);
        if (mtbdd_map_is_empty(map)) return a;
    }

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_COMPOSE);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_COMPOSE, a, map, 0, &result)) {
        sylvan_stats_count(MTBDD_COMPOSE_CACHED);
        return result;
    }

    /* Recursive calls */
    mtbdd_refs_spawn(mtbdd_compose_SPAWN(lace, node_getlow(a, n), map));
    MTBDD high = mtbdd_refs_push(mtbdd_compose_CALL(lace, node_gethigh(a, n), map));
    MTBDD low = mtbdd_refs_push(mtbdd_refs_sync(mtbdd_compose_SYNC(lace)));

    /* Calculate result */
    MTBDD r = mtbdd_map_key(map) == v ? mtbdd_map_value(map) : mtbdd_make_node(v, mtbdd_undefined, bdd_true);
    mtbdd_refs_push(r);
    result = mtbdd_ite_CALL(lace, r, high, low);
    mtbdd_refs_pop(3);

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_COMPOSE, a, map, 0, result)) {
        sylvan_stats_count(MTBDD_COMPOSE_CACHEDPUT);
    }

    return result;
}

/**
 * Compute minimum leaf in the MTBDD (for Integer, Double, Rational MTBDDs)
 */
MTBDD mtbdd_find_min_CALL(lace_worker* lace, MTBDD a)
{
    /* Check terminal case */
    if (a == mtbdd_undefined) return mtbdd_undefined;
    mtbddnode* na = MTBDD_GETNODE(a);
    if (mtbddnode_isleaf(na)) return a;

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_MINIMUM);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_MINIMUM, a, 0, 0, &result)) {
        sylvan_stats_count(MTBDD_MINIMUM_CACHED);
        return result;
    }

    /* Call recursive */
    mtbdd_find_min_SPAWN(lace, node_getlow(a, na));
    MTBDD high = mtbdd_find_min_CALL(lace, node_gethigh(a, na));
    MTBDD low = mtbdd_find_min_SYNC(lace);

    /* Determine lowest */
    mtbddnode* nl = MTBDD_GETNODE(low);
    mtbddnode* nh = MTBDD_GETNODE(high);

    if (mtbddnode_gettype(nl) == 0 && mtbddnode_gettype(nh) == 0) {
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
        assert(0); // failure
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_MINIMUM, a, 0, 0, result)) {
        sylvan_stats_count(MTBDD_MINIMUM_CACHEDPUT);
    }

    return result;
}

/**
 * Compute maximum leaf in the MTBDD (for Integer, Double, Rational MTBDDs)
 */
MTBDD mtbdd_find_max_CALL(lace_worker* lace, MTBDD a)
{
    /* Check terminal case */
    if (a == mtbdd_undefined) return mtbdd_undefined;
    mtbddnode* na = MTBDD_GETNODE(a);
    if (mtbddnode_isleaf(na)) return a;

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_MAXIMUM);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_MAXIMUM, a, 0, 0, &result)) {
        sylvan_stats_count(MTBDD_MAXIMUM_CACHED);
        return result;
    }

    /* Call recursive */
    mtbdd_find_max_SPAWN(lace, node_getlow(a, na));
    MTBDD high = mtbdd_find_max_CALL(lace, node_gethigh(a, na));
    MTBDD low = mtbdd_find_max_SYNC(lace);

    /* Determine highest */
    mtbddnode* nl = MTBDD_GETNODE(low);
    mtbddnode* nh = MTBDD_GETNODE(high);

    if (mtbddnode_gettype(nl) == 0 && mtbddnode_gettype(nh) == 0) {
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
        assert(0); // failure
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_MAXIMUM, a, 0, 0, result)) {
        sylvan_stats_count(MTBDD_MAXIMUM_CACHEDPUT);
    }

    return result;
}

/**
 * Calculate the number of satisfying variable assignments according to <variables>.
 */
double mtbdd_sat_count_CALL(lace_worker* lace, MTBDD dd, size_t nvars)
{
    /* Trivial cases */
    if (dd == mtbdd_undefined) return 0.0;

    if (mtbdd_is_leaf(dd)) {
        // test if 0
        mtbddnode* dd_node = MTBDD_GETNODE(dd);
        if (dd != bdd_true) {
            if (mtbddnode_gettype(dd_node) == 0 && mtbdd_leaf_int64(dd) == 0) return 0.0;
            else if (mtbddnode_gettype(dd_node) == 1 && mtbdd_leaf_double(dd) == 0.0) return 0.0;
            else if (mtbddnode_gettype(dd_node) == 2 && mtbdd_leaf_value(dd) == 1) return 0.0;
        }
        return (double)powl(2.0L, (long double)nvars);
    }

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    union {
        double d;
        uint64_t s;
    } hack;

    /* Consult cache */
    if (cache_get3(CACHE_BDD_SATCOUNT, dd, 0, nvars, &hack.s)) {
        sylvan_stats_count(BDD_SATCOUNT_CACHED);
        return hack.d;
    }

    mtbdd_sat_count_SPAWN(lace, mtbdd_node_high(dd), nvars-1);
    double low = mtbdd_sat_count_CALL(lace, mtbdd_node_low(dd), nvars-1);
    hack.d = low + mtbdd_sat_count_SYNC(lace);

    if (cache_put3(CACHE_BDD_SATCOUNT, dd, 0, nvars, hack.s)) {
        sylvan_stats_count(BDD_SATCOUNT_CACHEDPUT);
    }

    return hack.d;
}

MTBDD
mtbdd_first_cube(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb)
{
    if (dd == mtbdd_undefined) {
        // the leaf dd is skipped
        return mtbdd_undefined;
    } else if (mtbdd_is_leaf(dd)) {
        // a leaf for which the filter returns 0 is skipped
        if (filter_cb != NULL && filter_cb(dd) == 0) return mtbdd_undefined;
        // ok, we have a leaf that is not skipped, go for it!
        while (variables != bdd_true) {
            *arr++ = 2;
            variables = mtbdd_node_high(variables);
        }
        return dd;
    } else if (variables == bdd_true) {
        // in the case of partial evaluation... treat like a leaf
        if (filter_cb != NULL && filter_cb(dd) == 0) return mtbdd_undefined;
        return dd;
    } else {
        // if variables == true, then dd must be a leaf. But then this line is unreachable.
        // if this assertion fails, then <variables> is not the support of <dd>.
        assert(variables != bdd_true);

        // get next variable from <variables>
        uint32_t v = mtbdd_node_variable(variables);
        variables = mtbdd_node_high(variables);

        // check if MTBDD is on this variable
        mtbddnode* n = MTBDD_GETNODE(dd);
        if (mtbddnode_getvariable(n) != v) {
            *arr = 2;
            return mtbdd_first_cube(dd, variables, arr+1, filter_cb);
        }

        // first maybe follow low
        MTBDD res = mtbdd_first_cube(node_getlow(dd, n), variables, arr+1, filter_cb);
        if (res != mtbdd_undefined) {
            *arr = 0;
            return res;
        }

        // if not low, try following high
        res = mtbdd_first_cube(node_gethigh(dd, n), variables, arr+1, filter_cb);
        if (res != mtbdd_undefined) {
            *arr = 1;
            return res;
        }
        
        // we've tried low and high, return false
        return mtbdd_undefined;
    }
}

MTBDD
mtbdd_next_cube(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb)
{
    if (mtbdd_is_leaf(dd)) {
        // we find the leaf in 'enum_next', then we've seen it before...
        return mtbdd_undefined;
    } else if (variables == bdd_true) {
        // in the case of partial evaluation... treat like a leaf
        return mtbdd_undefined;
    } else {
        // if variables == true, then dd must be a leaf. But then this line is unreachable.
        // if this assertion fails, then <variables> is not the support of <dd>.
        assert(variables != bdd_true);

        variables = mtbdd_node_high(variables);

        if (*arr == 0) {
            // previous was low
            mtbddnode* n = MTBDD_GETNODE(dd);
            MTBDD res = mtbdd_next_cube(node_getlow(dd, n), variables, arr+1, filter_cb);
            if (res != mtbdd_undefined) {
                return res;
            } else {
                // try to find new in high branch
                res = mtbdd_first_cube(node_gethigh(dd, n), variables, arr+1, filter_cb);
                if (res != mtbdd_undefined) {
                    *arr = 1;
                    return res;
                } else {
                    return mtbdd_undefined;
                }
            }
        } else if (*arr == 1) {
            // previous was high
            mtbddnode* n = MTBDD_GETNODE(dd);
            return mtbdd_next_cube(node_gethigh(dd, n), variables, arr+1, filter_cb);
        } else {
            // previous was either
            return mtbdd_next_cube(dd, variables, arr+1, filter_cb);
        }
    }
}

MTBDD
mtbdd_first_minterm(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb)
{
    if (dd == mtbdd_undefined) {
        // the leaf False is skipped
        return mtbdd_undefined;
    } else if (variables == bdd_true) {
        // if this assertion fails, then <variables> is not the support of <dd>.
        // actually, remove this check to allow for "partial" enumeration
        // assert(mtbdd_is_leaf(dd));
        // for _first, just return the leaf; there is nothing to set, though.
        if (filter_cb != NULL && filter_cb(dd) == 0) return mtbdd_undefined;
        return dd;
    } else if (mtbdd_is_leaf(dd)) {
        // a leaf for which the filter returns 0 is skipped
        if (filter_cb != NULL && filter_cb(dd) == 0) return mtbdd_undefined;
        // for all remaining variables, set to 0
        while (variables != bdd_true) {
            *arr++ = 0;
            variables = mtbdd_node_high(variables);
        }
        return dd;
    } else {
        // get next variable from <variables>
        mtbddnode* nv = MTBDD_GETNODE(variables);
        variables = node_gethigh(variables, nv);

        // get cofactors
        mtbddnode* ndd = MTBDD_GETNODE(dd);
        MTBDD low, high;
        if (mtbddnode_getvariable(ndd) == mtbddnode_getvariable(nv)) {
            low = node_getlow(dd, ndd);
            high = node_gethigh(dd, ndd);
        } else {
            low = high = dd;
        }

        // first maybe follow low
        MTBDD res = mtbdd_first_minterm(low, variables, arr+1, filter_cb);
        if (res != mtbdd_undefined) {
            *arr = 0;
            return res;
        }

        // if not low, try following high
        res = mtbdd_first_minterm(high, variables, arr+1, filter_cb);
        if (res != mtbdd_undefined) {
            *arr = 1;
            return res;
        }

        // we've tried low and high, return false
        return mtbdd_undefined;
    }
}

MTBDD
mtbdd_next_minterm(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb)
{
    if (dd == mtbdd_undefined) {
        // the leaf False is skipped
        return mtbdd_undefined;
    } else if (variables == bdd_true) {
        // if this assertion fails, then <variables> is not the support of <dd>.
        // actually, remove this check to allow for "partial" enumeration
        // assert(mtbdd_is_leaf(dd));
        // no next if there are no variables
        return mtbdd_undefined;
    } else {
        // get next variable from <variables>
        mtbddnode* nv = MTBDD_GETNODE(variables);
        variables = node_gethigh(variables, nv);

        // filter leaf (if leaf) or get cofactors (if not leaf)
        mtbddnode* ndd = MTBDD_GETNODE(dd);
        MTBDD low, high;
        if (mtbdd_is_leaf(dd)) {
            // a leaf for which the filter returns 0 is skipped
            if (filter_cb != NULL && filter_cb(dd) == 0) return mtbdd_undefined;
            low = high = dd;
        } else {
            // get cofactors
            if (mtbddnode_getvariable(ndd) == mtbddnode_getvariable(nv)) {
                low = node_getlow(dd, ndd);
                high = node_gethigh(dd, ndd);
            } else {
                low = high = dd;
            }
        }

        // try recursive next first
        if (*arr == 0) {
            MTBDD res = mtbdd_next_minterm(low, variables, arr+1, filter_cb);
            if (res != mtbdd_undefined) return res;
        } else if (*arr == 1) {
            return mtbdd_next_minterm(high, variables, arr+1, filter_cb);
            // if *arr was 1 and _next returns False, return False
        } else {
            // the array is invalid...
            assert(*arr == 0 || *arr == 1);
            return mtbdd_invalid;  // in Release mode, the assertion is empty code
        }

        // previous was low, try following high
        MTBDD res = mtbdd_first_minterm(high, variables, arr+1, filter_cb);
        if (res == mtbdd_undefined) return mtbdd_undefined;

        // succesful, set arr
        *arr = 1;
        return res;
    }
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
 * TASK(MTBDD, g, MTBDD, in) { ... return g of <in> ... }
 * MTBDD x_vars = ...;  // the cube of variables x
 * MTBDD result = mtbdd_eval_compose(dd, x_vars, TASK(g));
 */
MTBDD mtbdd_eval_compose_CALL(lace_worker* lace, MTBDD dd, MTBDD vars, mtbdd_eval_compose_cb cb)
{
    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(MTBDD_EVAL_COMPOSE);

    /* Check cache */
    MTBDD result;
    if (cache_get3(CACHE_MTBDD_EVAL_COMPOSE, dd, vars, (size_t)cb, &result)) {
        sylvan_stats_count(MTBDD_EVAL_COMPOSE_CACHED);
        return result;
    }

    if (mtbdd_is_leaf(dd) || vars == bdd_true) {
        /* Apply */
        result = cb(lace, dd);
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
            result = cb(lace, dd);
        } else {
            /* If this fails, then there are variables in f/g BEFORE vars, which breaks functionality. */
            assert(vv == var);
            if (vv != var) return mtbdd_invalid;

            /* Get cofactors */
            MTBDD ddlow = node_getlow(dd, ndd);
            MTBDD ddhigh = node_gethigh(dd, ndd);

            /* Recursive */
            _vars = node_gethigh(_vars, nvars);
            mtbdd_refs_spawn(mtbdd_eval_compose_SPAWN(lace, ddhigh, _vars, cb));
            MTBDD low = mtbdd_refs_push(mtbdd_eval_compose_CALL(lace, ddlow, _vars, cb));
            MTBDD high = mtbdd_refs_sync(mtbdd_eval_compose_SYNC(lace));
            mtbdd_refs_pop(1);
            result = mtbdd_make_node(var, low, high);
        }
    }

    /* Store in cache */
    if (cache_put3(CACHE_MTBDD_EVAL_COMPOSE, dd, vars, (size_t)cb, result)) {
        sylvan_stats_count(MTBDD_EVAL_COMPOSE_CACHEDPUT);
    }

    return result;
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
            uint64_t value = mtbddnode_getvalue(node);
            value = sylvan_mt_hash(type, value, value);
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
            uint32_t type = mtbddnode_gettype(n);
            uint64_t value = mtbddnode_getvalue(n);
            sylvan_mt_write_binary(type, value, out);
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
            uint64_t value = mtbddnode_getvalue(&node);
            sylvan_mt_read_binary(type, &value, in);
            arr[i] = mtbdd_leaf(type, value);
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
bdd_set_is_valid(MTBDD set)
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
MTBDDMAP
mtbdd_map_set(MTBDDMAP map, uint32_t key, MTBDD value)
{
    if (mtbdd_map_is_empty(map)) {
        return mtbdd_makemapnode(key, mtbdd_map_empty(), value);
    }

    mtbddnode* n = MTBDD_GETNODE(map);
    uint32_t k = mtbddnode_getvariable(n);

    if (k < key) {
        // add recursively and rebuild tree
        MTBDDMAP low = mtbdd_map_set(node_getlow(map, n), key, value);
        return mtbdd_makemapnode(k, low, node_gethigh(map, n));
    } else if (k > key) {
        return mtbdd_makemapnode(key, map, value);
    } else {
        // replace old
        return mtbdd_makemapnode(key, node_getlow(map, n), value);
    }
}

/**
 * Add all values from map2 to map1, overwrites if key already in map1.
 */
MTBDDMAP
mtbdd_map_update(MTBDDMAP map1, MTBDDMAP map2)
{
    if (mtbdd_map_is_empty(map1)) return map2;
    if (mtbdd_map_is_empty(map2)) return map1;

    mtbddnode* n1 = MTBDD_GETNODE(map1);
    mtbddnode* n2 = MTBDD_GETNODE(map2);
    uint32_t k1 = mtbddnode_getvariable(n1);
    uint32_t k2 = mtbddnode_getvariable(n2);

    MTBDDMAP result;
    if (k1 < k2) {
        MTBDDMAP low = mtbdd_map_update(node_getlow(map1, n1), map2);
        result = mtbdd_makemapnode(k1, low, node_gethigh(map1, n1));
    } else if (k1 > k2) {
        MTBDDMAP low = mtbdd_map_update(map1, node_getlow(map2, n2));
        result = mtbdd_makemapnode(k2, low, node_gethigh(map2, n2));
    } else {
        MTBDDMAP low = mtbdd_map_update(node_getlow(map1, n1), node_getlow(map2, n2));
        result = mtbdd_makemapnode(k2, low, node_gethigh(map2, n2));
    }

    return result;
}

/**
 * Remove the key <key> from the map and return the result
 */
MTBDDMAP
mtbdd_map_remove(MTBDDMAP map, uint32_t key)
{
    if (mtbdd_map_is_empty(map)) return map;

    mtbddnode* n = MTBDD_GETNODE(map);
    uint32_t k = mtbddnode_getvariable(n);

    if (k < key) {
        MTBDDMAP low = mtbdd_map_remove(node_getlow(map, n), key);
        return mtbdd_makemapnode(k, low, node_gethigh(map, n));
    } else if (k > key) {
        return map;
    } else {
        return node_getlow(map, n);
    }
}

/**
 * Remove all keys in the cube <variables> from the map and return the result
 */
MTBDDMAP
mtbdd_map_remove_all(MTBDDMAP map, MTBDD variables)
{
    if (mtbdd_map_is_empty(map)) return map;
    if (variables == bdd_true) return map;

    mtbddnode* n1 = MTBDD_GETNODE(map);
    mtbddnode* n2 = MTBDD_GETNODE(variables);
    uint32_t k1 = mtbddnode_getvariable(n1);
    uint32_t k2 = mtbddnode_getvariable(n2);

    if (k1 < k2) {
        MTBDDMAP low = mtbdd_map_remove_all(node_getlow(map, n1), variables);
        return mtbdd_makemapnode(k1, low, node_gethigh(map, n1));
    } else if (k1 > k2) {
        return mtbdd_map_remove_all(map, node_gethigh(variables, n2));
    } else {
        return mtbdd_map_remove_all(node_getlow(map, n1), node_gethigh(variables, n2));
    }
}
