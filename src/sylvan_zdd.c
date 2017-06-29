/*
 * Copyright 2016 Tom van Dijk, Johannes Kepler University Linz
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
// #include <sylvan_config.h>

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// #include <sylvan.h>

#include <sylvan_refs.h>
#include <sylvan_sl.h>

/**
 * Basic ZDD node manipulation
 */

/**
 * Return 1 if the DD is a leaf, 0 otherwise
 */
int
zdd_isleaf(ZDD dd)
{
    if (dd == zdd_true || dd == zdd_false) return 1;
    return zddnode_isleaf(ZDD_GETNODE(dd));
}

/**
 * Get the DD variable
 */
uint32_t
zdd_getvar(ZDD node)
{
    return zddnode_getvariable(ZDD_GETNODE(node));
}

/**
 * Get the low edge of the ZDD
 */
ZDD
zdd_getlow(ZDD zdd)
{
    return zddnode_low(zdd, ZDD_GETNODE(zdd));
}

/**
 * Get the high edge of the ZDD
 */
ZDD
zdd_gethigh(ZDD zdd)
{
    return zddnode_high(zdd, ZDD_GETNODE(zdd));
}

/**
 * Get the type of ZDD leaf
 * 0: int64_t
 * 1: double
 * 2: fraction
 */
uint16_t
zdd_gettype(ZDD leaf)
{
    return zddnode_gettype(ZDD_GETNODE(leaf));
}

uint64_t
zdd_getvalue(ZDD leaf)
{
    if (leaf == zdd_false || leaf == zdd_true) return leaf;
    return zddnode_getvalue(ZDD_GETNODE(leaf));
}

int64_t
zdd_getint64(ZDD leaf)
{
    uint64_t value = zddnode_getvalue(ZDD_GETNODE(leaf));
    return *(int64_t*)&value;
}

double
zdd_getdouble(ZDD leaf)
{
    uint64_t value = zddnode_getvalue(ZDD_GETNODE(leaf));
    return *(double*)&value;
}

/**
 * Implementation of garbage collection
 */

/**
 * During garbage collection, recursively mark ZDD nodes in the nodes table to keep.
 */
VOID_TASK_IMPL_1(zdd_gc_mark_rec, ZDD, zdd)
{
    if (zdd == zdd_true) return;
    if (zdd == zdd_false) return;

    // Mark, and if returns 0, we are done
    if (llmsset_mark(nodes, ZDD_GETINDEX(zdd)) != 0) {
        // The node was not yet marked, so go recursive if not a leaf
        zddnode_t n = ZDD_GETNODE(zdd);
        if (!zddnode_isleaf(n)) {
            // Recursively mark low and high
            SPAWN(zdd_gc_mark_rec, zddnode_getlow(n));
            CALL(zdd_gc_mark_rec, zddnode_gethigh(n));
            SYNC(zdd_gc_mark_rec);
        }
    }
}

/**
 * External references (we only offer reference-by-pointer, not by-value)
 */

refs_table_t zdd_protected;
static int zdd_protected_created = 0;

void
zdd_protect(ZDD *a)
{
    if (!zdd_protected_created) {
        // In C++, sometimes zdd_protect is called before Sylvan is initialized. Just create a table.
        protect_create(&zdd_protected, 4096);
        zdd_protected_created = 1;
    }
    protect_up(&zdd_protected, (size_t)a);
}

void
zdd_unprotect(ZDD *a)
{
    if (zdd_protected.refs_table != NULL) protect_down(&zdd_protected, (size_t)a);
}

size_t
zdd_count_protected()
{
    return protect_count(&zdd_protected);
}

/**
 * Mark all external references (during garbage collection)
 */
VOID_TASK_0(zdd_gc_mark_protected)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = protect_iter(&zdd_protected, 0, zdd_protected.refs_size);
    while (it != NULL) {
        BDD *to_mark = (BDD*)protect_next(&zdd_protected, &it, zdd_protected.refs_size);
        SPAWN(zdd_gc_mark_rec, *to_mark);
        count++;
    }
    while (count--) {
        SYNC(zdd_gc_mark_rec);
    }
}

/**
 * Internal references (spawn/sync, push/pop)
 */
typedef struct zdd_refs_task
{
    Task *t;
    void *f;
} *zdd_refs_task_t;

typedef struct zdd_refs_internal
{
    ZDD **pbegin, **pend, **pcur;
    ZDD *rbegin, *rend, *rcur;
    zdd_refs_task_t sbegin, send, scur;
} *zdd_refs_internal_t;

DECLARE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);

VOID_TASK_2(zdd_refs_mark_p_par, ZDD**, begin, size_t, count)
{
    if (count < 32) {
        while (count) {
            zdd_gc_mark_rec(**(begin++));
            count--;
        }
    } else {
        SPAWN(zdd_refs_mark_p_par, begin, count / 2);
        CALL(zdd_refs_mark_p_par, begin + (count / 2), count - count / 2);
        SYNC(zdd_refs_mark_p_par);
    }
}

VOID_TASK_2(zdd_refs_mark_r_par, ZDD*, begin, size_t, count)
{
    if (count < 32) {
        while (count) {
            zdd_gc_mark_rec(*begin++);
            count--;
        }
    } else {
        SPAWN(zdd_refs_mark_r_par, begin, count / 2);
        CALL(zdd_refs_mark_r_par, begin + (count / 2), count - count / 2);
        SYNC(zdd_refs_mark_r_par);
    }
}

VOID_TASK_2(zdd_refs_mark_s_par, zdd_refs_task_t, begin, size_t, count)
{
    if (count < 32) {
        while (count) {
            Task *t = begin->t;
            if (!TASK_IS_STOLEN(t)) return;
            if (t->f == begin->f && TASK_IS_COMPLETED(t)) {
                zdd_gc_mark_rec(*(BDD*)TASK_RESULT(t));
            }
            begin += 1;
            count -= 1;
        }
    } else {
        if (!TASK_IS_STOLEN(begin->t)) return;
        SPAWN(zdd_refs_mark_s_par, begin, count / 2);
        CALL(zdd_refs_mark_s_par, begin + (count / 2), count - count / 2);
        SYNC(zdd_refs_mark_s_par);
    }
}

VOID_TASK_0(zdd_refs_mark_task)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    SPAWN(zdd_refs_mark_p_par, zdd_refs_key->pbegin, zdd_refs_key->pcur-zdd_refs_key->pbegin);
    SPAWN(zdd_refs_mark_r_par, zdd_refs_key->rbegin, zdd_refs_key->rcur-zdd_refs_key->rbegin);
    CALL(zdd_refs_mark_s_par, zdd_refs_key->sbegin, zdd_refs_key->scur-zdd_refs_key->sbegin);
    SYNC(zdd_refs_mark_r_par);
    SYNC(zdd_refs_mark_p_par);
}

VOID_TASK_0(zdd_refs_mark)
{
    TOGETHER(zdd_refs_mark_task);
}

VOID_TASK_0(zdd_refs_init_task)
{
    zdd_refs_internal_t s = (zdd_refs_internal_t)malloc(sizeof(struct zdd_refs_internal));
    s->pcur = s->pbegin = (ZDD**)malloc(sizeof(ZDD*) * 1024);
    s->pend = s->pbegin + 1024;
    s->rcur = s->rbegin = (ZDD*)malloc(sizeof(ZDD) * 1024);
    s->rend = s->rbegin + 1024;
    s->scur = s->sbegin = (zdd_refs_task_t)malloc(sizeof(struct zdd_refs_task) * 1024);
    s->send = s->sbegin + 1024;
    SET_THREAD_LOCAL(zdd_refs_key, s);
}

VOID_TASK_0(zdd_refs_init)
{
    INIT_THREAD_LOCAL(zdd_refs_key);
    TOGETHER(zdd_refs_init_task);
}

void
zdd_refs_ptrs_up(zdd_refs_internal_t zdd_refs_key)
{
    size_t size = zdd_refs_key->pend - zdd_refs_key->pbegin;
    zdd_refs_key->pbegin = (ZDD**)realloc(zdd_refs_key->pbegin, sizeof(ZDD*) * size*2);
    zdd_refs_key->pcur = zdd_refs_key->pbegin + size;
    zdd_refs_key->pend = zdd_refs_key->pend + size * 2;
}

ZDD __attribute__((noinline))
zdd_refs_refs_up(zdd_refs_internal_t zdd_refs_key, ZDD res)
{
    long size = zdd_refs_key->rend - zdd_refs_key->rbegin;
    zdd_refs_key->rbegin = (ZDD*)realloc(zdd_refs_key->rbegin, sizeof(ZDD) * size * 2);
    zdd_refs_key->rcur = zdd_refs_key->rbegin + size;
    zdd_refs_key->rend = zdd_refs_key->rbegin + (size * 2);
    return res;
}

void __attribute__((noinline))
zdd_refs_tasks_up(zdd_refs_internal_t zdd_refs_key)
{
    long size = zdd_refs_key->send - zdd_refs_key->sbegin;
    printf("up with size %ld\n", size);
    zdd_refs_key->sbegin = (zdd_refs_task_t)realloc(zdd_refs_key->sbegin, sizeof(struct zdd_refs_task) * size * 2);
    zdd_refs_key->scur = zdd_refs_key->sbegin + size;
    zdd_refs_key->send = zdd_refs_key->sbegin + (size * 2);
}

void __attribute__((unused))
zdd_refs_pushptr(ZDD *ptr)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    *zdd_refs_key->pcur++ = ptr;
    if (zdd_refs_key->pcur == zdd_refs_key->pend) zdd_refs_ptrs_up(zdd_refs_key);
}

void __attribute__((unused))
zdd_refs_popptr(size_t amount)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    zdd_refs_key->pcur -= amount;
}

ZDD __attribute__((unused))
zdd_refs_push(ZDD zdd)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    *(zdd_refs_key->rcur++) = zdd;
    if (zdd_refs_key->rcur == zdd_refs_key->rend) return zdd_refs_refs_up(zdd_refs_key, zdd);
    else return zdd;
}

void __attribute__((unused))
zdd_refs_pop(long amount)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    zdd_refs_key->rcur -= amount;
}

void __attribute__((unused))
zdd_refs_spawn(Task *t)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    zdd_refs_key->scur->t = t;
    zdd_refs_key->scur->f = t->f;
    zdd_refs_key->scur += 1;
    if (zdd_refs_key->scur == zdd_refs_key->send) zdd_refs_tasks_up(zdd_refs_key);
}

ZDD __attribute__((unused))
zdd_refs_sync(ZDD result)
{
    LOCALIZE_THREAD_LOCAL(zdd_refs_key, zdd_refs_internal_t);
    zdd_refs_key->scur -= 1;
    return result;
}

/**
 * Initialize and quit functions
 */

static int zdd_initialized = 0;

static void
zdd_quit()
{
    if (zdd_protected_created) {
        protect_free(&zdd_protected);
        zdd_protected_created = 0;
    }

    zdd_initialized = 0;
}

void
sylvan_init_zdd()
{
    sylvan_init_mt();

    if (zdd_initialized) return;
    zdd_initialized = 1;

    sylvan_register_quit(zdd_quit);
    sylvan_gc_add_mark(TASK(zdd_gc_mark_protected));
    sylvan_gc_add_mark(TASK(zdd_refs_mark));

    if (!zdd_protected_created) {
        protect_create(&zdd_protected, 4096);
        zdd_protected_created = 1;
    }

    RUN(zdd_refs_init);
}

/**
 * Basic ZDD node creation functionality
 */
ZDD
zdd_makeleaf(uint16_t type, uint64_t value)
{
    struct zddnode n;
    zddnode_makeleaf(&n, type, value);

    int custom = sylvan_mt_has_custom_hash(type);

    int created;
    uint64_t index = custom ? llmsset_lookupc(nodes, n.a, n.b, &created) : llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        RUN(sylvan_gc);

        index = custom ? llmsset_lookupc(nodes, n.a, n.b, &created) : llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    // TODO: rename this to "leaf nodes created" - we may want to treat leaf nodes as distinct from BDD/ZDD internals
    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return (ZDD)index;
}

/**
 * Node creation primitive.
 *
 * Returns the ZDD representing the formula <var> then <high> else <low>.
 * Variable <nextvar> is the next variable in the domain, necessary to correctly
 * perform the ZDD minimization rule.
 */
ZDD
_zdd_makenode(uint32_t var, ZDD low, ZDD high)
{
    // Checked by macro: if (high == zdd_false) return low;

    /* if low had a mark, it is moved to the result */
#if ZDD_COMPLEMENT_EDGES
    int mark = ZDD_HASMARK(low);
    low = ZDD_STRIPMARK(low);
#else
    assert(!ZDD_HASMARK(low));
    assert(!ZDD_HASMARK(high));
    int mark = 0;
#endif

    struct zddnode n;
    zddnode_makenode(&n, var, low, high);

    int created;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        zdd_refs_push(low);
        zdd_refs_push(high);
        RUN(sylvan_gc);
        zdd_refs_pop(2);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(ZDD_NODES_CREATED);
    else sylvan_stats_count(ZDD_NODES_REUSED);

    return mark ? index | zdd_complement : index;
}

ZDD
zdd_makemapnode(uint32_t var, ZDD low, ZDD high)
{
    // in a ZDDMAP, the low edges eventually lead to 0 and cannot have a complemented low edge
    assert(!ZDD_HASMARK(low));

    struct zddnode n;
    zddnode_makemapnode(&n, var, low, high);

    int created;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        zdd_refs_push(low);
        zdd_refs_push(high);
        RUN(sylvan_gc);
        zdd_refs_pop(2);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return index;
}

/**
 * Obtain a ZDD representing a positive literal of variable <var>.
 */
ZDD
zdd_ithvar(uint32_t var)
{
    return zdd_makenode(var, zdd_false, zdd_true);
}

/**
 * Obtain a ZDD representing a negative literal of variable <var>.
 */
ZDD
zdd_nithvar(uint32_t var)
{
    return zdd_makenode(var, zdd_true, zdd_false);
}

/**
 * Evaluate a ZDD, assigning <value> (1 or 0) to <variable>;
 * <variable> is the current variable in the domain
 */
ZDD
zdd_eval(ZDD dd, uint32_t variable, int value)
{
    // If <variable> was skipped, return false if value is true
    if (zdd_isleaf(dd)) return value ? zdd_false : dd;
    zddnode_t n = ZDD_GETNODE(dd);
    uint32_t var = zddnode_getvariable(n);
    if (variable < var) return value ? zdd_false : dd;
    assert(variable == var);
    // Otherwise, follow low/high edge...
    return value ? zddnode_high(dd, n) : zddnode_low(dd, n);
}

/**
 * Convert an MTBDD to a ZDD
 */
