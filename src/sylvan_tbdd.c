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

#include <sylvan_config.h>

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sylvan.h>
#include <sylvan_int.h>

#include <sylvan_refs.h>
#include <sylvan_sl.h>

/**
 * Primitives
 */

int
tbdd_isleaf(TBDD dd)
{
    return TBDD_GETINDEX(dd) <= 1 ? 1 : 0;
}

uint32_t
tbdd_getvar(TBDD node)
{
    return tbddnode_getvariable(TBDD_GETNODE(node));
}

TBDD
tbdd_getlow(TBDD tbdd)
{
    return tbddnode_low(tbdd, TBDD_GETNODE(tbdd));
}

TBDD
tbdd_gethigh(TBDD tbdd)
{
    return tbddnode_high(tbdd, TBDD_GETNODE(tbdd));
}

/**
 * Implementation of garbage collection
 */

/**
 * Recursively mark MDD nodes as 'in use'
 */
VOID_TASK_IMPL_1(tbdd_gc_mark_rec, MDD, tbdd)
{
    if (tbdd == tbdd_true) return;
    if (tbdd == tbdd_false) return;

    if (llmsset_mark(nodes, TBDD_GETINDEX(tbdd))) {
        tbddnode_t n = TBDD_GETNODE(tbdd);
        SPAWN(tbdd_gc_mark_rec, tbddnode_getlow(n));
        CALL(tbdd_gc_mark_rec, tbddnode_gethigh(n));
        SYNC(tbdd_gc_mark_rec);
    }
}

/**
 * External references
 */

refs_table_t tbdd_protected;
static int tbdd_protected_created = 0;

void
tbdd_protect(TBDD *a)
{
    if (!tbdd_protected_created) {
        // In C++, sometimes tbdd_protect is called before Sylvan is initialized. Just create a table.
        protect_create(&tbdd_protected, 4096);
        tbdd_protected_created = 1;
    }
    protect_up(&tbdd_protected, (size_t)a);
}

void
tbdd_unprotect(TBDD *a)
{
    if (tbdd_protected.refs_table != NULL) protect_down(&tbdd_protected, (size_t)a);
}

size_t
tbdd_count_protected()
{
    return protect_count(&tbdd_protected);
}

/* Called during garbage collection */

VOID_TASK_0(tbdd_gc_mark_protected)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = protect_iter(&tbdd_protected, 0, tbdd_protected.refs_size);
    while (it != NULL) {
        BDD *to_mark = (BDD*)protect_next(&tbdd_protected, &it, tbdd_protected.refs_size);
        SPAWN(tbdd_gc_mark_rec, *to_mark);
        count++;
    }
    while (count--) {
        SYNC(tbdd_gc_mark_rec);
    }
}

/* Infrastructure for internal markings */
DECLARE_THREAD_LOCAL(tbdd_refs_key, tbdd_refs_internal_t);

VOID_TASK_0(tbdd_refs_mark_task)
{
    LOCALIZE_THREAD_LOCAL(tbdd_refs_key, tbdd_refs_internal_t);
    size_t i, j=0;
    for (i=0; i<tbdd_refs_key->r_count; i++) {
        if (j >= 40) {
            while (j--) SYNC(tbdd_gc_mark_rec);
            j=0;
        }
        SPAWN(tbdd_gc_mark_rec, tbdd_refs_key->results[i]);
        j++;
    }
    for (i=0; i<tbdd_refs_key->s_count; i++) {
        Task *t = tbdd_refs_key->spawns[i];
        if (!TASK_IS_STOLEN(t)) break;
        if (TASK_IS_COMPLETED(t)) {
            if (j >= 40) {
                while (j--) SYNC(tbdd_gc_mark_rec);
                j=0;
            }
            SPAWN(tbdd_gc_mark_rec, *(BDD*)TASK_RESULT(t));
            j++;
        }
    }
    while (j--) SYNC(tbdd_gc_mark_rec);
}

VOID_TASK_0(tbdd_refs_mark)
{
    TOGETHER(tbdd_refs_mark_task);
}

VOID_TASK_0(tbdd_refs_init_task)
{
    tbdd_refs_internal_t s = (tbdd_refs_internal_t)malloc(sizeof(struct tbdd_refs_internal));
    s->r_size = 128;
    s->r_count = 0;
    s->s_size = 128;
    s->s_count = 0;
    s->results = (BDD*)malloc(sizeof(BDD) * 128);
    s->spawns = (Task**)malloc(sizeof(Task*) * 128);
    SET_THREAD_LOCAL(tbdd_refs_key, s);
}

VOID_TASK_0(tbdd_refs_init)
{
    INIT_THREAD_LOCAL(tbdd_refs_key);
    TOGETHER(tbdd_refs_init_task);
    sylvan_gc_add_mark(TASK(tbdd_refs_mark));
}

/**
 * Initialize and quit functions
 */

static int tbdd_initialized = 0;

static void
tbdd_quit()
{
    if (tbdd_protected_created) {
        protect_free(&tbdd_protected);
        tbdd_protected_created = 0;
    }

    tbdd_initialized = 0;
}

void
sylvan_init_tbdd()
{
    if (tbdd_initialized) return;
    tbdd_initialized = 1;

    sylvan_register_quit(tbdd_quit);
    sylvan_gc_add_mark(TASK(tbdd_gc_mark_protected));

    if (!tbdd_protected_created) {
        protect_create(&tbdd_protected, 4096);
        tbdd_protected_created = 1;
    }

    LACE_ME;
    CALL(tbdd_refs_init);
}

/**
 * Node creation primitive.
 *
 * Returns the TBDD representing the formula <var> then <high> else <low>.
 * Variable <nextvar> is the next variable in the domain, necessary to correctly
 * perform the ZDD minimization rule.
 */
TBDD
tbdd_makenode(uint32_t var, TBDD low, TBDD high, uint32_t nextvar)
{
    struct tbddnode n;

    if (low == high) {
        /**
         * Same children (BDD minimization)
         * Just return one of them, this is correct in all cases.
         */
        return low;
    }

    /* if low had a mark, it is moved to the result */
#if TBDD_COMPLEMENT_EDGES
    int mark = TBDD_HASMARK(low);
#else
    assert(!TBDD_HASMARK(low));
    assert(!TBDD_HASMARK(high));
    int mark = 0;
#endif

    if (high == tbdd_false) {
        /**
         * high equals False (ZDD minimization)
         * low != False (because low != high)
         * if tag is next in domain just update tag to var
         * if tag is * (all BDD minimization)
         */
        /* check if no next var; then low must be a terminal */
        if (nextvar == 0xFFFFF) return TBDD_SETTAG(low, var);
        /* check if next var is skipped with ZDD rule */
        if (nextvar == TBDD_GETTAG(low)) return TBDD_SETTAG(low, var);
        /* nodes are skipped with (k,k), so we must make the next node */
        tbddnode_makenode(&n, nextvar, low, low);
    } else {
        /**
         * No minimization rule.
         */
        tbddnode_makenode(&n, var, low, high);
    }

    int created;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        tbdd_refs_push(low);
        tbdd_refs_push(high);
        sylvan_gc();
        tbdd_refs_pop(2);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(TBDD_NODES_CREATED);
    else sylvan_stats_count(TBDD_NODES_REUSED);

    TBDD result = TBDD_SETTAG(index, var);
    return mark ? result | tbdd_complement : result;
}

TBDD
tbdd_makemapnode(uint32_t var, TBDD low, TBDD high)
{
    struct tbddnode n;
    uint64_t index;
    int created;

    // in an TBDDMAP, the low edges eventually lead to 0 and cannot have a low mark
    assert(!TBDD_HASMARK(low));

    tbddnode_makemapnode(&n, var, low, high);
    index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        tbdd_refs_push(low);
        tbdd_refs_push(high);
        sylvan_gc();
        tbdd_refs_pop(2);

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
 * Change the tag on an edge; this function ensures the minimization rules are followed.
 * This is relevant when the new tag is identical to the variable of the node.
 */
TBDD
tbdd_settag(TBDD dd, uint32_t tag)
{
    if (TBDD_GETINDEX(dd) > 1) {
        tbddnode_t n = TBDD_GETNODE(dd);
        uint32_t var = tbddnode_getvariable(n);
        assert(tag <= var);
        if (var == tag) {
            TBDD low = tbddnode_low(dd, n);
            TBDD high = tbddnode_high(dd, n);
            if (low == high) return low;
        }
    }
    return TBDD_SETTAG(dd, tag);
}

/**
 * Evaluate a TBDD, assigning <value> (1 or 0) to <variable>;
 * <variable> is the current variable in the domain, and <nextvar> the
 * next variable in the domain.
 */
TBDD
tbdd_eval(TBDD dd, uint32_t variable, int value, uint32_t next_var)
{
    uint32_t tag = TBDD_GETTAG(dd);
    if (variable < tag) return dd;
    assert(variable == tag);
    if (tbdd_isleaf(dd)) return value ? tbdd_false : tbdd_settag(dd, next_var);
    tbddnode_t n = TBDD_GETNODE(dd);
    uint32_t var = tbddnode_getvariable(n);
    if (variable < var) return value ? tbdd_false : tbdd_settag(dd, next_var);
    assert(variable == var);
    return value ? tbddnode_high(dd, n) : tbddnode_low(dd, n);
}

/**
 * Obtain a TBDD representing a positive literal of variable <var>.
 */
TBDD
tbdd_ithvar(uint32_t var)
{
    return tbdd_makenode(var, tbdd_false, tbdd_true, 0xfffff);
}

/**
 * Obtain a TBDD representing a negative literal of variable <var>.
 */
TBDD
tbdd_nithvar(uint32_t var)
{
    return tbdd_makenode(var, tbdd_true, tbdd_false, 0xfffff);
}

/**
 * Convert an MTBDD to a TBDD
 */
TASK_IMPL_2(TBDD, tbdd_from_mtbdd, MTBDD, dd, MTBDD, domain)
{
    /* Special treatment for True and False */
    if (dd == mtbdd_false) return tbdd_false;
    if (dd == mtbdd_true) return tbdd_true;

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Count operation */
    sylvan_stats_count(TBDD_FROM_MTBDD);

    /* First (maybe) match domain with dd */
    mtbddnode_t ndd = MTBDD_GETNODE(dd);
    mtbddnode_t ndomain = NULL;
    /* Get variable and cofactors */
    assert(domain != mtbdd_true && domain != mtbdd_false);
    ndomain = MTBDD_GETNODE(domain);
    uint32_t domain_var = mtbddnode_getvariable(ndomain);

    const uint32_t var = mtbddnode_getvariable(ndd);
    while (domain_var != var) {
        assert(domain_var < var);
        domain = mtbddnode_followhigh(domain, ndomain);
        assert(domain != mtbdd_true && domain != mtbdd_false);
        ndomain = MTBDD_GETNODE(domain);
        domain_var = mtbddnode_getvariable(ndomain);
    }

    /* Check cache */
    TBDD result;
    if (cache_get(CACHE_TBDD_FROM_MTBDD|dd, domain, 0, &result)) {
        sylvan_stats_count(TBDD_FROM_MTBDD_CACHED);
        return result;
    }

    /* Get variable and cofactors */
    MTBDD dd_low = mtbddnode_followlow(dd, ndd);
    MTBDD dd_high = mtbddnode_followhigh(dd, ndd);

    /* Recursive */
    MTBDD next_domain = mtbddnode_followhigh(domain, ndomain);
    tbdd_refs_spawn(SPAWN(tbdd_from_mtbdd, dd_high, next_domain));
    TBDD low = tbdd_refs_push(CALL(tbdd_from_mtbdd, dd_low, next_domain));
    TBDD high = tbdd_refs_sync(SYNC(tbdd_from_mtbdd));
    tbdd_refs_pop(1);
    uint32_t next_domain_var = next_domain != mtbdd_true ? mtbdd_getvar(next_domain) : 0xfffff;
    result = tbdd_makenode(var, low, high, next_domain_var);

    /* Store in cache */
    if (cache_put(CACHE_TBDD_FROM_MTBDD|dd, domain, 0, result)) {
        sylvan_stats_count(TBDD_FROM_MTBDD_CACHEDPUT);
    }

    return result;
}

/**
 * Convert a TBDD to an MTBDD.
 */
TASK_IMPL_2(TBDD, tbdd_to_mtbdd, TBDD, dd, TBDD, dom)
{
    /* Special treatment for True and False */
    if (dd == tbdd_false) return mtbdd_false;
    if (dd == tbdd_true) return mtbdd_true;

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Count operation */
    sylvan_stats_count(TBDD_TO_MTBDD);

    /* Check cache */
    TBDD result;
    if (cache_get3(CACHE_TBDD_TO_MTBDD, dd, dom, 0, &result)) {
        sylvan_stats_count(TBDD_TO_MTBDD_CACHED);
        return result;
    }

    /**
     * Get dd variable, domain variable, and next domain variable
     */
    const tbddnode_t dd_node = TBDD_NOTAG(dd) == tbdd_true ? NULL : TBDD_GETNODE(dd);
    const uint32_t dd_tag = TBDD_GETTAG(dd);
    const uint32_t dd_var = dd_node == NULL ? 0xfffff : tbddnode_getvariable(dd_node);

    const tbddnode_t dom_node = TBDD_GETNODE(dom);
    const uint32_t dom_var = tbddnode_getvariable(dom_node);
    const TBDD dom_next = tbddnode_high(dom, dom_node);
    const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

    assert(dom_var <= dd_tag);
    assert(dom_var <= dd_var);

    /**
     * Get cofactors
     */
    TBDD dd0, dd1;
    if (dom_var < dd_tag) {
        dd0 = dd1 = dd;
    } else if (dom_var < dd_var) {
        dd0 = tbdd_settag(dd, dom_next_var);
        dd1 = tbdd_false;
    } else {
        dd0 = tbddnode_low(dd, dd_node);
        dd1 = tbddnode_high(dd, dd_node);
    }

    mtbdd_refs_spawn(SPAWN(tbdd_to_mtbdd, dd0, dom_next));
    MTBDD high = tbdd_to_mtbdd(dd1, dom_next);
    MTBDD low = mtbdd_refs_sync(SYNC(tbdd_to_mtbdd));
    result = mtbdd_makenode(dom_var, low, high);

    /* Store in cache */
    if (cache_put3(CACHE_TBDD_TO_MTBDD, dd, dom, 0, result)) {
        sylvan_stats_count(TBDD_TO_MTBDD_CACHEDPUT);
    }

    return result;
}

/**
 * Create a cube of positive literals of the variables in arr.
 * This represents sets of variables, also variable domains.
 */
TBDD
tbdd_from_array(uint32_t *arr, size_t len)
{
    if (len == 0) return tbdd_true;
    else if (len == 1) return tbdd_makenode(*arr, tbdd_false, tbdd_true, 0xfffff);
    else return tbdd_makenode(arr[0], tbdd_false, tbdd_from_array(arr+1, len-1), arr[1]);
}

/**
 * Combine two domains
 */
TBDD
tbdd_merge_domains(TBDD dom1, TBDD dom2)
{
    if (dom1 == tbdd_true) return dom2;
    if (dom2 == tbdd_true) return dom1;

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_MERGE_DOMAINS);

    /**
     * Consult cache
     */
    TBDD result;
    if (cache_get3(CACHE_TBDD_MERGE_DOMAINS, dom1, dom2, 0, &result)) {
        sylvan_stats_count(TBDD_MERGE_DOMAINS_CACHED);
        return result;
    }

    tbddnode_t dom1_node = TBDD_GETNODE(dom1);
    tbddnode_t dom2_node = TBDD_GETNODE(dom2);
    uint32_t dom1_var = tbddnode_getvariable(dom1_node);
    uint32_t dom2_var = tbddnode_getvariable(dom2_node);
    if (dom1_var < dom2_var) {
        TBDD sub = tbdd_merge_domains(tbddnode_high(dom1, dom1_node), dom2);
        result = tbdd_makenode(dom1_var, tbdd_false, sub, dom2_var);
    } else if (dom2_var < dom1_var) {
        TBDD sub = tbdd_merge_domains(dom1, tbddnode_high(dom2, dom2_node));
        result = tbdd_makenode(dom2_var, tbdd_false, sub, dom2_var);
    } else {
        TBDD sub = tbdd_merge_domains(tbddnode_high(dom1, dom1_node), tbddnode_high(dom2, dom2_node));
        uint32_t var_next = sub == tbdd_true ? 0xfffff : tbdd_getvar(sub);
        result = tbdd_makenode(dom1_var, tbdd_false, sub, var_next);
    }

    if (cache_put3(CACHE_TBDD_MERGE_DOMAINS, dom1, dom2, 0, result)) {
        sylvan_stats_count(TBDD_MERGE_DOMAINS_CACHEDPUT);
    }

    return result;
}

