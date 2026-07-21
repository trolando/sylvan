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
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "avl.h"
#include "refs.h"
#include "sha2.h"

/**
 * Implementation of garbage collection
 */

/* Recursively mark LISTDD nodes as 'in use' */
void listdd_gc_mark_CALL(lace_worker* lace, LISTDD mdd)
{
    if (mdd <= listdd_empty_list) return;

    nodes_mark_rec_CALL(lace, nodes, mdd);
}

/**
 * External references
 */

refs_table_t listdd_refs;
refs_table_t listdd_protected;
static int listdd_protected_created = 0;

LISTDD
listdd_ref(LISTDD a)
{
    if (a == listdd_empty_list || a == listdd_empty) return a;
    refs_up(&listdd_refs, a);
    return a;
}

void
listdd_deref(LISTDD a)
{
    if (a == listdd_empty_list || a == listdd_empty) return;
    refs_down(&listdd_refs, a);
}

size_t
listdd_ref_count(void)
{
    return refs_count(&listdd_refs);
}

void
listdd_protect(LISTDD *a)
{
    if (!listdd_protected_created) {
        // In C++, sometimes listdd_protect is called before Sylvan is initialized. Just create a table.
        protect_create(&listdd_protected, 4096);
        listdd_protected_created = 1;
    }
    protect_up(&listdd_protected, (size_t)a);
}

void
listdd_unprotect(LISTDD *a)
{
    if (listdd_protected.refs_table != NULL) protect_down(&listdd_protected, (size_t)a);
}

size_t
listdd_protected_count(void)
{
    return protect_count(&listdd_protected);
}

/* Called during garbage collection */
void listdd_gc_mark_external_refs_CALL(lace_worker* lace)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = refs_iter(&listdd_refs, 0, listdd_refs.refs_size);
    while (it != NULL) {
        listdd_gc_mark_SPAWN(lace, refs_next(&listdd_refs, &it, listdd_refs.refs_size));
        count++;
    }
    while (count--) {
        listdd_gc_mark_SYNC(lace);
    }
}

void listdd_gc_mark_protected(lace_worker* lace)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = protect_iter(&listdd_protected, 0, listdd_protected.refs_size);
    while (it != NULL) {
        LISTDD *to_mark = (LISTDD*)protect_next(&listdd_protected, &it, listdd_protected.refs_size);
        listdd_gc_mark_SPAWN(lace, *to_mark);
        count++;
    }
    while (count--) {
        listdd_gc_mark_SYNC(lace);
    }
}

/* Infrastructure for internal markings */
typedef struct listdd_refs_task
{
    lace_task* t;
    void* f;
} *listdd_refs_task_t;

typedef struct listdd_refs_internal
{
    const LISTDD **pbegin, **pend, **pcur;
    LISTDD *rbegin, *rend, *rcur;
    listdd_refs_task_t sbegin, send, scur;
} *listdd_refs_internal_t;

SYLVAN_TLS listdd_refs_internal_t listdd_refs_key;

TASK(void, listdd_refs_mark_p_par, const LISTDD**, begin, size_t, count)

void listdd_refs_mark_p_par_CALL(lace_worker* lace, const LISTDD** begin, size_t count)
{
    if (count < 32) {
        while (count) {
            listdd_gc_mark_CALL(lace, **(begin++));
            count--;
        }
    } else {
        listdd_refs_mark_p_par_SPAWN(lace, begin, count / 2);
        listdd_refs_mark_p_par_CALL(lace, begin + (count / 2), count - count / 2);
        listdd_refs_mark_p_par_SYNC(lace);
    }
}

TASK(void, listdd_refs_mark_r_par, LISTDD*, begin, size_t, count)

void listdd_refs_mark_r_par_CALL(lace_worker* lace, LISTDD* begin, size_t count)
{
    if (count < 32) {
        while (count) {
            listdd_gc_mark_CALL(lace, *begin++);
            count--;
        }
    } else {
        listdd_refs_mark_r_par_SPAWN(lace, begin, count / 2);
        listdd_refs_mark_r_par_CALL(lace, begin + (count / 2), count - count / 2);
        listdd_refs_mark_r_par_SYNC(lace);
    }
}

TASK(void, listdd_refs_mark_s_par, listdd_refs_task_t, begin, size_t, count)

void listdd_refs_mark_s_par_CALL(lace_worker* lace, listdd_refs_task_t begin, size_t count)
{
    if (count < 32) {
        while (count) {
            lace_task* t = begin->t;
            if (!lace_is_stolen_task(t)) return;
            if (t->f == begin->f && lace_is_completed_task(t)) {
                listdd_gc_mark_CALL(lace, *(BDD*)lace_task_result(t));
            }
            begin += 1;
            count -= 1;
        }
    } else {
        if (!lace_is_stolen_task(begin->t)) return;
        listdd_refs_mark_s_par_SPAWN(lace, begin, count / 2);
        listdd_refs_mark_s_par_CALL(lace, begin + (count / 2), count - count / 2);
        listdd_refs_mark_s_par_SYNC(lace);
    }
}

TASK(void, listdd_refs_mark_task)

void listdd_refs_mark_task_CALL(lace_worker* lace)
{
    listdd_refs_mark_p_par_SPAWN(lace, listdd_refs_key->pbegin, (size_t)(listdd_refs_key->pcur-listdd_refs_key->pbegin));
    listdd_refs_mark_r_par_SPAWN(lace, listdd_refs_key->rbegin, (size_t)(listdd_refs_key->rcur-listdd_refs_key->rbegin));
    listdd_refs_mark_s_par_CALL(lace, listdd_refs_key->sbegin, (size_t)(listdd_refs_key->scur-listdd_refs_key->sbegin));
    listdd_refs_mark_r_par_SYNC(lace);
    listdd_refs_mark_p_par_SYNC(lace);
}

TASK(void, listdd_refs_mark)

void listdd_refs_mark_CALL(lace_worker* lace)
{
    listdd_refs_mark_task_TOGETHER();
}

void
listdd_refs_init_key(void)
{
    assert(lace_is_worker()); // only use inside Lace workers
    listdd_refs_internal_t s = (listdd_refs_internal_t)malloc(sizeof(struct listdd_refs_internal));
    s->pcur = s->pbegin = (const LISTDD**)malloc(sizeof(LISTDD*) * 1024);
    s->pend = s->pbegin + 1024;
    s->rcur = s->rbegin = (LISTDD*)malloc(sizeof(LISTDD) * 1024);
    s->rend = s->rbegin + 1024;
    s->scur = s->sbegin = (listdd_refs_task_t)malloc(sizeof(struct listdd_refs_task) * 1024);
    s->send = s->sbegin + 1024;
    listdd_refs_key = s;
}

TASK(void, listdd_refs_free)

void listdd_refs_free_CALL(lace_worker* lace)
{
    free(listdd_refs_key->pbegin);
    free(listdd_refs_key->rbegin);
    free(listdd_refs_key->sbegin);
    free(listdd_refs_key);
}

TASK(void, listdd_refs_init_task)
void listdd_refs_init_task_CALL(lace_worker* lace)
{
    listdd_refs_init_key();
}

TASK(void, listdd_refs_init)
void listdd_refs_init_CALL(lace_worker* lace)
{
    listdd_refs_init_task_TOGETHER();
    sylvan_gc_add_mark(listdd_refs_mark_CALL);
}

void
listdd_refs_ptrs_up(listdd_refs_internal_t refs)
{
    size_t size = (size_t)(refs->pend - refs->pbegin);
    refs->pbegin = (const LISTDD**)realloc(refs->pbegin, sizeof(LISTDD*) * size * 2);
    refs->pcur = refs->pbegin + size;
    refs->pend = refs->pbegin + (size * 2);
}

LISTDD SYLVAN_NOINLINE
listdd_refs_refs_up(listdd_refs_internal_t refs, LISTDD res)
{
    size_t size = (size_t)(refs->rend - refs->rbegin);
    refs->rbegin = (LISTDD*)realloc(refs->rbegin, sizeof(LISTDD) * size * 2);
    refs->rcur = refs->rbegin + size;
    refs->rend = refs->rbegin + (size * 2);
    return res;
}

void SYLVAN_NOINLINE
listdd_refs_tasks_up(listdd_refs_internal_t refs)
{
    size_t size = (size_t)(refs->send - refs->sbegin);
    refs->sbegin = (listdd_refs_task_t)realloc(refs->sbegin, sizeof(struct listdd_refs_task) * size * 2);
    refs->scur = refs->sbegin + size;
    refs->send = refs->sbegin + (size * 2);
}

void
listdd_refs_pushptr(const LISTDD *ptr)
{
    // If you get a segfault here (null dereference) then you're running this from outside Lace threads
    *listdd_refs_key->pcur++ = ptr;
    if (listdd_refs_key->pcur == listdd_refs_key->pend) listdd_refs_ptrs_up(listdd_refs_key);
}

void
listdd_refs_popptr(size_t amount)
{
    listdd_refs_key->pcur -= amount;
}

LISTDD
listdd_refs_push(LISTDD lddmc)
{
    // If you get a segfault here (null dereference) then you're running this from outside Lace threads
    *(listdd_refs_key->rcur++) = lddmc;
    if (listdd_refs_key->rcur == listdd_refs_key->rend) return listdd_refs_refs_up(listdd_refs_key, lddmc);
    else return lddmc;
}

void
listdd_refs_pop(long amount)
{
    listdd_refs_key->rcur -= amount;
}

void
listdd_refs_spawn(lace_task *t)
{
    listdd_refs_key->scur->t = t;
    listdd_refs_key->scur->f = t->f;
    listdd_refs_key->scur += 1;
    if (listdd_refs_key->scur == listdd_refs_key->send) listdd_refs_tasks_up(listdd_refs_key);
}

LISTDD
listdd_refs_sync(LISTDD result)
{
    listdd_refs_key->scur -= 1;
    return result;
}

TASK(void, listdd_gc_mark_serialize)

/**
 * Initialize and quit functions
 */

static void
listdd_quit(void)
{
    listdd_refs_free_TOGETHER();
    refs_free(&listdd_refs);

    if (listdd_protected_created) {
        protect_free(&listdd_protected);
        listdd_protected_created = 0;
    }
}

void
listdd_init(void)
{
    sylvan_register_quit(listdd_quit);
    sylvan_gc_add_mark(listdd_gc_mark_external_refs_CALL);
    sylvan_gc_add_mark(listdd_gc_mark_protected);
    sylvan_gc_add_mark(listdd_gc_mark_serialize_CALL);

    refs_create(&listdd_refs, 1024);
    if (!listdd_protected_created) {
        protect_create(&listdd_protected, 4096);
        listdd_protected_created = 1;
    }

    listdd_refs_init();
}

/**
 * Primitives
 */

LISTDD
listdd_make_node(uint32_t value, LISTDD ifeq, LISTDD ifneq)
{
    LISTDD result;
    if (_listdd_try_make_node(&result, value, ifeq, ifneq) != SYLVAN_OK) {
        fprintf(stderr, "LISTDD Unique table full, %zu of %zu buckets filled!\n", nodes_count_nodes(nodes), nodes_get_size(nodes));
        exit(1);
    }
    return result;
}

int
_listdd_try_make_node(LISTDD *destination, uint32_t value, LISTDD ifeq, LISTDD ifneq)
{
    if (destination == NULL || ifeq == listdd_invalid || ifneq == listdd_invalid) return SYLVAN_ERR_INVALID;
    if (ifeq == listdd_empty) { *destination = ifneq; return SYLVAN_OK; }

    // check if correct (should be false, or next in value)
    if (ifneq == listdd_empty_list) return SYLVAN_ERR_INVALID;
    if (ifneq != listdd_empty && value >= mddnode_getvalue(LDD_GETNODE(ifneq))) return SYLVAN_ERR_INVALID;

    struct mddnode n;
    mddnode_make(&n, value, ifneq, ifeq);

    int created;
    uint64_t index = nodes_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        listdd_refs_push(ifeq);
        listdd_refs_push(ifneq);
        sylvan_gc(); // FIXME can be just sylvan_gc_CALL?
        listdd_refs_pop(2);

        index = nodes_lookup(nodes, n.a, n.b, &created);
        if (index == 0) return SYLVAN_ERR_OOM;
    }

    if (created) sylvan_stats_count(LDD_NODES_CREATED);
    else sylvan_stats_count(LDD_NODES_REUSED);

    *destination = (LISTDD)index;
    return SYLVAN_OK;
}

LISTDD
listdd_make_copy_node(LISTDD ifeq, LISTDD ifneq)
{
    LISTDD result;
    if (_listdd_try_make_copy_node(&result, ifeq, ifneq) != SYLVAN_OK) {
        fprintf(stderr, "LISTDD Unique table full, %zu of %zu buckets filled!\n", nodes_count_nodes(nodes), nodes_get_size(nodes));
        exit(1);
    }
    return result;
}

int
_listdd_try_make_copy_node(LISTDD *destination, LISTDD ifeq, LISTDD ifneq)
{
    if (destination == NULL || ifeq == listdd_invalid || ifneq == listdd_invalid) return SYLVAN_ERR_INVALID;
    struct mddnode n;
    mddnode_makecopy(&n, ifneq, ifeq);

    int created;
    uint64_t index = nodes_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        listdd_refs_push(ifeq);
        listdd_refs_push(ifneq);
        sylvan_gc();
        listdd_refs_pop(2);

        index = nodes_lookup(nodes, n.a, n.b, &created);
        if (index == 0) return SYLVAN_ERR_OOM;
    }

    if (created) sylvan_stats_count(LDD_NODES_CREATED);
    else sylvan_stats_count(LDD_NODES_REUSED);

    *destination = (LISTDD)index;
    return SYLVAN_OK;
}

