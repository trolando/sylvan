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

/* Primitives */
int
tzdd_isleaf(TZDD bdd)
{
    if (bdd == tzdd_true || bdd == tzdd_false) return 1;
    return tzddnode_isleaf(TZDD_GETNODE(bdd));
}

uint32_t
tzdd_getvar(TZDD node)
{
    return tzddnode_getvariable(TZDD_GETNODE(node));
}

TZDD
tzdd_getpos(TZDD tzdd)
{
    return tzddnode_pos(tzdd, TZDD_GETNODE(tzdd));
}

TZDD
tzdd_getneg(TZDD tzdd)
{
    return tzddnode_neg(tzdd, TZDD_GETNODE(tzdd));
}

TZDD
tzdd_getzero(TZDD tzdd)
{
    return tzddnode_zero(tzdd, TZDD_GETNODE(tzdd));
}

uint32_t
tzdd_gettype(TZDD leaf)
{
    return tzddnode_gettype(TZDD_GETNODE(leaf));
}

uint64_t
tzdd_getvalue(TZDD leaf)
{
    return tzddnode_getvalue(TZDD_GETNODE(leaf));
}

/**
 * Implementation of garbage collection
 */

/* Recursively mark MDD nodes as 'in use' */
VOID_TASK_IMPL_1(tzdd_gc_mark_rec, MDD, tzdd)
{
    if (tzdd == tzdd_true) return;
    if (tzdd == tzdd_false) return;

    if (llmsset_mark(nodes, TZDD_GETINDEX(tzdd))) {
        tzddnode_t n = TZDD_GETNODE(tzdd);
        if (!tzddnode_isleaf(n)) {
            SPAWN(tzdd_gc_mark_rec, tzddnode_getpos(n));
            SPAWN(tzdd_gc_mark_rec, tzddnode_getneg(n));
            CALL(tzdd_gc_mark_rec, tzddnode_getzero(n));
            SYNC(tzdd_gc_mark_rec);
            SYNC(tzdd_gc_mark_rec);
        }
    }
}

/**
 * External references
 */

refs_table_t tzdd_refs;
refs_table_t tzdd_protected;
static int tzdd_protected_created = 0;

MDD
tzdd_ref(MDD a)
{
    if (a == tzdd_true || a == tzdd_false) return a;
    refs_up(&tzdd_refs, TZDD_GETINDEX(a));
    return a;
}

void
tzdd_deref(MDD a)
{
    if (a == tzdd_true || a == tzdd_false) return;
    refs_down(&tzdd_refs, TZDD_GETINDEX(a));
}

size_t
tzdd_count_refs()
{
    return refs_count(&tzdd_refs);
}

void
tzdd_protect(TZDD *a)
{
    if (!tzdd_protected_created) {
        // In C++, sometimes tzdd_protect is called before Sylvan is initialized. Just create a table.
        protect_create(&tzdd_protected, 4096);
        tzdd_protected_created = 1;
    }
    protect_up(&tzdd_protected, (size_t)a);
}

void
tzdd_unprotect(TZDD *a)
{
    if (tzdd_protected.refs_table != NULL) protect_down(&tzdd_protected, (size_t)a);
}

size_t
tzdd_count_protected()
{
    return protect_count(&tzdd_protected);
}

/* Called during garbage collection */
VOID_TASK_0(tzdd_gc_mark_external_refs)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = refs_iter(&tzdd_refs, 0, tzdd_refs.refs_size);
    while (it != NULL) {
        SPAWN(tzdd_gc_mark_rec, refs_next(&tzdd_refs, &it, tzdd_refs.refs_size));
        count++;
    }
    while (count--) {
        SYNC(tzdd_gc_mark_rec);
    }
}

VOID_TASK_0(tzdd_gc_mark_protected)
{
    // iterate through refs hash table, mark all found
    size_t count=0;
    uint64_t *it = protect_iter(&tzdd_protected, 0, tzdd_protected.refs_size);
    while (it != NULL) {
        BDD *to_mark = (BDD*)protect_next(&tzdd_protected, &it, tzdd_protected.refs_size);
        SPAWN(tzdd_gc_mark_rec, *to_mark);
        count++;
    }
    while (count--) {
        SYNC(tzdd_gc_mark_rec);
    }
}

/* Infrastructure for internal markings */
DECLARE_THREAD_LOCAL(tzdd_refs_key, tzdd_refs_internal_t);