/**
 * Create a cube of literals of the given domain with the values given in <arr>.
 * Uses True as the leaf.
 */
TBDD tbdd_cube(TBDD dom, uint8_t *arr)
{
    if (dom == tbdd_true) return tbdd_true;
    tbddnode_t n = TBDD_GETNODE(dom);
    uint32_t dom_var = tbddnode_getvariable(n);
    TBDD dom_next = tbddnode_high(dom, n);
    uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));
    TBDD res = tbdd_cube(dom_next, arr+1);
    if (*arr == 0) {
        return tbdd_makenode(dom_var, res, tbdd_false, dom_next_var);
    } else if (*arr == 1) {
        return tbdd_makenode(dom_var, tbdd_false, res, dom_next_var);
    } else if (*arr == 2) {
        return tbdd_makenode(dom_var, res, res, dom_next_var);
    }
    return tbdd_invalid;
}

/**
 * Same as tbdd_cube, but adds the cube to an existing set.
 */
TASK_IMPL_3(TBDD, tbdd_union_cube, TBDD, set, TBDD, dom, uint8_t*, arr)
{
    /**
     * Terminal cases
     */
    if (dom == tbdd_true) return tbdd_true;
    if (set == tbdd_true) return tbdd_true;
    if (set == tbdd_false) return tbdd_cube(dom, arr);

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_UNION_CUBE);

    /**
     * Get set variable, domain variable, and next domain variable
     */
    const tbddnode_t set_node = TBDD_NOTAG(set) == tbdd_true ? NULL : TBDD_GETNODE(set);
    const uint32_t set_var = set_node == NULL ? 0xfffff : tbddnode_getvariable(set_node);
    const uint32_t set_tag = TBDD_GETTAG(set);
    const tbddnode_t dom_node = TBDD_GETNODE(dom);
    const uint32_t dom_var = tbddnode_getvariable(dom_node);
    const TBDD dom_next = tbddnode_high(dom, dom_node);
    const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

    assert(dom_var <= set_tag);
    assert(dom_var <= set_var);

    TBDD set0, set1;
    if (dom_var < set_tag) {
        set0 = set1 = set;
    } else if (dom_var < set_var) {
        set0 = tbdd_settag(set, dom_next_var);
        set1 = tbdd_false;
    } else {
        set0 = tbddnode_low(set, set_node);
        set1 = tbddnode_high(set, set_node);
    }

    if (*arr == 0) {
        TBDD low = tbdd_union_cube(set0, dom_next, arr+1);
        return tbdd_makenode(dom_var, low, set1, dom_next_var);
    } else if (*arr == 1) {
        TBDD high = tbdd_union_cube(set1, dom_next, arr+1);
        return tbdd_makenode(dom_var, set0, high, dom_next_var);
    } else if (*arr == 2) {
        tbdd_refs_spawn(SPAWN(tbdd_union_cube, set0, dom_next, arr+1));
        TBDD high = tbdd_union_cube(set1, dom_next, arr+1);
        tbdd_refs_push(high);
        TBDD low = tbdd_refs_sync(SYNC(tbdd_union_cube));
        tbdd_refs_pop(1);
        return tbdd_makenode(dom_var, low, high, dom_next_var);
    }

    return tbdd_invalid;
}

/**
 * Add variables to the domain of a set.
 */
TASK_IMPL_3(TBDD, tbdd_extend_domain, TBDD, set, TBDD, from, TBDD, to)
{
    /**
     * Terminal cases
     */
    if (set == tbdd_true) return tbdd_true;
    if (set == tbdd_false) return tbdd_false;
    if (from == to) return set;
    // assert(from != tbdd_true);
    // assert(to != tbdd_true);

    /**
     * Test for garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_EXTEND_DOMAIN);

    /**
     * Get set variable, domain variable, and next domain variable
     */
    tbddnode_t set_node = TBDD_NOTAG(set) == tbdd_true ? NULL : TBDD_GETNODE(set);
    uint32_t set_var = set_node == NULL ? 0xfffff : tbddnode_getvariable(set_node);
    uint32_t set_tag = TBDD_GETTAG(set);

    tbddnode_t from_node = TBDD_GETNODE(from);
    uint32_t from_var = tbddnode_getvariable(from_node);

    tbddnode_t to_node = TBDD_GETNODE(to);
    uint32_t to_var = tbddnode_getvariable(to_node);

    // to <= from <= tag <= var
    // assert(to_var <= from_var);
    // assert(from_var <= set_tag);
    // assert(set_tag <= set_var);

    /**
     * Forward from and to domains to the set tag
     */

    while (from_var < set_tag) {
        from = tbddnode_high(from, from_node);
        // assert(from != tbdd_true);
        from_node = TBDD_GETNODE(from);
        from_var = tbddnode_getvariable(from_node);
    }

    while (to_var < set_tag) {
        to = tbddnode_high(to, to_node);
        // assert(to != tbdd_true);
        to_node = TBDD_GETNODE(to);
        to_var = tbddnode_getvariable(to_node);
    }

    /**
     * If the domains are equal now, we're done
     */
    if (from == to) return set;

    // tag == to == from <= var
    // assert(from_var == set_tag);
    // assert(to_var == set_tag);

    /**
     * Forward domains towards var as long as they are on the same variable
     */

    while (to_var == from_var && to_var < set_var) {
        from = tbddnode_high(from, from_node);
        to = tbddnode_high(to, to_node);
        // assert(to != tbdd_true);
        from_node = from == tbdd_true ? NULL : TBDD_GETNODE(from);
        from_var = from_node == NULL ? 0xfffff : tbddnode_getvariable(from_node);
        to_node = TBDD_GETNODE(to);
        to_var = tbddnode_getvariable(to_node);
    }

    /**
     * At this point, either we need to insert a (k,k) node because a variable
     * is introduced, or we are splitting on the variable of the set
     */

    // tag <= to < from <= var   OR   tag <= to == from == var
    // assert(set_tag <= to_var);
    // assert(to_var <= from_var);
    // assert(from_var <= set_var);

    /**
     * Check the cache
     */
    TBDD result;
    if (cache_get3(CACHE_TBDD_EXTEND_DOMAIN, set, from, to, &result)) {
        sylvan_stats_count(TBDD_EXTEND_DOMAIN_CACHED);
        return result;
    }

    if (to_var < from_var) {
        /* insert a node between the tag and the var */
        TBDD to_next = tbddnode_high(to, to_node);
        uint32_t to_next_var = to_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(to_next));
        set = tbdd_settag(set, from_var);
        result = tbdd_extend_domain(set, from, to_next);
        result = tbdd_makenode(to_var, result, result, to_next_var);
        result = tbdd_makenode(set_tag, result, tbdd_false, to_var);
    } else {
        /* normal recursion */
        TBDD from_next = tbddnode_high(from, from_node);
        uint32_t from_next_var = from_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(from_next));
        TBDD to_next = tbddnode_high(to, to_node);
        uint32_t to_next_var = to_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(to_next));

        // assert(to_var == from_var);
        // assert(to_next_var <= from_next_var);
        // assert(from_var < from_next_var);
        // assert(to_var < to_next_var);

        TBDD set0, set1;
        if (from_var < set_tag) {
            set0 = set1 = set;
        } else if (from_var < set_var) {
            set0 = tbdd_settag(set, from_next_var);
            set1 = tbdd_false;
        } else {
            set0 = tbddnode_low(set, set_node);
            set1 = tbddnode_high(set, set_node);
        }

        tbdd_refs_spawn(SPAWN(tbdd_extend_domain, set0, from_next, to_next));
        TBDD high = CALL(tbdd_extend_domain, set1, from_next, to_next);
        tbdd_refs_push(high);
        TBDD low = tbdd_refs_sync(SYNC(tbdd_extend_domain));
        tbdd_refs_pop(1);
        result = tbdd_makenode(to_var, low, high, to_next_var);
        result = tbdd_makenode(set_tag, result, tbdd_false, to_var);
    }

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_TBDD_EXTEND_DOMAIN, set, from, to, result)) {
        sylvan_stats_count(TBDD_EXTEND_DOMAIN_CACHEDPUT);
    }

    return result;
}

/**
 * Implementation of the AND operator for Boolean TBDDs
 * We interpret <a> and <b> under the given domain <dom>
 */