LISTDD
listdd_extend_node(LISTDD mdd, uint32_t value, LISTDD ifeq)
{
    if (mdd <= listdd_empty_list) return listdd_make_node(value, ifeq, mdd);

    mddnode* n = LDD_GETNODE(mdd);
    if (mddnode_getcopy(n)) return listdd_make_copy_node(mddnode_getdown(n), listdd_extend_node(mddnode_getright(n), value, ifeq));
    uint32_t n_value = mddnode_getvalue(n);
    if (n_value < value) return listdd_make_node(n_value, mddnode_getdown(n), listdd_extend_node(mddnode_getright(n), value, ifeq));
    if (n_value == value) return listdd_make_node(value, ifeq, mddnode_getright(n));
    /* (n_value > value) */ return listdd_make_node(value, ifeq, mdd);
}

uint32_t
listdd_node_value(LISTDD mdd)
{
    return mddnode_getvalue(LDD_GETNODE(mdd));
}

LISTDD
listdd_node_down(LISTDD mdd)
{
    return mddnode_getdown(LDD_GETNODE(mdd));
}

LISTDD
listdd_node_right(LISTDD mdd)
{
    return mddnode_getright(LDD_GETNODE(mdd));
}

LISTDD
listdd_follow(LISTDD mdd, uint32_t value)
{
    for (;;) {
        if (mdd <= listdd_empty_list) return mdd;
        const mddnode* n = LDD_GETNODE(mdd);
        if (!mddnode_getcopy(n)) {
            const uint32_t v = mddnode_getvalue(n);
            if (v == value) return mddnode_getdown(n);
            if (v > value) return listdd_empty;
        }
        mdd = mddnode_getright(n);
    }
}

int
listdd_is_copy_node(LISTDD mdd)
{
    if (mdd <= listdd_empty_list) return 0;

    mddnode* n = LDD_GETNODE(mdd);
    return mddnode_getcopy(n) ? 1 : 0;
}

LISTDD
listdd_follow_copy(LISTDD mdd)
{
    if (mdd <= listdd_empty_list) return listdd_empty;

    mddnode* n = LDD_GETNODE(mdd);
    if (mddnode_getcopy(n)) return mddnode_getdown(n);
    else return listdd_empty;
}

/**
 * LISTDD operations
 */
static inline int
match_ldds(LISTDD *one, LISTDD *two)
{
    LISTDD m1 = *one, m2 = *two;
    if (m1 == listdd_empty || m2 == listdd_empty) return 0;
    mddnode* n1 = LDD_GETNODE(m1);
    mddnode* n2 = LDD_GETNODE(m2);
    uint32_t v1 = mddnode_getvalue(n1), v2 = mddnode_getvalue(n2);
    while (v1 != v2) {
        if (v1 < v2) {
            m1 = mddnode_getright(n1);
            if (m1 == listdd_empty) return 0;
            n1 = LDD_GETNODE(m1);
            v1 = mddnode_getvalue(n1);
        } else if (v1 > v2) {
            m2 = mddnode_getright(n2);
            if (m2 == listdd_empty) return 0;
            n2 = LDD_GETNODE(m2);
            v2 = mddnode_getvalue(n2);
        }
    }
    *one = m1;
    *two = m2;
    return 1;
}

int listdd_union_CALL(lace_worker* lace, LISTDD *destination, LISTDD a, LISTDD b)
{
    if (destination == NULL || a == listdd_invalid || b == listdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminal cases */
    if (a == b) { *destination = a; return SYLVAN_OK; }
    if (a == listdd_empty) { *destination = b; return SYLVAN_OK; }
    if (b == listdd_empty) { *destination = a; return SYLVAN_OK; }
    if (a == listdd_empty_list || b == listdd_empty_list) return SYLVAN_ERR_INVALID;

    /* Test gc */
    sylvan_gc_test(lace);

    sylvan_stats_count(LDD_UNION);

    /* Improve cache behavior */
    if (a < b) { LISTDD tmp=b; b=a; a=tmp; }

    /* Access cache */
    LISTDD computed = listdd_invalid;
    listdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_MDD_UNION, a, b, 0, &computed)) {
        sylvan_stats_count(LDD_UNION_CACHED);
        *destination = computed;
        listdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Get nodes */
    mddnode* na = LDD_GETNODE(a);
    mddnode* nb = LDD_GETNODE(b);

    const int na_copy = mddnode_getcopy(na) ? 1 : 0;
    const int nb_copy = mddnode_getcopy(nb) ? 1 : 0;
    const uint32_t na_value = mddnode_getvalue(na);
    const uint32_t nb_value = mddnode_getvalue(nb);

    LISTDD down = listdd_invalid;
    LISTDD right = listdd_invalid;
    listdd_refs_pushptr(&down);
    listdd_refs_pushptr(&right);
    int status = SYLVAN_OK;

    /* Perform recursive calculation */
    if (na_copy && nb_copy) {
        listdd_union_SPAWN(lace, &down, mddnode_getdown(na), mddnode_getdown(nb));
        status = listdd_union_CALL(lace, &right, mddnode_getright(na), mddnode_getright(nb));
        int down_status = listdd_union_SYNC(lace);
        if (status == SYLVAN_OK) status = down_status;
        if (status == SYLVAN_OK) status = _listdd_try_make_copy_node(&computed, down, right);
    } else if (na_copy) {
        status = listdd_union_CALL(lace, &right, mddnode_getright(na), b);
        if (status == SYLVAN_OK) status = _listdd_try_make_copy_node(&computed, mddnode_getdown(na), right);
    } else if (nb_copy) {
        status = listdd_union_CALL(lace, &right, a, mddnode_getright(nb));
        if (status == SYLVAN_OK) status = _listdd_try_make_copy_node(&computed, mddnode_getdown(nb), right);
    } else if (na_value < nb_value) {
        status = listdd_union_CALL(lace, &right, mddnode_getright(na), b);
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, na_value, mddnode_getdown(na), right);
    } else if (na_value == nb_value) {
        listdd_union_SPAWN(lace, &down, mddnode_getdown(na), mddnode_getdown(nb));
        status = listdd_union_CALL(lace, &right, mddnode_getright(na), mddnode_getright(nb));
        int down_status = listdd_union_SYNC(lace);
        if (status == SYLVAN_OK) status = down_status;
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, na_value, down, right);
    } else /* na_value > nb_value */ {
        status = listdd_union_CALL(lace, &right, a, mddnode_getright(nb));
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, nb_value, mddnode_getdown(nb), right);
    }

    listdd_refs_popptr(2);
    if (status != SYLVAN_OK) {
        listdd_refs_popptr(1);
        return status;
    }

    /* Write to cache */
    if (cache_put3(CACHE_MDD_UNION, a, b, 0, computed)) sylvan_stats_count(LDD_UNION_CACHEDPUT);

    *destination = computed;
    listdd_refs_popptr(1);
    return SYLVAN_OK;
}

int listdd_diff_CALL(lace_worker* lace, LISTDD *destination, LISTDD a, LISTDD b)
{
    if (destination == NULL || a == listdd_invalid || b == listdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminal cases */
    if (a == b) { *destination = listdd_empty; return SYLVAN_OK; }
    if (a == listdd_empty) { *destination = listdd_empty; return SYLVAN_OK; }
    if (b == listdd_empty) { *destination = a; return SYLVAN_OK; }
    if (a == listdd_empty_list || b == listdd_empty_list) return SYLVAN_ERR_INVALID;

    /* Test gc */
    sylvan_gc_test(lace);

    sylvan_stats_count(LDD_MINUS);

    /* Access cache */
    LISTDD computed = listdd_invalid;
    listdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_MDD_MINUS, a, b, 0, &computed)) {
        sylvan_stats_count(LDD_MINUS_CACHED);
        *destination = computed;
        listdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Get nodes */
    mddnode* na = LDD_GETNODE(a);
    mddnode* nb = LDD_GETNODE(b);
    uint32_t na_value = mddnode_getvalue(na);
    uint32_t nb_value = mddnode_getvalue(nb);

    LISTDD down = listdd_invalid;
    LISTDD right = listdd_invalid;
    listdd_refs_pushptr(&down);
    listdd_refs_pushptr(&right);
    int status = SYLVAN_OK;

    /* Perform recursive calculation */
    if (na_value < nb_value) {
        status = listdd_diff_CALL(lace, &right, mddnode_getright(na), b);
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, na_value, mddnode_getdown(na), right);
    } else if (na_value == nb_value) {
        listdd_diff_SPAWN(lace, &right, mddnode_getright(na), mddnode_getright(nb));
        status = listdd_diff_CALL(lace, &down, mddnode_getdown(na), mddnode_getdown(nb));
        int right_status = listdd_diff_SYNC(lace);
        if (status == SYLVAN_OK) status = right_status;
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, na_value, down, right);
    } else /* na_value > nb_value */ {
        status = listdd_diff_CALL(lace, &computed, a, mddnode_getright(nb));
    }

    listdd_refs_popptr(2);
    if (status != SYLVAN_OK) {
        listdd_refs_popptr(1);
        return status;
    }

    /* Write to cache */
    if (cache_put3(CACHE_MDD_MINUS, a, b, 0, computed)) sylvan_stats_count(LDD_MINUS_CACHEDPUT);

    *destination = computed;
    listdd_refs_popptr(1);
    return SYLVAN_OK;
}

/* result: a plus b; difference: b minus a */
int listdd_union_diff_CALL(lace_worker* lace, LISTDD *destination, LISTDD *difference_destination, LISTDD a, LISTDD b)
{
    if (destination == NULL || difference_destination == NULL ||
        a == listdd_invalid || b == listdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Terminal cases */
    if (a == b) {
        *destination = a;
        *difference_destination = listdd_empty;
        return SYLVAN_OK;
    }
    if (a == listdd_empty) {
        *destination = b;
        *difference_destination = b;
        return SYLVAN_OK;
    }
    if (b == listdd_empty) {
        *destination = a;
        *difference_destination = listdd_empty;
        return SYLVAN_OK;
    }

    if (a == listdd_empty_list || b == listdd_empty_list) return SYLVAN_ERR_INVALID;

    /* Test gc */
    sylvan_gc_test(lace);

    /* Maybe not the ideal way */
    sylvan_stats_count(LDD_ZIP);

    /* Access cache */
    LISTDD computed = listdd_invalid;
    LISTDD difference = listdd_invalid;
    listdd_refs_pushptr(&computed);
    listdd_refs_pushptr(&difference);
    if (cache_get3(CACHE_MDD_UNION, a, b, 0, &computed) &&
        cache_get3(CACHE_MDD_MINUS, b, a, 0, &difference)) {
        sylvan_stats_count(LDD_ZIP);
        *destination = computed;
        *difference_destination = difference;
        listdd_refs_popptr(2);
        return SYLVAN_OK;
    }

    /* Get nodes */
    mddnode* na = LDD_GETNODE(a);
    mddnode* nb = LDD_GETNODE(b);
    uint32_t na_value = mddnode_getvalue(na);
    uint32_t nb_value = mddnode_getvalue(nb);

    LISTDD down = listdd_invalid;
    LISTDD down_difference = listdd_invalid;
    LISTDD right = listdd_invalid;
    LISTDD right_difference = listdd_invalid;
    listdd_refs_pushptr(&down);
    listdd_refs_pushptr(&down_difference);
    listdd_refs_pushptr(&right);
    listdd_refs_pushptr(&right_difference);
    int status = SYLVAN_OK;

    /* Perform recursive calculation */
    if (na_value < nb_value) {
        status = listdd_union_diff_CALL(lace, &right, &difference, mddnode_getright(na), b);
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, na_value, mddnode_getdown(na), right);
    } else if (na_value == nb_value) {
        listdd_union_diff_SPAWN(lace, &down, &down_difference, mddnode_getdown(na), mddnode_getdown(nb));
        status = listdd_union_diff_CALL(lace, &right, &right_difference, mddnode_getright(na), mddnode_getright(nb));
        int down_status = listdd_union_diff_SYNC(lace);
        if (status == SYLVAN_OK) status = down_status;
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, na_value, down, right);
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&difference, na_value, down_difference, right_difference);
    } else /* na_value > nb_value */ {
        status = listdd_union_diff_CALL(lace, &right, &right_difference, a, mddnode_getright(nb));
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, nb_value, mddnode_getdown(nb), right);
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&difference, nb_value, mddnode_getdown(nb), right_difference);
    }

    listdd_refs_popptr(4);
    if (status != SYLVAN_OK) {
        listdd_refs_popptr(2);
        return status;
    }

    /* Write to cache */
    int c1 = cache_put3(CACHE_MDD_UNION, a, b, 0, computed);
    int c2 = cache_put3(CACHE_MDD_MINUS, b, a, 0, difference);
    if (c1 && c2) sylvan_stats_count(LDD_ZIP_CACHEDPUT);

    *destination = computed;
    *difference_destination = difference;
    listdd_refs_popptr(2);
    return SYLVAN_OK;
}

int listdd_intersection_CALL(lace_worker* lace, LISTDD *destination, LISTDD a, LISTDD b)
{
    if (destination == NULL || a == listdd_invalid || b == listdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminal cases */
    if (a == b) { *destination = a; return SYLVAN_OK; }
    if (a == listdd_empty || b == listdd_empty) { *destination = listdd_empty; return SYLVAN_OK; }
    if (a == listdd_empty_list || b == listdd_empty_list) return SYLVAN_ERR_INVALID;

    /* Test gc */
    sylvan_gc_test(lace);

    sylvan_stats_count(LDD_INTERSECT);

    /* Get nodes */
    mddnode* na = LDD_GETNODE(a);
    mddnode* nb = LDD_GETNODE(b);
    uint32_t na_value = mddnode_getvalue(na);
    uint32_t nb_value = mddnode_getvalue(nb);

    /* Skip nodes if possible */
    while (na_value != nb_value) {
        if (na_value < nb_value) {
            a = mddnode_getright(na);
            if (a == listdd_empty) { *destination = listdd_empty; return SYLVAN_OK; }
            na = LDD_GETNODE(a);
            na_value = mddnode_getvalue(na);
        }
        if (nb_value < na_value) {
            b = mddnode_getright(nb);
            if (b == listdd_empty) { *destination = listdd_empty; return SYLVAN_OK; }
            nb = LDD_GETNODE(b);
            nb_value = mddnode_getvalue(nb);
        }
    }

    /* Access cache */
    LISTDD computed = listdd_invalid;
    listdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_MDD_INTERSECT, a, b, 0, &computed)) {
        sylvan_stats_count(LDD_INTERSECT_CACHED);
        *destination = computed;
        listdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Perform recursive calculation */
    LISTDD down = listdd_invalid;
    LISTDD right = listdd_invalid;
    listdd_refs_pushptr(&down);
    listdd_refs_pushptr(&right);
    listdd_intersection_SPAWN(lace, &right, mddnode_getright(na), mddnode_getright(nb));
    int status = listdd_intersection_CALL(lace, &down, mddnode_getdown(na), mddnode_getdown(nb));
    int right_status = listdd_intersection_SYNC(lace);
    if (status == SYLVAN_OK) status = right_status;
    if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, na_value, down, right);
    listdd_refs_popptr(2);
    if (status != SYLVAN_OK) {
        listdd_refs_popptr(1);
        return status;
    }

    /* Write to cache */
    if (cache_put3(CACHE_MDD_INTERSECT, a, b, 0, computed)) sylvan_stats_count(LDD_INTERSECT_CACHEDPUT);

    *destination = computed;
    listdd_refs_popptr(1);
    return SYLVAN_OK;
}