VOID_TASK_0(tzdd_refs_mark_task)
{
    LOCALIZE_THREAD_LOCAL(tzdd_refs_key, tzdd_refs_internal_t);
    size_t i, j=0;
    for (i=0; i<tzdd_refs_key->r_count; i++) {
        if (j >= 40) {
            while (j--) SYNC(tzdd_gc_mark_rec);
            j=0;
        }
        SPAWN(tzdd_gc_mark_rec, tzdd_refs_key->results[i]);
        j++;
    }
    for (i=0; i<tzdd_refs_key->s_count; i++) {
        Task *t = tzdd_refs_key->spawns[i];
        if (!TASK_IS_STOLEN(t)) break;
        if (TASK_IS_COMPLETED(t)) {
            if (j >= 40) {
                while (j--) SYNC(tzdd_gc_mark_rec);
                j=0;
            }
            SPAWN(tzdd_gc_mark_rec, *(BDD*)TASK_RESULT(t));
            j++;
        }
    }
    while (j--) SYNC(tzdd_gc_mark_rec);
}

VOID_TASK_0(tzdd_refs_mark)
{
    TOGETHER(tzdd_refs_mark_task);
}

VOID_TASK_0(tzdd_refs_init_task)
{
    tzdd_refs_internal_t s = (tzdd_refs_internal_t)malloc(sizeof(struct tzdd_refs_internal));
    s->r_size = 128;
    s->r_count = 0;
    s->s_size = 128;
    s->s_count = 0;
    s->results = (BDD*)malloc(sizeof(BDD) * 128);
    s->spawns = (Task**)malloc(sizeof(Task*) * 128);
    SET_THREAD_LOCAL(tzdd_refs_key, s);
}

VOID_TASK_0(tzdd_refs_init)
{
    INIT_THREAD_LOCAL(tzdd_refs_key);
    TOGETHER(tzdd_refs_init_task);
    sylvan_gc_add_mark(TASK(tzdd_refs_mark));
}

/**
 * Initialize and quit functions
 */

static int tzdd_initialized = 0;

static void
tzdd_quit()
{
    refs_free(&tzdd_refs);
    if (tzdd_protected_created) {
        protect_free(&tzdd_protected);
        tzdd_protected_created = 0;
    }

    tzdd_initialized = 0;
}

void
sylvan_init_tzdd()
{
    if (tzdd_initialized) return;
    tzdd_initialized = 1;

    sylvan_register_quit(tzdd_quit);
    sylvan_gc_add_mark(TASK(tzdd_gc_mark_external_refs));
    sylvan_gc_add_mark(TASK(tzdd_gc_mark_protected));

    refs_create(&tzdd_refs, 1024);
    if (!tzdd_protected_created) {
        protect_create(&tzdd_protected, 4096);
        tzdd_protected_created = 1;
    }

    LACE_ME;
    CALL(tzdd_refs_init);
}

/**
 * Primitives
 */
TZDD
tzdd_makeleaf(uint32_t type, uint64_t value)
{
    struct tzddnode n;
    tzddnode_makeleaf(&n, type, value);

    int created;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        sylvan_gc();

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return (TZDD)index;
}

TZDD
tzdd_makenode(uint32_t var, TZDD pos, TZDD neg, TZDD zero)
{
    /* Normalization rules */
    
    struct tzddnode n;

    /* Like ZDD... "true" to 0 = skip */
    if (pos == tzdd_false && neg == tzdd_false) {
        return zero;
    } else {
        /* fine, go ahead */
        tzddnode_makenode(&n, var, pos, neg, zero);
    }

    int created;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        tzdd_refs_push(pos);
        tzdd_refs_push(neg);
        tzdd_refs_push(zero);
        sylvan_gc();
        tzdd_refs_pop(3);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(TZDD_NODES_CREATED);
    else sylvan_stats_count(TZDD_NODES_REUSED);

    return (TZDD)index;
}

TZDD
tzdd_makemapnode(uint32_t var, TZDD pos, TZDD neg)
{
    struct tzddnode n;
    uint64_t index;
    int created;

    tzddnode_makemapnode(&n, var, pos, neg);
    index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        tzdd_refs_push(pos);
        tzdd_refs_push(neg);
        sylvan_gc();
        tzdd_refs_pop(2);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return (TZDD)index;
}

TASK_IMPL_1(TZDD, tzdd_make_clause, int32_t*, literals)
{
    if (*literals == 0) return tzdd_true; // empty clause
    TZDD rec = CALL(tzdd_make_clause, literals+1);
    int32_t lit = *literals;
    if (lit<0) {
        return tzdd_makenode(-lit, tzdd_false, rec, tzdd_false);
    } else {
        return tzdd_makenode(lit, rec, tzdd_false, tzdd_false);
    }
}