TASK_IMPL_3(TBDD, tbdd_and, TBDD, a, TBDD, b, TBDD, dom)
{
    /**
     * Check the case where A or B is False
     */
    if (a == tbdd_false || b == tbdd_false) return tbdd_false;
    if (a == tbdd_true) return b;
    if (b == tbdd_true) return a;

    /**
     * Get the tags
     */
    uint32_t a_tag = TBDD_GETTAG(a);
    uint32_t b_tag = TBDD_GETTAG(b);
    uint32_t mintag = a_tag < b_tag ? a_tag : b_tag;

    /**
     * Check the case A \and A == A
     * Also checks the case True \and True == True
     */
    if (TBDD_NOTAG(a) == TBDD_NOTAG(b)) return tbdd_settag(a, mintag);

    assert(dom != tbdd_true);

    /**
     * Switch A and B if A > B (for cache)
     */
    if (TBDD_GETINDEX(a) > TBDD_GETINDEX(b)) {
        TBDD t = a;
        a = b;
        b = t;
        uint32_t tt = a_tag;
        a_tag = b_tag;
        b_tag = tt;
    }

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_AND);

    /**
     * Check the cache
     */
    TBDD result;
    const TBDD _dom = dom;
    if (cache_get3(CACHE_TBDD_AND, a, b, dom, &result)) {
        sylvan_stats_count(TBDD_AND_CACHED);
        return result;
    }

    /**
     * Get the vars
     */
    tbddnode_t a_node = TBDD_NOTAG(a) == tbdd_true ? NULL : TBDD_GETNODE(a);
    uint32_t a_var = TBDD_NOTAG(a) == tbdd_true ? 0xfffff : tbddnode_getvariable(a_node);
    tbddnode_t b_node = TBDD_NOTAG(b) == tbdd_true ? NULL : TBDD_GETNODE(b);
    uint32_t b_var = TBDD_NOTAG(b) == tbdd_true ? 0xfffff : tbddnode_getvariable(b_node);
    uint32_t minvar = a_var < b_var ? a_var : b_var;

    assert(minvar < 0xfffff);

    /**
     * Forward domain to pivot variable
     */
    tbddnode_t dom_node = TBDD_GETNODE(dom);
    uint32_t dom_var = tbddnode_getvariable(dom_node);
    while (dom_var != minvar) {
        assert(dom_var < minvar);
        dom = tbddnode_high(dom, dom_node);
        assert(dom != tbdd_true);
        dom_node = TBDD_GETNODE(dom);
        dom_var = tbddnode_getvariable(dom_node);
    }

    /**
     * Get next variable in domain
     */
    const TBDD dom_next = tbddnode_high(dom, dom_node);
    const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

    assert(dom_var < dom_next_var);

    /**
     * Get cofactors for A
     */
    TBDD a0, a1;
    if (minvar < a_tag) {
        a0 = a1 = a;
    } else if (minvar < a_var) {
        a0 = tbdd_settag(a, dom_next_var);
        a1 = tbdd_false;
    } else {
        a0 = tbddnode_low(a, a_node);
        a1 = tbddnode_high(a, a_node);
    }

    /**
     * Get cofactors for B
     */
    TBDD b0, b1;
    if (minvar < b_tag) {
        b0 = b1 = b;
    } else if (minvar < b_var) {
        b0 = tbdd_settag(b, dom_next_var);
        b1 = tbdd_false;
    } else {
        b0 = tbddnode_low(b, b_node);
        b1 = tbddnode_high(b, b_node);
    }

    assert(TBDD_GETTAG(a0) >= dom_next_var);
    assert(TBDD_GETTAG(a1) >= dom_next_var);
    assert(TBDD_GETTAG(b0) >= dom_next_var);
    assert(TBDD_GETTAG(b1) >= dom_next_var);

    /**
     * Now we call recursive tasks
     */
    tbdd_refs_spawn(SPAWN(tbdd_and, a0, b0, dom_next));
    TBDD high = a1 == tbdd_false || b1 == tbdd_false ? tbdd_false : CALL(tbdd_and, a1, b1, dom_next);
    tbdd_refs_push(high);
    TBDD low = tbdd_refs_sync(SYNC(tbdd_and));
    tbdd_refs_pop(1);

    /**
     * Compute result node
     */
    result = tbdd_makenode(minvar, low, high, dom_next_var);
    if (mintag < minvar) result = tbdd_makenode(mintag, result, tbdd_false, minvar);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_TBDD_AND, a, b, _dom, result)) {
        sylvan_stats_count(TBDD_AND_CACHEDPUT);
    }

    return result;
}

/**
 * Compute the and operator for two boolean TBDDs on different domains
 * The domain of the result is the combination of the two given domains
 */
TASK_IMPL_4(TBDD, tbdd_and_dom, TBDD, a, TBDD, dom_a, TBDD, b, TBDD, dom_b)
{
    /**
     * Terminal cases
     */
    if (a == tbdd_false || b == tbdd_false) return tbdd_false;
    if (a == tbdd_true && b == tbdd_true) return tbdd_true;
    if (dom_a == tbdd_true) return b;
    if (dom_b == tbdd_true) return a;
    if (dom_a == dom_b) return CALL(tbdd_and, a, b, dom_a);

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_AND_DOM);

    /**
     * Switch A and B if A > B (for cache)
     */
    if (TBDD_GETINDEX(a) > TBDD_GETINDEX(b)) {
        TBDD t = a;
        a = b;
        b = t;
        TBDD dom_t = dom_a;
        dom_a = dom_b;
        dom_b = dom_t;
    }

    /**
     * Obtain nodes, tags, variables
     */
    tbddnode_t a_node = TBDD_GETINDEX(a) <= 1 ? NULL : TBDD_GETNODE(a);
    uint32_t a_tag = TBDD_GETTAG(a);
    uint32_t a_var = a_node == NULL ? 0xfffff : tbddnode_getvariable(a_node);

    tbddnode_t b_node = TBDD_GETINDEX(b) <= 1 ? NULL : TBDD_GETNODE(b);
    uint32_t b_tag = TBDD_GETTAG(b);
    uint32_t b_var = b_node == NULL ? 0xfffff : tbddnode_getvariable(b_node);

    tbddnode_t dom_a_node = TBDD_GETNODE(dom_a);
    uint32_t dom_a_var = tbddnode_getvariable(dom_a_node);

    tbddnode_t dom_b_node = TBDD_GETNODE(dom_b);
    uint32_t dom_b_var = tbddnode_getvariable(dom_b_node);

    uint32_t newtag = 0xfffff;
    TBDD result;

    while (1) {
        if (dom_a_var < dom_b_var) {
            if (dom_a_var < a_tag && newtag == 0xfffff) {
                dom_a = tbddnode_high(dom_a, dom_a_node);
                if (dom_a == tbdd_true) return b;
                dom_a_node = TBDD_GETNODE(dom_a);
                dom_a_var = tbddnode_getvariable(dom_a_node);
                continue;
            }
            if (dom_a_var == a_tag && dom_a_var < a_var) {
                if (newtag == 0xfffff) newtag = a_tag;
                dom_a = tbddnode_high(dom_a, dom_a_node);
                if (dom_a == tbdd_true) return tbdd_makenode(newtag, b, tbdd_false, dom_b_var);
                dom_a_node = TBDD_GETNODE(dom_a);
                dom_a_var = tbddnode_getvariable(dom_a_node);
                a = tbdd_settag(a, dom_a_var);
                a_tag = TBDD_GETTAG(a);
                a_node = TBDD_GETINDEX(a) <= 1 ? NULL : TBDD_GETNODE(a);
                a_var = a_node == NULL ? 0xfffff : tbddnode_getvariable(a_node);
                continue;
            }
        }
        if (dom_b_var < dom_a_var) {
            if (dom_b_var < b_tag && newtag == 0xfffff) {
                dom_b = tbddnode_high(dom_b, dom_b_node);
                if (dom_b == tbdd_true) return a;
                dom_b_node = TBDD_GETNODE(dom_b);
                dom_b_var = tbddnode_getvariable(dom_b_node);
                continue;
            }
            if (dom_b_var == b_tag && dom_b_var < b_var) {
                if (newtag == 0xfffff) newtag = b_tag;
                dom_b = tbddnode_high(dom_b, dom_b_node);
                if (dom_b == tbdd_true) return tbdd_makenode(newtag, a, tbdd_false, dom_a_var);
                dom_b_node = TBDD_GETNODE(dom_b);
                dom_b_var = tbddnode_getvariable(dom_b_node);
                b = tbdd_settag(b, dom_b_var);
                b_tag = TBDD_GETTAG(b);
                b_node = TBDD_GETINDEX(b) <= 1 ? NULL : TBDD_GETNODE(b);
                b_var = b_node == NULL ? 0xfffff : tbddnode_getvariable(b_node);
                continue;
            }
        }
        if (dom_a_var == dom_b_var) {
            if (dom_a_var < a_tag && dom_b_var < b_tag && newtag == 0xfffff) {
                dom_a = tbddnode_high(dom_a, dom_a_node);
                if (dom_a == tbdd_true) return b;
                dom_a_node = TBDD_GETNODE(dom_a);
                dom_a_var = tbddnode_getvariable(dom_a_node);
                dom_b = tbddnode_high(dom_b, dom_b_node);
                if (dom_b == tbdd_true) return a;
                dom_b_node = TBDD_GETNODE(dom_b);
                dom_b_var = tbddnode_getvariable(dom_b_node);
                continue;
            }
            if (dom_a_var == a_tag && dom_b_var == b_tag && a_tag < a_var && b_tag < b_var) {
                if (newtag == 0xfffff) newtag = a_tag;
                dom_a = tbddnode_high(dom_a, dom_a_node);
                if (dom_a == tbdd_true) {
                    if (newtag < dom_b_var) return tbdd_makenode(newtag, b, tbdd_false, dom_b_var);
                    else return b;
                }
                dom_a_node = TBDD_GETNODE(dom_a);
                dom_a_var = tbddnode_getvariable(dom_a_node);
                a = tbdd_settag(a, dom_a_var);
                a_tag = TBDD_GETTAG(a);
                a_node = TBDD_GETINDEX(a) <= 1 ? NULL : TBDD_GETNODE(a);
                a_var = a_node == NULL ? 0xfffff : tbddnode_getvariable(a_node);
                dom_b = tbddnode_high(dom_b, dom_b_node);
                if (dom_b == tbdd_true) {
                    if (newtag < dom_a_var) return tbdd_makenode(newtag, a, tbdd_false, dom_a_var);
                    else return a;
                }
                dom_b_node = TBDD_GETNODE(dom_b);
                dom_b_var = tbddnode_getvariable(dom_b_node);
                b = tbdd_settag(b, dom_b_var);
                b_tag = TBDD_GETTAG(b);
                b_node = TBDD_GETINDEX(b) <= 1 ? NULL : TBDD_GETNODE(b);
                b_var = b_node == NULL ? 0xfffff : tbddnode_getvariable(b_node);
                continue;
            }
        }
        break;
    }

    assert(dom_a_var <= a_tag && a_tag <= a_var);
    assert(dom_b_var <= b_tag && b_tag <= b_var);

    const uint32_t dom_var = dom_a_var < dom_b_var ? dom_a_var : dom_b_var;

    /**
     * Check the cache
     */
    if (cache_get6(CACHE_TBDD_AND_DOM, a, b, dom_a, dom_b, 0, &result, NULL)) {
        sylvan_stats_count(TBDD_AND_DOM_CACHED);
        if (newtag < dom_var) result = tbdd_makenode(newtag, result, tbdd_false, dom_var);
        return result;
    }

    if (dom_a_var < dom_b_var) {
        /**
         * Get next domain
         */
        const TBDD dom_a_next = tbddnode_high(dom_a, dom_a_node);
        const uint32_t dom_a_next_var = dom_a_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_a_next));

        /**
         * Compute cofactors
         */
        TBDD a0, a1;
        if (dom_a_var < a_tag) {
            a0 = a1 = a;
        } else if (dom_a_var < a_var) {
            a0 = tbdd_settag(a, dom_a_next_var);
            a1 = tbdd_false;
        } else {
            a0 = tbddnode_low(a, a_node);
            a1 = tbddnode_high(a, a_node);
        }

        /**
         * Now we call recursive tasks
         */
        TBDD low, high;
        tbdd_refs_spawn(SPAWN(tbdd_and_dom, a0, dom_a_next, b, dom_b));
        high = CALL(tbdd_and_dom, a1, dom_a_next, b, dom_b);
        tbdd_refs_push(high);
        low = tbdd_refs_sync(SYNC(tbdd_and_dom));
        tbdd_refs_pop(1);

        /**
         * Compute result node
         */
        uint32_t next_var = dom_a_next_var < dom_b_var ? dom_a_next_var : dom_b_var;
        result = tbdd_makenode(dom_var, low, high, next_var);
    } else if (dom_b_var < dom_a_var) {
        /**
         * Get next domains
         */
        const TBDD dom_b_next = tbddnode_high(dom_b, dom_b_node);
        const uint32_t dom_b_next_var = dom_b_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_b_next));

        /**
         * Compute cofactors
         */
        TBDD b0, b1;
        if (dom_b_var < b_tag) {
            b0 = b1 = b;
        } else if (dom_b_var < b_var) {
            b0 = tbdd_settag(b, dom_b_next_var);
            b1 = tbdd_false;
        } else {
            b0 = tbddnode_low(b, b_node);
            b1 = tbddnode_high(b, b_node);
        }

        /**
         * Now we call recursive tasks
         */
        TBDD low, high;
        tbdd_refs_spawn(SPAWN(tbdd_and_dom, a, dom_a, b0, dom_b_next));
        high = CALL(tbdd_and_dom, a, dom_a, b1, dom_b_next);
        tbdd_refs_push(high);
        low = tbdd_refs_sync(SYNC(tbdd_and_dom));
        tbdd_refs_pop(1);

        /**
         * Compute result node
         */
        uint32_t next_var = dom_b_next_var < dom_a_var ? dom_b_next_var : dom_a_var;
        result = tbdd_makenode(dom_b_var, low, high, next_var);
    } else {
        /**
         * Get next domains
         */
        const TBDD dom_a_next = tbddnode_high(dom_a, dom_a_node);
        const TBDD dom_b_next = tbddnode_high(dom_b, dom_b_node);
        const uint32_t dom_a_next_var = dom_a_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_a_next));
        const uint32_t dom_b_next_var = dom_b_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_b_next));

        /**
         * Compute cofactors
         */
        TBDD a0, a1;
        if (dom_a_var < a_tag) {
            a0 = a1 = a;
        } else if (dom_a_var < a_var) {
            a0 = tbdd_settag(a, dom_a_next_var);
            a1 = tbdd_false;
        } else {
            a0 = tbddnode_low(a, a_node);
            a1 = tbddnode_high(a, a_node);
        }

        TBDD b0, b1;
        if (dom_b_var < b_tag) {
            b0 = b1 = b;
        } else if (dom_b_var < b_var) {
            b0 = tbdd_settag(b, dom_b_next_var);
            b1 = tbdd_false;
        } else {
            b0 = tbddnode_low(b, b_node);
            b1 = tbddnode_high(b, b_node);
        }

        /**
         * Now we call recursive tasks
         */
        TBDD low, high;
        tbdd_refs_spawn(SPAWN(tbdd_and_dom, a0, dom_a_next, b0, dom_b_next));
        high = CALL(tbdd_and_dom, a1, dom_a_next, b1, dom_b_next);
        tbdd_refs_push(high);
        low = tbdd_refs_sync(SYNC(tbdd_and_dom));
        tbdd_refs_pop(1);

        /**
         * Compute result node
         */
        uint32_t next_var = dom_a_next_var < dom_b_next_var ? dom_a_next_var : dom_b_next_var;
        result = tbdd_makenode(dom_var, low, high, next_var);
    }

    /**
     * Cache the result
     */
    if (cache_put6(CACHE_TBDD_AND_DOM, a, b, dom_a, dom_b, 0, result, 0)) {
        sylvan_stats_count(TBDD_AND_DOM_CACHEDPUT);
    }

    if (newtag < dom_var) result = tbdd_makenode(newtag, result, tbdd_false, dom_var);

    return result;
}