TASK_IMPL_2(ZDD, zdd_from_mtbdd, MTBDD, dd, MTBDD, dom)
{
    /* Special treatment for False */
    if (dd == mtbdd_false) return zdd_false;
    if (dd == mtbdd_true && dom == mtbdd_true) return zdd_true;
    if (dom == mtbdd_true) {
        assert(mtbdd_isleaf(dd));
        // A MTBDD leaf is identical to a ZDD leaf...
        return dd;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Count operation */
    sylvan_stats_count(ZDD_FROM_MTBDD);

    /* Check cache */
    ZDD result;
    if (cache_get3(CACHE_ZDD_FROM_MTBDD, dd, dom, 0, &result)) {
        sylvan_stats_count(ZDD_FROM_MTBDD_CACHED);
        return result;
    }

    const mtbddnode_t dd_node = dd == zdd_true ? NULL : MTBDD_GETNODE(dd);
    if (dd == zdd_true || mtbddnode_isleaf(dd_node)) {
        const mtbddnode_t dom_node = MTBDD_GETNODE(dom);
        const uint32_t dom_var = mtbddnode_getvariable(dom_node);
        const MTBDD dom_next = mtbddnode_followhigh(dom, dom_node);
        result = zdd_from_mtbdd(dd, dom_next);
        result = zdd_makenode(dom_var, result, result);
    } else {
        /* Get variables */
        const uint32_t var = mtbddnode_getvariable(dd_node);
        const mtbddnode_t dom_node = MTBDD_GETNODE(dom);
        const uint32_t dom_var = mtbddnode_getvariable(dom_node);
        assert(dom_var <= var);

        /* Get cofactors */
        const MTBDD dd0 = dom_var == var ? mtbddnode_followlow(dd, dd_node) : dd;
        const MTBDD dd1 = dom_var == var ? mtbddnode_followhigh(dd, dd_node) : dd;

        /* Recursive */
        const MTBDD dom_next = mtbddnode_followhigh(dom, dom_node);
        zdd_refs_spawn(SPAWN(zdd_from_mtbdd, dd1, dom_next));
        const ZDD low = zdd_refs_push(CALL(zdd_from_mtbdd, dd0, dom_next));
        const ZDD high = zdd_refs_sync(SYNC(zdd_from_mtbdd));
        zdd_refs_pop(1);
        result = zdd_makenode(dom_var, low, high);
    }

    /* Store in cache */
    if (cache_put3(CACHE_ZDD_FROM_MTBDD, dd, dom, 0, result)) {
        sylvan_stats_count(ZDD_FROM_MTBDD_CACHEDPUT);
    }

    return result;
}

/**
 * Convert a ZDD to an MTBDD.
 */
TASK_IMPL_2(ZDD, zdd_to_mtbdd, ZDD, dd, ZDD, dom)
{
    /* Special treatment for True and False */
    if (dd == zdd_false) return mtbdd_false;
    if (dd == zdd_true && dom == zdd_true) return mtbdd_true;
    if (dom == zdd_true) {
        assert(zdd_isleaf(dd));
        // A MTBDD leaf is identical to a ZDD leaf...
        return dd;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Count operation */
    sylvan_stats_count(ZDD_TO_MTBDD);

    /* Check cache */
    ZDD result;
    if (cache_get3(CACHE_ZDD_TO_MTBDD, dd, dom, 0, &result)) {
        sylvan_stats_count(ZDD_TO_MTBDD_CACHED);
        return result;
    }

    const zddnode_t dd_node = dd == zdd_true ? NULL : ZDD_GETNODE(dd);
    if (dd == zdd_true || zddnode_isleaf(dd_node)) {
        const zddnode_t dom_node = ZDD_GETNODE(dom);
        const uint32_t dom_var = zddnode_getvariable(dom_node);
        const MTBDD dom_next = zddnode_high(dom, dom_node);
        result = zdd_to_mtbdd(dd, dom_next);
        result = mtbdd_makenode(dom_var, result, mtbdd_false);
    } else {
        /* Get variables */
        const zddnode_t dom_node = ZDD_GETNODE(dom);
        const uint32_t dd_var = zddnode_getvariable(dd_node);
        const uint32_t dom_var = zddnode_getvariable(dom_node);
        assert(dom_var <= dd_var);

        /* Get cofactors */
        const ZDD dd0 = dom_var == dd_var ? zddnode_low(dd, dd_node) : dd;
        const ZDD dd1 = dom_var == dd_var ? zddnode_high(dd, dd_node) : zdd_false;

        /* Recursive */
        const ZDD dom_next = zddnode_high(dom, dom_node);
        mtbdd_refs_spawn(SPAWN(zdd_to_mtbdd, dd1, dom_next));
        const MTBDD low = mtbdd_refs_push(zdd_to_mtbdd(dd0, dom_next));
        const MTBDD high = mtbdd_refs_sync(SYNC(zdd_to_mtbdd));
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(dom_var, low, high);
    }

    /* Store in cache */
    if (cache_put3(CACHE_ZDD_TO_MTBDD, dd, dom, 0, result)) {
        sylvan_stats_count(ZDD_TO_MTBDD_CACHEDPUT);
    }

    return result;
}

/**
 * Create a variable set, represented as the function that evaluates
 * to True for all assignments to its variables.
 * This represents sets of variables, also variable domains.
 */
ZDD
zdd_set_from_array(uint32_t *arr, size_t len)
{
    if (len == 0) return zdd_true;
    else if (len == 1) return zdd_makenode(*arr, zdd_true, zdd_true);
    else {
        ZDD res = zdd_set_from_array(arr+1, len-1);
        return zdd_makenode(*arr, res, res);
    }
}

/**
 * Write all variables in a variable set to the given array.
 * The array must be suffiently large.
 */
void
zdd_set_to_array(ZDD set, uint32_t *arr)
{
    if (set == zdd_true) return;
    zddnode_t set_node = ZDD_GETNODE(set);
    *arr = zddnode_getvariable(set_node);
    zdd_set_to_array(zddnode_high(set, set_node), arr+1);
}

/**
 * Compute the number of variables in a given set of variables.
 */
size_t
zdd_set_count(ZDD set)
{
    if (set == zdd_true) return 0;
    return 1 + zdd_set_count(zdd_gethigh(set));
}

/**
 * Compute the union of <set1> and <set2>.
 */
ZDD
zdd_set_union(ZDD set1, ZDD set2)
{
    if (set1 == zdd_true) return set2;
    if (set2 == zdd_true) return set1;
    if (set1 == set2) return set1;

    zddnode_t set1_node = ZDD_GETNODE(set1);
    zddnode_t set2_node = ZDD_GETNODE(set2);
    uint32_t set1_var = zddnode_getvariable(set1_node);
    uint32_t set2_var = zddnode_getvariable(set2_node);

    if (set1_var < set2_var) {
        ZDD sub = zdd_set_union(zddnode_high(set1, set1_node), set2);
        return zdd_makenode(set1_var, sub, sub);
    } else if (set1_var > set2_var) {
        ZDD sub = zdd_set_union(set1, zddnode_high(set2, set2_node));
        return zdd_makenode(set2_var, sub, sub);
    } else {
        ZDD sub = zdd_set_union(zddnode_high(set1, set1_node), zddnode_high(set2, set2_node));
        return zdd_makenode(set1_var, sub, sub);
    }
}

/**
 * Remove variables in <set2> from <set1>.
 */
ZDD
zdd_set_minus(ZDD set1, ZDD set2)
{
    if (set1 == zdd_true) return zdd_true;
    if (set2 == zdd_true) return set1;

    zddnode_t set1_node = ZDD_GETNODE(set1);
    zddnode_t set2_node = ZDD_GETNODE(set2);
    uint32_t set1_var = zddnode_getvariable(set1_node);
    uint32_t set2_var = zddnode_getvariable(set2_node);

    if (set1_var == set2_var) {
        return zdd_set_minus(zddnode_high(set1, set1_node), zddnode_high(set2, set2_node));
    }

    if (set1_var > set2_var) {
        return zdd_set_minus(set1, zddnode_high(set2, set2_node));
    }

    /* set1_var < set2_var */
    ZDD res = zdd_set_minus(zddnode_high(set1, set1_node), set2);
    return zdd_makenode(set1_var, res, res);
}

/**
 * Returns 1 if <set> contains <var>, 0 otherwise.
 */
int
zdd_set_contains(ZDD set, uint32_t var)
{
    if (set == zdd_true) return 0;

    zddnode_t set_node = ZDD_GETNODE(set);
    uint32_t set_var = zddnode_getvariable(set_node);
    if (var < set_var) return 0;
    else if (var == set_var) return 1;
    else return zdd_set_contains(zddnode_high(set, set_node), var);
}

/**
 * Adds the variable <var> to <set>.
 */
ZDD
zdd_set_add(ZDD set, uint32_t var)
{
    if (set == zdd_true) return zdd_makenode(var, zdd_true, zdd_true);

    zddnode_t set_node = ZDD_GETNODE(set);
    uint32_t set_var = zddnode_getvariable(set_node);
    if (var < set_var) return zdd_makenode(var, set, set);
    else if (var == set_var) return set;
    else {
        ZDD sub = zddnode_high(set, set_node);
        ZDD res = zdd_set_add(sub, var);
        res = sub == res ? set : zdd_makenode(set_var, res, res);
        return res;
    }
}

/**
 * Removes the variable <var> from <set>.
 */
ZDD
zdd_set_remove(ZDD set, uint32_t var)
{
    if (set == zdd_true) return zdd_true;

    zddnode_t set_node = ZDD_GETNODE(set);
    uint32_t set_var = zddnode_getvariable(set_node);
    if (var < set_var) return set;
    else if (var == set_var) return zddnode_high(set, set_node);
    else {
        ZDD sub = zddnode_high(set, set_node);
        ZDD res = zdd_set_remove(sub, var);
        res = sub == res ? set : zdd_makenode(set_var, res, res);
        return res;
    }
}

/**
 * Convert a ZDD set to a MTBDD set
 */
MTBDD
zdd_set_to_mtbdd(ZDD set)
{
    if (set == zdd_true) return mtbdd_true;
    zddnode_t set_node = ZDD_GETNODE(set);
    uint32_t set_var = zddnode_getvariable(set_node);
    return mtbdd_makenode(set_var, mtbdd_false, zdd_set_to_mtbdd(zddnode_high(set, set_node)));
}

/**
 * Create a cube of literals of the given domain with the values given in <arr>.
 * Uses True as the leaf.
 */
ZDD
zdd_cube(ZDD dom, uint8_t *arr, ZDD leaf)
{
    if (dom == zdd_true) return leaf;
    const zddnode_t dom_node = ZDD_GETNODE(dom);
    const uint32_t dom_var = zddnode_getvariable(dom_node);
    const ZDD dom_next = zddnode_high(dom, dom_node);
    const ZDD res = zdd_cube(dom_next, arr+1, leaf);
    if (*arr == 0) {
        return zdd_makenode(dom_var, res, zdd_false);
    } else if (*arr == 1) {
        return zdd_makenode(dom_var, zdd_false, res);
    } else if (*arr == 2) {
        return zdd_makenode(dom_var, res, res);
    } else {
        return zdd_invalid;
    }
}

/**
 * Same as zdd_cube, but adds the cube to an existing set.
 */
TASK_IMPL_4(ZDD, zdd_union_cube, ZDD, set, ZDD, dom, uint8_t*, arr, ZDD, leaf)
{
    /**
     * Terminal cases
     */
    if (dom == zdd_true) return leaf;
    if (set == zdd_false) return zdd_cube(dom, arr, leaf);

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_UNION_CUBE);

    /**
     * Get set variable, domain variable, and next domain variable
     */
    const zddnode_t set_node = set == zdd_true ? NULL : ZDD_GETNODE(set);
    const uint32_t set_var = set_node == NULL || zddnode_isleaf(set_node) ? 0xffffffff : zddnode_getvariable(set_node);
    const zddnode_t dom_node = ZDD_GETNODE(dom);
    const uint32_t dom_var = zddnode_getvariable(dom_node);
    const ZDD dom_next = zddnode_high(dom, dom_node);

    assert(dom_var <= set_var);

    ZDD set0 = dom_var < set_var ? set : zddnode_low(set, set_node);
    ZDD set1 = dom_var < set_var ? zdd_false : zddnode_high(set, set_node);

    if (*arr == 0) {
        ZDD low = zdd_union_cube(set0, dom_next, arr+1, leaf);
        return zdd_makenode(dom_var, low, set1);
    } else if (*arr == 1) {
        ZDD high = zdd_union_cube(set1, dom_next, arr+1, leaf);
        return zdd_makenode(dom_var, set0, high);
    } else if (*arr == 2) {
        zdd_refs_spawn(SPAWN(zdd_union_cube, set0, dom_next, arr+1, leaf));
        ZDD high = zdd_union_cube(set1, dom_next, arr+1, leaf);
        zdd_refs_push(high);
        ZDD low = zdd_refs_sync(SYNC(zdd_union_cube));
        zdd_refs_pop(1);
        return zdd_makenode(dom_var, low, high);
    } else {
        assert(0);
        return zdd_invalid;
    }
}

/**
 * zdd_cube, but for clauses...
 * Add a clause, using a standard encoding: -1, 1, -2, 2, -3, 3, starting from 2n, 2n+1
 * Literals must be ordered by variable, no variable twice, ending with 0
 */
ZDD
zdd_clause(int32_t* clause)
{
    const int32_t c = *clause;
    if (c == 0) return zdd_true;
    const uint32_t v = c < 0 ? 2*(-c) : 2*c + 1;
    return zdd_makenode(v, zdd_false, zdd_clause(clause+1));
}

/**
 * Does the set contain the given clause
 * returns:
 * - 0 (no)
 * - 1 (yes, exactly)
 */
int
zdd_clause_contains(ZDD set, int32_t *clause)
{
    const int32_t c = *clause;
    if (set == zdd_false) return 0;
    if (set == zdd_true) return c == 0 ? 1 : 0;
    if (c == 0) return 0;

    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);
    const uint32_t v = c < 0 ? -2*c : 2*c + 1;

    if (set_var == v) return zdd_clause_contains(zddnode_high(set, set_node), clause+1);
    else if (set_var > v) return 0;
    else return zdd_clause_contains(zddnode_low(set, set_node), clause);
}

/**
 * zdd_union_cube, but for clauses...
 */
TASK_IMPL_2(ZDD, zdd_add_clause, ZDD, set, int32_t*, clause)
{
    if (set == zdd_false) return zdd_clause(clause);

    if (set & zdd_complement) {
        assert(set == zdd_true);
        // subsume empty clause
        return zdd_true;
        /*
        const int32_t c = *clause;
        if (c == 0) return zdd_true;
        const uint32_t v = c < 0 ? 2*(-c) : 2*c + 1;
        return zdd_makenode(v, zdd_true, zdd_clause(clause+1));
        */
    }

    const int32_t c = *clause;
    if (c == 0) {
        // subsume empty clause
        return zdd_true;
    }

    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);

    /*
    const int32_t c = *clause;
    if (c == 0) {
        const ZDD set0 = zddnode_low(set, set_node);
        if (set0 == zdd_true) {
            return set;
        } else {
            const ZDD low = zdd_add_clause(set0, clause);
            return zdd_makenode(set_var, low, zddnode_high(set, set_node));
        }
    }*/

    const uint32_t v = c < 0 ? 2*(-c) : 2*c + 1;
    if (set_var < v) {
        const ZDD set0 = zddnode_low(set, set_node);
        const ZDD low = zdd_add_clause(set0, clause);
        return zdd_makenode(set_var, low, zddnode_high(set, set_node));
    } else if (set_var > v) {
        return zdd_makenode(v, set, zdd_clause(clause+1));
    } else {
        const ZDD set1 = zddnode_high(set, set_node);
        const ZDD high = zdd_add_clause(set1, clause+1);
        return zdd_makenode(set_var, zddnode_low(set, set_node), high);
    }
}