// proj: -1 (rest 0), 0 (no match), 1 (match)
int listdd_match_CALL(lace_worker* lace, LISTDD *destination, LISTDD a, LISTDD b, LISTDD proj)
{
    if (destination == NULL || a == listdd_invalid || b == listdd_invalid || proj == listdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }
    if (a == b) { *destination = a; return SYLVAN_OK; }
    if (a == listdd_empty || b == listdd_empty) { *destination = listdd_empty; return SYLVAN_OK; }
    if (proj <= listdd_empty_list) return SYLVAN_ERR_INVALID;

    mddnode* p_node = LDD_GETNODE(proj);
    uint32_t p_val = mddnode_getvalue(p_node);
    if (p_val == (uint32_t)-1) { *destination = a; return SYLVAN_OK; }

    if (a == listdd_empty_list || (p_val == 1 && b == listdd_empty_list)) return SYLVAN_ERR_INVALID;

    /* Test gc */
    sylvan_gc_test(lace);

    /* Skip nodes if possible */
    if (p_val == 1) {
        if (!match_ldds(&a, &b)) { *destination = listdd_empty; return SYLVAN_OK; }
    }

    sylvan_stats_count(LDD_MATCH);

    /* Access cache */
    LISTDD computed = listdd_invalid;
    listdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_MDD_MATCH, a, b, proj, &computed)) {
        sylvan_stats_count(LDD_MATCH_CACHED);
        *destination = computed;
        listdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Perform recursive calculation */
    mddnode* na = LDD_GETNODE(a);
    LISTDD down = listdd_invalid;
    LISTDD right = listdd_invalid;
    listdd_refs_pushptr(&down);
    listdd_refs_pushptr(&right);
    if (p_val == 1) {
        mddnode* nb = LDD_GETNODE(b);
        listdd_match_SPAWN(lace, &right, mddnode_getright(na), mddnode_getright(nb), proj);
        int status = listdd_match_CALL(lace, &down, mddnode_getdown(na), mddnode_getdown(nb), mddnode_getdown(p_node));
        int right_status = listdd_match_SYNC(lace);
        if (status == SYLVAN_OK) status = right_status;
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, mddnode_getvalue(na), down, right);
        listdd_refs_popptr(2);
        if (status != SYLVAN_OK) { listdd_refs_popptr(1); return status; }
    } else {
        listdd_match_SPAWN(lace, &right, mddnode_getright(na), b, proj);
        int status = listdd_match_CALL(lace, &down, mddnode_getdown(na), b, mddnode_getdown(p_node));
        int right_status = listdd_match_SYNC(lace);
        if (status == SYLVAN_OK) status = right_status;
        if (status == SYLVAN_OK) status = _listdd_try_make_node(&computed, mddnode_getvalue(na), down, right);
        listdd_refs_popptr(2);
        if (status != SYLVAN_OK) { listdd_refs_popptr(1); return status; }
    }

    /* Write to cache */
    if (cache_put3(CACHE_MDD_MATCH, a, b, proj, computed)) sylvan_stats_count(LDD_MATCH_CACHEDPUT);

    *destination = computed;
    listdd_refs_popptr(1);
    return SYLVAN_OK;
}

/* Transitional value adapters for ListDD tasks not converted yet. */
static LISTDD
listdd_union_value_CALL(lace_worker *lace, LISTDD a, LISTDD b)
{
    LISTDD result = listdd_invalid;
    listdd_refs_pushptr(&result);
    int status = listdd_union_CALL(lace, &result, a, b);
    listdd_refs_popptr(1);
    return status == SYLVAN_OK ? result : listdd_invalid;
}

static LISTDD
listdd_diff_value_CALL(lace_worker *lace, LISTDD a, LISTDD b)
{
    LISTDD result = listdd_invalid;
    listdd_refs_pushptr(&result);
    int status = listdd_diff_CALL(lace, &result, a, b);
    listdd_refs_popptr(1);
    return status == SYLVAN_OK ? result : listdd_invalid;
}

static LISTDD
listdd_intersection_value_CALL(lace_worker *lace, LISTDD a, LISTDD b)
{
    LISTDD result = listdd_invalid;
    listdd_refs_pushptr(&result);
    int status = listdd_intersection_CALL(lace, &result, a, b);
    listdd_refs_popptr(1);
    return status == SYLVAN_OK ? result : listdd_invalid;
}

#define listdd_union_CALL(lace, a, b) listdd_union_value_CALL((lace), (a), (b))
#define listdd_diff_CALL(lace, a, b) listdd_diff_value_CALL((lace), (a), (b))
#define listdd_intersection_CALL(lace, a, b) listdd_intersection_value_CALL((lace), (a), (b))

TASK(LISTDD, listdd_relprod_help, uint32_t, val, LISTDD, set, LISTDD, rel, LISTDD, proj)

LISTDD listdd_relprod_help_CALL(lace_worker* lace, uint32_t val, LISTDD set, LISTDD rel, LISTDD proj)
{
    return listdd_make_node(val, listdd_rel_next_CALL(lace, set, rel, proj), listdd_empty);
}

// meta: -1 (end; rest not in rel), 0 (not in rel), 1 (read), 2 (write), 3 (only-read), 4 (only-write), 5 (action label)
LISTDD listdd_rel_next_CALL(lace_worker* lace, LISTDD set, LISTDD rel, LISTDD meta)
{
    // for an empty set of source states, or an empty transition relation, return the empty set
    if (set == listdd_empty) return listdd_empty;
    if (rel == listdd_empty) return listdd_empty;
    if (meta == listdd_empty_list) return set; // we assume that if meta is finished, then the rest is not in rel

    mddnode* n_meta = LDD_GETNODE(meta);
    uint32_t m_val = mddnode_getvalue(n_meta);

    // if meta is -1, then no other variables are in the transition relation
    if (m_val == (uint32_t)-1) return set;

    // if meta is not 0, then both set and rel must be an internal LDD node
    if (m_val != 0 && m_val != 5) assert(set != listdd_empty_list && rel != listdd_empty_list);

    /* Skip nodes if possible */
    if (!mddnode_getcopy(LDD_GETNODE(rel))) {
        // if we "read" or "only-read", then match LDDs set and rel
        // if no match, then return the empty set
        if (m_val == 1 || m_val == 3) {
            if (!match_ldds(&set, &rel)) return listdd_empty;
        }
    }

    /* Test gc */
    sylvan_gc_test(lace);

    sylvan_stats_count(LDD_RELPROD);

    /* Access cache */
    LISTDD result;
    LISTDD _set=set, _rel=rel;
    if (cache_get3(CACHE_MDD_RELPROD, set, rel, meta, &result)) {
        sylvan_stats_count(LDD_RELPROD_CACHED);
        return result;
    }

    mddnode* n_set = LDD_GETNODE(set);
    mddnode* n_rel = LDD_GETNODE(rel);

    /* Recursive operations */
    if (m_val == 0) { // not in rel
        listdd_refs_spawn(listdd_rel_next_SPAWN(lace, mddnode_getright(n_set), rel, meta));
        LISTDD down = listdd_rel_next_CALL(lace, mddnode_getdown(n_set), rel, mddnode_getdown(n_meta));
        listdd_refs_push(down);
        LISTDD right = listdd_refs_sync(listdd_rel_next_SYNC(lace));
        listdd_refs_pop(1);
        result = listdd_make_node(mddnode_getvalue(n_set), down, right);
    } else if (m_val == 5) { // action label
        listdd_refs_spawn(listdd_rel_next_SPAWN(lace, set, mddnode_getright(n_rel), meta));
        LISTDD down = listdd_rel_next_CALL(lace, set, mddnode_getdown(n_rel), mddnode_getdown(n_meta));
        listdd_refs_push(down);
        LISTDD right = listdd_refs_sync(listdd_rel_next_SYNC(lace));
        listdd_refs_push(right);
        result = listdd_union_CALL(lace, down, right);
        listdd_refs_pop(2);
    } else if (m_val == 1) { // read
        // read layer: if not copy, then set&rel are already matched
        listdd_refs_spawn(listdd_rel_next_SPAWN(lace, set, mddnode_getright(n_rel), meta)); // spawn next read in list

        // for this read, either it is copy ('for all') or it is normal match
        if (mddnode_getcopy(n_rel)) {
            // spawn for every value to copy (set)
            int count = 0;
            for (;;) {
                // stay same level of set (for write)
                listdd_refs_spawn(listdd_rel_next_SPAWN(lace, set, mddnode_getdown(n_rel), mddnode_getdown(n_meta)));
                count++;
                set = mddnode_getright(n_set);
                if (set == listdd_empty) break;
                n_set = LDD_GETNODE(set);
            }

            // sync+union (one by one)
            result = listdd_empty;
            while (count--) {
                listdd_refs_push(result);
                LISTDD result2 = listdd_refs_sync(listdd_rel_next_SYNC(lace));
                listdd_refs_push(result2);
                result = listdd_union_CALL(lace, result, result2);
                listdd_refs_pop(2);
            }
        } else {
            // stay same level of set (for write)
            result = listdd_rel_next_CALL(lace, set, mddnode_getdown(n_rel), mddnode_getdown(n_meta));
        }

        listdd_refs_push(result);
        LISTDD result2 = listdd_refs_sync(listdd_rel_next_SYNC(lace)); // sync next read in list
        listdd_refs_push(result2);
        result = listdd_union_CALL(lace, result, result2);
        listdd_refs_pop(2);
    } else if (m_val == 3) { // only-read
        if (mddnode_getcopy(n_rel)) {
            // copy on read ('for any value')
            // result = union(result_with_copy, result_without_copy)
            listdd_refs_spawn(listdd_rel_next_SPAWN(lace, set, mddnode_getright(n_rel), meta)); // spawn without_copy

            // spawn for every value to copy (set)
            int count = 0;
            for (;;) {
                listdd_refs_spawn(listdd_relprod_help_SPAWN(lace, mddnode_getvalue(n_set), mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta)));
                count++;
                set = mddnode_getright(n_set);
                if (set == listdd_empty) break;
                n_set = LDD_GETNODE(set);
            }

            // sync+union (one by one)
            result = listdd_empty;
            while (count--) {
                listdd_refs_push(result);
                LISTDD result2 = listdd_refs_sync(listdd_relprod_help_SYNC(lace));
                listdd_refs_push(result2);
                result = listdd_union_CALL(lace, result, result2);
                listdd_refs_pop(2);
            }

            // add result from without_copy
            listdd_refs_push(result);
            LISTDD result2 = listdd_refs_sync(listdd_rel_next_SYNC(lace));
            listdd_refs_push(result2);
            result = listdd_union_CALL(lace, result, result2);
            listdd_refs_pop(2);
        } else {
            // only-read, without copy
            listdd_refs_spawn(listdd_rel_next_SPAWN(lace, mddnode_getright(n_set), mddnode_getright(n_rel), meta));
            LISTDD down = listdd_rel_next_CALL(lace, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta));
            listdd_refs_push(down);
            LISTDD right = listdd_refs_sync(listdd_rel_next_SYNC(lace));
            listdd_refs_pop(1);
            result = listdd_make_node(mddnode_getvalue(n_set), down, right);
        }
    } else if (m_val == 2 || m_val == 4) {
        // write, only-write
        if (m_val == 4) {
            // only-write, so we need to include 'for all variables'
            // the reason is that we did not have a read phase, so we need to 'insert' a read phase here
            listdd_refs_spawn(listdd_rel_next_SPAWN(lace, mddnode_getright(n_set), rel, meta)); // next in set
        }

        // if we're here and we are only-write, then we read the current value

        // spawn for every value to write (rel)
        int count = 0;
        for (;;) {
            uint32_t value;
            if (mddnode_getcopy(n_rel)) value = mddnode_getvalue(n_set);
            else value = mddnode_getvalue(n_rel);
            listdd_refs_spawn(listdd_relprod_help_SPAWN(lace, value, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta)));
            count++;
            rel = mddnode_getright(n_rel);
            if (rel == listdd_empty) break;
            n_rel = LDD_GETNODE(rel);
        }

        // sync+union (one by one)
        result = listdd_empty;
        while (count--) {
            listdd_refs_push(result);
            LISTDD result2 = listdd_refs_sync(listdd_relprod_help_SYNC(lace));
            listdd_refs_push(result2);
            result = listdd_union_CALL(lace, result, result2);
            listdd_refs_pop(2);
        }

        if (m_val == 4) {
            // sync+union with other variables
            listdd_refs_push(result);
            LISTDD result2 = listdd_refs_sync(listdd_rel_next_SYNC(lace));
            listdd_refs_push(result2);
            result = listdd_union_CALL(lace, result, result2);
            listdd_refs_pop(2);
        }
    }

    /* Write to cache */
    if (cache_put3(CACHE_MDD_RELPROD, _set, _rel, meta, result)) sylvan_stats_count(LDD_RELPROD_CACHEDPUT);

    return result;
}

TASK(LISTDD, listdd_relprod_union_help, uint32_t, val, LISTDD, set, LISTDD, rel, LISTDD, proj, LISTDD, un)