/**
 * Implementation of the ITE operator for Boolean TBDDs
 * We interpret <a>, <b> and <c> under the given domain <dom>
 */
TASK_IMPL_4(TBDD, tbdd_ite, TBDD, a, TBDD, b, TBDD, c, TBDD, dom)
{
    /**
     * Trivial cases (similar to sylvan_ite)
     */
    if (a == tbdd_true) return b;
    if (a == tbdd_false) return c;
    if (a == b) b = tbdd_true;
    if (a == c) c = tbdd_false;
    if (c == tbdd_false) return tbdd_and(a, b, dom);
    if (b == c) return b;
    // not much more here, because negation is not constant...

    assert(dom != tbdd_true);

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_ITE);

    /**
     * Check the cache
     */
    TBDD result;
    const TBDD _dom = dom;
    if (cache_get6(CACHE_TBDD_ITE|a, b, c, dom, 0, 0, &result, NULL)) {
        sylvan_stats_count(TBDD_ITE_CACHED);
        return result;
    }

    /**
     * Obtain variables and tags
     */
    uint32_t a_var, b_var, c_var;
    tbddnode_t a_node, b_node, c_node;

    // a cannot be False
    if (TBDD_NOTAG(a) == tbdd_true) {
        a_node = NULL;
        a_var = 0xfffff;
    } else {
        a_node = TBDD_GETNODE(a);
        a_var = tbddnode_getvariable(a_node);
    }

    // b can be True or False
    if (b == tbdd_false || TBDD_NOTAG(b) == tbdd_true) {
        b_node = NULL;
        b_var = 0xfffff;
    } else {
        b_node = TBDD_GETNODE(b);
        b_var = tbddnode_getvariable(b_node);
    }

    // c cannot be False
    if (c == tbdd_false || TBDD_NOTAG(c) == tbdd_true) {
        c_node = NULL;
        c_var = 0xfffff;
    } else {
        c_node = TBDD_GETNODE(c);
        c_var = tbddnode_getvariable(c_node);
    }

    uint32_t a_tag = TBDD_GETTAG(a);
    uint32_t b_tag = TBDD_GETTAG(b);
    uint32_t c_tag = TBDD_GETTAG(c);

    uint32_t minvar = a_var < b_var ? (a_var < c_var ? a_var : c_var) : (b_var < c_var ? b_var : c_var);
    uint32_t mintag = a_tag < b_tag ? (a_tag < c_tag ? a_tag : c_tag) : (b_tag < c_tag ? b_tag : c_tag);

    /**
     * Compute the pivot variable
     * if tags are the same: lowest variable
     * otherwise: lowest tag
     */
    const uint32_t var = (a_tag == b_tag && b_tag == c_tag) ? minvar : mintag;
    assert(var != 0xfffff);

    /**
     * Forward domain to pivot variable
     */
    tbddnode_t dom_node = TBDD_GETNODE(dom);
    uint32_t dom_var = tbddnode_getvariable(dom_node);
    while (dom_var != var) {
        assert(dom_var < var);
        dom = tbddnode_high(dom, dom_node);
        assert(dom != tbdd_true);
        dom_node = TBDD_GETNODE(dom);
        dom_var = tbddnode_getvariable(dom_node);
    }

    /**
     * Get next variable in domain
     */
    TBDD dom_next = tbddnode_high(dom, dom_node);
    uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

    if (a_var == var) assert(a_tag == mintag);
    if (b_var == var) assert(b_tag == mintag);
    if (c_var == var) assert(c_tag == mintag);
    if (a_var != var && a_tag == mintag) assert(var >= a_tag);
    if (b_var != var && b_tag == mintag) assert(var >= b_tag);
    if (c_var != var && c_tag == mintag) assert(var >= c_tag);

    /**
     * Get cofactors for A
     */
    TBDD a0, a1;
    if (var < a_tag) {
        a0 = a1 = a;
    } else if (var < a_var) {
        a0 = tbdd_settag(a, dom_next_var);
        a1 = tbdd_false;
    } else {
        a0 = tbddnode_low(a, a_node);
        a1 = tbddnode_high(a, a_node);
    }

    /**
     * Get cofactors for B
     */
    TBDD b0, b1;
    if (var < b_tag) {
        b0 = b1 = b;
    } else if (var < b_var) {
        b0 = tbdd_settag(b, dom_next_var);
        b1 = tbdd_false;
    } else {
        b0 = tbddnode_low(b, b_node);
        b1 = tbddnode_high(b, b_node);
    }

    /**
     * Get cofactors for C
     */
    TBDD c0, c1;
    if (var < c_tag) {
        c0 = c1 = c;
    } else if (var < c_var) {
        c0 = tbdd_settag(c, dom_next_var);
        c1 = tbdd_false;
    } else {
        c0 = tbddnode_low(c, c_node);
        c1 = tbddnode_high(c, c_node);
    }

    assert(TBDD_GETTAG(a0) >= dom_next_var);
    assert(TBDD_GETTAG(a1) >= dom_next_var);
    assert(TBDD_GETTAG(b0) >= dom_next_var);
    assert(TBDD_GETTAG(b1) >= dom_next_var);
    assert(TBDD_GETTAG(c0) >= dom_next_var);
    assert(TBDD_GETTAG(c1) >= dom_next_var);

    /**
     * Now we call recursive tasks
     */
    tbdd_refs_spawn(SPAWN(tbdd_ite, a0, b0, c0, dom_next));
    TBDD high = CALL(tbdd_ite, a1, b1, c1, dom_next);
    tbdd_refs_push(high);
    TBDD low = tbdd_refs_sync(SYNC(tbdd_ite));
    tbdd_refs_pop(1);

    /**
     * Compute result node
     */
    result = tbdd_makenode(var, low, high, dom_next_var);
    if (mintag < var) result = tbdd_makenode(mintag, result, tbdd_false, var);

    /**
     * Cache the result
     */
    if (cache_put6(CACHE_TBDD_ITE|a, b, c, _dom, 0, 0, result, 0)) {
        sylvan_stats_count(TBDD_ITE_CACHEDPUT);
    }

    return result;
}

/**
 * Implementation of the OR operator for Boolean TBDDs
 * We interpret <a> and <b> under the given domain <dom>
 */
TASK_IMPL_3(TBDD, tbdd_or, TBDD, a, TBDD, b, TBDD, dom)
{
    /**
     * Trivial cases (similar to sylvan_ite)
     */
    if (a == tbdd_true) return tbdd_true;
    if (b == tbdd_true) return tbdd_true;
    if (a == tbdd_false) return b;
    if (b == tbdd_false) return a;
    if (a == b) return a;

    assert(dom != tbdd_true);
    
    uint32_t a_tag = TBDD_GETTAG(a);
    uint32_t b_tag = TBDD_GETTAG(b);
    if (TBDD_NOTAG(a) == TBDD_NOTAG(b)) return tbdd_settag(a, a_tag < b_tag ? b_tag : a_tag);

    uint32_t mintag = a_tag < b_tag ? a_tag : b_tag;

    /**
     * Forward domain to mintag
     */
    tbddnode_t dom_node = TBDD_GETNODE(dom);
    uint32_t dom_var = tbddnode_getvariable(dom_node);
    while (dom_var != mintag) {
        assert(dom_var < mintag);
        dom = tbddnode_high(dom, dom_node);
        assert(dom != tbdd_true);
        dom_node = TBDD_GETNODE(dom);
        dom_var = tbddnode_getvariable(dom_node);
    }

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_OR);

    /**
     * Check the cache
     */
    TBDD result;
    const TBDD _dom = dom;
    if (cache_get3(CACHE_TBDD_OR, a, b, dom, &result)) {
        sylvan_stats_count(TBDD_OR_CACHED);
        return result;
    }

    /**
     * Obtain variables and tags
     */
    uint32_t a_var, b_var;
    tbddnode_t a_node, b_node;

    if (TBDD_NOTAG(a) == tbdd_true) {
        a_node = NULL;
        a_var = 0xfffff;
    } else {
        a_node = TBDD_GETNODE(a);
        a_var = tbddnode_getvariable(a_node);
    }

    if (TBDD_NOTAG(b) == tbdd_true) {
        b_node = NULL;
        b_var = 0xfffff;
    } else {
        b_node = TBDD_GETNODE(b);
        b_var = tbddnode_getvariable(b_node);
    }

    uint32_t minvar = a_var < b_var ? a_var : b_var;
    uint32_t newtag, var;

    /**
     * Compute the pivot variable
     */
    if (a_tag < b_tag) {
        if (a_tag < minvar) {
            TBDD dom_next = tbddnode_high(dom, dom_node);
            uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));
            result = tbdd_or(tbdd_settag(a, dom_next_var), b, dom_next);
            result = tbdd_makenode(a_tag, result, b, dom_next_var);

            /**
             * Cache the result
             */
            if (cache_put3(CACHE_TBDD_OR, a, b, _dom, result)) {
                sylvan_stats_count(TBDD_OR_CACHEDPUT);
            }

            return result;
        } else {
            newtag = var = minvar;
        }
    } else if (b_tag < a_tag) {
        if (b_tag < minvar) {
            TBDD dom_next = tbddnode_high(dom, dom_node);
            uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));
            result = tbdd_or(a, tbdd_settag(b, dom_next_var), dom_next);
            result = tbdd_makenode(b_tag, result, a, dom_next_var);

            /**
             * Cache the result
             */
            if (cache_put3(CACHE_TBDD_OR, a, b, _dom, result)) {
                sylvan_stats_count(TBDD_OR_CACHEDPUT);
            }

            return result;
        } else {
            newtag = var = minvar;
        }
    } else /* a_tag == b_tag */ {
        newtag = a_tag;
        var = minvar;

        /**
         * Forward domain to pivot variable
         */
        while (dom_var != var) {
            assert(dom_var < var);
            dom = tbddnode_high(dom, dom_node);
            assert(dom != tbdd_true);
            dom_node = TBDD_GETNODE(dom);
            dom_var = tbddnode_getvariable(dom_node);
        }
    }

    /**
     * Get next variable in domain
     */
    TBDD dom_next = tbddnode_high(dom, dom_node);
    uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

    /**
     * Get cofactors for A
     */
    TBDD a0, a1;
    if (var < a_tag) {
        a0 = a1 = a;
    } else if (var < a_var) {
        a0 = tbdd_settag(a, dom_next_var);
        a1 = tbdd_false;
    } else {
        a0 = tbddnode_low(a, a_node);
        a1 = tbddnode_high(a, a_node);
    }

    /**
     * Get cofactors for B
     */
    TBDD b0, b1;
    if (var < b_tag) {
        b0 = b1 = b;
    } else if (var < b_var) {
        b0 = tbdd_settag(b, dom_next_var);
        b1 = tbdd_false;
    } else {
        b0 = tbddnode_low(b, b_node);
        b1 = tbddnode_high(b, b_node);
    }

    assert(TBDD_GETTAG(a0) >= dom_next_var);
    assert(TBDD_GETTAG(a1) >= dom_next_var);
    assert(TBDD_GETTAG(b0) >= dom_next_var);
    assert(TBDD_GETTAG(b1) >= dom_next_var);

    /**
     * Now we call recursive tasks
     */
    tbdd_refs_spawn(SPAWN(tbdd_or, a0, b0, dom_next));
    TBDD high = CALL(tbdd_or, a1, b1, dom_next);
    tbdd_refs_push(high);
    TBDD low = tbdd_refs_sync(SYNC(tbdd_or));
    tbdd_refs_pop(1);

    /**
     * Compute result node
     */
    result = tbdd_makenode(var, low, high, dom_next_var);
    if (newtag < var) result = tbdd_makenode(newtag, result, tbdd_false, var);

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_TBDD_OR, a, b, _dom, result)) {
        sylvan_stats_count(TBDD_OR_CACHEDPUT);
    }

    return result;
}

/**
 * Compute the not operator
 */