/**
 * zdd_union_cube, but for clauses...
 * subsumption-free
 */
TASK_IMPL_2(ZDD, zdd_add_clause_sf, ZDD, set, int32_t*, clause)
{
    if (set == zdd_false) return zdd_clause(clause);
    if (set == zdd_true) return zdd_true; // empty clause subsumes all

    const int32_t c = *clause;
    if (c == 0) return zdd_true; // empty clause subsumes all

    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);

    const uint32_t v = c < 0 ? 2*(-c) : 2*c + 1;
    if (set_var < v) {
        /**
         * low  := recur(set_low, clause)
         * high := subsume(set_high, low)
         * result := node(set_var, low, high)
         */
        const ZDD low = zdd_refs_push(zdd_add_clause_sf(zddnode_low(set, set_node), clause));
        const ZDD high = zdd_clause_subsume(zddnode_high(set, set_node), low);
        zdd_refs_pop(1);
        return zdd_makenode(set_var, low, high);
    } else if (set_var > v) {
        /**
         * low := set
         * high := subsume(zdd_clause(...), set)
         * result := node(v, low)
         */
        ZDD result = zdd_refs_push(zdd_clause(clause+1));
        result = zdd_clause_subsume(result, set);
        zdd_refs_pop(1);
        return zdd_makenode(v, set, result);
    } else {
        /**
         * low := set_low
         * high := recur(set_high, clause+1) subsume set_low
         */
        ZDD result = zdd_refs_push(zdd_add_clause_sf(zddnode_high(set, set_node), clause+1));
        const ZDD set_low = zddnode_low(set, set_node);
        result = zdd_clause_subsume(result, set_low);
        zdd_refs_pop(1);
        return zdd_makenode(set_var, set_low, result);
    }
}

/**
 * Remove clauses from <A> that are subsumed by <B>
 */
TASK_IMPL_2(ZDD, zdd_clause_subsume, ZDD, a, ZDD, b)
{
    /**
     * Rules:
     * 0 \\ *    = 0       (no clauses)
     * * \\ 1    = 0       (empty clause subsumes everything)
     * 1 \\ Node = 1       (nothing subsumes empty clause)
     * 1 \\ 0    = 1       (nothing subsumes empty clause)
     * Node \\ 0 = Node    (no subsumption)
     * Node(v, pv, nv, dc) \\ Node(v, pv', nv', dc') =>
     *             Node(v, (pv \\ pv') \\ dc', (nv \\ nv') \\ dc', dc \\ dc')
     *                     (recursive)
     */
    if (a == zdd_false) return zdd_false;
    if (b == zdd_true) return zdd_false;
    if (a == zdd_true) return zdd_true;
    if (b == zdd_false) return a;

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_SUBSUME);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_SUBSUME, a, b, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_SUBSUME_CACHED);
        return result;
    }

    /**
     * Get variable (literal) of each node
     */
    const zddnode_t a_node = ZDD_GETNODE(a);
    const zddnode_t b_node = ZDD_GETNODE(b);
    const uint32_t a_var = zddnode_getvariable(a_node);
    const uint32_t b_var = zddnode_getvariable(b_node);

    /**
     * Compute minimum variable (variable)
     */
    const uint32_t minvar = (a_var/2 < b_var/2) ? (a_var/2) : (b_var/2);
    const uint32_t nv = minvar*2, pv = nv+1;

    ZDD a_nv, a_pv, a_dc;
    if (a_var == nv) {
        a_nv = zddnode_high(a, a_node);
        ZDD a0 = zddnode_low(a, a_node);
        if (a0 == zdd_false || a0 == zdd_true) {
            a_pv = zdd_false;
            a_dc = a0;
        } else {
            const zddnode_t a0_node = ZDD_GETNODE(a0);
            if (zddnode_getvariable(a0_node) == pv) {
                a_pv = zddnode_high(a0, a0_node);
                a_dc = zddnode_low(a0, a0_node);
            } else {
                a_pv = zdd_false;
                a_dc = a0;
            }
        }
    } else if (a_var == pv) {
        a_nv = zdd_false;
        a_pv = zddnode_high(a, a_node);
        a_dc = zddnode_low(a, a_node);
    } else {
        a_nv = zdd_false;
        a_pv = zdd_false;
        a_dc = a;
    }

    ZDD b_nv, b_pv, b_dc;
    if (b_var == nv) {
        b_nv = zddnode_high(b, b_node);
        ZDD b0 = zddnode_low(b, b_node);
        if (b0 == zdd_false || b0 == zdd_true) {
            b_pv = zdd_false;
            b_dc = b0;
        } else {
            const zddnode_t b0_node = ZDD_GETNODE(b0);
            if (zddnode_getvariable(b0_node) == pv) {
                b_pv = zddnode_high(b0, b0_node);
                b_dc = zddnode_low(b0, b0_node);
            } else {
                b_pv = zdd_false;
                b_dc = b0;
            }
        }
    } else if (b_var == pv) {
        b_nv = zdd_false;
        b_pv = zddnode_high(b, b_node);
        b_dc = zddnode_low(b, b_node);
    } else {
        b_nv = zdd_false;
        b_pv = zdd_false;
        b_dc = b;
    }

    /**
     * To compute:
     * - n := (a_nv \\ b_nv) \\ b_dc
     * - p := (a_pv \\ b_pv) \\ b_dc
     * - d := a_dc \\ b_dc
     */

    ZDD n, p, d;
    n = p = d = zdd_false;
    zdd_refs_pushptr(&n);
    zdd_refs_pushptr(&p);
    zdd_refs_pushptr(&d);

    zdd_refs_spawn(SPAWN(zdd_clause_subsume, a_dc, b_dc));
    zdd_refs_spawn(SPAWN(zdd_clause_subsume, a_nv, b_nv));
    p = zdd_clause_subsume(a_pv, b_pv);
    p = zdd_clause_subsume(p, b_dc);
    n = zdd_refs_sync(SYNC(zdd_clause_subsume));
    n = zdd_clause_subsume(n, b_dc);
    d = zdd_refs_sync(SYNC(zdd_clause_subsume));

    result = zdd_makenode(pv, d, p);
    result = zdd_makenode(nv, result, n);
    zdd_refs_popptr(3);

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_SUBSUME, a, b, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_SUBSUME_CACHEDPUT);
    }

    /**
     * Return result
     */
    return result;
}

/**
 */
TASK_IMPL_1(ZDD, zdd_clause_self_subsume, ZDD, set)
{
    if (set == zdd_true) return zdd_true;
    if (set == zdd_false) return zdd_false;
    
    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_SELF_SUBSUME);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_SELF_SUBSUME, set, 0, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_SELF_SUBSUME_CACHED);
        return result;
    }

    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);

    const uint32_t nv = set_var & ~1;
    const uint32_t pv = nv + 1;

    ZDD set_nv, set_pv, set_dc;
    if (set_var == nv) {
        set_nv = zddnode_high(set, set_node);
        const ZDD set0 = zddnode_low(set, set_node);
        if (set0 == zdd_true) {
            set_pv = zdd_false;
            set_dc = zdd_true;
        } else if (set0 == zdd_false) {
            set_pv = zdd_false;
            set_dc = zdd_false;
        } else {
            const zddnode_t set0_node = ZDD_GETNODE(set0);
            if (zddnode_getvariable(set0_node) == pv) {
                set_pv = zddnode_high(set0, set0_node);
                set_dc = zddnode_low(set0, set0_node);
            } else {
                set_pv = zdd_false;
                set_dc = set0;
            }
        }
    } else {
        set_nv = zdd_false;
        set_pv = zddnode_high(set, set_node);
        set_dc = zddnode_low(set, set_node);
    }

    /**
     * To compute:
     * n := SF(nv) \\ SF(dc)
     * p := SF(pv) \\ SF(dc)
     * d := SF(dc)
     *
     * So:
     * d := SF(dc)
     * p := SF(pv) \\ d
     * n := SF(nv) \\ d
     */

    ZDD p, n, d;
    p = n = d = zdd_false;
    zdd_refs_pushptr(&p);
    zdd_refs_pushptr(&n);
    zdd_refs_pushptr(&d);

    zdd_refs_spawn(SPAWN(zdd_clause_self_subsume, set_pv));
    zdd_refs_spawn(SPAWN(zdd_clause_self_subsume, set_nv));
    d = zdd_clause_self_subsume(set_dc);
    n = zdd_refs_sync(SYNC(zdd_clause_self_subsume));
    p = zdd_refs_sync(SYNC(zdd_clause_self_subsume));
    zdd_refs_spawn(SPAWN(zdd_clause_subsume, p, d));
    n = zdd_clause_subsume(n, d);
    p = zdd_refs_sync(SYNC(zdd_clause_subsume));

    result = zdd_makenode(pv, d, p);
    result = zdd_makenode(nv, result, n);
    zdd_refs_popptr(3);

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_SELF_SUBSUME, set, 0, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_SELF_SUBSUME_CACHEDPUT);
    }

    /**
     * Return result
     */
    return result;
}

/**
 * Return all clauses that have at least one of the (ZDD) variables in the given set
 * The govem <lits> can be either a ZDD domain or a cube of literals
 */
TASK_IMPL_2(ZDD, zdd_clause_environment, ZDD, set, ZDD, lits)
{
    /**
     * Terminal cases
     */
    if (set == zdd_false) return zdd_false;
    if (set == zdd_true) return zdd_false; // an empty clause is not in the environment
    if (lits == zdd_true) return zdd_false; // no literals = no environment
    assert(lits != zdd_false);

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_ENVIRONMENT);

    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);
    zddnode_t lits_node = ZDD_GETNODE(lits);
    uint32_t lits_var = zddnode_getvariable(lits_node);
    ZDD lits_next = zddnode_high(lits, lits_node);

    while (lits_var < set_var) {
        if (lits_next == zdd_true) return zdd_false;
        lits = lits_next;
        lits_node = ZDD_GETNODE(lits);
        lits_var = zddnode_getvariable(lits_node);
        lits_next = zddnode_high(lits, lits_node);
    }

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_ENVIRONMENT, set, lits, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_ENVIRONMENT_CACHED);
        return result;
    }

    if (set_var < lits_var) {
        // current var not in lits
        const ZDD set0 = zddnode_low(set, set_node);
        const ZDD set1 = zddnode_high(set, set_node);
        zdd_refs_spawn(SPAWN(zdd_clause_environment, set0, lits));
        ZDD high = zdd_refs_push(zdd_clause_environment(set1, lits));
        ZDD low = zdd_refs_sync(SYNC(zdd_clause_environment));
        zdd_refs_pop(1);
        result = zdd_makenode(set_var, low, high);
    } else /* set_var == lits_var */ {
        const ZDD set0 = zddnode_low(set, set_node);
        const ZDD set1 = zddnode_high(set, set_node);
        ZDD low = zdd_clause_environment(set0, lits_next);
        result = zdd_makenode(set_var, low, set1);
    }

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_ENVIRONMENT, set, lits, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_ENVIRONMENT_CACHEDPUT);
    }

    return result;
}

/**
 * Propagate units (simple version)
 * Also reduces subsets containing empty clause to empty clause
 */
TASK_IMPL_2(ZDD, zdd_clause_up, ZDD, set, ZDD, units)
{
    // if (set & zdd_complement) assert(set == zdd_true);
    if (set & zdd_complement) return zdd_true;
    // if (set == zdd_true) return set;
    if (set == zdd_false) return set;
    if (units == zdd_true) return set;
    assert(units != zdd_true);

    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);

    zddnode_t units_node = ZDD_GETNODE(units);
    uint32_t units_var = zddnode_getvariable(units_node);
    ZDD units_next = zddnode_low(units, units_node);

    while ((units_var/2) < (set_var/2)) {
        if (units_next == zdd_true) return set;
        units = units_next;
        units_node = ZDD_GETNODE(units);
        units_var = zddnode_getvariable(units_node);
        units_next = zddnode_low(units, units_node);
    }

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_UP);

    /**
     * Check the cache
     */
    MTBDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_UP, set, units, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_UP_CACHED);
        return result;
    }

    if (units_var == set_var) {
        // same literal
        result = zdd_clause_up(zddnode_low(set, set_node), units);
    } else if ((units_var/2) == (set_var/2)) {
        // different literal, same variable
        const ZDD set0 = zddnode_low(set, set_node);
        const ZDD set1 = zddnode_high(set, set_node);
        zdd_refs_spawn(SPAWN(zdd_clause_up, set0, units));
        ZDD high = zdd_refs_push(zdd_clause_up(set1, units));
        ZDD low = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_up)));
        if (high & zdd_complement || low & zdd_complement) {
            // low or high contains empty clause
            result = zdd_true;
        } else {
            result = zdd_clause_or(low, high);
            assert((result & zdd_complement) == 0);
        }
        zdd_refs_pop(2);
    } else {
        const ZDD set0 = zddnode_low(set, set_node);
        const ZDD set1 = zddnode_high(set, set_node);
        zdd_refs_spawn(SPAWN(zdd_clause_up, set0, units));
        ZDD high = zdd_refs_push(zdd_clause_up(set1, units));
        ZDD low = zdd_refs_sync(SYNC(zdd_clause_up));
        zdd_refs_pop(1);
        if (low & zdd_complement) {
            result = zdd_true;
        } else if (low != set0 || high != set1) {
            result = zdd_makenode(set_var, low, high);
        } else {
            result = set;
        }
    }

    if (result & zdd_complement) assert(result == zdd_true);

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_UP, set, units, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_UP_CACHEDPUT);
    }

    return result;
}

/**
 * Subsumption-free union (if inputs are s-f)
 */