LISTDD listdd_relprod_union_help_CALL(lace_worker* lace, uint32_t val, LISTDD set, LISTDD rel, LISTDD proj, LISTDD un)
{
    return listdd_make_node(val, listdd_rel_next_union_CALL(lace, set, rel, proj, un), listdd_empty);
}

// meta: -1 (end; rest not in rel), 0 (not in rel), 1 (read), 2 (write), 3 (only-read), 4 (only-write)
LISTDD listdd_rel_next_union_CALL(lace_worker* lace, LISTDD set, LISTDD rel, LISTDD meta, LISTDD un)
{
    if (set == listdd_empty) return un;
    if (rel == listdd_empty) return un;
    if (un == listdd_empty) return listdd_rel_next_CALL(lace, set, rel, meta);
    if (meta == listdd_empty_list) return listdd_union_CALL(lace, set, un);

    mddnode* n_meta = LDD_GETNODE(meta);
    uint32_t m_val = mddnode_getvalue(n_meta);
    if (m_val == (uint32_t)-1) return listdd_union_CALL(lace, set, un);

    // check depths (this triggers on logic error)
    if (m_val != 0 && m_val != 5) assert(set != listdd_empty_list && rel != listdd_empty_list && un != listdd_empty_list);

    /* Skip nodes if possible */
    if (!mddnode_getcopy(LDD_GETNODE(rel))) {
        // if we "read" or "only-read", then match LDDs set and rel
        // if no match, then return un (the empty set union un)
        if (m_val == 1 || m_val == 3) {
            if (!match_ldds(&set, &rel)) return un;
        }
    }

    /* Test gc */
    sylvan_gc_test(lace);

    sylvan_stats_count(LDD_RELPROD_UNION);

    /* Access cache */
    LISTDD result;
    LISTDD _set=set, _rel=rel, _un=un;
    if (cache_get4(CACHE_MDD_RELPROD, set, rel, meta, un, &result)) {
        sylvan_stats_count(LDD_RELPROD_UNION_CACHED);
        return result;
    }

    /* Get nodes */
    mddnode* n_set = LDD_GETNODE(set);
    mddnode* n_rel = LDD_GETNODE(rel);
    mddnode* n_un = LDD_GETNODE(un);

    /* Now check the special cases where we can determine that un.value < result.value */
    if (m_val == 0 || m_val == 3) {
        // if m_val == 0, no read/write, then result.value = set.value
        // if m_val == 3, only read (write same regardless of copy), then result.value = set.value
        uint32_t set_value = mddnode_getvalue(n_set);
        uint32_t un_value = mddnode_getvalue(n_un);
        if (un_value < set_value) {
            LISTDD right = listdd_rel_next_union_CALL(lace, set, rel, meta, mddnode_getright(n_un));
            if (right == mddnode_getright(n_un)) return un;
            else return listdd_make_node(mddnode_getvalue(n_un), mddnode_getdown(n_un), right);
        }
    } else if (m_val == 2 || m_val == 4) {
        // if we write, then we only know for certain that un.value < result.value if
        // the root of rel is not a copy node
        if (!mddnode_getcopy(n_rel)) {
            uint32_t rel_value = mddnode_getvalue(n_rel);
            uint32_t un_value = mddnode_getvalue(n_un);
            if (un_value < rel_value) {
                LISTDD right = listdd_rel_next_union_CALL(lace, set, rel, meta, mddnode_getright(n_un));
                if (right == mddnode_getright(n_un)) return un;
                else return listdd_make_node(mddnode_getvalue(n_un), mddnode_getdown(n_un), right);
            }
        }
    }

    /* Recursive operations */
    if (m_val == 0) {
        // current <set> is not in the transition relation
        uint32_t set_value = mddnode_getvalue(n_set);
        uint32_t un_value = mddnode_getvalue(n_un);
        // set_value > un_value already checked above
        if (set_value < un_value) {
            listdd_refs_spawn(listdd_rel_next_union_SPAWN(lace, mddnode_getright(n_set), rel, meta, un));
            // going down, we don't need _union, since un does not contain this subtree
            LISTDD down = listdd_rel_next_CALL(lace, mddnode_getdown(n_set), rel, mddnode_getdown(n_meta));
            listdd_refs_push(down);
            LISTDD right = listdd_refs_sync(listdd_rel_next_union_SYNC(lace));
            listdd_refs_pop(1);
            result = listdd_make_node(mddnode_getvalue(n_set), down, right);
        } else /* set_value == un_value */ {
            assert(set_value == un_value);
            listdd_refs_spawn(listdd_rel_next_union_SPAWN(lace, mddnode_getright(n_set), rel, meta, mddnode_getright(n_un)));
            LISTDD down = listdd_rel_next_union_CALL(lace, mddnode_getdown(n_set), rel, mddnode_getdown(n_meta), mddnode_getdown(n_un));
            listdd_refs_push(down);
            LISTDD right = listdd_refs_sync(listdd_rel_next_union_SYNC(lace));
            listdd_refs_pop(1);
            if (right == mddnode_getright(n_un) && down == mddnode_getdown(n_un)) result = un;
            else result = listdd_make_node(mddnode_getvalue(n_set), down, right);
        }
    } else if (m_val == 5) {
        listdd_refs_spawn(listdd_rel_next_union_SPAWN(lace, set, mddnode_getright(n_rel), meta, un));
        LISTDD down = listdd_rel_next_union_CALL(lace, set, mddnode_getdown(n_rel), mddnode_getdown(n_meta), un);
        listdd_refs_push(down);
        LISTDD right = listdd_refs_sync(listdd_rel_next_union_SYNC(lace));
        listdd_refs_push(right);
        result = listdd_union_CALL(lace, down, right);
        listdd_refs_pop(2);
    } else if (m_val == 1) {
        // First we also spawn for the next read value, and merge results after
        listdd_refs_spawn(listdd_rel_next_union_SPAWN(lace, set, mddnode_getright(n_rel), meta, un));

        // for this read, either it is a copy read ('for all') or it is normal match
        if (mddnode_getcopy(n_rel)) {
            // spawn for every value in set (copy = for all)
            int count = 0;
            for (;;) {
                // stay same level of set and un (for write level, this was no only-read)
                listdd_refs_spawn(listdd_rel_next_union_SPAWN(lace, set, mddnode_getdown(n_rel), mddnode_getdown(n_meta), un));
                count++;
                set = mddnode_getright(n_set);
                if (set == listdd_empty) break;
                n_set = LDD_GETNODE(set);
            }

            // sync+union (one by one)
            result = listdd_empty;
            while (count--) {
                listdd_refs_push(result);
                LISTDD result2 = listdd_refs_sync(listdd_rel_next_union_SYNC(lace));
                listdd_refs_push(result2);
                result = listdd_union_CALL(lace, result, result2);
                listdd_refs_pop(2);
            }
        } else {
            // read level: if not copy read, then set and rel are already matched
            // stay same level of set and un (for write level, this was no only-read)
            result = listdd_rel_next_union_CALL(lace, set, mddnode_getdown(n_rel), mddnode_getdown(n_meta), un);
        }

        // now merge the result with the result from the next read value
        listdd_refs_push(result);
        LISTDD result2 = listdd_refs_sync(listdd_rel_next_union_SYNC(lace));
        listdd_refs_push(result2);
        result = listdd_union_CALL(lace, result, result2);
        listdd_refs_pop(2);
    } else if (m_val == 3) { // only-read
        // un < set already checked above
        if (mddnode_getcopy(n_rel)) {
            // copy on read ('for any value')
            // result = union(result_with_copy, result_without_copy)
            listdd_refs_spawn(listdd_rel_next_union_SPAWN(lace, set, mddnode_getright(n_rel), meta, un)); // spawn without_copy

            // spawn for every value to copy (iterate over set)
            int count = 0;
            //result = listdd_empty;
            for (;;) {
                uint32_t set_value = mddnode_getvalue(n_set);
                uint32_t un_value = mddnode_getvalue(n_un);
                if (un_value < set_value) {
                    // this is a bit tricky because the SYNC assumes we SPAWN a relprod_union_help
                    // the result of this will simply be "un_value, mddnode_getdown(n_un), false" which is intended
                    listdd_refs_spawn(listdd_relprod_union_help_SPAWN(lace, un_value, listdd_empty, listdd_empty, listdd_empty_list, mddnode_getdown(n_un)));
                    count++;
                    un = mddnode_getright(n_un);
                    if (un == listdd_empty) {
                        // if un is now false, then we have a normal relprod for the rest...
                        result = listdd_rel_next_CALL(lace, set, rel, meta);
                        break;
                    }
                    n_un = LDD_GETNODE(un);
                } else if (un_value > set_value) {
                    // this is a bit tricky because the SYNC assumes we SPAWN a relprod_union_help
                    // the result of this will simply be a normal relprod
                    listdd_refs_spawn(listdd_relprod_union_help_SPAWN(lace, set_value, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), listdd_empty));
                    count++;
                    set = mddnode_getright(n_set);
                    if (set == listdd_empty) {
                        // if set is now false, then the tail result to merge with is un
                        result = un;
                        break;
                    }
                    n_set = LDD_GETNODE(set);
                } else /* un_value == set_value */ {
                    listdd_refs_spawn(listdd_relprod_union_help_SPAWN(lace, set_value, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), mddnode_getdown(n_un)));
                    count++;
                    set = mddnode_getright(n_set);
                    un = mddnode_getright(n_un);
                    if (set == listdd_empty) {
                        // if set is now false, then the tail result to merge with is un
                        result = un;
                        break;
                    } else if (un == listdd_empty) {
                        // if un is now false, then we have a normal relprod for the rest...
                        result = listdd_rel_next_CALL(lace, set, rel, meta);
                        break;
                    }
                    n_set = LDD_GETNODE(set);
                    n_un = LDD_GETNODE(un);
                }
            }

            // sync+union (one by one)
            while (count--) {
                listdd_refs_push(result);
                LISTDD result2 = listdd_refs_sync(listdd_relprod_union_help_SYNC(lace));
                listdd_refs_push(result2);
                result = listdd_union_CALL(lace, result, result2);
                listdd_refs_pop(2);
            }

            // add result from without_copy
            listdd_refs_push(result);
            LISTDD result2 = listdd_refs_sync(listdd_rel_next_union_SYNC(lace));
            listdd_refs_push(result2);
            result = listdd_union_CALL(lace, result, result2);
            listdd_refs_pop(2);
        } else {
            // only-read, not a copy node
            uint32_t set_value = mddnode_getvalue(n_set);
            uint32_t un_value = mddnode_getvalue(n_un);

            // we already checked un_value < set_value
            if (un_value > set_value) {
                listdd_refs_spawn(listdd_rel_next_union_SPAWN(lace, mddnode_getright(n_set), mddnode_getright(n_rel), meta, un));
                LISTDD down = listdd_rel_next_CALL(lace, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta));
                listdd_refs_push(down);
                LISTDD right = listdd_refs_sync(listdd_rel_next_union_SYNC(lace));
                listdd_refs_pop(1);
                result = listdd_make_node(set_value, down, right);
            } else /* un_value == set_value */ {
                assert(un_value == set_value);
                listdd_refs_spawn(listdd_rel_next_union_SPAWN(lace, mddnode_getright(n_set), mddnode_getright(n_rel), meta, mddnode_getright(n_un)));
                LISTDD down = listdd_rel_next_union_CALL(lace, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), mddnode_getdown(n_un));
                listdd_refs_push(down);
                LISTDD right = listdd_refs_sync(listdd_rel_next_union_SYNC(lace));
                listdd_refs_pop(1);
                result = listdd_make_node(set_value, down, right);
            }
        }
    } else if (m_val == 2 || m_val == 4) { // write, only-write
        if (m_val == 4) {
            // only-write, so we need to include 'for all variables' because we did not 'read'
            listdd_refs_spawn(listdd_rel_next_union_SPAWN(lace, mddnode_getright(n_set), rel, meta, un)); // next in set
        }

        // spawn for every value to write (rel)
        int count = 0;
        for (;;) {
            uint32_t value;
            // we write 'set' if copy node, or 'rel' otherwise
            if (mddnode_getcopy(n_rel)) value = mddnode_getvalue(n_set);
            else value = mddnode_getvalue(n_rel);
            uint32_t un_value = mddnode_getvalue(n_un);
            if (un_value < value) {
                // the result of this will simply be "un_value, mddnode_getdown(n_un), false" which is intended
                listdd_refs_spawn(listdd_relprod_union_help_SPAWN(lace, un_value, listdd_empty, listdd_empty, listdd_empty_list, mddnode_getdown(n_un)));
                count++;
                un = mddnode_getright(n_un);
                if (un == listdd_empty) {
                    result = listdd_rel_next_CALL(lace, set, rel, meta);
                    break;
                }
                n_un = LDD_GETNODE(un);
            } else if (un_value > value) {
                listdd_refs_spawn(listdd_relprod_union_help_SPAWN(lace, value, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), listdd_empty));
                count++;
                rel = mddnode_getright(n_rel);
                if (rel == listdd_empty) {
                    result = un;
                    break;
                }
                n_rel = LDD_GETNODE(rel);
            } else /* un_value == value */ {
                listdd_refs_spawn(listdd_relprod_union_help_SPAWN(lace, value, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), mddnode_getdown(n_un)));
                count++;
                rel = mddnode_getright(n_rel);
                un = mddnode_getright(n_un);
                if (rel == listdd_empty) {
                    result = un;
                    break;
                } else if (un == listdd_empty) {
                    result = listdd_rel_next_CALL(lace, set, rel, meta);
                    break;
                }
                n_rel = LDD_GETNODE(rel);
                n_un = LDD_GETNODE(un);
            }
        }

        // sync+union (one by one)
        while (count--) {
            listdd_refs_push(result);
            LISTDD result2 = listdd_refs_sync(listdd_relprod_union_help_SYNC(lace));
            listdd_refs_push(result2);
            result = listdd_union_CALL(lace, result, result2);
            listdd_refs_pop(2);
        }

        if (m_val == 4) {
            // sync+union with other variables
            listdd_refs_push(result);
            LISTDD result2 = listdd_refs_sync(listdd_rel_next_union_SYNC(lace));
            listdd_refs_push(result2);
            result = listdd_union_CALL(lace, result, result2);
            listdd_refs_pop(2);
        }
    }

    /* Write to cache */
    if (cache_put4(CACHE_MDD_RELPROD, _set, _rel, meta, _un, result)) sylvan_stats_count(LDD_RELPROD_UNION_CACHEDPUT);

    return result;
}