TASK_IMPL_2(TBDD, tbdd_not, TBDD, dd, TBDD, dom)
{
    /**
     * Trivial cases (similar to sylvan_ite)
     */
    if (dd == tbdd_true) return tbdd_false;
    if (dd == tbdd_false) return tbdd_true;

    assert(dom != tbdd_true);

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_NOT);

    /**
     * Check the cache
     */
    TBDD result;
    const TBDD _dom = dom;
    if (cache_get3(CACHE_TBDD_NOT, dd, dom, 0, &result)) {
        sylvan_stats_count(TBDD_NOT_CACHED);
        return result;
    }

    /**
     * Obtain variables and tags
     */
    tbddnode_t dd_node = TBDD_GETINDEX(dd) <= 1 ? NULL : TBDD_GETNODE(dd);
    uint32_t dd_var = dd_node == NULL ? 0xfffff : tbddnode_getvariable(dd_node);
    uint32_t dd_tag = TBDD_GETTAG(dd);

    /**
     * Forward domain to tag
     */
    tbddnode_t dom_node = TBDD_GETNODE(dom);
    uint32_t dom_var = tbddnode_getvariable(dom_node);
    while (dom_var != dd_tag) {
        assert(dom_var < dd_tag);
        dom = tbddnode_high(dom, dom_node);
        assert(dom != tbdd_true);
        dom_node = TBDD_GETNODE(dom);
        dom_var = tbddnode_getvariable(dom_node);
    }

    /**
     * Get next variable in domain
     */
    const TBDD dom_next = tbddnode_high(dom, dom_node);
    const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

    /**
     * Either dom_var is (k,0) node, or dom_var is variable of node
     */
    if (dom_var < dd_var) {
        TBDD dd0 = tbdd_settag(dd, dom_next_var);
        TBDD low = CALL(tbdd_not, dd0, dom_next);
        result = tbdd_makenode(dd_tag, low, tbdd_true, dom_next_var);
    } else {
        TBDD dd0 = tbddnode_low(dd, dd_node);
        TBDD dd1 = tbddnode_high(dd, dd_node);

        assert(TBDD_GETTAG(dd0) >= dom_next_var);
        assert(TBDD_GETTAG(dd1) >= dom_next_var);

        /**
         * Now we call recursive tasks
         */
        tbdd_refs_spawn(SPAWN(tbdd_not, dd0, dom_next));
        TBDD high = CALL(tbdd_not, dd1, dom_next);
        tbdd_refs_push(high);
        TBDD low = tbdd_refs_sync(SYNC(tbdd_not));
        tbdd_refs_pop(1);

        /**
         * Compute result node
         */
        result = tbdd_makenode(dd_tag, low, high, dom_next_var);
    }

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_TBDD_NOT, dd, _dom, 0, result)) {
        sylvan_stats_count(TBDD_NOT_CACHEDPUT);
    }

    return result;
}

/**
 * Compute existential quantification, but stay in same domain
 */
TASK_IMPL_3(TBDD, tbdd_exists, TBDD, dd, TBDD, vars, TBDD, dom)
{
    /**
     * Trivial cases
     */
    if (dd == tbdd_true) return dd;
    if (dd == tbdd_false) return dd;
    if (vars == tbdd_true) return dd;

    assert(dom != tbdd_true);

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_EXISTS);

    /**
     * Check the cache
     */
    TBDD result;
    const TBDD _dom = dom;
    if (cache_get3(CACHE_TBDD_EXISTS, dd, vars, dom, &result)) {
        sylvan_stats_count(TBDD_EXISTS_CACHED);
        return result;
    }

    /**
     * Obtain tag and var
     */
    const tbddnode_t dd_node = TBDD_NOTAG(dd) == tbdd_true ? NULL : TBDD_GETNODE(dd);
    const uint32_t dd_var = dd_node == NULL ? 0xfffff : tbddnode_getvariable(dd_node);
    const uint32_t dd_tag = TBDD_GETTAG(dd);

    /**
     * Obtain next variable to remove
     */
    tbddnode_t vars_node = TBDD_GETNODE(vars);
    uint32_t vars_var = tbddnode_getvariable(vars_node);

    /**
     * Forward <vars> to tag (skip to-remove when before tag)
     */
    while (vars_var < dd_tag) {
        vars = tbddnode_high(vars, vars_node);
        if (vars == tbdd_true) return dd;
        vars_node = TBDD_GETNODE(vars);
        vars_var = tbddnode_getvariable(vars_node);
    }

    /**
     * Compute pivot variable
     */
    const uint32_t var = vars_var < dd_var ? vars_var : dd_var;

    /**
     * Forward domain and get dom_var and dom_next_var
     */
    tbddnode_t dom_node = TBDD_GETNODE(dom);
    uint32_t dom_var = tbddnode_getvariable(dom_node);

    while (dom_var != var) {
        assert(dom_var < var);
        dom = tbddnode_high(dom, dom_node);
        assert(dom != tbdd_true);
        dom_node = TBDD_GETNODE(dom);
        dom_var = tbddnode_getvariable(dom_node);
    }

    const TBDD dom_next = tbddnode_high(dom, dom_node);
    const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

    /**
     * Get cofactors
     */
    TBDD dd0, dd1;
    assert(var >= dd_tag);
    if (var < dd_var) {
        /**
         * If var != dd_var, then it must be vars_var
         * Therefore we must quantify...
         */
        dd0 = tbdd_settag(dd, dom_next_var);
        TBDD vars_next = tbddnode_high(vars, vars_node);
        result = CALL(tbdd_exists, dd0, vars_next, dom_next);
        if (dd_tag != var) result = tbdd_makenode(dd_tag, result, tbdd_false, var);
    } else {
        dd0 = tbddnode_low(dd, dd_node);
        dd1 = tbddnode_high(dd, dd_node);

        if (var == vars_var) {
            // Quantify

            /**
             * Now we call recursive tasks
             */
            TBDD vars_next = tbddnode_high(vars, vars_node);
            if (dd0 == dd1) {
                result = CALL(tbdd_exists, dd0, vars_next, dom_next);
            } else {
                tbdd_refs_spawn(SPAWN(tbdd_exists, dd0, vars_next, dom_next));
                TBDD high = CALL(tbdd_exists, dd1, vars_next, dom_next);
                tbdd_refs_push(high);
                TBDD low = tbdd_refs_sync(SYNC(tbdd_exists));
                tbdd_refs_push(low);
                result = tbdd_or(low, high, dom);
                tbdd_refs_pop(2);
            }

            /**
             * Compute result node
             */
            if (dd_tag != var) result = tbdd_makenode(dd_tag, result, tbdd_false, var);
        } else {
            // Keep

            /**
             * Now we call recursive tasks
             */
            TBDD low, high;
            if (dd0 == dd1) {
                low = high = CALL(tbdd_exists, dd0, vars, dom_next);
            } else {
                tbdd_refs_spawn(SPAWN(tbdd_exists, dd0, vars, dom_next));
                high = CALL(tbdd_exists, dd1, vars, dom_next);
                tbdd_refs_push(high);
                low = tbdd_refs_sync(SYNC(tbdd_exists));
                tbdd_refs_pop(1);
            }

            /**
             * Compute result node
             */
            result = tbdd_makenode(var, low, high, dom_next_var);
            if (dd_tag != var) result = tbdd_makenode(dd_tag, result, tbdd_false, var);
        }
    }

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_TBDD_EXISTS, dd, vars, _dom, result)) {
        sylvan_stats_count(TBDD_EXISTS_CACHEDPUT);
    }

    return result;
}

/**
 * Compute existential quantification to a smaller domain
 * Remove all variables from <dd> that are not in <newdom>
 */
TASK_IMPL_2(TBDD, tbdd_exists_dom, TBDD, dd, TBDD, dom)
{
    /**
     * Trivial cases
     */
    if (dd == tbdd_true) return dd;
    if (dd == tbdd_false) return dd;
    if (dom == tbdd_true) return tbdd_true;

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_EXISTS);

    /**
     * Obtain tag
     */
    uint32_t dd_tag = TBDD_GETTAG(dd);

    /**
     * Obtain domain variable
     */
    tbddnode_t dom_node = TBDD_GETNODE(dom);
    uint32_t dom_var = tbddnode_getvariable(dom_node);

    /**
     * Forward domain to tag
     */
    while (dom_var < dd_tag) {
        dom = tbddnode_high(dom, dom_node);
        if (dom == tbdd_true) return tbdd_true;
        dom_node = TBDD_GETNODE(dom);
        dom_var = tbddnode_getvariable(dom_node);
    }

    /**
     * If the dd is True, then use domain variable as new tag
     */
    if (TBDD_NOTAG(dd) == tbdd_true) {
        return dd_tag == dom_var ? dd : tbdd_settag(tbdd_true, dom_var);
    }

    /**
     * Obtain variable
     */
    tbddnode_t dd_node = TBDD_GETNODE(dd);
    uint32_t dd_var = tbddnode_getvariable(dd_node);

    /**
     * The current domain variable is the tag of the result
     */
    uint32_t newtag = dom_var;

    /**
     * Forward domain to var
     */
    while (dom_var < dd_var) {
        dom = tbddnode_high(dom, dom_node);
        if (dom == tbdd_true) return tbdd_settag(tbdd_true, newtag);
        dom_node = TBDD_GETNODE(dom);
        dom_var = tbddnode_getvariable(dom_node);
    }

    /**
     * Check the cache
     */
    TBDD result;
    if (cache_get3(CACHE_TBDD_EXISTS, dd, dom, 0, &result)) {
        sylvan_stats_count(TBDD_EXISTS_CACHED);
        if (newtag != dom_var) result = tbdd_makenode(newtag, result, tbdd_false, dom_var);
        return result;
    }

    /**
     * Get cofactors
     */
    TBDD dd0 = tbddnode_low(dd, dd_node);
    TBDD dd1 = tbddnode_high(dd, dd_node);

    if (dom_var == dd_var) {
        // Keep variable

        /**
         * Get next variable in domain
         */
        TBDD dom_next = tbddnode_high(dom, dom_node);

        /**
         * Now we call recursive tasks
         */
        TBDD low, high;
        if (dd0 == dd1) {
            low = high = CALL(tbdd_exists_dom, dd0, dom_next);
        } else {
            tbdd_refs_spawn(SPAWN(tbdd_exists_dom, dd0, dom_next));
            high = CALL(tbdd_exists_dom, dd1, dom_next);
            tbdd_refs_push(high);
            low = tbdd_refs_sync(SYNC(tbdd_exists_dom));
            tbdd_refs_pop(1);
        }

        /**
         * Compute result node
         */
        uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));
        result = tbdd_makenode(dom_var, low, high, dom_next_var);
    } else {
        // Quantify variable
        assert(dom_var > dd_var);

        /**
         * Now we call recursive tasks
         */
        if (dd0 == dd1) {
            result = CALL(tbdd_exists_dom, dd0, dom);
        } else {
            tbdd_refs_spawn(SPAWN(tbdd_exists_dom, dd0, dom));
            TBDD high = CALL(tbdd_exists_dom, dd1, dom);
            tbdd_refs_push(high);
            TBDD low = tbdd_refs_sync(SYNC(tbdd_exists_dom));
            tbdd_refs_push(low);

            /**
             * Compute result node
             */
            result = low == high ? low : tbdd_or(low, high, dom);
            tbdd_refs_pop(2);
        }
    }

    /**
     * Cache the result
     */
    if (cache_put3(CACHE_TBDD_EXISTS, dd, dom, 0, result)) {
        sylvan_stats_count(TBDD_EXISTS_CACHEDPUT);
    }

    /**
     * Add tag on top
     */
    if (newtag != dom_var) result = tbdd_makenode(newtag, result, tbdd_false, dom_var);

    return result;
}

/**
 * Compute the application of a transition relation to a set.
 * Assumes interleaved variables, with s even and t odd (s+1).
 * Assumes <dom> describes the domain of <set>; <vars> describes the domain of <rel>.
 * Assumes all variables in <dom> are even. (s variables)
 * Assumes all s variables in <vars> are in <dom>.
 * Assumes the relation does not contain other information but the transitions.
 */