TASK_IMPL_2(ZDD, zdd_clause_union, ZDD, a, ZDD, b)
{
    /**
     * Trivial cases
     */
    if (a == zdd_false) return b;
    if (b == zdd_false) return a;
    if (a == b) return a;
    if (a == zdd_true) return zdd_true; // contains empty clause
    if (b == zdd_true) return zdd_true; // contains empty clause

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_UNION);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_UNION, a, b, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_UNION_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    const zddnode_t a_node = zdd_isleaf(a) ? NULL : ZDD_GETNODE(a);
    const uint32_t a_var = a_node == NULL ? 0xffffffff : zddnode_getvariable(a_node);
    const zddnode_t b_node = zdd_isleaf(b) ? NULL : ZDD_GETNODE(b);
    const uint32_t b_var = b_node == NULL ? 0xffffffff : zddnode_getvariable(b_node);
    uint32_t minvar = a_var < b_var ? a_var : b_var;
    assert(minvar != 0xffffffff);

    /**
     * Compute nv and pv
     */

    const uint32_t nv = minvar & ~1;
    const uint32_t pv = nv + 1;

    ZDD a_nv, a_pv, a_dc;
    if (a_var == nv) {
        a_nv = zddnode_high(a, a_node);
        const ZDD a0 = zddnode_low(a, a_node);
        if (a0 == zdd_true) {
            a_pv = zdd_false;
            a_dc = zdd_true;
        } else if (a0 == zdd_false) {
            a_pv = zdd_false;
            a_dc = zdd_false;
        } else {
            const zddnode_t a0_node = ZDD_GETNODE(a0);
            if (zddnode_getvariable(a0_node) == pv) {
                a_pv = zddnode_high(a0, a0_node);
                a_dc = zddnode_low(a0, a0_node);
            } else {
                a_pv = zdd_false;
                a_dc = a0;
            }
        }
    } else if (a_var == pv) {
        a_nv = zdd_false;
        a_pv = zddnode_high(a, a_node);
        a_dc = zddnode_low(a, a_node);
    } else {
        a_nv = a_pv = zdd_false;
        a_dc = a;
    }

    ZDD b_nv, b_pv, b_dc;
    if (b_var == nv) {
        b_nv = zddnode_high(b, b_node);
        const ZDD b0 = zddnode_low(b, b_node);
        if (b0 == zdd_true) {
            b_pv = zdd_false;
            b_dc = zdd_true;
        } else if (b0 == zdd_false) {
            b_pv = zdd_false;
            b_dc = zdd_false;
        } else {
            const zddnode_t b0_node = ZDD_GETNODE(b0);
            if (zddnode_getvariable(b0_node) == pv) {
                b_pv = zddnode_high(b0, b0_node);
                b_dc = zddnode_low(b0, b0_node);
            } else {
                b_pv = zdd_false;
                b_dc = b0;
            }
        }
    } else if (b_var == pv) {
        b_nv = zdd_false;
        b_pv = zddnode_high(b, b_node);
        b_dc = zddnode_low(b, b_node);
    } else {
        b_nv = b_pv = zdd_false;
        b_dc = b;
    }

    /**
     * To compute:
     * res_dc := a_dc U b_dc
     * res_pv := (a_pv U b_pv) \\ res_dc
     * res_nv := (a_nv U b_nv) \\ res_dc
     */

    ZDD res_dc, res_pv, res_nv;
    res_dc = res_pv = res_nv = zdd_false;
    zdd_refs_pushptr(&res_dc);
    zdd_refs_pushptr(&res_nv);
    zdd_refs_pushptr(&res_pv);

    zdd_refs_spawn(SPAWN(zdd_clause_union, a_nv, b_nv));
    zdd_refs_spawn(SPAWN(zdd_clause_union, a_pv, b_pv));
    res_dc = zdd_clause_union(a_dc, b_dc);
    res_pv = zdd_refs_sync(SYNC(zdd_clause_union));
    res_nv = zdd_refs_sync(SYNC(zdd_clause_union));

    zdd_refs_spawn(SPAWN(zdd_clause_subsume, res_nv, res_dc));
    res_pv = zdd_clause_subsume(res_pv, res_dc);
    res_nv = zdd_refs_sync(SYNC(zdd_clause_subsume));

    result = zdd_makenode(pv, res_dc, res_pv);
    result = zdd_makenode(nv, result, res_nv);

    zdd_refs_popptr(3);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_UNION, a, b, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_UNION_CACHEDPUT);
    }

    return result;
}

/**
 * Implementation of the OR operator for Boolean ZDDs
 * But also reduces things with empty clause to empty clause
 */
TASK_IMPL_2(ZDD, zdd_clause_or, ZDD, a, ZDD, b)
{
    /**
     * Trivial cases (similar to sylvan_ite)
     */
    if (a == zdd_false) return b;
    if (b == zdd_false) return a;
    if (a == b) return a;
    if (a == zdd_true) return zdd_true; // contains empty clause
    if (b == zdd_true) return zdd_true; // contains empty clause

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_OR);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_OR, a, b, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_OR_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    const zddnode_t a_node = zdd_isleaf(a) ? NULL : ZDD_GETNODE(a);
    const uint32_t a_var = a_node == NULL ? 0xffffffff : zddnode_getvariable(a_node);
    const zddnode_t b_node = zdd_isleaf(b) ? NULL : ZDD_GETNODE(b);
    const uint32_t b_var = b_node == NULL ? 0xffffffff : zddnode_getvariable(b_node);
    uint32_t minvar = a_var < b_var ? a_var : b_var;
    assert(minvar != 0xffffffff);

    /**
     * Get the cofactors
     */
    ZDD a0 = minvar < a_var ? a : zddnode_low(a, a_node);
    ZDD a1 = minvar < a_var ? zdd_false : zddnode_high(a, a_node);
    ZDD b0 = minvar < b_var ? b : zddnode_low(b, b_node);
    ZDD b1 = minvar < b_var ? zdd_false : zddnode_high(b, b_node);

    /**
     * Now we call recursive tasks
     */
    zdd_refs_spawn(SPAWN(zdd_clause_or, a0, b0));
    ZDD high = CALL(zdd_clause_or, a1, b1);
    zdd_refs_push(high);
    ZDD low = zdd_refs_sync(SYNC(zdd_clause_or));
    zdd_refs_pop(1);

    /**
     * Compute result node
     */
    result = zdd_makenode(minvar, low, high);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_OR, a, b, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_OR_CACHEDPUT);
    }

    return result;
}

/**
 * Implementation of intersection of two clause sets
 * 0 X b = 0
 * a X 0 = 0
 * 1 X b = b
 * a X 1 = a
 * a X a = a
 * (v,a1,a0) X (v,b1,b0) = (v,a1 X b1,a0 X b0)
 */
TASK_IMPL_2(ZDD, zdd_clause_intersect, ZDD, a, ZDD, b)
{
    /**
     * Check the case where A or B is False
     */
    if (a == zdd_false || b == zdd_false) return zdd_false;
    if (a == zdd_true) return b;
    if (b == zdd_true) return a;
    if (a == b) return a;

    /**
     * Switch A and B if A > B (for cache)
     */
    if (ZDD_GETINDEX(a) > ZDD_GETINDEX(b)) {
        ZDD t = a;
        a = b;
        b = t;
    }

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_INTERSECT);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_INTERSECT, a, b, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_INTERSECT_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    const zddnode_t a_node = ZDD_GETNODE(a);
    const uint32_t a_var = zddnode_getvariable(a_node);
    const zddnode_t b_node = ZDD_GETNODE(b);
    const uint32_t b_var = zddnode_getvariable(b_node);
    uint32_t minvar = a_var < b_var ? a_var : b_var;

    /**
     * Get cofactors for A and B
     */
    ZDD a0 = minvar < a_var ? a : zddnode_low(a, a_node);
    ZDD a1 = minvar < a_var ? zdd_false : zddnode_high(a, a_node);
    ZDD b0 = minvar < b_var ? b : zddnode_low(b, b_node);
    ZDD b1 = minvar < b_var ? zdd_false : zddnode_high(b, b_node);

    /**
     * Now we call recursive tasks
     */
    ZDD low, high;
    if (a1 == zdd_false || b1 == zdd_false) {
        low = zdd_clause_intersect(a0, b0);
        high = zdd_false;
    } else {
        zdd_refs_spawn(SPAWN(zdd_clause_intersect, a0, b0));
        high = zdd_clause_intersect(a1, b1);
        zdd_refs_push(high);
        low = zdd_refs_sync(SYNC(zdd_clause_intersect));
        zdd_refs_pop(1);
    }

    /**
     * Compute result node
     */
    result = zdd_makenode(minvar, low, high);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_INTERSECT, a, b, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_INTERSECT_CACHEDPUT);
    }

    return result;
}

/**
 * Expand...
 */
TASK_IMPL_1(ZDD, zdd_clause_expand, ZDD, set)
{
    if (set == zdd_true) return mtbdd_false; // the empty clause
    if (set == zdd_false) return mtbdd_true; // no clause

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_EXPAND);

    /**
     * Check the cache
     */
    MTBDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_EXPAND, set, 0, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_EXPAND_CACHED);
        return result;
    }

    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);

    const uint32_t nv = set_var & ~1;
    const uint32_t pv = nv + 1;

    ZDD set_nv, set_pv, set_dc;
    if (set_var == nv) {
        set_nv = zddnode_high(set, set_node);
        const ZDD set0 = zddnode_low(set, set_node);
        if (set0 == zdd_true) {
            set_pv = zdd_false;
            set_dc = zdd_true;
        } else if (set0 == zdd_false) {
            set_pv = zdd_false;
            set_dc = zdd_false;
        } else {
            const zddnode_t set0_node = ZDD_GETNODE(set0);
            if (zddnode_getvariable(set0_node) == pv) {
                set_pv = zddnode_high(set0, set0_node);
                set_dc = zddnode_low(set0, set0_node);
            } else {
                set_pv = zdd_false;
                set_dc = set0;
            }
        }
    } else {
        set_nv = zdd_false;
        set_pv = zddnode_high(set, set_node);
        set_dc = zddnode_low(set, set_node);
    }

    zdd_refs_spawn(SPAWN(zdd_clause_expand, set_nv));
    zdd_refs_spawn(SPAWN(zdd_clause_expand, set_pv));
    ZDD res_dc = zdd_refs_push(zdd_clause_expand(set_dc));
    ZDD res_pv = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_expand)));
    ZDD res_nv = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_expand)));

    zdd_refs_spawn(SPAWN(zdd_or, res_dc, res_nv));
    res_pv = zdd_refs_push(zdd_or(res_dc, res_pv));
    res_nv = zdd_refs_sync(SYNC(zdd_or));

    result = zdd_makenode(pv, zdd_false, res_pv);
    result = zdd_makenode(nv, result, res_nv);
    zdd_refs_pop(4);

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_EXPAND, set, 0, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_EXPAND_CACHEDPUT);
    }

    return result;
}


/**
 * Compute clause distribution of A and B, also remove subsumed clauses.
 * Assumes A and B are subsumption-free.
 * 0 Xs b = 0
 * 1 Xs b = b
 * a Xs 0 = 0
 * a Xs 1 = b
 * a Xs a = a
 * (v,a1,a0,adc) Xs (v,b1,b0,bdc) = ... (complex)
 */
TASK_IMPL_2(ZDD, zdd_clause_distribution, ZDD, a, ZDD, b)
{
    /**
     * Trivial cases
     */
    if (a == zdd_false) return zdd_false;
    if (b == zdd_false) return zdd_false;
    if (a == b) return a;
    if (a == zdd_true) return b;
    if (b == zdd_true) return a;

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_DISTRIBUTION);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_DISTRIBUTION, a, b, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_DISTRIBUTION_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    const zddnode_t a_node = zdd_isleaf(a) ? NULL : ZDD_GETNODE(a);
    const uint32_t a_var = a_node == NULL ? 0xffffffff : zddnode_getvariable(a_node);
    const zddnode_t b_node = zdd_isleaf(b) ? NULL : ZDD_GETNODE(b);
    const uint32_t b_var = b_node == NULL ? 0xffffffff : zddnode_getvariable(b_node);
    uint32_t minvar = a_var < b_var ? a_var : b_var;
    assert(minvar != 0xffffffff);

    /**
     * Compute nv and pv
     */

    const uint32_t nv = minvar & ~1;
    const uint32_t pv = nv + 1;

    ZDD a_nv, a_pv, a_dc;
    if (a_var == nv) {
        a_nv = zddnode_high(a, a_node);
        const ZDD a0 = zddnode_low(a, a_node);
        if (a0 == zdd_true) {
            a_pv = zdd_false;
            a_dc = zdd_true;
        } else if (a0 == zdd_false) {
            a_pv = zdd_false;
            a_dc = zdd_false;
        } else {
            const zddnode_t a0_node = ZDD_GETNODE(a0);
            if (zddnode_getvariable(a0_node) == pv) {
                a_pv = zddnode_high(a0, a0_node);
                a_dc = zddnode_low(a0, a0_node);
            } else {
                a_pv = zdd_false;
                a_dc = a0;
            }
        }
    } else if (a_var == pv) {
        a_nv = zdd_false;
        a_pv = zddnode_high(a, a_node);
        a_dc = zddnode_low(a, a_node);
    } else {
        a_nv = zdd_false;
        a_pv = zdd_false;
        a_dc = a;
    }

    ZDD b_nv, b_pv, b_dc;
    if (b_var == nv) {
        b_nv = zddnode_high(b, b_node);
        const ZDD b0 = zddnode_low(b, b_node);
        if (b0 == zdd_true) {
            b_pv = zdd_false;
            b_dc = zdd_true;
        } else if (b0 == zdd_false) {
            b_pv = zdd_false;
            b_dc = zdd_false;
        } else {
            const zddnode_t b0_node = ZDD_GETNODE(b0);
            if (zddnode_getvariable(b0_node) == pv) {
                b_pv = zddnode_high(b0, b0_node);
                b_dc = zddnode_low(b0, b0_node);
            } else {
                b_pv = zdd_false;
                b_dc = b0;
            }
        }
    } else if (b_var == pv) {
        b_nv = zdd_false;
        b_pv = zddnode_high(b, b_node);
        b_dc = zddnode_low(b, b_node);
    } else {
        b_nv = b_pv = zdd_false;
        b_dc = b;
    }

    ZDD res_pv, res_nv, res_dc;
    res_pv = res_nv = res_dc = zdd_false;
    zdd_refs_pushptr(&res_pv);
    zdd_refs_pushptr(&res_nv);
    zdd_refs_pushptr(&res_dc);

    zdd_refs_spawn(SPAWN(zdd_clause_distribution, a_nv, b_nv));
    zdd_refs_spawn(SPAWN(zdd_clause_distribution, a_nv, b_dc));
    zdd_refs_spawn(SPAWN(zdd_clause_distribution, a_dc, b_nv));

    zdd_refs_spawn(SPAWN(zdd_clause_distribution, a_pv, b_pv));
    zdd_refs_spawn(SPAWN(zdd_clause_distribution, a_pv, b_dc));
    zdd_refs_spawn(SPAWN(zdd_clause_distribution, a_dc, b_pv));

    res_dc = zdd_clause_distribution(a_dc, b_dc);

    ZDD dcpv = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_distribution)));
    ZDD pvdc = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_distribution)));
    res_pv = zdd_clause_union(pvdc, dcpv);
    zdd_refs_pop(2);
    ZDD pvpv = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_distribution)));
    res_pv = zdd_clause_union(res_pv, pvpv);
    zdd_refs_pop(1);
    res_pv = zdd_clause_subsume(res_pv, res_dc);

    ZDD dcnv = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_distribution)));
    ZDD nvdc = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_distribution)));
    zdd_refs_pop(2);
    res_nv = zdd_clause_union(nvdc, dcnv);
    ZDD nvnv = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_distribution)));
    res_nv = zdd_clause_union(res_nv, nvnv);
    zdd_refs_pop(1);
    res_nv = zdd_clause_subsume(res_nv, res_dc);

    result = zdd_makenode(pv, res_dc, res_pv);
    result = zdd_makenode(nv, result, res_nv);
    zdd_refs_popptr(3);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_DISTRIBUTION, a, b, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_DISTRIBUTION_CACHEDPUT);
    }

    return result;
}


/**
 * Perform Quine-McCluskey style resolution...
 * (merge all clauses of the form C,x C,!x)
 */