TASK(LISTDD, listdd_relprev_help, uint32_t, val, LISTDD, set, LISTDD, rel, LISTDD, proj, LISTDD, uni)

LISTDD listdd_relprev_help_CALL(lace_worker* lace, uint32_t val, LISTDD set, LISTDD rel, LISTDD proj, LISTDD uni)
{
    return listdd_make_node(val, listdd_rel_prev_CALL(lace, set, rel, proj, uni), listdd_empty);
}

/**
 * Calculate all predecessors to a in uni according to rel[meta]
 * <meta> follows the same semantics as relprod
 * i.e. 0 (not in rel), 1 (read), 2 (write), 3 (only-read), 4 (only-write), -1 (end; rest=0), 5 (action label)
 */
LISTDD listdd_rel_prev_CALL(lace_worker* lace, LISTDD set, LISTDD rel, LISTDD meta, LISTDD uni)
{
    if (set == listdd_empty) return listdd_empty;
    if (rel == listdd_empty) return listdd_empty;
    if (uni == listdd_empty) return listdd_empty;

    mddnode* n_meta = LDD_GETNODE(meta);
    uint32_t m_val = mddnode_getvalue(n_meta);
    if (m_val == (uint32_t)-1) {
        if (set == uni) return set;
        else return listdd_intersection_value_CALL(lace, set, uni);
    }

    if (m_val != 0 && m_val != 5) assert(set != listdd_empty_list && rel != listdd_empty_list && uni != listdd_empty_list);

    /* Skip nodes if possible */
    if (m_val == 0) {
        // not in rel: match set and uni ('intersect')
        if (!match_ldds(&set, &uni)) return listdd_empty;
    } else if (mddnode_getcopy(LDD_GETNODE(rel))) {
        // read+copy: no matching (pre is everything in uni)
        // write+copy: no matching (match after split: set and uni)
        // only-read+copy: match set and uni
        // only-write+copy: no matching (match after split: set and uni)
        if (m_val == 3) {
            if (!match_ldds(&set, &uni)) return listdd_empty;
        }
    } else if (m_val == 1) {
        // read: match uni and rel
        if (!match_ldds(&uni, &rel)) return listdd_empty;
    } else if (m_val == 2) {
        // write: match set and rel
        if (!match_ldds(&set, &rel)) return listdd_empty;
    } else if (m_val == 3) {
        // only-read: match uni and set and rel
        mddnode* n_set = LDD_GETNODE(set);
        mddnode* n_rel = LDD_GETNODE(rel);
        mddnode* n_uni = LDD_GETNODE(uni);
        uint32_t n_set_value = mddnode_getvalue(n_set);
        uint32_t n_rel_value = mddnode_getvalue(n_rel);
        uint32_t n_uni_value = mddnode_getvalue(n_uni);
        while (n_uni_value != n_rel_value || n_rel_value != n_set_value) {
            if (n_uni_value < n_rel_value || n_uni_value < n_set_value) {
                uni = mddnode_getright(n_uni);
                if (uni == listdd_empty) return listdd_empty;
                n_uni = LDD_GETNODE(uni);
                n_uni_value = mddnode_getvalue(n_uni);
            }
            if (n_set_value < n_rel_value || n_set_value < n_uni_value) {
                set = mddnode_getright(n_set);
                if (set == listdd_empty) return listdd_empty;
                n_set = LDD_GETNODE(set);
                n_set_value = mddnode_getvalue(n_set);
            }
            if (n_rel_value < n_set_value || n_rel_value < n_uni_value) {
                rel = mddnode_getright(n_rel);
                if (rel == listdd_empty) return listdd_empty;
                n_rel = LDD_GETNODE(rel);
                n_rel_value = mddnode_getvalue(n_rel);
            }
        }
    } else if (m_val == 4) {
        // only-write: match set and rel (then use whole universe)
        if (!match_ldds(&set, &rel)) return listdd_empty;
    }

    /* Test gc */
    sylvan_gc_test(lace);

    sylvan_stats_count(LDD_RELPREV);

    /* Access cache */
    LISTDD result;
    LISTDD _set=set, _rel=rel, _uni=uni;
    if (cache_get4(CACHE_MDD_RELPREV, set, rel, meta, uni, &result)) {
        sylvan_stats_count(LDD_RELPREV_CACHED);
        return result;
    }

    mddnode* n_set = LDD_GETNODE(set);
    mddnode* n_rel = LDD_GETNODE(rel);
    mddnode* n_uni = LDD_GETNODE(uni);

    /* Recursive operations */
    if (m_val == 0) { // not in rel
        // m_val == 0 : not in rel (intersection set and universe)
        listdd_refs_spawn(listdd_rel_prev_SPAWN(lace, mddnode_getright(n_set), rel, meta, mddnode_getright(n_uni)));
        LISTDD down = listdd_rel_prev_CALL(lace, mddnode_getdown(n_set), rel, mddnode_getdown(n_meta), mddnode_getdown(n_uni));
        listdd_refs_push(down);
        LISTDD right = listdd_refs_sync(listdd_rel_prev_SYNC(lace));
        listdd_refs_pop(1);
        result = listdd_make_node(mddnode_getvalue(n_set), down, right);
    } else if (m_val == 5) {
        listdd_refs_spawn(listdd_rel_prev_SPAWN(lace, set, mddnode_getright(n_rel), meta, uni));
        LISTDD down = listdd_rel_prev_CALL(lace, set, mddnode_getdown(n_rel), mddnode_getdown(n_meta), uni);
        listdd_refs_push(down);
        LISTDD right = listdd_refs_sync(listdd_rel_prev_SYNC(lace));
        listdd_refs_push(right);
        result = listdd_union_CALL(lace, down, right);
        listdd_refs_pop(2);
    } else if (m_val == 1) { // read level
        // result value is in case of copy: everything in uni!
        // result value is in case of not-copy: match uni and rel!
        listdd_refs_spawn(listdd_rel_prev_SPAWN(lace, set, mddnode_getright(n_rel), meta, uni)); // next in rel
        if (mddnode_getcopy(n_rel)) {
            // result is everything in uni
            // spawn for every value to have been read (uni)
            int count = 0;
            for (;;) {
                listdd_refs_spawn(listdd_relprev_help_SPAWN(lace, mddnode_getvalue(n_uni), set, mddnode_getdown(n_rel), mddnode_getdown(n_meta), uni));
                count++;
                uni = mddnode_getright(n_uni);
                if (uni == listdd_empty) break;
                n_uni = LDD_GETNODE(uni);
            }

            // sync+union (one by one)
            result = listdd_empty;
            while (count--) {
                listdd_refs_push(result);
                LISTDD result2 = listdd_refs_sync(listdd_relprev_help_SYNC(lace));
                listdd_refs_push(result2);
                result = listdd_union_CALL(lace, result, result2);
                listdd_refs_pop(2);
            }
        } else {
            // already matched
            LISTDD down = listdd_rel_prev_CALL(lace, set, mddnode_getdown(n_rel), mddnode_getdown(n_meta), uni);
            result = listdd_make_node(mddnode_getvalue(n_uni), down, listdd_empty);
        }
        listdd_refs_push(result);
        LISTDD result2 = listdd_refs_sync(listdd_rel_prev_SYNC(lace));
        listdd_refs_push(result2);
        result = listdd_union_CALL(lace, result, result2);
        listdd_refs_pop(2);
    } else if (m_val == 3) { // only-read level
        // result value is in case of copy: match set and uni! (already done first match)
        // result value is in case of not-copy: match set and uni and rel!
        listdd_refs_spawn(listdd_rel_prev_SPAWN(lace, set, mddnode_getright(n_rel), meta, uni)); // next in rel
        if (mddnode_getcopy(n_rel)) {
            // spawn for every matching set+uni
            int count = 0;
            for (;;) {
                listdd_refs_spawn(listdd_relprev_help_SPAWN(lace, mddnode_getvalue(n_uni), mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), mddnode_getdown(n_uni)));
                count++;
                uni = mddnode_getright(n_uni);
                if (!match_ldds(&set, &uni)) break;
                n_set = LDD_GETNODE(set);
                n_uni = LDD_GETNODE(uni);
            }

            // sync+union (one by one)
            result = listdd_empty;
            while (count--) {
                listdd_refs_push(result);
                LISTDD result2 = listdd_refs_sync(listdd_relprev_help_SYNC(lace));
                listdd_refs_push(result2);
                result = listdd_union_CALL(lace, result, result2);
                listdd_refs_pop(2);
            }
        } else {
            // already matched
            LISTDD down = listdd_rel_prev_CALL(lace, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), mddnode_getdown(n_uni));
            result = listdd_make_node(mddnode_getvalue(n_uni), down, listdd_empty);
        }
        listdd_refs_push(result);
        LISTDD result2 = listdd_refs_sync(listdd_rel_prev_SYNC(lace));
        listdd_refs_push(result2);
        result = listdd_union_CALL(lace, result, result2);
        listdd_refs_pop(2);
    } else if (m_val == 2) { // write level
        // note: the read level has already matched the uni that was read.
        // write+copy: only for the one set equal to uni...
        // write: match set and rel (already done)
        listdd_refs_spawn(listdd_rel_prev_SPAWN(lace, set, mddnode_getright(n_rel), meta, uni));
        if (mddnode_getcopy(n_rel)) {
            LISTDD down = listdd_follow(set, mddnode_getvalue(n_uni));
            if (down != listdd_empty) {
                result = listdd_rel_prev_CALL(lace, down, mddnode_getdown(n_rel), mddnode_getdown(n_meta), mddnode_getdown(n_uni));
            } else {
                result = listdd_empty;
            }
        } else {
            result = listdd_rel_prev_CALL(lace, mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), mddnode_getdown(n_uni));
        }
        listdd_refs_push(result);
        LISTDD result2 = listdd_refs_sync(listdd_rel_prev_SYNC(lace));
        listdd_refs_push(result2);
        result = listdd_union_CALL(lace, result, result2);
        listdd_refs_pop(2);
    } else if (m_val == 4) { // only-write level
        // only-write+copy: match set and uni after spawn
        // only-write: match set and rel (already done)
        listdd_refs_spawn(listdd_rel_prev_SPAWN(lace, set, mddnode_getright(n_rel), meta, uni));
        if (mddnode_getcopy(n_rel)) {
            // spawn for every matching set+uni
            int count = 0;
            for (;;) {
                if (!match_ldds(&set, &uni)) break;
                n_set = LDD_GETNODE(set);
                n_uni = LDD_GETNODE(uni);
                listdd_refs_spawn(listdd_relprev_help_SPAWN(lace, mddnode_getvalue(n_uni), mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), mddnode_getdown(n_uni)));
                count++;
                uni = mddnode_getright(n_uni);
            }

            // sync+union (one by one)
            result = listdd_empty;
            while (count--) {
                listdd_refs_push(result);
                LISTDD result2 = listdd_refs_sync(listdd_relprev_help_SYNC(lace));
                listdd_refs_push(result2);
                result = listdd_union_CALL(lace, result, result2);
                listdd_refs_pop(2);
            }
        } else {
            // spawn for every value in universe!!
            int count = 0;
            for (;;) {
                listdd_refs_spawn(listdd_relprev_help_SPAWN(lace, mddnode_getvalue(n_uni), mddnode_getdown(n_set), mddnode_getdown(n_rel), mddnode_getdown(n_meta), mddnode_getdown(n_uni)));
                count++;
                uni = mddnode_getright(n_uni);
                if (uni == listdd_empty) break;
                n_uni = LDD_GETNODE(uni);
            }

            // sync+union (one by one)
            result = listdd_empty;
            while (count--) {
                listdd_refs_push(result);
                LISTDD result2 = listdd_refs_sync(listdd_relprev_help_SYNC(lace));
                listdd_refs_push(result2);
                result = listdd_union_CALL(lace, result, result2);
                listdd_refs_pop(2);
            }
        }
        listdd_refs_push(result);
        LISTDD result2 = listdd_refs_sync(listdd_rel_prev_SYNC(lace));
        listdd_refs_push(result2);
        result = listdd_union_CALL(lace, result, result2);
        listdd_refs_pop(2);
    }

    /* Write to cache */
    if (cache_put4(CACHE_MDD_RELPREV, _set, _rel, meta, _uni, result)) sylvan_stats_count(LDD_RELPREV_CACHEDPUT);

    return result;
}