/**
 * Add a clause to the clause database
 * Assumes literals are in correct order, and terminated by 0
 */
TASK_IMPL_2(TZDD, tzdd_add_clause, TZDD, db, int32_t*, literals)
{
    /*
     * false means clause not in db // empty set
     * true means clause in db // empty clause
     */
    
    /*
     * add(1, ...) = 1
     * add(X, "0") = 1 // add empty clause to db, subume res
     * add([v, A, B, C], 0) = ...
     */

    /* Terminal cases that immediately return */
    if (db == tzdd_true) {
        return tzdd_true;
    } else if (db == tzdd_false) {
        return CALL(tzdd_make_clause, literals);
    } else if (*literals == 0) {
        // union 'true' + db
        // if we are not adding the empty clause, then upstream we already have stuff going on...
        // so auto-subsume
        return tzdd_true;
    }

    /* Check cache (maybe) */
    sylvan_gc_test();
    /* missing: op counter */

    int32_t lit = *literals;
    int32_t var = (lit > 0) ? lit : -lit;

    tzddnode_t ndb = TZDD_GETNODE(db);
    int32_t vardb = tzddnode_getvariable(ndb);

    TZDD result;

    if (vardb < var) {
        TZDD zero = tzddnode_getpos(ndb);
        TZDD rec = CALL(tzdd_add_clause, zero, literals);
        if (rec == zero) {
            /* db didnt change */
            result = db;
        } else {
            TZDD pos = tzddnode_getpos(ndb);
            TZDD neg = tzddnode_getpos(ndb);
            result = tzdd_makenode(var, pos, neg, rec);
        }
    } else if (vardb == var) {
        if (lit > 0) {
            TZDD pos = tzddnode_getpos(ndb);
            TZDD rec = CALL(tzdd_add_clause, pos, literals+1);
            if (pos == rec) {
                /* db didnt change */
                result = db;
            } else {
                TZDD neg = tzddnode_getpos(ndb);
                TZDD zero = tzddnode_getpos(ndb);
                result = tzdd_makenode(var, rec, neg, zero);
            }
        } else {
            TZDD neg = tzddnode_getpos(ndb);
            TZDD rec = CALL(tzdd_add_clause, neg, literals+1);
            if (neg == rec) {
                /* db didnt change */
                result = db;
            } else {
                TZDD pos = tzddnode_getpos(ndb);
                TZDD zero = tzddnode_getpos(ndb);
                result = tzdd_makenode(var, pos, rec, zero);
            }
        }
    } else /* vardb > var */ {
        TZDD rec = CALL(tzdd_make_clause, literals+1);
        if (lit > 0) {
            result = tzdd_makenode(var, rec, tzdd_false, db);
        } else {
            result = tzdd_makenode(var, tzdd_false, rec, db);
        }
    }

    /* Write to cache (maybe) */

    /* Return result */
    return result;
}

/**
 * Helper function for recursive unmarking
 */
static void
tzdd_unmark_rec(TZDD tzdd)
{
    tzddnode_t n = TZDD_GETNODE(tzdd);
    if (!tzddnode_getmark(n)) return;
    tzddnode_setmark(n, 0);
    if (tzddnode_isleaf(n)) return;
    tzdd_unmark_rec(tzddnode_getpos(n));
    tzdd_unmark_rec(tzddnode_getneg(n));
    tzdd_unmark_rec(tzddnode_getzero(n));
}

/**
 * Count number of nodes in TZDD
 */

static size_t
tzdd_nodecount_mark(TZDD tzdd)
{
    if (tzdd == tzdd_true) return 0; // do not count true/false leaf
    if (tzdd == tzdd_false) return 0; // do not count true/false leaf
    tzddnode_t n = TZDD_GETNODE(tzdd);
    if (tzddnode_getmark(n)) return 0;
    tzddnode_setmark(n, 1);
    if (tzddnode_isleaf(n)) return 1; // count leaf as 1
    return 1 + tzdd_nodecount_mark(tzddnode_getpos(n)) + tzdd_nodecount_mark(tzddnode_getneg(n)) + tzdd_nodecount_mark(tzddnode_getzero(n));
}

size_t
tzdd_nodecount_more(const TZDD *tzdds, size_t count)
{
    size_t result = 0, i;
    for (i=0; i<count; i++) result += tzdd_nodecount_mark(tzdds[i]);
    for (i=0; i<count; i++) tzdd_unmark_rec(tzdds[i]);
    return result;
}