TASK_IMPL_1(ZDD, zdd_clause_qmc, ZDD, set)
{
    /**
     * Trivial cases
     */
    if (set == zdd_false) return zdd_false;
    if (set == zdd_true) return zdd_true;

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_QMC);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_QMC, set, 0, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_QMC_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);

    /**
     * Compute nv and pv
     */
    const uint32_t nv = set_var & ~1;
    const uint32_t pv = nv + 1;

    ZDD set_nv, set_pv, set_dc;
    if (set_var == nv) {
        set_nv = zddnode_high(set, set_node);
        const ZDD set0 = zddnode_low(set, set_node);
        if (set0 == zdd_true) {
            set_pv = zdd_false;
            set_dc = zdd_true;
        } else if (set0 == zdd_false) {
            set_pv = zdd_false;
            set_dc = zdd_false;
        } else {
            const zddnode_t set0_node = ZDD_GETNODE(set0);
            if (zddnode_getvariable(set0_node) == pv) {
                set_pv = zddnode_high(set0, set0_node);
                set_dc = zddnode_low(set0, set0_node);
            } else {
                set_pv = zdd_false;
                set_dc = set0;
            }
        }
    } else {
        set_nv = zdd_false;
        set_pv = zddnode_high(set, set_node);
        set_dc = zddnode_low(set, set_node);
    }

    ZDD p, n, d, pn;
    p = n = d = pn = zdd_false;
    zdd_refs_pushptr(&p);
    zdd_refs_pushptr(&n);
    zdd_refs_pushptr(&d);
    zdd_refs_pushptr(&pn);

    /*
    p = zdd_clause_qmc(set_pv);
    n = zdd_clause_qmc(set_nv);
    d = zdd_clause_qmc(set_dc);
    pn = zdd_clause_intersect(p, n);
    d = zdd_clause_union(d, pn);
    p = zdd_clause_subsume(p, d);
    n = zdd_clause_subsume(n, d);
    */

    zdd_refs_spawn(SPAWN(zdd_clause_qmc, set_dc));
    zdd_refs_spawn(SPAWN(zdd_clause_qmc, set_nv));
    p = CALL(zdd_clause_qmc, set_pv);
    n = zdd_refs_sync(SYNC(zdd_clause_qmc));
    pn = CALL(zdd_clause_intersect, n, p);
    d = zdd_refs_sync(SYNC(zdd_clause_qmc));
    d = CALL(zdd_clause_union, d, pn);

    zdd_refs_spawn(SPAWN(zdd_clause_subsume, n, d));
    zdd_refs_spawn(SPAWN(zdd_clause_subsume, p, d));
    p = zdd_refs_sync(SYNC(zdd_clause_subsume));
    n = zdd_refs_sync(SYNC(zdd_clause_subsume));

    result = zdd_makenode(pv, d, p);
    result = zdd_makenode(nv, result, n);

    zdd_refs_popptr(4);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_QMC, set, 0, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_QMC_CACHEDPUT);
    }

    return result;
}

/**
 * Compute the cofactor of a clause set given literal <lit>.
 */
TASK_IMPL_2(ZDD, zdd_clause_cof, ZDD, set, uint32_t, lit)
{
    /**
     * Trivial cases
     */
    if (set == zdd_false) return zdd_false;

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_COF);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_COF, set, lit, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_COF_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    const zddnode_t set_node = set == zdd_true ? NULL : ZDD_GETNODE(set);
    const uint32_t set_var = set == zdd_true ? 0xffffffff : zddnode_getvariable(set_node);
    const uint32_t minvar = set_var < lit ? set_var : lit;

    /**
     * Compute nv and pv
     */

    const uint32_t nv = minvar & ~1;
    const uint32_t pv = nv + 1;

    /**
     * Compute cofactors
     */

    ZDD set_nv, set_pv, set_dc;
    if (set_var == nv) {
        set_nv = zddnode_high(set, set_node);
        const ZDD set0 = zddnode_low(set, set_node);
        if (set0 == zdd_true) {
            set_pv = zdd_false;
            set_dc = zdd_true;
        } else if (set0 == zdd_false) {
            set_pv = zdd_false;
            set_dc = zdd_false;
        } else {
            const zddnode_t set0_node = ZDD_GETNODE(set0);
            if (zddnode_getvariable(set0_node) == pv) {
                set_pv = zddnode_high(set0, set0_node);
                set_dc = zddnode_low(set0, set0_node);
            } else {
                set_pv = zdd_false;
                set_dc = set0;
            }
        }
    } else if (set_var == pv) {
        set_nv = zdd_false;
        set_pv = zddnode_high(set, set_node);
        set_dc = zddnode_low(set, set_node);
    } else {
        set_nv = zdd_false;
        set_pv = zdd_false;
        set_dc = set;
    }

    if (nv == lit) {
        // union_sub of pv and dc
        return zdd_clause_union(set_pv, set_dc);
    } else if (pv == lit) {
        // union_sub of nv and dc
        return zdd_clause_union(set_nv, set_dc);
    } else {
        // lit > pv , so just ordinary recursion
        zdd_refs_spawn(SPAWN(zdd_clause_cof, set_nv, lit));
        zdd_refs_spawn(SPAWN(zdd_clause_cof, set_pv, lit));
        ZDD res_dc = zdd_refs_push(zdd_clause_cof(set_dc, lit));
        ZDD res_pv = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_cof)));
        ZDD res_nv = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_cof)));
        result = zdd_makenode(pv, res_dc, res_pv);
        result = zdd_makenode(nv, result, res_nv);
        zdd_refs_pop(3);
    }

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_COF, set, lit, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_COF_CACHEDPUT);
    }

    return result;
}

/**
 * Compute the BDD of all assignments that match the given clauses
 * restricted to the given invariant <inv>. (set to True for no restriction)
 */
TASK_IMPL_2(MTBDD, zdd_clause_sat, ZDD, set, MTBDD, inv)
{
    if (inv == mtbdd_false) return mtbdd_false;
    if (set == zdd_true) return mtbdd_false; // the empty clause
    if (set == zdd_false) return mtbdd_true; // no clause

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_SAT);

    /**
     * Check the cache
     */
    MTBDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_SAT, set, inv, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_SAT_CACHED);
        return result;
    }

    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);
    const mtbddnode_t inv_node = inv == mtbdd_true ? NULL : MTBDD_GETNODE(inv);
    const uint32_t inv_var = inv == mtbdd_true ? 0xffffffff : mtbddnode_getvariable(inv_node);

    const uint32_t nv = set_var & ~1;
    const uint32_t pv = nv + 1;
    const uint32_t v = nv/2;

    if (inv_var < v) {
        // invariant before clauses
        const MTBDD inv0 = mtbddnode_followlow(inv, inv_node);
        const MTBDD inv1 = mtbddnode_followhigh(inv, inv_node);
        mtbdd_refs_spawn(SPAWN(zdd_clause_sat, set, inv0));
        MTBDD high = mtbdd_refs_push(zdd_clause_sat(set, inv1));
        MTBDD low = mtbdd_refs_sync(SYNC(zdd_clause_sat));
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(inv_var, low, high);
    } else {
        ZDD set_nv, set_pv, set_dc;
        if (set_var == nv) {
            set_nv = zddnode_high(set, set_node);
            const ZDD set0 = zddnode_low(set, set_node);
            if (set0 == zdd_true) {
                set_pv = zdd_false;
                set_dc = zdd_true;
            } else if (set0 == zdd_false) {
                set_pv = zdd_false;
                set_dc = zdd_false;
            } else {
                const zddnode_t set0_node = ZDD_GETNODE(set0);
                if (zddnode_getvariable(set0_node) == pv) {
                    set_pv = zddnode_high(set0, set0_node);
                    set_dc = zddnode_low(set0, set0_node);
                } else {
                    set_pv = zdd_false;
                    set_dc = set0;
                }
            }
        } else {
            set_nv = zdd_false;
            set_pv = zddnode_high(set, set_node);
            set_dc = zddnode_low(set, set_node);
        }

        // low: (var is false): then set_pv AND set_dc
        // high: (var is true): then set_nv AND set_dc

        const MTBDD inv0 = (inv_var == v) ? mtbddnode_followlow(inv, inv_node) : inv;
        const MTBDD inv1 = (inv_var == v) ? mtbddnode_followhigh(inv, inv_node) : inv;

        mtbdd_refs_spawn(SPAWN(zdd_clause_sat, set_nv, inv1));
        mtbdd_refs_spawn(SPAWN(zdd_clause_sat, set_pv, inv0));
        MTBDD Fdc = mtbdd_refs_push(zdd_clause_sat(set_dc, inv));
        MTBDD F1 = mtbdd_refs_push(mtbdd_refs_sync(SYNC(zdd_clause_sat)));
        MTBDD F0 = mtbdd_refs_push(mtbdd_refs_sync(SYNC(zdd_clause_sat)));
        // mtbdd_refs_spawn(SPAWN(sylvan_and, Fdc, F1, 0));
        // MTBDD high = mtbdd_refs_push(sylvan_and(Fdc, F0));
        // MTBDD low = mtbdd_refs_sync(SYNC(sylvan_and));
        // mtbdd_refs_pop(4);
        // result = mtbdd_makenode(nv/2, low, high);
        result = mtbdd_makenode(v, F1, F0);
        mtbdd_refs_pop(2); // F0 and F1
        mtbdd_refs_push(result);
        result = sylvan_and(result, Fdc);
        mtbdd_refs_pop(2); // Fdc and result
    }

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_SAT, set, inv, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_SAT_CACHEDPUT);
    }

    return result;
}

/**
 * Compute the set of variables involved in a set of clauses
 * (not the literals; the variables!)
 */
TASK_IMPL_1(ZDD, zdd_clause_support, ZDD, dd)
{
    if (dd == zdd_true || dd == zdd_false) return zdd_true;

    /**
     * Perhaps execute garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_SUPPORT);

    /**
     * Consult cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_CLAUSE_SUPPORT, dd, 0, 0, &result)) {
        sylvan_stats_count(ZDD_CLAUSE_SUPPORT_CACHED);
        return result;
    }

    const zddnode_t dd_node = ZDD_GETNODE(dd);
    const ZDD dd0 = zddnode_low(dd, dd_node);
    const ZDD dd1 = zddnode_high(dd, dd_node);
    zdd_refs_spawn(SPAWN(zdd_clause_support, dd0));
    ZDD high = zdd_refs_push(CALL(zdd_clause_support, dd1));
    ZDD low = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_support)));
    result = zdd_set_union(low, high);
    zdd_refs_pop(2);
    const uint32_t v = zddnode_getvariable(dd_node) / 2;
    if (result == zdd_true || zdd_getvar(result) != v) {
        result = zdd_makenode(v, result, result);
    }

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_ZDD_CLAUSE_SUPPORT, dd, 0, 0, result)) {
        sylvan_stats_count(ZDD_CLAUSE_SUPPORT_CACHEDPUT);
    }

    return result;
}

/**
 * Extract all unit clauses from a set of clauses into set of literals
 * Returns zdd_false to indicate the empty clause is found.
 */
TASK_IMPL_1(ZDD, zdd_clause_units, ZDD, set)
{
    if (set & zdd_complement) return zdd_false; // contains empty clause!
    if (set == zdd_false) return zdd_true; // no units
    
    const zddnode_t set_node = ZDD_GETNODE(set);
    const ZDD sub0 = zdd_clause_units(zddnode_low(set, set_node));

    if (zddnode_high(set, set_node) & zdd_complement) {
        /* found a unit */
        if (zddnode_high(set, set_node) != zdd_true) printf("oi\n");
        return zdd_makenode(zddnode_getvariable(set_node), sub0, sub0);
    } else {
        /* not a unit */
        return sub0;
    }
}

int zdd_clause_units_contradict(ZDD set)
{
    if (set == zdd_true) return 0;
    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);
    const ZDD set0 = zddnode_high(set, set_node);
    if ((set_var & 1) == 0) {
        // check it
        if (set0 == zdd_true) return 0;
        const zddnode_t set0_node = ZDD_GETNODE(set0);
        if (zddnode_getvariable(set0_node) == set_var+1) return 1;
        else return zdd_clause_units_contradict(set0);
    } else {
        return zdd_clause_units_contradict(set0);
    }
}

/**
 * Given the support (zdd_support) of a set of clauses, compute the pure literals
 */
ZDD zdd_clause_pure(ZDD set)
{
    // No literals, then no pure literals
    if (set == zdd_true) return zdd_true;
    const zddnode_t set_node = ZDD_GETNODE(set);
    const uint32_t set_var = zddnode_getvariable(set_node);
    const ZDD set0 = zddnode_high(set, set_node);
    // Only one literal, then it's a pure literal
    if (set0 == zdd_true) return set;
    const zddnode_t set0_node = ZDD_GETNODE(set0);
    const uint32_t set0_var = zddnode_getvariable(set0_node);
    if (set_var/2 == set0_var/2) {
        // Literals of same variable, so not pure.
        return zdd_clause_pure(zddnode_high(set0, set0_node));
    } else {
        // Literals of different variable, so pure.
        ZDD res = zdd_clause_pure(set0);
        return zdd_makenode(set_var, res, res);
    }
}

/**
 * ISOP algorithm as implemented in Cudd
 * Given lower bound L and upper bound U as BDDs.
 *
 * if L == 0 return 0, 0
 * if U == 1 return 1, 1
 * if U == 0 or L == 1 abort
 *
 * check cache
 *
 * v := minvar(L, U)
 * Lv, Lnv, Uv, Unv := cofactor(L, v), cofactor(U, v)
 * 
 * Lsub0 := and(Lnv, !Uv)
 * Usub0 := Unv
 * Lsub1 := and(Lv, !Unv)
 * Usub1 := Uv
 *
 * Isub0, zddIsub0 := ISOP(Lsub0, Usub0)
 * Isub1, zddIsub1 := ISOP(Lsub1, Usub1)
 * 
 * Lsuper0 := and(Lnv, !Isub0)
 * Lsuper1 := and(Lv, !Isub1)
 * Usuper0 := Unv
 * Usuper1 := Uv
 *
 * Ld := or(Lsuper0, Lsuper1)
 * Ud := and(Usuper0, Usuper1)
 * Id, zddId := ISOP(Ld, Ud)
 *
 * x := ithvar(v)
 * term0 := !v * Isub0
 * term1 := v * Isub1
 * sum := term0 + term1
 * r := sum + Id
 *
 * y := makeZdds(v, zdd_Isub0, zdd_Isub1, zdd_Id)
 *
 * put in cache
 *
 * return r, y
 */

/**
 * ISOC: returns BDD of the set described by the returned ZDD of clauses
 *
 * Given a set S and a dontcare set DC; or bounds L and U (L=S, U=S+DC),
 * compute an irredundant set of clauses.
 *
 * Reasoning from the perspective that each clause removes assignments.
 *
 * We recursively compute first for clauses with ~v and v; here the lower bounds stay
 * the same but the upper bound can be relaxed for assignments that can also be forbidden
 * for v=1 (thus do not need ~v), or v=0 (thus do not need v).
 * That means: Usub0 := or(Unv, ~Lv)
 *             Usub1 := or(Uv, ~Lnv)
 * Then the dontcare computation is based on the "remaining" bounds:
 *             Usuper0 := or(Unv, ~Isub0)
 *             Usuper1 := or(Uv, ~Isub1)
 *             Ld, Ud := or(Lsuper0, Lsuper1), and(Usuper0, Usuper1)
 *
 * if L == 0 return 0, 1  // the empty clause forbids all, BDD false
 * if U == 1 return 1, 0  // the empty set of clauses forbids none, BDD true
 * if U == 0 or L == 1 abort
 *
 * check cache
 *
 * v := minvar(L, U)
 * Lv, Lnv, Uv, Unv := cofactor(L, v), cofactor(U, v)
 * 
 * Lsub0 := Lnv
 * Lsub1 := Lv
 * Usub0 := or(Unv, !Lv)
 * Usub1 := or(Uv, !Lnv)
 *
 * Isub0, zddIsub0 := ISOC(Lsub0, Usub0)
 * Isub1, zddIsub1 := ISOC(Lsub1, Usub1)
 *
 * Lsuper0 := Lnv
 * Lsuper1 := Lv
 * Usuper0 := or(Unv, !Isub0)
 * Usuper1 := or(Uv, !Isub1)
 *
 * Ld := or(Lsuper0, Lsuper1)
 * Ud := and(Usuper0, Usuper1)
 * Id, zddId := ISOC(Ld, Ud)
 *
 * r := or(Id, node(v, Isub0, Isub1))
 * y := makeZdds(v, zdd_Isub0, zdd_Isub1, zdd_Id)
 *
 * put in cache
 *
 * return r, y
 */