// Same 'proj' as project. So: proj: -2 (end; quantify rest), -1 (end; keep rest), 0 (quantify), 1 (keep)
LISTDD listdd_join_CALL(lace_worker* lace, LISTDD a, LISTDD b, LISTDD a_proj, LISTDD b_proj)
{
    if (a == listdd_empty || b == listdd_empty) return listdd_empty;

    /* Test gc */
    sylvan_gc_test(lace);

    mddnode* n_a_proj = LDD_GETNODE(a_proj);
    mddnode* n_b_proj = LDD_GETNODE(b_proj);
    uint32_t a_proj_val = mddnode_getvalue(n_a_proj);
    uint32_t b_proj_val = mddnode_getvalue(n_b_proj);

    while (a_proj_val == 0 && b_proj_val == 0) {
        a_proj = mddnode_getdown(n_a_proj);
        b_proj = mddnode_getdown(n_b_proj);
        n_a_proj = LDD_GETNODE(a_proj);
        n_b_proj = LDD_GETNODE(b_proj);
        a_proj_val = mddnode_getvalue(n_a_proj);
        b_proj_val = mddnode_getvalue(n_b_proj);
    }

    if (a_proj_val == (uint32_t)-2) return b; // no a left
    if (b_proj_val == (uint32_t)-2) return a; // no b left
    if (a_proj_val == (uint32_t)-1 && b_proj_val == (uint32_t)-1) return listdd_intersection_CALL(lace, a, b);

    // At this point, only proj_val {-1, 0, 1}; max one with -1; max one with 0.
    const int keep_a = a_proj_val != 0;
    const int keep_b = b_proj_val != 0;

    if (keep_a && keep_b) {
        // If both 'keep', then match values
        if (!match_ldds(&a, &b)) return listdd_empty;
    }

    sylvan_stats_count(LDD_JOIN);

    /* Access cache */
    LISTDD result;
    if (cache_get4(CACHE_MDD_JOIN, a, b, a_proj, b_proj, &result)) {
        sylvan_stats_count(LDD_JOIN_CACHED);
        return result;
    }

    /* Perform recursive calculation */
    const mddnode* na = LDD_GETNODE(a);
    const mddnode* nb = LDD_GETNODE(b);
    uint32_t val;
    LISTDD down;

    // Make copies (for cache)
    LISTDD _a_proj = a_proj, _b_proj = b_proj;
    if (keep_a) {
        if (keep_b) {
            val = mddnode_getvalue(nb);
            listdd_refs_spawn(listdd_join_SPAWN(lace, mddnode_getright(na), mddnode_getright(nb), a_proj, b_proj));
            if (a_proj_val != (uint32_t)-1) a_proj = mddnode_getdown(n_a_proj);
            if (b_proj_val != (uint32_t)-1) b_proj = mddnode_getdown(n_b_proj);
            down = listdd_join_CALL(lace, mddnode_getdown(na), mddnode_getdown(nb), a_proj, b_proj);
        } else {
            val = mddnode_getvalue(na);
            listdd_refs_spawn(listdd_join_SPAWN(lace, mddnode_getright(na), b, a_proj, b_proj));
            if (a_proj_val != (uint32_t)-1) a_proj = mddnode_getdown(n_a_proj);
            if (b_proj_val != (uint32_t)-1) b_proj = mddnode_getdown(n_b_proj);
            down = listdd_join_CALL(lace, mddnode_getdown(na), b, a_proj, b_proj);
        }
    } else {
        val = mddnode_getvalue(nb);
        listdd_refs_spawn(listdd_join_SPAWN(lace, a, mddnode_getright(nb), a_proj, b_proj));
        if (a_proj_val != (uint32_t)-1) a_proj = mddnode_getdown(n_a_proj);
        if (b_proj_val != (uint32_t)-1) b_proj = mddnode_getdown(n_b_proj);
        down = listdd_join_CALL(lace, a, mddnode_getdown(nb), a_proj, b_proj);
    }

    listdd_refs_push(down);
    LISTDD right = listdd_refs_sync(listdd_join_SYNC(lace));
    listdd_refs_pop(1);
    result = listdd_make_node(val, down, right);

    /* Write to cache */
    if (cache_put4(CACHE_MDD_JOIN, a, b, _a_proj, _b_proj, result)) sylvan_stats_count(LDD_JOIN_CACHEDPUT);

    return result;
}

// so: proj: -2 (end; quantify rest), -1 (end; keep rest), 0 (quantify), 1 (keep)
LISTDD listdd_project_CALL(lace_worker* lace, const LISTDD mdd, const LISTDD proj)
{
    if (mdd == listdd_empty) return listdd_empty; // projection of empty is empty
    if (mdd == listdd_empty_list) return listdd_empty_list; // projection of universe is universe...

    mddnode* p_node = LDD_GETNODE(proj);
    uint32_t p_val = mddnode_getvalue(p_node);
    if (p_val == (uint32_t)-1) return mdd;
    if (p_val == (uint32_t)-2) return listdd_empty_list; // because we always end with true.

    sylvan_gc_test(lace);

    sylvan_stats_count(LDD_PROJECT);

    LISTDD result;
    if (cache_get3(CACHE_MDD_PROJECT, mdd, proj, 0, &result)) {
        sylvan_stats_count(LDD_PROJECT_CACHED);
        return result;
    }

    mddnode* n = LDD_GETNODE(mdd);

    if (p_val == 1) { // keep
        listdd_refs_spawn(listdd_project_SPAWN(lace, mddnode_getright(n), proj));
        LISTDD down = listdd_project_CALL(lace, mddnode_getdown(n), mddnode_getdown(p_node));
        listdd_refs_push(down);
        LISTDD right = listdd_refs_sync(listdd_project_SYNC(lace));
        listdd_refs_pop(1);
        result = listdd_make_node(mddnode_getvalue(n), down, right);
    } else { // quantify
        if (mddnode_getdown(n) == listdd_empty_list) { // assume lowest level
            result = listdd_empty_list;
        } else {
            int count = 0;
            LISTDD p_down = mddnode_getdown(p_node), _mdd=mdd;
            while (1) {
                listdd_refs_spawn(listdd_project_SPAWN(lace, mddnode_getdown(n), p_down));
                count++;
                _mdd = mddnode_getright(n);
                assert(_mdd != listdd_empty_list);
                if (_mdd == listdd_empty) break;
                n = LDD_GETNODE(_mdd);
            }
            result = listdd_empty;
            while (count--) {
                listdd_refs_push(result);
                LISTDD down = listdd_refs_sync(listdd_project_SYNC(lace));
                listdd_refs_push(down);
                result = listdd_union_CALL(lace, result, down);
                listdd_refs_pop(2);
            }
        }
    }

    if (cache_put3(CACHE_MDD_PROJECT, mdd, proj, 0, result)) sylvan_stats_count(LDD_PROJECT_CACHEDPUT);

    return result;
}

// so: proj: -2 (end; quantify rest), -1 (end; keep rest), 0 (quantify), 1 (keep)
LISTDD listdd_project_diff_CALL(lace_worker* lace, const LISTDD mdd, const LISTDD proj, LISTDD avoid)
{
    // This implementation assumed "avoid" has correct depth
    if (avoid == listdd_empty_list) return listdd_empty;
    if (mdd == avoid) return listdd_empty;
    if (mdd == listdd_empty) return listdd_empty; // projection of empty is empty
    if (mdd == listdd_empty_list) return listdd_empty_list; // avoid != listdd_empty_list

    mddnode* p_node = LDD_GETNODE(proj);
    uint32_t p_val = mddnode_getvalue(p_node);
    if (p_val == (uint32_t)-1) return listdd_diff_value_CALL(lace, mdd, avoid);
    if (p_val == (uint32_t)-2) return listdd_empty_list;

    sylvan_gc_test(lace);

    sylvan_stats_count(LDD_PROJECT_MINUS);

    LISTDD result;
    if (cache_get3(CACHE_MDD_PROJECT, mdd, proj, avoid, &result)) {
        sylvan_stats_count(LDD_PROJECT_MINUS_CACHED);
        return result;
    }

    mddnode* n = LDD_GETNODE(mdd);

    if (p_val == 1) { // keep
        // move 'avoid' until it matches
        uint32_t val = mddnode_getvalue(n);
        LISTDD a_down = listdd_empty;
        while (avoid != listdd_empty) {
            mddnode* a_node = LDD_GETNODE(avoid);
            uint32_t a_val = mddnode_getvalue(a_node);
            if (a_val > val) {
                break;
            } else if (a_val == val) {
                a_down = mddnode_getdown(a_node);
                break;
            }
            avoid = mddnode_getright(a_node);
        }
        listdd_refs_spawn(listdd_project_diff_SPAWN(lace, mddnode_getright(n), proj, avoid));
        LISTDD down = listdd_project_diff_CALL(lace, mddnode_getdown(n), mddnode_getdown(p_node), a_down);
        listdd_refs_push(down);
        LISTDD right = listdd_refs_sync(listdd_project_diff_SYNC(lace));
        listdd_refs_pop(1);
        result = listdd_make_node(val, down, right);
    } else { // quantify
        if (mddnode_getdown(n) == listdd_empty_list) { // assume lowest level
            result = listdd_empty_list;
        } else {
            int count = 0;
            LISTDD p_down = mddnode_getdown(p_node), _mdd=mdd;
            while (1) {
                listdd_refs_spawn(listdd_project_diff_SPAWN(lace, mddnode_getdown(n), p_down, avoid));
                count++;
                _mdd = mddnode_getright(n);
                assert(_mdd != listdd_empty_list);
                if (_mdd == listdd_empty) break;
                n = LDD_GETNODE(_mdd);
            }
            result = listdd_empty;
            while (count--) {
                listdd_refs_push(result);
                LISTDD down = listdd_refs_sync(listdd_project_diff_SYNC(lace));
                listdd_refs_push(down);
                result = listdd_union_CALL(lace, result, down);
                listdd_refs_pop(2);
            }
        }
    }

    if (cache_put3(CACHE_MDD_PROJECT, mdd, proj, avoid, result)) sylvan_stats_count(LDD_PROJECT_MINUS_CACHEDPUT);

    return result;
}

LISTDD
listdd_add(LISTDD a, uint32_t* values, size_t count)
{
    if (a == listdd_empty) return listdd_singleton(values, count);
    if (a == listdd_empty_list) {
        assert(count == 0);
        return listdd_empty_list;
    }
    assert(count != 0);

    mddnode* na = LDD_GETNODE(a);
    uint32_t na_value = mddnode_getvalue(na);

    /* Only create a new node if something actually changed */

    if (na_value < *values) {
        LISTDD right = listdd_add(mddnode_getright(na), values, count);
        if (right == mddnode_getright(na)) return a; // no actual change
        return listdd_make_node(na_value, mddnode_getdown(na), right);
    } else if (na_value == *values) {
        LISTDD down = listdd_add(mddnode_getdown(na), values+1, count-1);
        if (down == mddnode_getdown(na)) return a; // no actual change
        return listdd_make_node(na_value, down, mddnode_getright(na));
    } else /* na_value > *values */ {
        return listdd_make_node(*values, listdd_singleton(values+1, count-1), a);
    }
}

LISTDD
listdd_relation_add(LISTDD a, uint32_t* values, int* copy, size_t count)
{
    if (a == listdd_empty) return listdd_relation_singleton(values, copy, count);
    if (a == listdd_empty_list) {
        assert(count == 0);
        return listdd_empty_list;
    }
    assert(count != 0);

    mddnode* na = LDD_GETNODE(a);

    /* Only create a new node if something actually changed */

    int na_copy = mddnode_getcopy(na);
    if (na_copy && *copy) {
        LISTDD down = listdd_relation_add(mddnode_getdown(na), values+1, copy+1, count-1);
        if (down == mddnode_getdown(na)) return a; // no actual change
        return listdd_make_copy_node(down, mddnode_getright(na));
    } else if (na_copy) {
        LISTDD right = listdd_relation_add(mddnode_getright(na), values, copy, count);
        if (right == mddnode_getright(na)) return a; // no actual change
        return listdd_make_copy_node(mddnode_getdown(na), right);
    } else if (*copy) {
        return listdd_make_copy_node(listdd_relation_singleton(values+1, copy+1, count-1), a);
    }

    uint32_t na_value = mddnode_getvalue(na);
    if (na_value < *values) {
        LISTDD right = listdd_relation_add(mddnode_getright(na), values, copy, count);
        if (right == mddnode_getright(na)) return a; // no actual change
        return listdd_make_node(na_value, mddnode_getdown(na), right);
    } else if (na_value == *values) {
        LISTDD down = listdd_relation_add(mddnode_getdown(na), values+1, copy+1, count-1);
        if (down == mddnode_getdown(na)) return a; // no actual change
        return listdd_make_node(na_value, down, mddnode_getright(na));
    } else /* na_value > *values */ {
        return listdd_make_node(*values, listdd_relation_singleton(values+1, copy+1, count-1), a);
    }
}

int
listdd_contains(LISTDD a, uint32_t* values, size_t count)
{
    while (1) {
        if (a == listdd_empty) return 0;
        if (a == listdd_empty_list) return 1;
        if (count <= 0) assert(count > 0); // size mismatch

        a = listdd_follow(a, *values);
        values++;
        count--;
    }
}

int
listdd_relation_contains(LISTDD a, uint32_t* values, int* copy, size_t count)
{
    while (1) {
        if (a == listdd_empty) return 0;
        if (a == listdd_empty_list) return 1;
        if (count <= 0) assert(count > 0); // size mismatch

        if (*copy) a = listdd_follow_copy(a);
        else a = listdd_follow(a, *values);
        values++;
        count--;
    }
}

LISTDD
listdd_singleton(uint32_t* values, size_t count)
{
    if (count == 0) return listdd_empty_list;
    return listdd_make_node(*values, listdd_singleton(values+1, count-1), listdd_empty);
}

LISTDD
listdd_relation_singleton(uint32_t* values, int* copy, size_t count)
{
    if (count == 0) return listdd_empty_list;
    if (*copy) return listdd_make_copy_node(listdd_relation_singleton(values+1, copy+1, count-1), listdd_empty);
    else return listdd_make_node(*values, listdd_relation_singleton(values+1, copy+1, count-1), listdd_empty);
}

/**
 * Count number of nodes for each level
 */

static void
listdd_nodecount_levels_mark(LISTDD mdd, size_t *variables)
{
    if (mdd <= listdd_empty_list) return;
    mddnode* n = LDD_GETNODE(mdd);
    if (!mddnode_getmark(n)) {
        mddnode_setmark(n, 1);
        (*variables) += 1;
        listdd_nodecount_levels_mark(mddnode_getright(n), variables);
        listdd_nodecount_levels_mark(mddnode_getdown(n), variables+1);
    }
}

static void
listdd_nodecount_levels_unmark(LISTDD mdd)
{
    if (mdd <= listdd_empty_list) return;
    mddnode* n = LDD_GETNODE(mdd);
    if (mddnode_getmark(n)) {
        mddnode_setmark(n, 0);
        listdd_nodecount_levels_unmark(mddnode_getright(n));
        listdd_nodecount_levels_unmark(mddnode_getdown(n));
    }
}

void
listdd_node_count_per_level(LISTDD mdd, size_t *variables)
{
    listdd_nodecount_levels_mark(mdd, variables);
    listdd_nodecount_levels_unmark(mdd);
}