TASK_IMPL_4(TBDD, tbdd_relnext, TBDD, set, TBDD, rel, TBDD, vars, TBDD, dom)
{
    /**
     * Trivial cases
     */
    if (set == tbdd_false) return tbdd_false;
    if (rel == tbdd_false) return tbdd_false;
    if (vars == tbdd_true) return set;
    assert(dom != tbdd_true); // because vars is not True

    /**
     * Maybe run garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_RELNEXT);

    /**
     * Obtain tag and var of set and rel
     */
    tbddnode_t set_node = TBDD_NOTAG(set) == tbdd_true ? NULL : TBDD_GETNODE(set);
    uint32_t set_var = set_node == NULL ? 0xfffff : tbddnode_getvariable(set_node);
    uint32_t set_tag = TBDD_GETTAG(set);

    tbddnode_t rel_node = TBDD_NOTAG(rel) == tbdd_true ? NULL : TBDD_GETNODE(rel);
    uint32_t rel_var = rel_node == NULL ? 0xfffff : tbddnode_getvariable(rel_node);
    uint32_t rel_tag = TBDD_GETTAG(rel);
    uint32_t rel_tag_s = rel_tag&(~1);
    uint32_t rel_tag_t = rel_tag_s+1;

    /**
     * Obtain domain variable
     */
    tbddnode_t dom_node = TBDD_GETNODE(dom);
    uint32_t dom_var = tbddnode_getvariable(dom_node);

    /**
     * Obtain relation variable
     */
    tbddnode_t vars_node = TBDD_GETNODE(vars);
    uint32_t vars_var = tbddnode_getvariable(vars_node);

    assert((dom_var&1) == 0);
    assert((vars_var&1) == 0);
    assert(dom_var <= vars_var);
    assert(set_tag == 0xfffff || (set_tag&1)==0);
    assert(set_var == 0xfffff || (set_var&1)==0);
    assert(vars_var <= rel_tag_s);

    /**
     * INVARIANTS for the rewriting rules
     * - dom_var <= set_tag <= set_var
     * - vars_var <= rel_tag_s <= rel_var
     * - dom_var <= vars_var
     *
     * There are a number of cases where we can push the vars/dom forward.
     *
     * Two rules that apply when no newtag has been set.
     * CASE: dom_var < set_tag, dom_var < vars_var
     *       Set contains *; relation does not apply
     *       ==> forward dom
     * CASE: dom_var < set_tag, dom_var == vars_var < rel_tag_s
     *       Set contains *; relation reads * and writes *
     *       ==> forward vars, then dom
     *
     * Two rules that set newtag to current dom_var when applying for the first time
     * CASE: dom_var == set_tag < set_var, dom_var < vars_var
     *       Set has only 0; relation does not apply
     *       ==> forward dom and set_tag
     * CASE: dom_var == set_tag < set_var, dom_var == vars_var == rel_tag_s < rel_tag_t < rel_var
     *       Set has only 0; relation reads * or 0 and writes 0
     *       ==> forward vars and rel_tag, then dom and set_tag
     */

    uint32_t newtag = 0xfffff;
    while (1) {
        /**
         * First check our invariants (debugging)
         */
        assert(dom_var <= set_tag && set_tag <= set_var);
        assert(vars_var <= rel_tag && rel_tag <= rel_var);
        assert(dom_var <= vars_var);

        if (dom_var < set_tag) {
            if (newtag != 0xfffff) break;
            if (dom_var < vars_var) {
                // forward dom
                dom = tbddnode_high(dom, dom_node);
                assert(dom != tbdd_true);
                dom_node = TBDD_GETNODE(dom);
                dom_var = tbddnode_getvariable(dom_node);
                continue;
            } else if (vars_var < rel_tag_s) {
                // forward vars
                vars = tbddnode_high(vars, vars_node);
                vars_node = TBDD_GETNODE(vars);
                vars = tbddnode_high(vars, vars_node);
                if (vars == tbdd_true) return set;
                vars_node = TBDD_GETNODE(vars);
                vars_var = tbddnode_getvariable(vars_node);
                // forward dom
                dom = tbddnode_high(dom, dom_node);
                assert(dom != tbdd_true);
                dom_node = TBDD_GETNODE(dom);
                dom_var = tbddnode_getvariable(dom_node);
                continue;
            }
        } else if (set_tag < set_var) {
            assert(dom_var == set_tag);
            if (dom_var < vars_var) {
                // set has only 0 (tag < var) and relation does not apply (dom < vars)
                // so forward dom and set_tag
                if (newtag == 0xfffff) newtag = set_tag;
                // forward dom
                dom = tbddnode_high(dom, dom_node);
                assert(dom != tbdd_true);
                dom_node = TBDD_GETNODE(dom);
                dom_var = tbddnode_getvariable(dom_node);
                // forward set
                set = tbdd_settag(set, dom_var);
                set_tag = TBDD_GETTAG(set);
                set_node = TBDD_NOTAG(set) == tbdd_true ? NULL : TBDD_GETNODE(set);
                set_var = set_node == NULL ? 0xfffff : tbddnode_getvariable(set_node);
                continue;
            } else if (vars_var == rel_tag_s && rel_tag_t < rel_var) {
                assert(dom_var == vars_var);
                // set has only 0 (tag < var) and relation applies (dom == vars)
                // current rel is either read *, write 0, or read 0, write 0
                // forward dom and set_tag and vars and rel_tag
                if (newtag == 0xfffff) newtag = set_tag;
                // forward vars
                vars = tbddnode_high(vars, vars_node);
                vars_node = TBDD_GETNODE(vars);
                vars = tbddnode_high(vars, vars_node);
                if (vars == tbdd_true) return tbdd_makenode(newtag, set, tbdd_false, dom_var);
                vars_node = TBDD_GETNODE(vars);
                vars_var = tbddnode_getvariable(vars_node);
                // forward rel
                rel = tbdd_settag(rel, vars_var);
                rel_tag = TBDD_GETTAG(rel);
                rel_tag_s = rel_tag&(~1);
                rel_tag_t = rel_tag_s+1;
                rel_node = TBDD_NOTAG(rel) == tbdd_true ? NULL : TBDD_GETNODE(rel);
                rel_var = rel_node == NULL ? 0xfffff : tbddnode_getvariable(rel_node);
                // forward dom
                dom = tbddnode_high(dom, dom_node);
                assert(dom != tbdd_true);
                dom_node = TBDD_GETNODE(dom);
                dom_var = tbddnode_getvariable(dom_node);
                // forward set
                set = tbdd_settag(set, dom_var);
                set_tag = TBDD_GETTAG(set);
                set_node = TBDD_NOTAG(set) == tbdd_true ? NULL : TBDD_GETNODE(set);
                set_var = set_node == NULL ? 0xfffff : tbddnode_getvariable(set_node);
                continue;
            }
        }

        // if we're here, no rules matched and we terminate
        break;
    }

    /**
     * Consult the cache
     */
    TBDD result;
    if (cache_get6(CACHE_TBDD_RELNEXT|set, rel, vars, dom, 0, 0, &result, 0)) {
        sylvan_stats_count(TBDD_RELNEXT_CACHED);
        if (newtag < dom_var) result = tbdd_makenode(newtag, result, tbdd_false, dom_var);
        return result;
    }

    /**
     * Select pivot variable
     */
    const uint32_t var = dom_var;
    const TBDD dom_next = tbddnode_high(dom, dom_node);
    const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

    if (dom_var < vars_var) {
        /**
         * Pivot variable is not a relation variable
         */

        /**
         * Obtain cofactors of set and compute with recursion
         */
        TBDD set0, set1;
        if (var < set_tag) {
            result = CALL(tbdd_relnext, set, rel, vars, dom_next);
            result = tbdd_makenode(var, result, result, dom_next_var);
        } else if (var < set_var) {
            TBDD set0 = tbdd_settag(set, dom_next_var);
            result = CALL(tbdd_relnext, set0, rel, vars, dom_next);
            result = tbdd_makenode(var, result, tbdd_false, dom_next_var);
        } else {
            set0 = tbddnode_low(set, set_node);
            set1 = tbddnode_high(set, set_node);
            tbdd_refs_spawn(SPAWN(tbdd_relnext, set0, rel, vars, dom_next));
            TBDD high = CALL(tbdd_relnext, set1, rel, vars, dom_next);
            tbdd_refs_push(high);
            TBDD low = tbdd_refs_sync(SYNC(tbdd_relnext));
            tbdd_refs_pop(1);
            result = tbdd_makenode(var, low, high, dom_next_var);
        }

        /**
         * Put in cache
         */

        if (cache_put6(CACHE_TBDD_RELNEXT|set, rel, vars, dom, 0, 0, result, 0)) {
            sylvan_stats_count(TBDD_RELNEXT_CACHEDPUT);
        }

        if (newtag < dom_var) result = tbdd_makenode(newtag, result, tbdd_false, dom_var);
        return result;
    }

    /**
     * If we are here, then the pivot variable is a relation variable
     */

    uint32_t var_s = var;
    uint32_t var_t = var_s + 1;

    TBDD vars_next = tbddnode_high(vars, vars_node);
    tbddnode_t vars_next_node = vars_next == tbdd_true ? NULL : TBDD_GETNODE(vars_next);
    uint32_t vars_next_var = vars_next_node == NULL ? 0xfffff : tbddnode_getvariable(vars_next_node);
    assert(vars_next_var == var_t);
    vars_next = tbddnode_high(vars_next, vars_next_node);
    vars_next_node = vars_next == tbdd_true ? NULL : TBDD_GETNODE(vars_next);
    vars_next_var = vars_next_node == NULL ? 0xfffff : tbddnode_getvariable(vars_next_node);

    /**
     * Obtain cofactors of set
     */
    TBDD set0, set1;
    if (var_s < set_tag) {
        set0 = set1 = set;
    } else if (var_s < set_var) {
        set0 = tbdd_settag(set, dom_next_var);
        set1 = tbdd_false;
    } else {
        set0 = tbddnode_low(set, set_node);
        set1 = tbddnode_high(set, set_node);
    }

    /**
     * Obtain cofactors of rel
     */
    TBDD rel0, rel1;
    if (var_s < rel_tag) {
        rel0 = rel1 = rel;
    } else if (var_s < rel_var) {
        rel0 = tbdd_settag(rel, var_t);
        rel1 = tbdd_false;
    } else {
        rel0 = tbddnode_low(rel, rel_node);
        rel1 = tbddnode_high(rel, rel_node);
    }

    /**
     * Obtain cofactors of rel0
     */
    const tbddnode_t rel0_node = TBDD_GETINDEX(rel0) <= 1 ? NULL : TBDD_GETNODE(rel0);
    const uint32_t rel0_tag = TBDD_GETTAG(rel0);
    const uint32_t rel0_var = rel0_node == NULL ? 0xfffff : tbddnode_getvariable(rel0_node);

    TBDD rel00, rel01;
    if (var_t < rel0_tag) {
        rel00 = rel01 = rel0;
    } else if (var_t < rel0_var) {
        rel00 = tbdd_settag(rel0, vars_next_var);
        rel01 = tbdd_false;
    } else {
        rel00 = tbddnode_low(rel0, rel0_node);
        rel01 = tbddnode_high(rel0, rel0_node);
    }

    /**
     * Obtain cofactors of rel1
     */
    const tbddnode_t rel1_node = TBDD_GETINDEX(rel1) <= 1 ? NULL : TBDD_GETNODE(rel1);
    const uint32_t rel1_tag = TBDD_GETTAG(rel1);
    const uint32_t rel1_var = rel1_node == NULL ? 0xfffff : tbddnode_getvariable(rel1_node);

    TBDD rel10, rel11;
    if (var_t < rel1_tag) {
        rel10 = rel11 = rel1;
    } else if (var_t < rel1_var) {
        rel10 = tbdd_settag(rel1, vars_next_var);
        rel11 = tbdd_false;
    } else {
        rel10 = tbddnode_low(rel1, rel1_node);
        rel11 = tbddnode_high(rel1, rel1_node);
    }

    /**
     * Perform recursive computations
     */
    tbdd_refs_spawn(SPAWN(tbdd_relnext, set0, rel00, vars_next, dom_next));
    tbdd_refs_spawn(SPAWN(tbdd_relnext, set0, rel01, vars_next, dom_next));
    tbdd_refs_spawn(SPAWN(tbdd_relnext, set1, rel10, vars_next, dom_next));
    TBDD res11 = CALL(tbdd_relnext, set1, rel11, vars_next, dom_next);
    tbdd_refs_push(res11);
    TBDD res10 = tbdd_refs_sync(SYNC(tbdd_relnext));
    tbdd_refs_push(res10);
    TBDD res01 = tbdd_refs_sync(SYNC(tbdd_relnext));
    tbdd_refs_push(res01);
    TBDD res00 = tbdd_refs_sync(SYNC(tbdd_relnext));
    tbdd_refs_push(res00);

    /**
     * Now compute res0 and res1
     */
    tbdd_refs_spawn(SPAWN(tbdd_or, res00, res10, dom_next));
    TBDD res1 = CALL(tbdd_or, res01, res11, dom_next);
    tbdd_refs_push(res1);
    TBDD res0 = tbdd_refs_sync(SYNC(tbdd_or));
    tbdd_refs_pop(5);

    /**
     * Now compute final result
     */
    result = tbdd_makenode(var_s, res0, res1, dom_next_var);

    /**
     * And put the result in the cache
     */
    if (cache_put6(CACHE_TBDD_RELNEXT|set, rel, vars, dom, 0, 0, result, 0)) {
        sylvan_stats_count(TBDD_RELNEXT_CACHEDPUT);
    }

    if (newtag < dom_var) result = tbdd_makenode(newtag, result, tbdd_false, dom_var);
    return result;
}

/**
 * Compute number of variables in a set of variables / domain
 */
static int tbdd_set_count(TBDD dom)
{
    int res = 0;
    while (dom != tbdd_true) {
        res++;
        dom = tbddnode_high(dom, TBDD_GETNODE(dom));
    }
    return res;
}