TASK_IMPL_3(ZDD, zdd_clause_isoc, MTBDD, L, MTBDD, U, MTBDD*, bddresptr)
{
    if (U == mtbdd_true) {
        if (bddresptr != NULL) *bddresptr = mtbdd_true;
        return zdd_false;
    } else if (L == mtbdd_false) {
        if (bddresptr != NULL) *bddresptr = mtbdd_false;
        return zdd_true;
    }
    assert(U != mtbdd_false && L != mtbdd_true);

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_CLAUSE_ISOC);

    /**
     * Check the cache
     */
    ZDD result;
    MTBDD bddres;
    if (cache_get6(CACHE_ZDD_CLAUSE_ISOC, L, U, 0, 0, 0, &result, &bddres)) {
        sylvan_stats_count(ZDD_CLAUSE_ISOC_CACHED);
        if (bddresptr != NULL) *bddresptr = bddres;
        return result;
    }

    /**
     * Compute variable and cofactors
     */
    mtbddnode_t L_node = MTBDD_GETNODE(L);
    mtbddnode_t U_node = MTBDD_GETNODE(U);
    uint32_t L_var = mtbddnode_getvariable(L_node);
    uint32_t U_var = mtbddnode_getvariable(U_node);
    uint32_t minvar = L_var < U_var ? L_var : U_var;

    MTBDD L0 = minvar == L_var ? mtbddnode_followlow(L, L_node) : L;
    MTBDD L1 = minvar == L_var ? mtbddnode_followhigh(L, L_node) : L;
    MTBDD U0 = minvar == U_var ? mtbddnode_followlow(U, U_node) : U;
    MTBDD U1 = minvar == U_var ? mtbddnode_followhigh(U, U_node) : U;

    /**
     * Compute Usub0 and Usub1
     * Usub0 := or(Unv, !Lv)
     * Usub1 := or(Uv, !Lnv)
     */
    MTBDD Usub0, Usub1;
    mtbdd_refs_spawn(SPAWN(sylvan_and, sylvan_not(U0), L1, 0));
    Usub1 = mtbdd_refs_push(sylvan_or(U1, sylvan_not(L0)));
    Usub0 = mtbdd_refs_push(sylvan_not(mtbdd_refs_sync(SYNC(sylvan_and))));

    /**
     * Compute recursive results for sub0 and sub1
     */
    MTBDD I0 = mtbdd_false, I1 = mtbdd_false;
    ZDD Z0, Z1;
    /* TODO mtbdd_refs_protect them */
    zdd_refs_spawn(SPAWN(zdd_clause_isoc, L0, Usub0, &I0));
    Z1 = zdd_refs_push(zdd_clause_isoc(L1, Usub1, &I1));
    Z0 = zdd_refs_push(zdd_refs_sync(SYNC(zdd_clause_isoc)));
    mtbdd_refs_pop(2); // Usub0, Usub1
    mtbdd_refs_push(I0); // TODO: use protect
    mtbdd_refs_push(I1); // TODO: use protect

    // printf("I0 %zu %zu\n", sylvan_and(L0, sylvan_not(I0)), sylvan_and(I0, sylvan_not(Usub0)));
    // printf("I1 %zu %zu\n", sylvan_and(L1, sylvan_not(I1)), sylvan_and(I1, sylvan_not(Usub1)));

    /**
     * Compute Usuper0 and Usuper1 and Ld and Ud
     * Usuper0 := or(U0, not(I0))
     * Usuper1 := or(U1, not(I1))
     * Ld := or(L0, L1)
     * Ud := and(Usuper0, Usuper1)
     */
    MTBDD Usuper0, Usuper1;
    mtbdd_refs_spawn(SPAWN(sylvan_and, sylvan_not(L0), sylvan_not(L1), 0));
    mtbdd_refs_spawn(SPAWN(sylvan_and, sylvan_not(U0), I0, 0));
    Usuper1 = mtbdd_refs_push(sylvan_or(U1, sylvan_not(I1)));
    Usuper0 = mtbdd_refs_push(sylvan_not(mtbdd_refs_sync(SYNC(sylvan_and))));
    MTBDD Ud = sylvan_and(Usuper0, Usuper1);
    mtbdd_refs_pop(2); // Usuper0, Usuper1
    mtbdd_refs_push(Ud);
    MTBDD Ld = mtbdd_refs_push(sylvan_not(mtbdd_refs_sync(SYNC(sylvan_and))));

    /**
     * Compute recursive result for dontcare
     */
    MTBDD Id = mtbdd_false;
    ZDD Zd = zdd_clause_isoc(Ld, Ud, &Id);
    mtbdd_refs_pop(2); // Ld, Ud
    mtbdd_refs_push(Id); // TODO: use protect
    zdd_refs_push(Zd);

    // printf("Id %zu %zu\n", sylvan_and(Ld, sylvan_not(Ld)), sylvan_and(Id, sylvan_not(Ud)));

    /**
     * Now we have: I0, I1, Id and Z0, Z1, Zd
     */
    MTBDD x = mtbdd_makenode(minvar, I0, I1);
    mtbdd_refs_push(x);
    bddres = sylvan_and(x, Id);
    mtbdd_refs_pop(1); // x
    mtbdd_refs_pop(3); // TODO: use unprotect for Id, I0, I1
    mtbdd_refs_push(bddres);

    // bddres = mtbdd_makenode(minvar, sylvan_and(I1, Id), sylvan_and(I0, Id));

    /* is actually between L and U?? */
    // printf("Check L <= Res <= U: %zu %zu\n", sylvan_and(L, sylvan_not(bddres)), sylvan_and(bddres, sylvan_not(U)));

    ZDD z = zdd_makenode(2*minvar+1, Zd, Z0);
    result = zdd_makenode(2*minvar, z, Z1);
    zdd_refs_pop(3);

    mtbdd_refs_pop(1);

    // printf("Check: %d %d %d\n", I0 == zdd_clause_sat(Z0), I1 == zdd_clause_sat(Z1), Id == zdd_clause_sat(Zd));
    // MTBDD c = zdd_clause_sat(result);
    // printf("Res: %d %zu %zu %d\n", c == bddres, bddres, c, minvar);

    /**
     * Put in cache
     */
    if (cache_put6(CACHE_ZDD_CLAUSE_ISOC, L, U, 0, 0, 0, result, bddres)) {
        sylvan_stats_count(ZDD_CLAUSE_ISOC_CACHEDPUT);
    }

    if (bddresptr != NULL) *bddresptr = bddres;
    return result;
}

/**
 * Extend the domain of a ZDD, such that all new variables take the given value.
 * The given value can be 0 (always negative), 1 (always positive), 2 (always dontcare)
 */
TASK_IMPL_3(ZDD, zdd_extend_domain, ZDD, set, ZDD, newvars, int, value)
{
    /**
     * Terminal cases
     */
    if (value == 0) return set; // uhm?
    if (value != 1 && value != 1) return zdd_invalid; // uhm??
    if (set == zdd_false) return zdd_false;
    if (newvars == zdd_true) return set;

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_EXTEND_DOMAIN);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_EXTEND_DOMAIN, set, newvars, value, &result)) {
        sylvan_stats_count(ZDD_EXTEND_DOMAIN_CACHED);
        return result;
    }

    /**
     * Get set variable, domain variable, and next domain variable
     */
    const zddnode_t set_node = set == zdd_true ? NULL : ZDD_GETNODE(set);
    const uint32_t set_var = set_node == NULL || zddnode_isleaf(set_node) ? 0xffffffff : zddnode_getvariable(set_node);
    const zddnode_t nv_node = ZDD_GETNODE(newvars);
    const uint32_t nv_var = zddnode_getvariable(nv_node);
    const uint32_t nv_next = zddnode_high(newvars, nv_node);

    if (nv_var < set_var) {
        if (value == 1) {
            result = zdd_extend_domain(set, nv_next, value);
            result = zdd_makenode(nv_var, zdd_false, result);
        } else {
            result = zdd_extend_domain(set, nv_next, value);
            result = zdd_makenode(nv_var, result, result);
        }
    } else {
        assert(nv_var != set_var);
        const ZDD set0 = zddnode_low(set, set_node);
        const ZDD set1 = zddnode_high(set, set_node);
        zdd_refs_spawn(SPAWN(zdd_extend_domain, set1, newvars, value));
        ZDD low = zdd_refs_push(CALL(zdd_extend_domain, set0, newvars, value));
        ZDD high = zdd_refs_sync(SYNC(zdd_extend_domain));
        zdd_refs_pop(1);
        result = zdd_makenode(set_var, low, high);
    }

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_ZDD_EXTEND_DOMAIN, set, newvars, value, result)) {
        sylvan_stats_count(ZDD_EXTEND_DOMAIN_CACHEDPUT);
    }

    return result;
}

/**
 * Calculate the support of a ZDD, i.e. the cube of all variables that appear in the ZDD nodes.
 */
TASK_IMPL_1(ZDD, zdd_support, ZDD, dd)
{
    if (dd == zdd_true || dd == zdd_false) return zdd_true;
    const zddnode_t dd_node = ZDD_GETNODE(dd);
    if (zddnode_isleaf(dd_node)) return zdd_true;

    /**
     * Perhaps execute garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_SUPPORT);

    /**
     * Consult cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_SUPPORT, dd, 0, 0, &result)) {
        sylvan_stats_count(ZDD_SUPPORT_CACHED);
        return result;
    }

    const ZDD dd0 = zddnode_low(dd, dd_node);
    const ZDD dd1 = zddnode_high(dd, dd_node);
    zdd_refs_spawn(SPAWN(zdd_support, dd0));
    ZDD high = zdd_refs_push(CALL(zdd_support, dd1));
    ZDD low = zdd_refs_push(zdd_refs_sync(SYNC(zdd_support)));
    result = zdd_set_union(low, high);
    zdd_refs_pop(2);
    result = zdd_makenode(zddnode_getvariable(dd_node), result, result);

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_ZDD_SUPPORT, dd, 0, 0, result)) {
        sylvan_stats_count(ZDD_SUPPORT_CACHEDPUT);
    }

    return result;
}

/**
 * Count the number of distinct paths leading to a non-False leaf.
 */
TASK_IMPL_1(double, zdd_pathcount, ZDD, dd)
{
    if (dd == zdd_false) return 0.0;
    if (dd == zdd_true) return 1.0;
    const zddnode_t dd_node = ZDD_GETNODE(dd);
    if (zddnode_isleaf(dd_node)) return 1.0;

    /**
     * Perhaps execute garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_PATHCOUNT);

    /**
     * Consult cache
     */
    union {
        double d;
        uint64_t s;
    } hack;

    if (cache_get3(CACHE_ZDD_PATHCOUNT, dd, 0, 0, &hack.s)) {
        sylvan_stats_count(ZDD_PATHCOUNT_CACHED);
        return hack.d;
    }

    /**
     * Recursive computation
     */
    const ZDD dd0 = zddnode_low(dd, dd_node);
    const ZDD dd1 = zddnode_high(dd, dd_node);
    SPAWN(zdd_pathcount, dd0);
    double result = CALL(zdd_pathcount, dd1);
    result += SYNC(zdd_pathcount);

    hack.d = result;
    if (cache_put3(CACHE_ZDD_PATHCOUNT, dd, 0, 0, hack.s)) {
        sylvan_stats_count(ZDD_PATHCOUNT_CACHEDPUT);
    }

    return result;
}

/**
 * Helper function for recursive unmarking
 */
static void
zdd_unmark_rec(ZDD zdd)
{
    zddnode_t n = ZDD_GETNODE(zdd);
    if (!zddnode_getmark(n)) return;
    zddnode_setmark(n, 0);
    zdd_unmark_rec(zddnode_getlow(n));
    zdd_unmark_rec(zddnode_gethigh(n));
}

/**
 * Mark and count all nodes (internal & leaves) in the given ZDD.
 * Not thread-safe.
 */
static size_t
zdd_nodecount_mark(ZDD zdd)
{
    // Note: the True/False leaf can be marked/unmarked, as buckets 0--1 are unused
    zddnode_t n = ZDD_GETNODE(zdd);
    if (zddnode_getmark(n)) return 0;
    zddnode_setmark(n, 1);
    return 1 + zdd_nodecount_mark(zddnode_getlow(n)) + zdd_nodecount_mark(zddnode_gethigh(n));
}

/**
 * Count the number of nodes (internal nodes plus leaves) in ZDDs.
 * Not thread-safe.
 */
size_t
zdd_nodecount(const ZDD *zdds, size_t count)
{
    size_t result = 0, i;
    for (i=0; i<count; i++) result += zdd_nodecount_mark(zdds[i]);
    for (i=0; i<count; i++) zdd_unmark_rec(zdds[i]);
    return result;
}

/**
 * Implementation of the AND operator for Boolean ZDDs
 */
TASK_IMPL_2(ZDD, zdd_and, ZDD, a, ZDD, b)
{
    /**
     * Check the case where A or B is False
     */
    if (a == zdd_false || b == zdd_false) return zdd_false;
    if (a == b) return a;

    /**
     * Switch A and B if A > B (for cache)
     */
    if (ZDD_GETINDEX(a) > ZDD_GETINDEX(b)) {
        ZDD t = a;
        a = b;
        b = t;
    }

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_AND);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_AND, a, b, 0, &result)) {
        sylvan_stats_count(ZDD_AND_CACHED);
        return result;
    }

    /**
     * b cannot be True
     * if a is True, then we only return True if b evaluates to True for 00000...
     */
    if (a == zdd_true) {
        ZDD _b = b;
        while (_b != zdd_true && _b != zdd_false) _b = zdd_getlow(_b);
        result = _b;
    } else {
        /**
         * Get the vars
         */
        const zddnode_t a_node = ZDD_GETNODE(a);
        const uint32_t a_var = zddnode_getvariable(a_node);
        const zddnode_t b_node = ZDD_GETNODE(b);
        const uint32_t b_var = zddnode_getvariable(b_node);
        uint32_t minvar = a_var < b_var ? a_var : b_var;

        /**
         * Get cofactors for A and B
         */
        ZDD a0 = minvar < a_var ? a : zddnode_low(a, a_node);
        ZDD a1 = minvar < a_var ? zdd_false : zddnode_high(a, a_node);
        ZDD b0 = minvar < b_var ? b : zddnode_low(b, b_node);
        ZDD b1 = minvar < b_var ? zdd_false : zddnode_high(b, b_node);

        /**
         * Now we call recursive tasks
         */
        ZDD low, high;
        if (a1 == zdd_false || b1 == zdd_false) {
            low = zdd_and(a0, b0);
            high = zdd_false;
        } else {
            zdd_refs_spawn(SPAWN(zdd_and, a0, b0));
            high = zdd_and(a1, b1);
            zdd_refs_push(high);
            low = zdd_refs_sync(SYNC(zdd_and));
            zdd_refs_pop(1);
        }

        /**
         * Compute result node
         */
        result = zdd_makenode(minvar, low, high);
    }

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_AND, a, b, 0, result)) {
        sylvan_stats_count(ZDD_AND_CACHEDPUT);
    }

    return result;
}