/**
 * Count number of nodes in LISTDD
 */

static size_t
listdd_nodecount_mark(LISTDD mdd)
{
    if (mdd <= listdd_empty_list) return 0;
    mddnode* n = LDD_GETNODE(mdd);
    if (mddnode_getmark(n)) return 0;
    mddnode_setmark(n, 1);
    return 1 + listdd_nodecount_mark(mddnode_getdown(n)) + listdd_nodecount_mark(mddnode_getright(n));
}

static void
listdd_nodecount_unmark(LISTDD mdd)
{
    if (mdd <= listdd_empty_list) return;
    mddnode* n = LDD_GETNODE(mdd);
    if (mddnode_getmark(n)) {
        mddnode_setmark(n, 0);
        listdd_nodecount_unmark(mddnode_getright(n));
        listdd_nodecount_unmark(mddnode_getdown(n));
    }
}

size_t
listdd_node_count(LISTDD mdd)
{
    size_t result = listdd_nodecount_mark(mdd);
    listdd_nodecount_unmark(mdd);
    return result;
}

/**
 * CALCULATE NUMBER OF VAR ASSIGNMENTS THAT YIELD TRUE
 */

long double listdd_count_CALL(lace_worker* lace, LISTDD mdd)
{
    if (mdd == listdd_empty) return 0.0;
    if (mdd == listdd_empty_list) return 1.0;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    sylvan_stats_count(LDD_SATCOUNTL);

    union {
        long double d;
        struct {
            uint64_t s1;
            uint64_t s2;
        } s;
    } hack = {0};

    if (cache_get3(CACHE_MDD_SATCOUNTL1, mdd, 0, 0, &hack.s.s1) &&
        cache_get3(CACHE_MDD_SATCOUNTL2, mdd, 0, 0, &hack.s.s2)) {
        sylvan_stats_count(LDD_SATCOUNTL_CACHED);
        return hack.d;
    }

    mddnode* n = LDD_GETNODE(mdd);

    listdd_count_SPAWN(lace, mddnode_getdown(n));
    long double right = listdd_count_CALL(lace, mddnode_getright(n));
    hack.d = right + listdd_count_SYNC(lace);

    int c1 = cache_put3(CACHE_MDD_SATCOUNTL1, mdd, 0, 0, hack.s.s1);
    int c2 = cache_put3(CACHE_MDD_SATCOUNTL2, mdd, 0, 0, hack.s.s2);
    if (c1 && c2) sylvan_stats_count(LDD_SATCOUNTL_CACHEDPUT);

    return hack.d;
}

LISTDD listdd_map_reduce_union_CALL(lace_worker* lace, LISTDD mdd, listdd_map_reduce_union_cb cb, void* context, uint32_t* values, size_t count)
{
    if (mdd == listdd_empty) return listdd_empty;
    if (mdd == listdd_empty_list) {
        return cb(values, count, context);
    }

    mddnode* n = LDD_GETNODE(mdd);

    listdd_refs_spawn(listdd_map_reduce_union_SPAWN(lace, mddnode_getright(n), cb, context, values, count));

    void *scratch = lace_scratch_mark(lace);
    uint32_t *newvalues = LACE_SCRATCH_ARRAY(lace, uint32_t, count+1);
    if (count > 0) memcpy(newvalues, values, sizeof(uint32_t)*count);
    newvalues[count] = mddnode_getvalue(n);
    LISTDD down = listdd_map_reduce_union_CALL(lace, mddnode_getdown(n), cb, context, newvalues, count+1);
    lace_scratch_reset(lace, scratch);

    if (down == listdd_empty) {
        LISTDD result = listdd_refs_sync(listdd_map_reduce_union_SYNC(lace));
        return result;
    }

    listdd_refs_push(down);
    LISTDD right = listdd_refs_sync(listdd_map_reduce_union_SYNC(lace));

    if (right == listdd_empty) {
        listdd_refs_pop(1);
        return down;
    } else {
        listdd_refs_push(right);
        LISTDD result = listdd_union_CALL(lace, down, right);
        listdd_refs_pop(2);
        return result;
    }
}

TASK(void, _lddmc_sat_all_nopar, LISTDD, mdd, listdd_enum_cb, cb, void*, context, uint32_t*, values, size_t, count)

void _lddmc_sat_all_nopar_CALL(lace_worker* lace, LISTDD mdd, listdd_enum_cb cb, void* context, uint32_t* values, size_t count)
{
    if (mdd == listdd_empty) return;
    if (mdd == listdd_empty_list) {
        cb(values, count, context);
        return;
    }

    mddnode* n = LDD_GETNODE(mdd);
    values[count] = mddnode_getvalue(n);
    _lddmc_sat_all_nopar_CALL(lace, mddnode_getdown(n), cb, context, values, count+1);
    _lddmc_sat_all_nopar_CALL(lace, mddnode_getright(n), cb, context, values, count);
}

void listdd_enumerate_CALL(lace_worker* lace, LISTDD mdd, listdd_enum_cb cb, void* context)
{
    // determine depth
    size_t count=0;
    LISTDD _mdd = mdd;
    while (_mdd > listdd_empty_list) {
        _mdd = mddnode_getdown(LDD_GETNODE(_mdd));
        assert(_mdd != listdd_empty);
        count++;
    }

    void *scratch = lace_scratch_mark(lace);
    uint32_t *values = count == 0 ? NULL : LACE_SCRATCH_ARRAY(lace, uint32_t, count);
    _lddmc_sat_all_nopar_CALL(lace, mdd, cb, context, values, 0);
    lace_scratch_reset(lace, scratch);
}

void listdd_enumerate_parallel_CALL(lace_worker* lace, LISTDD mdd, listdd_enum_cb cb, void* context, uint32_t* values, size_t count)
{
    if (mdd == listdd_empty) return;
    if (mdd == listdd_empty_list) {
        cb(values, count, context);
        return;
    }

    mddnode* n = LDD_GETNODE(mdd);

    listdd_enumerate_parallel_SPAWN(lace, mddnode_getright(n), cb, context, values, count);

    void *scratch = lace_scratch_mark(lace);
    uint32_t *newvalues = LACE_SCRATCH_ARRAY(lace, uint32_t, count+1);
    if (count > 0) memcpy(newvalues, values, sizeof(uint32_t)*count);
    newvalues[count] = mddnode_getvalue(n);
    listdd_enumerate_parallel_CALL(lace, mddnode_getdown(n), cb, context, newvalues, count+1);
    lace_scratch_reset(lace, scratch);

    listdd_enumerate_parallel_SYNC(lace);
}

struct listdd_match_sat_info
{
    LISTDD mdd;
    LISTDD match;
    LISTDD proj;
    size_t count;
    uint32_t *values;
};

// proj: -1 (rest 0), 0 (no match), 1 (match)
TASK(void, listdd_match_sat, struct listdd_match_sat_info *, info, listdd_enum_cb, cb, void*, context)
void listdd_match_sat_CALL(lace_worker* lace, struct listdd_match_sat_info * info, listdd_enum_cb cb, void* context)
{
    LISTDD a = info->mdd, b = info->match, proj = info->proj;

    if (a == listdd_empty || b == listdd_empty) return;

    if (a == listdd_empty_list) {
        assert(b == listdd_empty_list);
        cb(info->values, info->count, context);
        return;
    }

    mddnode* p_node = LDD_GETNODE(proj);
    uint32_t p_val = mddnode_getvalue(p_node);
    if (p_val == (uint32_t)-1) {
        assert(b == listdd_empty_list);
        listdd_enumerate_parallel_CALL(lace, a, cb, context, info->values, info->count);
        return;
    }

    /* Get nodes */
    mddnode* na = LDD_GETNODE(a);
    mddnode* nb = LDD_GETNODE(b);
    uint32_t na_value = mddnode_getvalue(na);
    uint32_t nb_value = mddnode_getvalue(nb);

    /* Skip nodes if possible */
    if (p_val == 1) {
        while (na_value != nb_value) {
            if (na_value < nb_value) {
                a = mddnode_getright(na);
                if (a == listdd_empty) return;
                na = LDD_GETNODE(a);
                na_value = mddnode_getvalue(na);
            }
            if (nb_value < na_value) {
                b = mddnode_getright(nb);
                if (b == listdd_empty) return;
                nb = LDD_GETNODE(b);
                nb_value = mddnode_getvalue(nb);
            }
        }
    }

    void *scratch = lace_scratch_mark(lace);
    struct listdd_match_sat_info ri, di;
    uint32_t *ri_values = info->count == 0 ? NULL : LACE_SCRATCH_ARRAY(lace, uint32_t, info->count);
    uint32_t *di_values = LACE_SCRATCH_ARRAY(lace, uint32_t, info->count+1);

    ri.mdd = mddnode_getright(na);
    di.mdd = mddnode_getdown(na);
    ri.match = b;
    di.match = p_val == 1 ? mddnode_getdown(nb) : b;
    ri.proj = proj;
    di.proj = mddnode_getdown(p_node);
    ri.count = info->count;
    di.count = info->count+1;
    ri.values = ri_values;
    di.values = di_values;
    if (ri.count > 0) memcpy(ri.values, info->values, sizeof(uint32_t)*info->count);
    if (di.count > 0) memcpy(di.values, info->values, sizeof(uint32_t)*info->count);
    di.values[info->count] = na_value;

    listdd_match_sat_SPAWN(lace, &ri, cb, context);
    listdd_match_sat_CALL(lace, &di, cb, context);
    listdd_match_sat_SYNC(lace);
    lace_scratch_reset(lace, scratch);
}

void listdd_enumerate_matching_parallel_CALL(lace_worker* lace, LISTDD mdd, LISTDD match, LISTDD proj, listdd_enum_cb cb, void* context)
{
    struct listdd_match_sat_info i;
    i.mdd = mdd;
    i.match = match;
    i.proj = proj;
    i.count = 0;
    i.values = NULL;
    listdd_match_sat_CALL(lace, &i, cb, context);
}

int
listdd_pick_values(LISTDD mdd, uint32_t* values, size_t count)
{
    if (mdd == listdd_empty) return 0;
    if (mdd == listdd_empty_list) return 1;
    assert(count != 0);
    mddnode* n = LDD_GETNODE(mdd);
    *values = mddnode_getvalue(n);
    return listdd_pick_values(mddnode_getdown(n), values+1, count-1);
}

LISTDD
listdd_pick(LISTDD mdd)
{
    if (mdd == listdd_empty) return listdd_empty;
    if (mdd == listdd_empty_list) return listdd_empty_list;
    mddnode* n = LDD_GETNODE(mdd);
    LISTDD down = listdd_pick(mddnode_getdown(n));
    return listdd_make_node(mddnode_getvalue(n), down, listdd_empty);
}

LISTDD listdd_transform_at_level_CALL(lace_worker* lace, LISTDD mdd, listdd_transform_at_level_cb cb, void* context, int depth)
{
    if (depth == 0 || mdd == listdd_empty || mdd == listdd_empty_list) {
        return cb(mdd, context);
    } else {
        mddnode* n = LDD_GETNODE(mdd);
        listdd_refs_spawn(listdd_transform_at_level_SPAWN(lace, mddnode_getright(n), cb, context, depth));
        LISTDD down = listdd_transform_at_level_CALL(lace, mddnode_getdown(n), cb, context, depth-1);
        listdd_refs_push(down);
        LISTDD right = listdd_refs_sync(listdd_transform_at_level_SYNC(lace));
        listdd_refs_pop(1);
        return listdd_make_node(mddnode_getvalue(n), down, right);
    }
}

void listdd_visit_CALL(lace_worker* lace, LISTDD mdd, listdd_visit_callbacks* cbs, size_t ctx_size, void* context)
{
    if (cbs->listdd_visit_pre(mdd, context) == 0) return;

    void *scratch = lace_scratch_mark(lace);
    void *context_down = lace_scratch_alloc(lace, ctx_size);
    void *context_right = lace_scratch_alloc(lace, ctx_size);
    cbs->listdd_visit_init_context(context_down, context, 1);
    cbs->listdd_visit_init_context(context_right, context, 0);

    listdd_visit_CALL(lace, mddnode_getdown(LDD_GETNODE(mdd)), cbs, ctx_size, context_down);
    listdd_visit_CALL(lace, mddnode_getright(LDD_GETNODE(mdd)), cbs, ctx_size, context_right);
    lace_scratch_reset(lace, scratch);

    cbs->listdd_visit_post(mdd, context);
}

void listdd_visit_parallel_CALL(lace_worker* lace, LISTDD mdd, listdd_visit_callbacks* cbs, size_t ctx_size, void* context)
{
    if (cbs->listdd_visit_pre(mdd, context) == 0) return;

    void *scratch = lace_scratch_mark(lace);
    void *context_down = lace_scratch_alloc(lace, ctx_size);
    void *context_right = lace_scratch_alloc(lace, ctx_size);
    cbs->listdd_visit_init_context(context_down, context, 1);
    cbs->listdd_visit_init_context(context_right, context, 0);

    listdd_visit_parallel_SPAWN(lace, mddnode_getdown(LDD_GETNODE(mdd)), cbs, ctx_size, context_down);
    listdd_visit_parallel_CALL(lace, mddnode_getright(LDD_GETNODE(mdd)), cbs, ctx_size, context_right);
    listdd_visit_parallel_SYNC(lace);

    lace_scratch_reset(lace, scratch);

    cbs->listdd_visit_post(mdd, context);
}

/**
 * GENERIC MARK/UNMARK METHODS
 */

static inline int
listdd_mark(mddnode* node)
{
    if (mddnode_getmark(node)) return 0;
    mddnode_setmark(node, 1);
    return 1;
}

static inline int
listdd_unmark(mddnode* node)
{
    if (mddnode_getmark(node)) {
        mddnode_setmark(node, 0);
        return 1;
    } else {
        return 0;
    }
}

static void
listdd_unmark_rec(mddnode* node)
{
    if (listdd_unmark(node)) {
        LISTDD node_right = mddnode_getright(node);
        if (node_right > listdd_empty_list) listdd_unmark_rec(LDD_GETNODE(node_right));
        LISTDD node_down = mddnode_getdown(node);
        if (node_down > listdd_empty_list) listdd_unmark_rec(LDD_GETNODE(node_down));
    }
}