TASK_IMPL_2(double, tbdd_satcount, TBDD, dd, TBDD, dom)
{
    /**
     * Handle False
     */
    if (dd == tbdd_false) return 0.0;

    /**
     * Handle no tag (True leaf)
     */
    uint32_t tag = TBDD_GETTAG(dd);
    if (tag == 0xfffff) {
        return powl(2.0L, tbdd_set_count(dom));
    }

    /**
     * Get domain variable
     */
    assert(dom != tbdd_true);
    tbddnode_t dom_node = TBDD_GETNODE(dom);
    uint32_t dom_var = tbddnode_getvariable(dom_node);

    /**
     * Count number of skipped nodes (BDD rule)
     */
    int skipped = 0;
    while (tag != dom_var) {
        skipped++;
        dom = tbddnode_high(dom, dom_node);
        assert(dom != tbdd_true);
        dom_node = TBDD_GETNODE(dom);
        dom_var = tbddnode_getvariable(dom_node);
    }

    /**
     * Handle True
     */
    if (TBDD_NOTAG(dd) == tbdd_true) {
        return powl(2.0, skipped);
    }

    /**
     * Perhaps execute garbage collection
     */
    sylvan_gc_test();

    /**
     * Count operation
     */
    sylvan_stats_count(TBDD_SATCOUNT);

    /**
     * Consult cache
     */
    union {
        double d;
        uint64_t s;
    } hack;

    const TBDD _dom = dom;
    if (cache_get3(CACHE_TBDD_SATCOUNT, dd, dom, 0, &hack.s)) {
        sylvan_stats_count(TBDD_SATCOUNT_CACHED);
        return hack.d * powl(2.0L, skipped);
    }

    /**
     * Get variable of dd
     */
    tbddnode_t dd_node = TBDD_GETNODE(dd);
    uint32_t dd_var = tbddnode_getvariable(dd_node);

    /**
     * Forward domain
     */
    while (dd_var != dom_var) {
        dom = tbddnode_high(dom, dom_node);
        assert(dom != tbdd_true);
        dom_node = TBDD_GETNODE(dom);
        dom_var = tbddnode_getvariable(dom_node);
    }

    SPAWN(tbdd_satcount, tbddnode_high(dd, dd_node), tbddnode_high(dom, dom_node));
    double result = CALL(tbdd_satcount, tbddnode_low(dd, dd_node), tbddnode_high(dom, dom_node));
    result += SYNC(tbdd_satcount);

    hack.d = result;
    if (cache_put3(CACHE_TBDD_SATCOUNT, dd, _dom, 0, hack.s)) {
        sylvan_stats_count(TBDD_SATCOUNT_CACHEDPUT);
    }

    return result * powl(2.0L, skipped);
}

TBDD tbdd_enum_first(TBDD dd, TBDD dom, uint8_t *arr)
{
    if (dd == tbdd_false) {
        return tbdd_false;
    } else if (dom == tbdd_true) {
        assert(dd == tbdd_true);
        return dd;
    } else {
        /**
         * Obtain domain variable
         */
        const tbddnode_t dom_node = TBDD_GETNODE(dom);
        const uint32_t dom_var = tbddnode_getvariable(dom_node);
        const TBDD dom_next = tbddnode_high(dom, dom_node);
        const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

        /**
         * Obtain cofactors
         */
        const tbddnode_t dd_node = TBDD_NOTAG(dd) == tbdd_true ? NULL : TBDD_GETNODE(dd);
        const uint32_t dd_var = dd_node == NULL ? 0xfffff : tbddnode_getvariable(dd_node);
        const uint32_t dd_tag = TBDD_GETTAG(dd);

        TBDD dd0, dd1;
        if (dom_var < dd_tag) {
            dd0 = dd1 = dd;
        } else if (dom_var < dd_var) {
            dd0 = tbdd_settag(dd, dom_next_var);
            dd1 = tbdd_false;
        } else {
            dd0 = tbddnode_low(dd, dd_node);
            dd1 = tbddnode_high(dd, dd_node);
        }

        /**
         * Try low first, else high, else return False
         */
        TBDD res = tbdd_enum_first(dd0, dom_next, arr+1);
        if (res != tbdd_false) {
            *arr = 0;
            return res;
        }

        res = tbdd_enum_first(dd1, dom_next, arr+1);
        if (res != tbdd_false) {
            *arr = 1;
            return res;
        }

        return tbdd_false;
    }
}

TBDD tbdd_enum_next(TBDD dd, TBDD dom, uint8_t *arr)
{
    if (dd == tbdd_false) {
        return tbdd_false;
    } else if (dom == tbdd_true) {
        assert(dd == tbdd_true);
        return tbdd_false;
    } else {
        /**
         * Obtain domain variable
         */
        const tbddnode_t dom_node = TBDD_GETNODE(dom);
        const uint32_t dom_var = tbddnode_getvariable(dom_node);
        const TBDD dom_next = tbddnode_high(dom, dom_node);
        const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

        /**
         * Obtain cofactors
         */
        const tbddnode_t dd_node = TBDD_NOTAG(dd) == tbdd_true ? NULL : TBDD_GETNODE(dd);
        const uint32_t dd_var = dd_node == NULL ? 0xfffff : tbddnode_getvariable(dd_node);
        const uint32_t dd_tag = TBDD_GETTAG(dd);

        TBDD dd0, dd1;
        if (dom_var < dd_tag) {
            dd0 = dd1 = dd;
        } else if (dom_var < dd_var) {
            dd0 = tbdd_settag(dd, dom_next_var);
            dd1 = tbdd_false;
        } else {
            dd0 = tbddnode_low(dd, dd_node);
            dd1 = tbddnode_high(dd, dd_node);
        }

        if (*arr == 0) {
            TBDD res = tbdd_enum_next(dd0, dom_next, arr+1);
            if (res == tbdd_false) {
                res = tbdd_enum_first(dd1, dom_next, arr+1);
                if (res != tbdd_false) *arr = 1;
            }
            return res;
        } else if (*arr == 1) {
            return tbdd_enum_next(dd1, dom_next, arr+1);
        } else {
            return tbdd_invalid;
        }
    }
}

VOID_TASK_5(tbdd_enum_do, TBDD, dd, TBDD, dom, tbdd_enum_cb, cb, void*, ctx, tbdd_trace_t, trace)
{
    if (dd == tbdd_false) {
        return;
    } else if (dom == tbdd_true) {
        /**
         * Compute trace length
         */
        size_t len = 0;
        tbdd_trace_t p = trace;
        while (p != NULL) {
            len++;
            p = p->prev;
        }
        /**
         * Fill array
         */
        uint8_t arr[len];
        for (size_t i=0; i<len; i++) {
            arr[len-i-1] = trace->val;
            trace = trace->prev;
        }
        /**
         * Call callback
         */
        WRAP(cb, ctx, arr, len);
    } else {
        /**
         * Obtain domain variable
         */
        const tbddnode_t dom_node = TBDD_GETNODE(dom);
        const uint32_t dom_var = tbddnode_getvariable(dom_node);
        const TBDD dom_next = tbddnode_high(dom, dom_node);
        const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

        /**
         * Obtain cofactors
         */
        const tbddnode_t dd_node = TBDD_NOTAG(dd) == tbdd_true ? NULL : TBDD_GETNODE(dd);
        const uint32_t dd_var = dd_node == NULL ? 0xfffff : tbddnode_getvariable(dd_node);
        const uint32_t dd_tag = TBDD_GETTAG(dd);

        TBDD dd0, dd1;
        if (dom_var < dd_tag) {
            dd0 = dd1 = dd;
        } else if (dom_var < dd_var) {
            dd0 = tbdd_settag(dd, dom_next_var);
            dd1 = tbdd_false;
        } else {
            dd0 = tbddnode_low(dd, dd_node);
            dd1 = tbddnode_high(dd, dd_node);
        }

        /**
         * Call recursive functions
         */
        struct tbdd_trace t0 = (struct tbdd_trace){trace, dom_var, 0};
        struct tbdd_trace t1 = (struct tbdd_trace){trace, dom_var, 1};
        SPAWN(tbdd_enum_do, dd0, dom_next, cb, ctx, &t0);
        CALL(tbdd_enum_do, dd1, dom_next, cb, ctx, &t1);
        SYNC(tbdd_enum_do);
    }
}

VOID_TASK_IMPL_4(tbdd_enum, TBDD, dd, TBDD, dom, tbdd_enum_cb, cb, void*, ctx)
{
    CALL(tbdd_enum_do, dd, dom, cb, ctx, NULL);
}

VOID_TASK_5(tbdd_enum_seq_do, TBDD, dd, TBDD, dom, tbdd_enum_cb, cb, void*, ctx, tbdd_trace_t, trace)
{
    if (dd == tbdd_false) {
        return;
    } else if (dom == tbdd_true) {
        /**
         * Compute trace length
         */
        size_t len = 0;
        tbdd_trace_t p = trace;
        while (p != NULL) {
            len++;
            p = p->prev;
        }
        /**
         * Fill array
         */
        uint8_t arr[len];
        for (size_t i=0; i<len; i++) {
            arr[len-i-1] = trace->val;
            trace = trace->prev;
        }
        /**
         * Call callback
         */
        WRAP(cb, ctx, arr, len);
    } else {
        /**
         * Obtain domain variable
         */
        const tbddnode_t dom_node = TBDD_GETNODE(dom);
        const uint32_t dom_var = tbddnode_getvariable(dom_node);
        const TBDD dom_next = tbddnode_high(dom, dom_node);
        const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

        /**
         * Obtain cofactors
         */
        const tbddnode_t dd_node = TBDD_NOTAG(dd) == tbdd_true ? NULL : TBDD_GETNODE(dd);
        const uint32_t dd_var = dd_node == NULL ? 0xfffff : tbddnode_getvariable(dd_node);
        const uint32_t dd_tag = TBDD_GETTAG(dd);

        TBDD dd0, dd1;
        if (dom_var < dd_tag) {
            dd0 = dd1 = dd;
        } else if (dom_var < dd_var) {
            dd0 = tbdd_settag(dd, dom_next_var);
            dd1 = tbdd_false;
        } else {
            dd0 = tbddnode_low(dd, dd_node);
            dd1 = tbddnode_high(dd, dd_node);
        }

        /**
         * Call recursive functions
         */
        struct tbdd_trace t0 = (struct tbdd_trace){trace, dom_var, 0};
        CALL(tbdd_enum_seq_do, dd0, dom_next, cb, ctx, &t0);
        struct tbdd_trace t1 = (struct tbdd_trace){trace, dom_var, 1};
        CALL(tbdd_enum_seq_do, dd1, dom_next, cb, ctx, &t1);
    }
}

VOID_TASK_IMPL_4(tbdd_enum_seq, TBDD, dd, TBDD, dom, tbdd_enum_cb, cb, void*, ctx)
{
    CALL(tbdd_enum_seq_do, dd, dom, cb, ctx, NULL);
}

TASK_6(TBDD, tbdd_collect_do, TBDD, dd, TBDD, dom, TBDD, res_dom, tbdd_collect_cb, cb, void*, ctx, tbdd_trace_t, trace)
{
    if (dd == tbdd_false) {
        return tbdd_false;
    } else if (dom == tbdd_true) {
        /**
         * Compute trace length
         */
        size_t len = 0;
        tbdd_trace_t p = trace;
        while (p != NULL) {
            len++;
            p = p->prev;
        }
        /**
         * Fill array
         */
        uint8_t arr[len];
        for (size_t i=0; i<len; i++) {
            arr[len-i-1] = trace->val;
            trace = trace->prev;
        }
        /**
         * Call callback
         */
        return WRAP(cb, ctx, arr, len);
    } else {
        /**
         * Obtain domain variable
         */
        const tbddnode_t dom_node = TBDD_GETNODE(dom);
        const uint32_t dom_var = tbddnode_getvariable(dom_node);
        const TBDD dom_next = tbddnode_high(dom, dom_node);
        const uint32_t dom_next_var = dom_next == tbdd_true ? 0xfffff : tbddnode_getvariable(TBDD_GETNODE(dom_next));

        /**
         * Obtain cofactors
         */
        const tbddnode_t dd_node = TBDD_NOTAG(dd) == tbdd_true ? NULL : TBDD_GETNODE(dd);
        const uint32_t dd_var = dd_node == NULL ? 0xfffff : tbddnode_getvariable(dd_node);
        const uint32_t dd_tag = TBDD_GETTAG(dd);

        TBDD dd0, dd1;
        if (dom_var < dd_tag) {
            dd0 = dd1 = dd;
        } else if (dom_var < dd_var) {
            dd0 = tbdd_settag(dd, dom_next_var);
            dd1 = tbdd_false;
        } else {
            dd0 = tbddnode_low(dd, dd_node);
            dd1 = tbddnode_high(dd, dd_node);
        }

        /**
         * Call recursive functions
         */
        struct tbdd_trace t0 = (struct tbdd_trace){trace, dom_var, 0};
        struct tbdd_trace t1 = (struct tbdd_trace){trace, dom_var, 1};
        tbdd_refs_spawn(SPAWN(tbdd_collect_do, dd0, dom_next, res_dom, cb, ctx, &t0));
        TBDD high = CALL(tbdd_collect_do, dd1, dom_next, res_dom, cb, ctx, &t1);
        tbdd_refs_push(high);
        TBDD low = tbdd_refs_sync(SYNC(tbdd_collect_do));
        tbdd_refs_push(low);
        TBDD res = tbdd_or(low, high, res_dom);
        tbdd_refs_pop(2);
        return res;
    }
}

TASK_IMPL_5(TBDD, tbdd_collect, TBDD, dd, TBDD, dom, TBDD, res_dom, tbdd_collect_cb, cb, void*, ctx)
{
    return CALL(tbdd_collect_do, dd, dom, res_dom, cb, ctx, NULL);
}