/**
 * Implementation of the ITE operator for Boolean ZDDs
 */
TASK_IMPL_4(ZDD, zdd_ite, ZDD, a, ZDD, b, ZDD, c, ZDD, dom)
{
    /**
     * Trivial cases
     */
    if (a == zdd_false) return c;
    if (a == b) return zdd_or(a, c);
    if (a == c || c == zdd_false) return zdd_and(a, b);
    if (b == c) return b;

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_ITE);

    /**
     * Get the vars
     */
    const zddnode_t a_node = zdd_isleaf(a) ? NULL : ZDD_GETNODE(a);
    const uint32_t a_var = a_node == NULL ? 0xffffffff : zddnode_getvariable(a_node);
    const zddnode_t b_node = zdd_isleaf(b) ? NULL : ZDD_GETNODE(b);
    const uint32_t b_var = b_node == NULL ? 0xffffffff : zddnode_getvariable(b_node);
    const zddnode_t c_node = zdd_isleaf(c) ? NULL : ZDD_GETNODE(c);
    const uint32_t c_var = c_node == NULL ? 0xffffffff : zddnode_getvariable(c_node);
    uint32_t minvar = a_var < b_var ? a_var : b_var;
    if (minvar > c_var) minvar = c_var;
    assert(minvar != 0xffffffff);

    /**
     * Move dom to minvar
     */
    zddnode_t dom_node = ZDD_GETNODE(dom);
    uint32_t dom_var = zddnode_getvariable(dom_node);
    ZDD dom_next = zddnode_high(dom, dom_node);
    while (dom_var != minvar) {
        assert(dom_next != zdd_true);
        dom = dom_next;
        dom_node = ZDD_GETNODE(dom);
        dom_var = zddnode_getvariable(dom_node);
        dom_next = zddnode_high(dom, dom_node);
    }

    /**
     * Check other trivial cases using dom for True
     *   - ITE(1,b,c) ==> b
     *   - ITE(a,1,0) ==> a
     *   - ITE(a,1,c) ==> or(a, c)
     *   - ITE(a,0,1) ==> not(a)
     */
    if (a == dom) return b;
    if (b == dom) return zdd_or(a, c);
    if (b == zdd_false && c == dom) return zdd_not(a, dom);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_ITE, a, b, c, &result)) {
        sylvan_stats_count(ZDD_ITE_CACHED);
        return result;
    }

    /**
     * Get the cofactors
     */
    const ZDD a0 = minvar < a_var ? a : zddnode_low(a, a_node);
    const ZDD a1 = minvar < a_var ? zdd_false : zddnode_high(a, a_node);
    const ZDD b0 = minvar < b_var ? b : zddnode_low(b, b_node);
    const ZDD b1 = minvar < b_var ? zdd_false : zddnode_high(b, b_node);
    const ZDD c0 = minvar < c_var ? c : zddnode_low(c, c_node);
    const ZDD c1 = minvar < c_var ? zdd_false : zddnode_high(c, c_node);

    /**
     * Now we call recursive tasks
     */
    zdd_refs_spawn(SPAWN(zdd_ite, a0, b0, c0, dom_next));
    ZDD high = CALL(zdd_ite, a1, b1, c1, dom_next);
    zdd_refs_push(high);
    ZDD low = zdd_refs_sync(SYNC(zdd_ite));
    zdd_refs_pop(1);

    /**
     * Compute result node
     */
    result = zdd_makenode(minvar, low, high);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_ITE, a, b, c, result)) {
        sylvan_stats_count(ZDD_ITE_CACHEDPUT);
    }

    return result;
}

/**
 * Implementation of the OR operator for Boolean ZDDs
 */
TASK_IMPL_2(ZDD, zdd_or, ZDD, a, ZDD, b)
{
    /**
     * Trivial cases (similar to sylvan_ite)
     */
    if (a == zdd_false) return b;
    if (b == zdd_false) return a;
    if (a == b) return a;
    // if (a == zdd_true) return zdd_true;
    // if (b == zdd_true) return zdd_true;

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_OR);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_OR, a, b, 0, &result)) {
        sylvan_stats_count(ZDD_OR_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    const zddnode_t a_node = zdd_isleaf(a) ? NULL : ZDD_GETNODE(a);
    const uint32_t a_var = a_node == NULL ? 0xffffffff : zddnode_getvariable(a_node);
    const zddnode_t b_node = zdd_isleaf(b) ? NULL : ZDD_GETNODE(b);
    const uint32_t b_var = b_node == NULL ? 0xffffffff : zddnode_getvariable(b_node);
    uint32_t minvar = a_var < b_var ? a_var : b_var;
    assert(minvar != 0xffffffff);

    /**
     * Get the cofactors
     */
    ZDD a0 = minvar < a_var ? a : zddnode_low(a, a_node);
    ZDD a1 = minvar < a_var ? zdd_false : zddnode_high(a, a_node);
    ZDD b0 = minvar < b_var ? b : zddnode_low(b, b_node);
    ZDD b1 = minvar < b_var ? zdd_false : zddnode_high(b, b_node);

    /**
     * Now we call recursive tasks
     */
    zdd_refs_spawn(SPAWN(zdd_or, a0, b0));
    ZDD high = CALL(zdd_or, a1, b1);
    zdd_refs_push(high);
    ZDD low = zdd_refs_sync(SYNC(zdd_or));
    zdd_refs_pop(1);

    /**
     * Compute result node
     */
    result = zdd_makenode(minvar, low, high);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_OR, a, b, 0, result)) {
        sylvan_stats_count(ZDD_OR_CACHEDPUT);
    }

    return result;
}

/**
 * Compute the not operator
 */
TASK_IMPL_2(ZDD, zdd_not, ZDD, dd, ZDD, dom)
{
    /**
     * Trivial cases (abusing the notion of dom representing True for all assignments)
     */
    if (dd == dom) return zdd_false;
    if (dd == zdd_false) return dom;
    assert(dom != zdd_true);

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_NOT);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_NOT, dd, dom, 0, &result)) {
        sylvan_stats_count(ZDD_NOT_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    const zddnode_t dd_node = zdd_isleaf(dd) ? NULL : ZDD_GETNODE(dd);
    const uint32_t dd_var = dd_node == NULL ? 0xffffffff : zddnode_getvariable(dd_node);
    const zddnode_t dom_node = ZDD_GETNODE(dom);
    const uint32_t dom_var = zddnode_getvariable(dom_node);

    assert(dom_var <= dd_var);

    /**
     * Recursively compute
     */
    if (dom_var < dd_var) {
        const ZDD dom_next = zddnode_high(dom, dom_node);
        const ZDD low = CALL(zdd_not, dd, dom_next);
        const ZDD high = dom_next; // dom represents True for all assignments
        result = zdd_makenode(dom_var, low, high);
    } else {
        const ZDD dd0 = zddnode_low(dd, dd_node);
        const ZDD dd1 = zddnode_high(dd, dd_node);

        /**
         * Now we call recursive tasks
         */
        const ZDD dom_next = zddnode_high(dom, dom_node);
        zdd_refs_spawn(SPAWN(zdd_not, dd0, dom_next));
        const ZDD high = CALL(zdd_not, dd1, dom_next);
        zdd_refs_push(high);
        const ZDD low = zdd_refs_sync(SYNC(zdd_not));
        zdd_refs_pop(1);

        /**
         * Compute result node
         */
        result = zdd_makenode(dom_var, low, high);
    }

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_NOT, dd, dom, 0, result)) {
        sylvan_stats_count(ZDD_NOT_CACHEDPUT);
    }

    return result;
}

/**
 * Compute logical DIFF of <a> and <b>. (set minus)
 */
TASK_IMPL_2(ZDD, zdd_diff, ZDD, a, ZDD, b)
{
    /**
     * Check the case where A or B is False
     */
    if (a == zdd_false) return zdd_false;
    if (b == zdd_false) return a;
    if (a == b) return zdd_false;

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_DIFF);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_DIFF, a, b, 0, &result)) {
        sylvan_stats_count(ZDD_DIFF_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    const zddnode_t a_node = zdd_isleaf(a) ? NULL : ZDD_GETNODE(a);
    const uint32_t a_var = a_node == NULL ? 0xffffffff : zddnode_getvariable(a_node);
    const zddnode_t b_node = zdd_isleaf(b) ? NULL : ZDD_GETNODE(b);
    const uint32_t b_var = b_node == NULL ? 0xffffffff : zddnode_getvariable(b_node);
    uint32_t minvar = a_var < b_var ? a_var : b_var;

    /**
     * Get the cofactors
     */
    const ZDD a0 = minvar < a_var ? a : zddnode_low(a, a_node);
    const ZDD a1 = minvar < a_var ? zdd_false : zddnode_high(a, a_node);
    const ZDD b0 = minvar < b_var ? b : zddnode_low(b, b_node);
    const ZDD b1 = minvar < b_var ? zdd_false : zddnode_high(b, b_node);

    /**
     * Now we call recursive tasks
     */
    zdd_refs_spawn(SPAWN(zdd_diff, a0, b0));
    ZDD high = CALL(zdd_diff, a1, b1);
    zdd_refs_push(high);
    ZDD low = zdd_refs_sync(SYNC(zdd_diff));
    zdd_refs_pop(1);

    /**
     * Compute result node
     */
    result = zdd_makenode(minvar, low, high);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_DIFF, a, b, 0, result)) {
        sylvan_stats_count(ZDD_DIFF_CACHEDPUT);
    }

    return result;
}

/**
 * Compute existential quantification, but stay in same domain
 */
TASK_IMPL_2(ZDD, zdd_exists, ZDD, dd, ZDD, vars)
{
    /**
     * Trivial cases
     */
    if (dd == zdd_true) return vars; // <vars> now represents True for the variables in <vars>
    if (dd == zdd_false) return dd;
    if (vars == zdd_true) return dd;

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_EXISTS);

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_EXISTS, dd, vars, 0, &result)) {
        sylvan_stats_count(ZDD_EXISTS_CACHED);
        return result;
    }

    /**
     * Obtain variables
     */
    const zddnode_t dd_node = ZDD_GETNODE(dd);
    const uint32_t dd_var = zddnode_getvariable(dd_node);
    const zddnode_t vars_node = ZDD_GETNODE(vars);
    const uint32_t vars_var = zddnode_getvariable(vars_node);

    /**
     * Compute pivot variable
     */
    if (vars_var < dd_var) {
        result = zdd_exists(dd, zddnode_high(vars, vars_node));
        result = zdd_makenode(vars_var, result, result);
    } else {
        /**
         * Get cofactors
         */
        const ZDD dd0 = zddnode_low(dd, dd_node);
        const ZDD dd1 = zddnode_high(dd, dd_node);

        if (vars_var == dd_var) {
            // Quantify

            /**
             * Now we call recursive tasks
             */
            const ZDD vars_next = zddnode_high(vars, vars_node);
            if (dd0 == dd1) {
                result = CALL(zdd_exists, dd0, vars_next);
            } else {
                zdd_refs_spawn(SPAWN(zdd_exists, dd0, vars_next));
                ZDD high = CALL(zdd_exists, dd1, vars_next);
                zdd_refs_push(high);
                ZDD low = zdd_refs_sync(SYNC(zdd_exists));
                zdd_refs_push(low);
                result = zdd_or(low, high);
                zdd_refs_pop(2);
            }

            result = zdd_makenode(vars_var, result, result);
        } else {
            // Keep

            /**
             * Now we call recursive tasks
             */
            ZDD low, high;
            if (dd0 == dd1) {
                low = high = CALL(zdd_exists, dd0, vars);
            } else {
                zdd_refs_spawn(SPAWN(zdd_exists, dd0, vars));
                high = CALL(zdd_exists, dd1, vars);
                zdd_refs_push(high);
                low = zdd_refs_sync(SYNC(zdd_exists));
                zdd_refs_pop(1);
            }

            /**
             * Compute result node
             */
            result = zdd_makenode(dd_var, low, high);
        }
    }

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_EXISTS, dd, vars, 0, result)) {
        sylvan_stats_count(ZDD_EXISTS_CACHEDPUT);
    }

    return result;
}

/**
 * Compute existential quantification to a smaller domain
 * Remove all variables from <dd> that are not in <newdom>
 */
TASK_IMPL_2(ZDD, zdd_project, ZDD, dd, ZDD, dom)
{
    /**
     * Trivial cases
     */
    if (dd == zdd_true) return dd;
    if (dd == zdd_false) return dd;
    if (dom == zdd_true) return zdd_true; // assuming dd is indeed Boolean

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(ZDD_PROJECT);

    /**
     * Obtain variables
     */
    const zddnode_t dd_node = ZDD_GETNODE(dd);
    const uint32_t dd_var = zddnode_getvariable(dd_node);

    /**
     * Move dom to dd_var
     */
    zddnode_t dom_node = ZDD_GETNODE(dom);
    uint32_t dom_var = zddnode_getvariable(dom_node);
    ZDD dom_next = zddnode_high(dom, dom_node);
    while (dom_var < dd_var) {
        dom = dom_next;
        if (dom == zdd_true) return zdd_true; // assuming dd is indeed Boolean
        dom_node = ZDD_GETNODE(dom);
        dom_var = zddnode_getvariable(dom_node);
        dom_next = zddnode_high(dom, dom_node);
    }

    /**
     * Check the cache
     */
    ZDD result;
    if (cache_get3(CACHE_ZDD_PROJECT, dd, dom, 0, &result)) {
        sylvan_stats_count(ZDD_PROJECT_CACHED);
        return result;
    }

    /**
     * Get cofactors
     */
    const ZDD dd0 = zddnode_low(dd, dd_node);
    const ZDD dd1 = zddnode_high(dd, dd_node);

    assert(dd_var <= dom_var);

    /**
     * Compute pivot variable
     */
    if (dd_var < dom_var) {
        // Quantify

        /**
         * Now we call recursive tasks
         */
        if (dd0 == dd1) {
            result = CALL(zdd_project, dd0, dom);
        } else {
            zdd_refs_spawn(SPAWN(zdd_project, dd0, dom));
            ZDD high = CALL(zdd_project, dd1, dom);
            zdd_refs_push(high);
            ZDD low = zdd_refs_sync(SYNC(zdd_project));
            zdd_refs_push(low);
            result = zdd_or(low, high);
            zdd_refs_pop(2);
        }
    } else {
        // Keep

        /**
         * Now we call recursive tasks
         */
        ZDD low, high;
        if (dd0 == dd1) {
            low = high = CALL(zdd_project, dd0, dom_next);
        } else {
            zdd_refs_spawn(SPAWN(zdd_project, dd0, dom_next));
            high = CALL(zdd_project, dd1, dom_next);
            zdd_refs_push(high);
            low = zdd_refs_sync(SYNC(zdd_project));
            zdd_refs_pop(1);
        }

        /**
         * Compute result node
         */
        result = zdd_makenode(dd_var, low, high);
    }

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_ZDD_PROJECT, dd, dom, 0, result)) {
        sylvan_stats_count(ZDD_PROJECT_CACHEDPUT);
    }

    return result;
}