/*************
 * DOT OUTPUT
*************/

static void
listdd_fprintdot_rec(FILE* out, LISTDD mdd)
{
    // assert(mdd > listdd_empty_list);

    // check mark
    mddnode* n = LDD_GETNODE(mdd);
    if (mddnode_getmark(n)) return;
    mddnode_setmark(n, 1);

    // print the node
    uint32_t val = mddnode_getvalue(n);
    fprintf(out, "%" PRIu64 " [shape=record, label=\"", mdd);
    if (mddnode_getcopy(n)) fprintf(out, "<c> *");
    else fprintf(out, "<%u> %u", val, val);
    LISTDD right = mddnode_getright(n);
    while (right != listdd_empty) {
        mddnode* n2 = LDD_GETNODE(right);
        uint32_t val2 = mddnode_getvalue(n2);
        fprintf(out, "|<%u> %u", val2, val2);
        right = mddnode_getright(n2);
        // assert(right != listdd_empty_list);
    }
    fprintf(out, "\"];\n");

    // recurse and print the edges
    for (;;) {
        LISTDD down = mddnode_getdown(n);
        // assert(down != listdd_empty);
        if (down > listdd_empty_list) {
            listdd_fprintdot_rec(out, down);
            if (mddnode_getcopy(n)) {
                fprintf(out, "%" PRIu64 ":c -> ", mdd);
            } else {
                fprintf(out, "%" PRIu64 ":%u -> ", mdd, mddnode_getvalue(n));
            }
            if (mddnode_getcopy(LDD_GETNODE(down))) {
                fprintf(out, "%" PRIu64 ":c [style=solid];\n", down);
            } else {
                fprintf(out, "%" PRIu64 ":%u [style=solid];\n", down, mddnode_getvalue(LDD_GETNODE(down)));
            }
        }
        right = mddnode_getright(n);
        if (right == listdd_empty) break;
        n = LDD_GETNODE(right);
    }
}

static void
listdd_fprintdot_unmark(LISTDD mdd)
{
    if (mdd <= listdd_empty_list) return;
    mddnode* n = LDD_GETNODE(mdd);
    if (mddnode_getmark(n)) {
        mddnode_setmark(n, 0);
        for (;;) {
            listdd_fprintdot_unmark(mddnode_getdown(n));
            mdd = mddnode_getright(n);
            if (mdd == listdd_empty) return;
            n = LDD_GETNODE(mdd);
        }
    }
}

void
listdd_fprint_dot(FILE *out, LISTDD mdd)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");

    // Special case: false
    if (mdd == listdd_empty) {
        fprintf(out, "0 [shape=record, label=\"False\"];\n");
        fprintf(out, "}\n");
        return;
    }

    // Special case: true
    if (mdd == listdd_empty_list) {
        fprintf(out, "1 [shape=record, label=\"True\"];\n");
        fprintf(out, "}\n");
        return;
    }

    listdd_fprintdot_rec(out, mdd);
    listdd_fprintdot_unmark(mdd);

    fprintf(out, "}\n");
}

void
listdd_print_dot(LISTDD mdd)
{
    listdd_fprint_dot(stdout, mdd);
}

/**
 * Some debug stuff
 */
void
listdd_fprint(FILE *f, LISTDD mdd)
{
    listdd_serialize_reset();
    size_t v = listdd_serialize_add(mdd);
    fprintf(f, "%zu,", v);
    listdd_serialize_totext(f);
}

void
listdd_print(LISTDD mdd)
{
    listdd_fprint(stdout, mdd);
}

/**
 * SERIALIZATION
 */

struct listdd_ser {
    LISTDD mdd;
    size_t assigned;
};

// Define a AVL tree type with prefix 'listdd_ser' holding
// nodes of struct listdd_ser with the following compare() function...
AVL(listdd_ser, struct listdd_ser)
{
    if (left->mdd > right->mdd) return 1;
    if (left->mdd < right->mdd) return -1;
    return 0;
}

// Define a AVL tree type with prefix 'listdd_ser_reversed' holding
// nodes of struct listdd_ser with the following compare() function...
AVL(listdd_ser_reversed, struct listdd_ser)
{
    if (left->assigned > right->assigned) return 1;
    if (left->assigned < right->assigned) return -1;
    return 0;
}

// Initially, both sets are empty
static avl_node *listdd_ser_set = NULL;
static avl_node *listdd_ser_reversed_set = NULL;

// Start counting (assigning numbers to MDDs) at 2
static size_t listdd_ser_counter = 2;
static size_t listdd_ser_done = 0;

// Given a LISTDD, assign unique numbers to all nodes
static size_t
listdd_serialize_assign_rec(LISTDD mdd)
{
    if (mdd <= listdd_empty_list) return mdd;

    mddnode* n = LDD_GETNODE(mdd);

    struct listdd_ser s, *ss;
    s.mdd = mdd;
    ss = listdd_ser_search(listdd_ser_set, &s);
    if (ss == NULL) {
        // assign dummy value
        s.assigned = 0;
        ss = listdd_ser_put(&listdd_ser_set, &s, NULL);

        // first assign recursively
        listdd_serialize_assign_rec(mddnode_getright(n));
        listdd_serialize_assign_rec(mddnode_getdown(n));

        // assign real value
        ss->assigned = listdd_ser_counter++;

        // put a copy in the reversed table
        listdd_ser_reversed_insert(&listdd_ser_reversed_set, ss);
    }

    return ss->assigned;
}

size_t
listdd_serialize_add(LISTDD mdd)
{
    return listdd_serialize_assign_rec(mdd);
}

void
listdd_serialize_reset(void)
{
    listdd_ser_free(&listdd_ser_set);
    listdd_ser_free(&listdd_ser_reversed_set);
    listdd_ser_counter = 2;
    listdd_ser_done = 0;
}

size_t
listdd_serialize_get(LISTDD mdd)
{
    if (mdd <= listdd_empty_list) return mdd;
    struct listdd_ser s, *ss;
    s.mdd = mdd;
    ss = listdd_ser_search(listdd_ser_set, &s);
    assert(ss != NULL);
    return ss->assigned;
}

LISTDD
listdd_serialize_get_reversed(size_t value)
{
    if ((LISTDD)value <= listdd_empty_list) return (LISTDD)value;
    struct listdd_ser s, *ss;
    s.assigned = value;
    ss = listdd_ser_reversed_search(listdd_ser_reversed_set, &s);
    assert(ss != NULL);
    return ss->mdd;
}

void
listdd_serialize_totext(FILE *out)
{
    avl_iter_t *it = listdd_ser_reversed_iter(listdd_ser_reversed_set);
    struct listdd_ser *s;

    fprintf(out, "[");
    while ((s=listdd_ser_reversed_iter_next(it))) {
        LISTDD mdd = s->mdd;
        mddnode* n = LDD_GETNODE(mdd);
        fprintf(out, "(%zu,v=%u,d=%zu,r=%zu),", s->assigned,
                                                mddnode_getvalue(n),
                                                listdd_serialize_get(mddnode_getdown(n)),
                                                listdd_serialize_get(mddnode_getright(n)));
    }
    fprintf(out, "]");

    listdd_ser_reversed_iter_free(it);
}

void
listdd_serialize_tofile(FILE *out)
{
    size_t count = avl_count(listdd_ser_reversed_set);
    assert(count >= listdd_ser_done);
    assert(count == listdd_ser_counter-2);
    count -= listdd_ser_done;
    fwrite(&count, sizeof(size_t), 1, out);

    struct listdd_ser *s;
    avl_iter_t *it = listdd_ser_reversed_iter(listdd_ser_reversed_set);

    /* Skip already written entries */
    size_t index = 0;
    while (index < listdd_ser_done && (s=listdd_ser_reversed_iter_next(it))) {
        assert(s->assigned == index+2);
        index++;
    }

    while ((s=listdd_ser_reversed_iter_next(it))) {
        assert(s->assigned == index+2);
        index++;

        mddnode* n = LDD_GETNODE(s->mdd);

        struct mddnode node;
        uint64_t right = listdd_serialize_get(mddnode_getright(n));
        uint64_t down = listdd_serialize_get(mddnode_getdown(n));
        if (mddnode_getcopy(n)) mddnode_makecopy(&node, right, down);
        else mddnode_make(&node, mddnode_getvalue(n), right, down);

        assert(right <= index);
        assert(down <= index);

        fwrite(&node, sizeof(struct mddnode), 1, out);
    }

    listdd_ser_done = listdd_ser_counter-2;
    listdd_ser_reversed_iter_free(it);
}

void
listdd_serialize_fromfile(FILE *in)
{
    size_t count, i;
    if (fread(&count, sizeof(size_t), 1, in) != 1) {
        // TODO FIXME return error
        printf("sylvan_serialize_fromfile: file format error, giving up\n");
        exit(-1);
    }

    for (i=1; i<=count; i++) {
        struct mddnode node;
        if (fread(&node, sizeof(struct mddnode), 1, in) != 1) {
            // TODO FIXME return error
            printf("sylvan_serialize_fromfile: file format error, giving up\n");
            exit(-1);
        }

        assert(mddnode_getright(&node) <= listdd_ser_done+1);
        assert(mddnode_getdown(&node) <= listdd_ser_done+1);

        LISTDD right = listdd_serialize_get_reversed(mddnode_getright(&node));
        LISTDD down = listdd_serialize_get_reversed(mddnode_getdown(&node));

        struct listdd_ser s;
        if (mddnode_getcopy(&node)) s.mdd = listdd_make_copy_node(down, right);
        else s.mdd = listdd_make_node(mddnode_getvalue(&node), down, right);
        s.assigned = listdd_ser_done+2; // starts at 0 but we want 2-based...
        listdd_ser_done++;

        listdd_ser_insert(&listdd_ser_set, &s);
        listdd_ser_reversed_insert(&listdd_ser_reversed_set, &s);
    }
}

void
listdd_serialize_fromfile_old(FILE *in)
{
    size_t count, i;
    if (fread(&count, sizeof(size_t), 1, in) != 1) {
        // TODO FIXME return error
        printf("sylvan_serialize_fromfile: file format error, giving up\n");
        exit(-1);
    }

    for (i=1; i<=count; i++) {
        struct mddnode node;
        if (fread(&node, sizeof(struct mddnode), 1, in) != 1) {
            // TODO FIXME return error
            printf("sylvan_serialize_fromfile: file format error, giving up\n");
            exit(-1);
        }

        assert(mddnode_old_getright(&node) <= listdd_ser_done+1);
        assert(mddnode_old_getdown(&node) <= listdd_ser_done+1);

        LISTDD right = listdd_serialize_get_reversed(mddnode_old_getright(&node));
        LISTDD down = listdd_serialize_get_reversed(mddnode_old_getdown(&node));

        struct listdd_ser s;
        if (mddnode_old_getcopy(&node)) s.mdd = listdd_make_copy_node(down, right);
        else s.mdd = listdd_make_node(mddnode_old_getvalue(&node), down, right);
        s.assigned = listdd_ser_done+2; // starts at 0 but we want 2-based...
        listdd_ser_done++;

        listdd_ser_insert(&listdd_ser_set, &s);
        listdd_ser_reversed_insert(&listdd_ser_reversed_set, &s);
    }
}

void listdd_gc_mark_serialize_CALL(lace_worker* lace)
{
    struct listdd_ser *s;
    avl_iter_t *it = listdd_ser_iter(listdd_ser_set);

    /* Iterate through nodes in serialization */
    while ((s=listdd_ser_iter_next(it))) {
        listdd_gc_mark_CALL(lace, s->mdd);
    }

    listdd_ser_iter_free(it);
}

static void
listdd_sha2_rec(LISTDD mdd, SHA256_CTX *ctx)
{
    if (mdd <= listdd_empty_list) {
        SHA256_Update(ctx, (void*)&mdd, sizeof(uint64_t));
        return;
    }

    mddnode* node = LDD_GETNODE(mdd);
    if (listdd_mark(node)) {
        uint32_t val = mddnode_getvalue(node);
        SHA256_Update(ctx, (void*)&val, sizeof(uint32_t));
        listdd_sha2_rec(mddnode_getdown(node), ctx);
        listdd_sha2_rec(mddnode_getright(node), ctx);
    }
}

void
listdd_print_sha256(LISTDD mdd)
{
    listdd_fprint_sha256(stdout, mdd);
}

void
listdd_fprint_sha256(FILE *out, LISTDD mdd)
{
    char buf[80];
    listdd_sha256(mdd, buf);
    fprintf(out, "%s", buf);
}

void
listdd_sha256(LISTDD mdd, char *target)
{
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    listdd_sha2_rec(mdd, &ctx);
    if (mdd > listdd_empty_list) listdd_unmark_rec(LDD_GETNODE(mdd));
    SHA256_End(&ctx, target);
}

#ifndef NDEBUG
size_t
listdd_is_valid(LISTDD mdd)
{
    if (mdd == listdd_empty_list) return 1;
    if (mdd == listdd_empty) return 0;

    int first = 1;
    size_t depth = 0;

    if (mdd != listdd_empty) {
        mddnode* n = LDD_GETNODE(mdd);
        if (mddnode_getcopy(n)) {
            mdd = mddnode_getright(n);
            depth = listdd_is_valid(mddnode_getdown(n));
            assert(depth >= 1);
        }
    }

    uint32_t value = 0;
    while (mdd != listdd_empty) {
        assert(nodes_is_marked(nodes, mdd));

        mddnode* n = LDD_GETNODE(mdd);
        uint32_t next_value = mddnode_getvalue(n);
        assert(mddnode_getcopy(n) == 0);
        if (first) {
            first = 0;
            depth = listdd_is_valid(mddnode_getdown(n));
            assert(depth >= 1);
        } else {
            assert(value < next_value);
            assert(depth == listdd_is_valid(mddnode_getdown(n)));
        }

        value = next_value;
        mdd = mddnode_getright(n);
    }

    return 1 + depth;
}
#endif