/**
 * Helper function for recursive unmarking
 */
static void
tbdd_unmark_rec(TBDD tbdd)
{
    if (TBDD_GETINDEX(tbdd) <= 1) return;
    tbddnode_t n = TBDD_GETNODE(tbdd);
    if (!tbddnode_getmark(n)) return;
    tbddnode_setmark(n, 0);
    tbdd_unmark_rec(tbddnode_getlow(n));
    tbdd_unmark_rec(tbddnode_gethigh(n));
}

/**
 * Count number of nodes in TBDD
 */

static size_t
tbdd_nodecount_mark(TBDD tbdd)
{
    if (TBDD_GETINDEX(tbdd) <= 1) return 0; // do not count true/false leaf
    tbddnode_t n = TBDD_GETNODE(tbdd);
    if (tbddnode_getmark(n)) return 0;
    tbddnode_setmark(n, 1);
    return 1 + tbdd_nodecount_mark(tbddnode_getlow(n)) + tbdd_nodecount_mark(tbddnode_gethigh(n));
}

size_t
tbdd_nodecount_more(const TBDD *tbdds, size_t count)
{
    size_t result = 0, i;
    for (i=0; i<count; i++) result += tbdd_nodecount_mark(tbdds[i]);
    for (i=0; i<count; i++) tbdd_unmark_rec(tbdds[i]);
    return result;
}

/**
 * Export to .dot file
 */
static inline int tag_to_label(TBDD tbdd)
{
    uint32_t tag = TBDD_GETTAG(tbdd);
    if (tag == 0xfffff) return -1;
    else return (int)tag;
}

static void
tbdd_fprintdot_rec(FILE *out, TBDD tbdd)
{
    tbddnode_t n = TBDD_GETNODE(tbdd); // also works for tbdd_false
    if (tbddnode_getmark(n)) return;
    tbddnode_setmark(n, 1);

    if (TBDD_GETINDEX(tbdd) == 0) {  // tbdd == tbdd_true || tbdd == tbdd_false
        fprintf(out, "0 [shape=box, style=filled, label=\"F\"];\n");
    } else if (TBDD_GETINDEX(tbdd) == 1) {  // tbdd == tbdd_true || tbdd == tbdd_false
        fprintf(out, "1 [shape=box, style=filled, label=\"T\"];\n");
    } else {
        fprintf(out, "%" PRIu64 " [label=\"%" PRIu32 "\\n%" PRIu64 "\"];\n",
                TBDD_GETINDEX(tbdd), tbddnode_getvariable(n), TBDD_GETINDEX(tbdd));

        tbdd_fprintdot_rec(out, tbddnode_getlow(n));
        tbdd_fprintdot_rec(out, tbddnode_gethigh(n));

        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=dashed, label=\" %d\"];\n",
                TBDD_GETINDEX(tbdd), TBDD_GETINDEX(tbddnode_getlow(n)),
                tag_to_label(tbddnode_getlow(n)));
        fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=solid dir=both arrowtail=%s, label=\" %d\"];\n",
                TBDD_GETINDEX(tbdd), TBDD_GETINDEX(tbddnode_gethigh(n)),
                tbddnode_getcomp(n) ? "dot" : "none", tag_to_label(tbddnode_gethigh(n)));
    }
}

void
tbdd_fprintdot(FILE *out, TBDD tbdd)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    fprintf(out, "root [style=invis];\n");
    fprintf(out, "root -> %" PRIu64 " [style=solid dir=both arrowtail=%s label=\" %d\"];\n",
            TBDD_GETINDEX(tbdd), TBDD_HASMARK(tbdd) ? "dot" : "none", tag_to_label(tbdd));

    tbdd_fprintdot_rec(out, tbdd);
    tbdd_unmark_rec(tbdd);

    fprintf(out, "}\n");
}

/**
 * Implementation of visitor operations
 */

VOID_TASK_IMPL_4(tbdd_visit_seq, TBDD, dd, tbdd_visit_pre_cb, pre_cb, tbdd_visit_post_cb, post_cb, void*, ctx)
{
    int children = 1;
    if (pre_cb != NULL) children = WRAP(pre_cb, dd, ctx);
    if (children && !tbdd_isleaf(dd)) {
        CALL(tbdd_visit_seq, tbdd_getlow(dd), pre_cb, post_cb, ctx);
        CALL(tbdd_visit_seq, tbdd_gethigh(dd), pre_cb, post_cb, ctx);
    }
    if (post_cb != NULL) WRAP(post_cb, dd, ctx);
}

VOID_TASK_IMPL_4(tbdd_visit_par, TBDD, dd, tbdd_visit_pre_cb, pre_cb, tbdd_visit_post_cb, post_cb, void*, ctx)
{
    int children = 1;
    if (pre_cb != NULL) children = WRAP(pre_cb, dd, ctx);
    if (children && !tbdd_isleaf(dd)) {
        SPAWN(tbdd_visit_par, tbdd_getlow(dd), pre_cb, post_cb, ctx);
        CALL(tbdd_visit_par, tbdd_gethigh(dd), pre_cb, post_cb, ctx);
        SYNC(tbdd_visit_par);
    }
    if (post_cb != NULL) WRAP(post_cb, dd, ctx);
}

/**
 * Writing TBDD files using a skiplist as a backend
 */

TASK_2(int, tbdd_writer_add_visitor_pre, TBDD, dd, sylvan_skiplist_t, sl)
{
    if (tbdd_isleaf(dd)) return 0;
    return sylvan_skiplist_get(sl, TBDD_GETINDEX(dd)) == 0 ? 1 : 0;
}

VOID_TASK_2(tbdd_writer_add_visitor_post, TBDD, dd, sylvan_skiplist_t, sl)
{
    if (TBDD_GETINDEX(dd) <= 1) return;
    sylvan_skiplist_assign_next(sl, TBDD_GETINDEX(dd));
}

sylvan_skiplist_t
tbdd_writer_start()
{
    size_t sl_size = nodes->table_size > 0x7fffffff ? 0x7fffffff : nodes->table_size;
    return sylvan_skiplist_alloc(sl_size);
}

VOID_TASK_IMPL_2(tbdd_writer_add, sylvan_skiplist_t, sl, TBDD, dd)
{
    tbdd_visit_seq(dd, (tbdd_visit_pre_cb)TASK(tbdd_writer_add_visitor_pre), (tbdd_visit_post_cb)TASK(tbdd_writer_add_visitor_post), (void*)sl);
}

void
tbdd_writer_writebinary(FILE *out, sylvan_skiplist_t sl)
{
    size_t nodecount = sylvan_skiplist_count(sl);
    fwrite(&nodecount, sizeof(size_t), 1, out);
    for (size_t i=1; i<=nodecount; i++) {
        TBDD dd = sylvan_skiplist_getr(sl, i);

        tbddnode_t n = TBDD_GETNODE(dd);
        struct tbddnode node;
        TBDD low = tbddnode_getlow(n);
        TBDD high = tbddnode_gethigh(n);
        if (TBDD_GETINDEX(low) > 1) low = TBDD_SETINDEX(low, sylvan_skiplist_get(sl, TBDD_GETINDEX(low)));
        if (TBDD_GETINDEX(high) > 1) high = TBDD_SETINDEX(high, sylvan_skiplist_get(sl, TBDD_GETINDEX(high)));
        tbddnode_makenode(&node, tbddnode_getvariable(n), low, high);
        fwrite(&node, sizeof(struct tbddnode), 1, out);
    }
}

uint64_t
tbdd_writer_get(sylvan_skiplist_t sl, TBDD dd)
{
    return TBDD_SETINDEX(dd, sylvan_skiplist_get(sl, TBDD_GETINDEX(dd)));
}

void
tbdd_writer_end(sylvan_skiplist_t sl)
{
    sylvan_skiplist_free(sl);
}

VOID_TASK_IMPL_3(tbdd_writer_tobinary, FILE *, out, TBDD *, dds, int, count)
{
    sylvan_skiplist_t sl = tbdd_writer_start();

    for (int i=0; i<count; i++) {
        CALL(tbdd_writer_add, sl, dds[i]);
    }

    tbdd_writer_writebinary(out, sl);

    fwrite(&count, sizeof(int), 1, out);

    for (int i=0; i<count; i++) {
        uint64_t v = tbdd_writer_get(sl, dds[i]);
        fwrite(&v, sizeof(uint64_t), 1, out);
    }

    tbdd_writer_end(sl);
}

void
tbdd_writer_writetext(FILE *out, sylvan_skiplist_t sl)
{
    fprintf(out, "[\n");
    size_t nodecount = sylvan_skiplist_count(sl);
    for (size_t i=1; i<=nodecount; i++) {
        TBDD dd = sylvan_skiplist_getr(sl, i);

        tbddnode_t n = TBDD_GETNODE(dd);
        TBDD low = tbddnode_getlow(n);
        TBDD high = tbddnode_gethigh(n);
        if (TBDD_GETINDEX(low) > 1) low = TBDD_SETINDEX(low, sylvan_skiplist_get(sl, TBDD_GETINDEX(low)));
        if (TBDD_GETINDEX(high) > 1) high = TBDD_SETINDEX(high, sylvan_skiplist_get(sl, TBDD_GETINDEX(high)));
        fprintf(out, "  node(%zu,%u,low(%u,%zu),%shigh(%u,%zu)),\n", i, tbddnode_getvariable(n), (uint32_t)TBDD_GETTAG(low), (size_t)TBDD_GETINDEX(low), TBDD_HASMARK(high)?"~":"", (uint32_t)TBDD_GETTAG(high), (size_t)TBDD_GETINDEX(high));
    }

    fprintf(out, "]");
}

VOID_TASK_IMPL_3(tbdd_writer_totext, FILE *, out, TBDD *, dds, int, count)
{
    sylvan_skiplist_t sl = tbdd_writer_start();

    for (int i=0; i<count; i++) {
        CALL(tbdd_writer_add, sl, dds[i]);
    }

    tbdd_writer_writetext(out, sl);

    fprintf(out, ",[");

    for (int i=0; i<count; i++) {
        uint64_t v = tbdd_writer_get(sl, dds[i]);
        fprintf(out, "%s%zu,", TBDD_HASMARK(v)?"~":"", (size_t)TBDD_STRIPMARK(v));
    }

    fprintf(out, "]\n");

    tbdd_writer_end(sl);
}

/**
 * Reading a file earlier written with tbdd_writer_writebinary
 * Returns an array with the conversion from stored identifier to TBDD
 * This array is allocated with malloc and must be freed afterwards.
 * This method does not support custom leaves.
 */
TASK_IMPL_1(uint64_t*, tbdd_reader_readbinary, FILE*, in)
{
    size_t nodecount;
    if (fread(&nodecount, sizeof(size_t), 1, in) != 1) {
        return NULL;
    }

    uint64_t *arr = malloc(sizeof(uint64_t)*(nodecount+1));
    arr[0] = 0;
    for (size_t i=1; i<=nodecount; i++) {
        struct tbddnode node;
        if (fread(&node, sizeof(struct tbddnode), 1, in) != 1) {
            free(arr);
            return NULL;
        }

        TBDD low = tbddnode_getlow(&node);
        TBDD high = tbddnode_gethigh(&node);
        if (TBDD_GETINDEX(low) > 0) low = TBDD_SETINDEX(low, arr[TBDD_GETINDEX(low)]);
        if (TBDD_GETINDEX(high) > 0) high = TBDD_SETINDEX(high, arr[TBDD_GETINDEX(high)]);
        if (low == high) {
            // we need to trick tbdd_makenode
            arr[i] = TBDD_SETTAG(tbdd_makenode(0, low, tbdd_false, tbddnode_getvariable(&node)), 0);
        } else {
            // nextvar does not matter
            arr[i] = TBDD_SETTAG(tbdd_makenode(tbddnode_getvariable(&node), low, high, 0xfffff), 0);
        }
    }

    return arr;
}

/**
 * Retrieve the TBDD of the given stored identifier.
 */
TBDD
tbdd_reader_get(uint64_t* arr, uint64_t identifier)
{
    return TBDD_SETINDEX(identifier, arr[TBDD_GETINDEX(identifier)]);
}

/**
 * Free the allocated translation array
 */
void
tbdd_reader_end(uint64_t *arr)
{
    free(arr);
}

/**
 * Reading a file earlier written with tbdd_writer_tobinary
 */
TASK_IMPL_3(int, tbdd_reader_frombinary, FILE*, in, TBDD*, dds, int, count)
{
    uint64_t *arr = CALL(tbdd_reader_readbinary, in);
    if (arr == NULL) return -1;

    /* Read stored count */
    int actual_count;
    if (fread(&actual_count, sizeof(int), 1, in) != 1) {
        tbdd_reader_end(arr);
        return -1;
    }

    /* If actual count does not agree with given count, abort */
    if (actual_count != count) {
        tbdd_reader_end(arr);
        return -1;
    }

    /* Read every stored identifier, and translate to TBDD */
    for (int i=0; i<count; i++) {
        uint64_t v;
        if (fread(&v, sizeof(uint64_t), 1, in) != 1) {
            tbdd_reader_end(arr);
            return -1;
        }
        dds[i] = tbdd_reader_get(arr, v);
    }

    tbdd_reader_end(arr);
    return 0;
}