ZDD zdd_enum_first(ZDD dd, ZDD dom, uint8_t *arr, zdd_enum_filter_cb filter_cb)
{
    if (dd == zdd_false) {
        return zdd_false;
    } else if (zdd_isleaf(dd)) {
        if (filter_cb != NULL && filter_cb(dd) == 0) return zdd_false;
        while (dom != zdd_true) {
            *arr++ = 0;
            dom = zdd_gethigh(dom);
        }
        return dd;
    } else {
        assert(dom != zdd_true);

        /**
         * Obtain domain variable
         */
        const zddnode_t dom_node = ZDD_GETNODE(dom);
        const uint32_t dom_var = zddnode_getvariable(dom_node);
        const ZDD dom_next = zddnode_high(dom, dom_node);
        const zddnode_t dd_node = ZDD_GETNODE(dd);
        const uint32_t dd_var = zddnode_getvariable(dd_node);

        if (dom_var < dd_var) {
            // try low only (high == zdd_false)
            ZDD res = zdd_enum_first(dd, dom_next, arr+1, filter_cb);
            if (res != zdd_false) {
                *arr = 0;
                return res;
            } else {
                return zdd_false;
            }
        } else {
            /**
             * Try low first, else high, else return False
             */
            ZDD res = zdd_enum_first(zddnode_low(dd, dd_node), dom_next, arr+1, filter_cb);
            if (res != zdd_false) {
                *arr = 0;
                return res;
            }

            res = zdd_enum_first(zddnode_high(dd, dd_node), dom_next, arr+1, filter_cb);
            if (res != zdd_false) {
                *arr = 1;
                return res;
            } else {
                return zdd_false;
            }
        }
    }
}

ZDD zdd_enum_next(ZDD dd, ZDD dom, uint8_t *arr, zdd_enum_filter_cb filter_cb)
{
    if (zdd_isleaf(dd)) return zdd_false; // only find a leaf in zdd_enum_first

    assert(dom != zdd_true);

    /**
     * Obtain domain variable
     */
    const zddnode_t dom_node = ZDD_GETNODE(dom);
    const uint32_t dom_var = zddnode_getvariable(dom_node);
    const ZDD dom_next = zddnode_high(dom, dom_node);
    const zddnode_t dd_node = ZDD_GETNODE(dd);
    const uint32_t dd_var = zddnode_getvariable(dd_node);

    if (dom_var < dd_var) {
        assert(*arr == 0);
        ZDD res = zdd_enum_next(dd, dom_next, arr+1, filter_cb);
        // high = False, no need to inspect high branch...
        return res;
    } else {
        if (*arr == 0) {
            ZDD res = zdd_enum_next(zddnode_low(dd, dd_node), dom_next, arr+1, filter_cb);
            if (res == zdd_false) {
                res = zdd_enum_first(zddnode_high(dd, dd_node), dom_next, arr+1, filter_cb);
                if (res != zdd_false) *arr = 1;
            }
            return res;
        } else if (*arr == 1) {
            return zdd_enum_next(zddnode_high(dd, dd_node), dom_next, arr+1, filter_cb);
        } else {
            assert(0);
            return zdd_invalid;
        }
    }
}

ZDD
zdd_clause_enum_first(ZDD dd, int32_t *arr)
{
    if (dd == zdd_false) {
        return zdd_false;
    } else if (dd == zdd_true) {
        *arr = 0;
        return zdd_true;
    } else {
        const zddnode_t dd_node = ZDD_GETNODE(dd);
        const uint32_t dd_var = zddnode_getvariable(dd_node);
        const int32_t c = dd_var & 1 ? dd_var/2 : -(dd_var/2);

        /**
         * This cannot return False; following high edges must always lead to zdd_true!
         */
        ZDD res = zdd_clause_enum_first(zddnode_high(dd, dd_node), arr+1);
        assert(res != zdd_false);
        *arr = c;
        return res;
    }
}

ZDD
zdd_clause_enum_next(ZDD dd, int32_t *arr)
{
    if (dd == zdd_true) return zdd_false; // only find a leaf in enum_first

    const zddnode_t dd_node = ZDD_GETNODE(dd);
    const uint32_t dd_var = zddnode_getvariable(dd_node);
    const int32_t c = dd_var & 1 ? dd_var/2 : -(dd_var/2);

    if (*arr == c) {
        // We followed this one previously
        ZDD res = zdd_clause_enum_next(zddnode_high(dd, dd_node), arr+1);
        if (res != zdd_false) return res;
        else return zdd_clause_enum_first(zddnode_low(dd, dd_node), arr);
    } else {
        return zdd_clause_enum_next(zddnode_low(dd, dd_node), arr);
    }
}

/**
 * Export to .dot file
 */
static void
zdd_fprintdot_rec(FILE *out, ZDD zdd)
{
    zddnode_t n = ZDD_GETNODE(zdd); // also works for zdd_false
    if (zddnode_getmark(n)) return;
    zddnode_setmark(n, 1);

    if (ZDD_GETINDEX(zdd) == 0) {  // zdd == zdd_true || zdd == zdd_false
        fprintf(out, "0 [shape=box, style=filled, label=\"F\"];\n");
    } else if (ZDD_GETINDEX(zdd) == 1) {  // zdd == zdd_true || zdd == zdd_false
        fprintf(out, "1 [shape=box, style=filled, label=\"T\"];\n");
    } else {
        fprintf(out, "%" PRIu64 " [label=\"%" PRIu32 "\\n%" PRIu64 "\"];\n",
                ZDD_GETINDEX(zdd), zddnode_getvariable(n), ZDD_GETINDEX(zdd));

        zdd_fprintdot_rec(out, zddnode_getlow(n));
        zdd_fprintdot_rec(out, zddnode_gethigh(n));

        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=dashed];\n",
                ZDD_GETINDEX(zdd), ZDD_GETINDEX(zddnode_getlow(n)));
        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n",
                ZDD_GETINDEX(zdd), ZDD_GETINDEX(zddnode_gethigh(n)),
                zddnode_getcomp(n) ? "dot" : "none");
    }
}

void
zdd_fprintdot(FILE *out, ZDD zdd)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    fprintf(out, "root [style=invis];\n");
    fprintf(out, "root -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n",
            ZDD_GETINDEX(zdd), ZDD_HASMARK(zdd) ? "dot" : "none");

    zdd_fprintdot_rec(out, zdd);
    zdd_unmark_rec(zdd);

    fprintf(out, "}\n");
}

/**
 * Implementation of visitor operations
 */

VOID_TASK_IMPL_4(zdd_visit_seq, ZDD, dd, zdd_visit_pre_cb, pre_cb, zdd_visit_post_cb, post_cb, void*, ctx)
{
    int children = 1;
    if (pre_cb != NULL) children = WRAP(pre_cb, dd, ctx);
    if (children && !zdd_isleaf(dd)) {
        CALL(zdd_visit_seq, zdd_getlow(dd), pre_cb, post_cb, ctx);
        CALL(zdd_visit_seq, zdd_gethigh(dd), pre_cb, post_cb, ctx);
    }
    if (post_cb != NULL) WRAP(post_cb, dd, ctx);
}

VOID_TASK_IMPL_4(zdd_visit_par, ZDD, dd, zdd_visit_pre_cb, pre_cb, zdd_visit_post_cb, post_cb, void*, ctx)
{
    int children = 1;
    if (pre_cb != NULL) children = WRAP(pre_cb, dd, ctx);
    if (children && !zdd_isleaf(dd)) {
        SPAWN(zdd_visit_par, zdd_getlow(dd), pre_cb, post_cb, ctx);
        CALL(zdd_visit_par, zdd_gethigh(dd), pre_cb, post_cb, ctx);
        SYNC(zdd_visit_par);
    }
    if (post_cb != NULL) WRAP(post_cb, dd, ctx);
}

/**
 * Writing ZDD files using a skiplist as a backend
 */

TASK_2(int, zdd_writer_add_visitor_pre, ZDD, dd, sylvan_skiplist_t, sl)
{
    if (zdd_isleaf(dd)) return 0;
    return sylvan_skiplist_get(sl, ZDD_GETINDEX(dd)) == 0 ? 1 : 0;
}

VOID_TASK_2(zdd_writer_add_visitor_post, ZDD, dd, sylvan_skiplist_t, sl)
{
    if (ZDD_GETINDEX(dd) <= 1) return;
    sylvan_skiplist_assign_next(sl, ZDD_GETINDEX(dd));
}

sylvan_skiplist_t
zdd_writer_start()
{
    size_t sl_size = nodes->table_size > 0x7fffffff ? 0x7fffffff : nodes->table_size;
    return sylvan_skiplist_alloc(sl_size);
}

VOID_TASK_IMPL_2(zdd_writer_add, sylvan_skiplist_t, sl, ZDD, dd)
{
    zdd_visit_seq(dd, (zdd_visit_pre_cb)TASK(zdd_writer_add_visitor_pre), (zdd_visit_post_cb)TASK(zdd_writer_add_visitor_post), (void*)sl);
}

void
zdd_writer_writebinary(FILE *out, sylvan_skiplist_t sl)
{
    size_t nodecount = sylvan_skiplist_count(sl);
    fwrite(&nodecount, sizeof(size_t), 1, out);
    for (size_t i=1; i<=nodecount; i++) {
        ZDD dd = sylvan_skiplist_getr(sl, i);

        zddnode_t n = ZDD_GETNODE(dd);
        struct zddnode node;
        ZDD low = zddnode_getlow(n);
        ZDD high = zddnode_gethigh(n);
        if (ZDD_GETINDEX(low) > 1) low = ZDD_SETINDEX(low, sylvan_skiplist_get(sl, ZDD_GETINDEX(low)));
        if (ZDD_GETINDEX(high) > 1) high = ZDD_SETINDEX(high, sylvan_skiplist_get(sl, ZDD_GETINDEX(high)));
        zddnode_makenode(&node, zddnode_getvariable(n), low, high);
        fwrite(&node, sizeof(struct zddnode), 1, out);
    }
}

uint64_t
zdd_writer_get(sylvan_skiplist_t sl, ZDD dd)
{
    return ZDD_SETINDEX(dd, sylvan_skiplist_get(sl, ZDD_GETINDEX(dd)));
}

void
zdd_writer_end(sylvan_skiplist_t sl)
{
    sylvan_skiplist_free(sl);
}

VOID_TASK_IMPL_3(zdd_writer_tobinary, FILE *, out, ZDD *, dds, int, count)
{
    sylvan_skiplist_t sl = zdd_writer_start();

    for (int i=0; i<count; i++) {
        CALL(zdd_writer_add, sl, dds[i]);
    }

    zdd_writer_writebinary(out, sl);

    fwrite(&count, sizeof(int), 1, out);

    for (int i=0; i<count; i++) {
        uint64_t v = zdd_writer_get(sl, dds[i]);
        fwrite(&v, sizeof(uint64_t), 1, out);
    }

    zdd_writer_end(sl);
}

void
zdd_writer_writetext(FILE *out, sylvan_skiplist_t sl)
{
    fprintf(out, "[\n");
    size_t nodecount = sylvan_skiplist_count(sl);
    for (size_t i=1; i<=nodecount; i++) {
        ZDD dd = sylvan_skiplist_getr(sl, i);

        zddnode_t n = ZDD_GETNODE(dd);
        ZDD low = zddnode_getlow(n);
        ZDD high = zddnode_gethigh(n);
        if (ZDD_GETINDEX(low) > 1) low = ZDD_SETINDEX(low, sylvan_skiplist_get(sl, ZDD_GETINDEX(low)));
        if (ZDD_GETINDEX(high) > 1) high = ZDD_SETINDEX(high, sylvan_skiplist_get(sl, ZDD_GETINDEX(high)));
        fprintf(out, "  node(%zu,%u,low(%zu),%shigh(%zu)),\n", i, zddnode_getvariable(n), (size_t)ZDD_GETINDEX(low), ZDD_HASMARK(high)?"~":"", (size_t)ZDD_GETINDEX(high));
    }

    fprintf(out, "]");
}

VOID_TASK_IMPL_3(zdd_writer_totext, FILE *, out, ZDD *, dds, int, count)
{
    sylvan_skiplist_t sl = zdd_writer_start();

    for (int i=0; i<count; i++) {
        CALL(zdd_writer_add, sl, dds[i]);
    }

    zdd_writer_writetext(out, sl);

    fprintf(out, ",[");

    for (int i=0; i<count; i++) {
        uint64_t v = zdd_writer_get(sl, dds[i]);
        fprintf(out, "%s%zu,", ZDD_HASMARK(v)?"~":"", (size_t)ZDD_STRIPMARK(v));
    }

    fprintf(out, "]\n");

    zdd_writer_end(sl);
}

/**
 * Reading a file earlier written with zdd_writer_writebinary
 * Returns an array with the conversion from stored identifier to ZDD
 * This array is allocated with malloc and must be freed afterwards.
 * This method does not support custom leaves.
 */
TASK_IMPL_1(uint64_t*, zdd_reader_readbinary, FILE*, in)
{
    size_t nodecount;
    if (fread(&nodecount, sizeof(size_t), 1, in) != 1) {
        return NULL;
    }

    uint64_t *arr = malloc(sizeof(uint64_t)*(nodecount+1));
    arr[0] = 0;
    for (size_t i=1; i<=nodecount; i++) {
        struct zddnode node;
        if (fread(&node, sizeof(struct zddnode), 1, in) != 1) {
            free(arr);
            return NULL;
        }

        ZDD low = zddnode_getlow(&node);
        ZDD high = zddnode_gethigh(&node);
        if (ZDD_GETINDEX(low) > 0) low = ZDD_SETINDEX(low, arr[ZDD_GETINDEX(low)]);
        if (ZDD_GETINDEX(high) > 0) high = ZDD_SETINDEX(high, arr[ZDD_GETINDEX(high)]);
        arr[i] = zdd_makenode(zddnode_getvariable(&node), low, high);
    }

    return arr;
}

/**
 * Retrieve the ZDD of the given stored identifier.
 */
ZDD
zdd_reader_get(uint64_t* arr, uint64_t identifier)
{
    return ZDD_SETINDEX(identifier, arr[ZDD_GETINDEX(identifier)]);
}

/**
 * Free the allocated translation array
 */
void
zdd_reader_end(uint64_t *arr)
{
    free(arr);
}

/**
 * Reading a file earlier written with zdd_writer_tobinary
 */
TASK_IMPL_3(int, zdd_reader_frombinary, FILE*, in, ZDD*, dds, int, count)
{
    uint64_t *arr = CALL(zdd_reader_readbinary, in);
    if (arr == NULL) return -1;

    /* Read stored count */
    int actual_count;
    if (fread(&actual_count, sizeof(int), 1, in) != 1) {
        zdd_reader_end(arr);
        return -1;
    }

    /* If actual count does not agree with given count, abort */
    if (actual_count != count) {
        zdd_reader_end(arr);
        return -1;
    }

    /* Read every stored identifier, and translate to ZDD */
    for (int i=0; i<count; i++) {
        uint64_t v;
        if (fread(&v, sizeof(uint64_t), 1, in) != 1) {
            zdd_reader_end(arr);
            return -1;
        }
        dds[i] = zdd_reader_get(arr, v);
    }

    zdd_reader_end(arr);
    return 0;
}
