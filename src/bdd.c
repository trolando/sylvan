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
#include <string.h>

#include <avl.h>

/**
 * Implementation of unary, binary and if-then-else operators.
 */
int
bdd_and_CALL(lace_worker* lace, BDD *destination, BDD a, BDD b)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;
    if (a == mtbdd_invalid || b == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminal cases */
    if (a == bdd_true) { *destination = b; return SYLVAN_OK; }
    if (b == bdd_true) { *destination = a; return SYLVAN_OK; }
    if (a == bdd_false || b == bdd_false || a == BDD_TOGGLEMARK(b)) {
        *destination = bdd_false;
        return SYLVAN_OK;
    }
    if (a == b) { *destination = a; return SYLVAN_OK; }

    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_AND);

    /* Improve for caching */
    if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        BDD t = b;
        b = a;
        a = t;
    }

    bddnode* na = MTBDD_GETNODE(a);
    bddnode* nb = MTBDD_GETNODE(b);

    uint32_t va = bddnode_getvariable(na);
    uint32_t vb = bddnode_getvariable(nb);
    uint32_t level = va < vb ? va : vb;

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_AND, a, b, bdd_false, &computed)) {
        sylvan_stats_count(BDD_AND_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    if (level == va) {
        aLow = node_low(a, na);
        aHigh = node_high(a, na);
    }
    if (level == vb) {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
    }

    // Recursive computation
    BDD low = mtbdd_invalid, high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);

    int spawned = 0;
    int status = SYLVAN_OK;

    if (aHigh == bdd_true) {
        high = bHigh;
    } else if (aHigh == bdd_false || bHigh == bdd_false) {
        high = bdd_false;
    } else if (bHigh == bdd_true) {
        high = aHigh;
    } else {
        bdd_and_SPAWN(lace, &high, aHigh, bHigh);
        spawned = 1;
    }

    if (aLow == bdd_true) {
        low = bLow;
    } else if (aLow == bdd_false || bLow == bdd_false) {
        low = bdd_false;
    } else if (bLow == bdd_true) {
        low = aLow;
    } else {
        status = bdd_and_CALL(lace, &low, aLow, bLow);
    }

    if (spawned) {
        int high_status = bdd_and_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
    }

    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    status = _mtbdd_try_make_node(&computed, level, low, high);
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    if (cache_put3(CACHE_BDD_AND, a, b, bdd_false, computed)) sylvan_stats_count(BDD_AND_CACHEDPUT);

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

BDD
bdd_and_legacy_CALL(lace_worker *lace, BDD a, BDD b)
{
    BDD result = mtbdd_invalid;
    mtbdd_refs_pushptr(&result);
    int status = bdd_and_CALL(lace, &result, a, b);
    mtbdd_refs_popptr(1);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

// FIXME improve documentation...
/*
    bdd_disjoint could be implemented as "bdd_and(a,b)==bdd_false",
    but this implementation avoids building new nodes and allows more short-circuitry.
*/
char bdd_disjoint_CALL(lace_worker* lace, BDD a, BDD b)
{
    /* Terminal cases */
    if (a == bdd_false || b == bdd_false) return 1;
    if (a == bdd_true || b == bdd_true) return 0; /* since a,b != bdd_false */
    if (a == b) return 0; /* since a,b != bdd_false */
    if (a == BDD_TOGGLEMARK(b)) return 1;

    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_DISJOINT);

    /* Improve for caching */
    if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        BDD t = b;
        b = a;
        a = t;
    }

    bddnode* na = MTBDD_GETNODE(a);
    bddnode* nb = MTBDD_GETNODE(b);

    uint32_t va = bddnode_getvariable(na);
    uint32_t vb = bddnode_getvariable(nb);
    uint32_t level = va < vb ? va : vb;

    {
        BDD result;
        if (cache_get3(CACHE_BDD_DISJOINT, a, b, bdd_false, &result)) {
            sylvan_stats_count(BDD_DISJOINT_CACHED);
            return (result==bdd_false ? 0 : 1);
        }
    }

    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    if (level == va) {
        aLow = node_low(a, na);
        aHigh = node_high(a, na);
    }
    if (level == vb) {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
    }

    int low=-1, high=-1, result;

    // Try to obtain the subresults without recursion (short-circuiting)

    if (aHigh == bdd_false || bHigh == bdd_false) {
        high = 1;
    } else if (aHigh == bdd_true || bHigh == bdd_true) {
        high = 0; /* since none of them is bdd_false */
    } else if (aHigh == bHigh) {
        high = 0; /* since none of them is bdd_false */
    } else if (aHigh == BDD_TOGGLEMARK(bHigh)) {
        high = 1;
    }

    if (aLow == bdd_false || bLow == bdd_false) {
        low = 1;
    } else if (aLow == bdd_true || bLow == bdd_true) {
        low = 0; /* since none of them is bdd_false */
    } else if (aLow == bLow) {
        low = 0; /* since none of them is bdd_false */
    } else if (aLow == BDD_TOGGLEMARK(bLow)) {
        low = 1;
    }

    // Compute the result, if necessary, by parallel recursion

    if (high==0 || low==0) {
        result = 0;
    }
    else {
        if (high==-1) bdd_disjoint_SPAWN(lace, aHigh, bHigh);
        if (low ==-1) low = bdd_disjoint_CALL(lace, aLow, bLow);
        if (high==-1) high = bdd_disjoint_SYNC(lace);
        result = high && low;
    }

    // Store result in the cache and then return

    {
        BDD to_cache = (result ? bdd_true : bdd_false);
        if (cache_put3(CACHE_BDD_DISJOINT, a, b, bdd_false, to_cache)) {
            sylvan_stats_count(BDD_DISJOINT_CACHEDPUT);
        }
    }

    return (char)result;
}

int bdd_xor_CALL(lace_worker* lace, BDD *destination, BDD a, BDD b)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;
    if (a == mtbdd_invalid || b == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminal cases */
    if (a == bdd_false) { *destination = b; return SYLVAN_OK; }
    if (b == bdd_false) { *destination = a; return SYLVAN_OK; }
    if (a == bdd_true) { *destination = bdd_not(b); return SYLVAN_OK; }
    if (b == bdd_true) { *destination = bdd_not(a); return SYLVAN_OK; }
    if (a == b) { *destination = bdd_false; return SYLVAN_OK; }
    if (a == bdd_not(b)) { *destination = bdd_true; return SYLVAN_OK; }

    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_XOR);

    /* Improve for caching */
    if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        BDD t = b;
        b = a;
        a = t;
    }

    // XOR(~A,B) => XOR(A,~B)
    if (BDD_HASMARK(a)) {
        a = BDD_STRIPMARK(a);
        b = bdd_not(b);
    }

    bddnode* na = MTBDD_GETNODE(a);
    bddnode* nb = MTBDD_GETNODE(b);

    uint32_t va = bddnode_getvariable(na);
    uint32_t vb = bddnode_getvariable(nb);
    uint32_t level = va < vb ? va : vb;

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_XOR, a, b, bdd_false, &computed)) {
        sylvan_stats_count(BDD_XOR_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    if (level == va) {
        aLow = node_low(a, na);
        aHigh = node_high(a, na);
    }
    if (level == vb) {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
    }

    // Recursive computation
    BDD low = mtbdd_invalid, high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);

    bdd_xor_SPAWN(lace, &high, aHigh, bHigh);
    int low_status = bdd_xor_CALL(lace, &low, aLow, bLow);
    int high_status = bdd_xor_SYNC(lace);
    if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return low_status != SYLVAN_OK ? low_status : high_status;
    }

    int status = _mtbdd_try_make_node(&computed, level, low, high);
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    if (cache_put3(CACHE_BDD_XOR, a, b, bdd_false, computed)) sylvan_stats_count(BDD_XOR_CACHEDPUT);

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

int bdd_ite_CALL(lace_worker *lace, BDD *destination, BDD a, BDD b, BDD c)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;
    if (a == mtbdd_invalid || b == mtbdd_invalid || c == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminal cases */
    if (a == bdd_true) { *destination = b; return SYLVAN_OK; }
    if (a == bdd_false) { *destination = c; return SYLVAN_OK; }
    if (a == b) b = bdd_true;
    if (a == bdd_not(b)) b = bdd_false;
    if (a == c) c = bdd_false;
    if (a == bdd_not(c)) c = bdd_true;
    if (b == c) { *destination = b; return SYLVAN_OK; }
    if (b == bdd_true && c == bdd_false) { *destination = a; return SYLVAN_OK; }
    if (b == bdd_false && c == bdd_true) { *destination = bdd_not(a); return SYLVAN_OK; }

    /* Cases that reduce to AND and XOR */

    // ITE(A,B,0) => AND(A,B)
    if (c == bdd_false) return bdd_and_CALL(lace, destination, a, b);

    // ITE(A,1,C) => ~AND(~A,~C)
    if (b == bdd_true) {
        BDD computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&computed);
        int status = bdd_and_CALL(lace, &computed, bdd_not(a), bdd_not(c));
        if (status == SYLVAN_OK) *destination = bdd_not(computed);
        mtbdd_refs_popptr(1);
        return status;
    }

    // ITE(A,0,C) => AND(~A,C)
    if (b == bdd_false) return bdd_and_CALL(lace, destination, bdd_not(a), c);

    // ITE(A,B,1) => ~AND(A,~B)
    if (c == bdd_true) {
        BDD computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&computed);
        int status = bdd_and_CALL(lace, &computed, a, bdd_not(b));
        if (status == SYLVAN_OK) *destination = bdd_not(computed);
        mtbdd_refs_popptr(1);
        return status;
    }

    /* At this point, there are no more terminals */

    /* Canonical for optimal cache use */

    // ITE(~A,B,C) => ITE(A,C,B)
    if (BDD_HASMARK(a)) {
        a = BDD_STRIPMARK(a);
        BDD t = c;
        c = b;
        b = t;
    }

    // ITE(A,~B,C) => ~ITE(A,B,~C)
    int mark = 0;
    if (BDD_HASMARK(b)) {
        b = bdd_not(b);
        c = bdd_not(c);
        mark = 1;
    }

    bddnode* na = MTBDD_GETNODE(a);
    bddnode* nb = MTBDD_GETNODE(b);
    bddnode* nc = MTBDD_GETNODE(c);

    uint32_t va = bddnode_getvariable(na);
    uint32_t vb = bddnode_getvariable(nb);
    uint32_t vc = bddnode_getvariable(nc);

    // Get lowest level
    uint32_t level = vb < vc ? vb : vc;

    // Fast case
    if (va < level && node_low(a, na) == bdd_false && node_high(a, na) == bdd_true) {
        BDD computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&computed);
        int status = _mtbdd_try_make_node(&computed, va, c, b);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(1);
            return status;
        }
        *destination = mark ? bdd_not(computed) : computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    if (va < level) level = va;

    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_ITE);

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_ITE, a, b, c, &computed)) {
        sylvan_stats_count(BDD_ITE_CACHED);
        *destination = mark ? bdd_not(computed) : computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    BDD cLow = c, cHigh = c;
    if (level == va) {
        aLow = node_low(a, na);
        aHigh = node_high(a, na);
    }
    if (level == vb) {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
    }
    if (level == vc) {
        cLow = node_low(c, nc);
        cHigh = node_high(c, nc);
    }

    // Recursive computation
    BDD low = mtbdd_invalid, high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);

    int spawned = 0;
    int status = SYLVAN_OK;

    if (aHigh == bdd_true) {
        high = bHigh;
    } else if (aHigh == bdd_false) {
        high = cHigh;
    } else {
        bdd_ite_SPAWN(lace, &high, aHigh, bHigh, cHigh);
        spawned = 1;
    }

    if (aLow == bdd_true) {
        low = bLow;
    } else if (aLow == bdd_false) {
        low = cLow;
    } else {
        status = bdd_ite_CALL(lace, &low, aLow, bLow, cLow);
    }

    if (spawned) {
        int high_status = bdd_ite_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
    }

    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    status = _mtbdd_try_make_node(&computed, level, low, high);
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    if (cache_put3(CACHE_BDD_ITE, a, b, c, computed)) sylvan_stats_count(BDD_ITE_CACHEDPUT);

    *destination = mark ? bdd_not(computed) : computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

BDD
bdd_ite_legacy_CALL(lace_worker *lace, BDD a, BDD b, BDD c)
{
    BDD result = mtbdd_invalid;
    mtbdd_refs_pushptr(&result);
    int status = bdd_ite_CALL(lace, &result, a, b, c);
    mtbdd_refs_popptr(1);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

/**
 * Compute constrain f@c, also called the generalized co-factor.
 * c is the "care function" - f@c equals f when c evaluates to True.
 */
BDD bdd_constrain_CALL(lace_worker* lace, BDD f, BDD c)
{
    /* Trivial cases */
    if (c == bdd_true) return f;
    if (c == bdd_false) return bdd_false;
    if (bdd_is_leaf(f)) return f;
    if (f == c) return bdd_true;
    if (f == bdd_not(c)) return bdd_false;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_CONSTRAIN);

    bddnode* nf = MTBDD_GETNODE(f);
    bddnode* nc = MTBDD_GETNODE(c);

    uint32_t vf = bddnode_getvariable(nf);
    uint32_t vc = bddnode_getvariable(nc);
    uint32_t level = vf < vc ? vf : vc;

    /* Make canonical */
    int mark = 0;
    if (BDD_HASMARK(f)) {
        f = BDD_STRIPMARK(f);
        mark = 1;
    }

    /* Consult cache */
    {
        BDD result;
        if (cache_get3(CACHE_BDD_CONSTRAIN, f, c, 0, &result)) {
            sylvan_stats_count(BDD_CONSTRAIN_CACHED);
            return mark ? bdd_not(result) : result;
        }
    }

    BDD fLow, fHigh, cLow, cHigh;

    if (level == vf) {
        fLow = node_low(f, nf);
        fHigh = node_high(f, nf);
    } else {
        fLow = fHigh = f;
    }

    if (level == vc) {
        cLow = node_low(c, nc);
        cHigh = node_high(c, nc);
    } else {
        cLow = cHigh = c;
    }

    BDD result;

    if (cLow == bdd_false) {
        /* cLow is False, so result equals fHigh @ cHigh */
        if (cHigh == bdd_true) result = fHigh;
        else result = bdd_constrain_CALL(lace, fHigh, cHigh);
    } else if (cHigh == bdd_false) {
        /* cHigh is False, so result equals fLow @ cLow */
        if (cLow == bdd_true) result = fLow;
        else result = bdd_constrain_CALL(lace, fLow, cLow);
    } else if (cLow == bdd_true) {
        /* cLow is True, so low result equals fLow */
        BDD high = bdd_constrain_CALL(lace, fHigh, cHigh);
        result = mtbdd_make_node(level, fLow, high);
    } else if (cHigh == bdd_true) {
        /* cHigh is True, so high result equals fHigh */
        BDD low = bdd_constrain_CALL(lace, fLow, cLow);
        result = mtbdd_make_node(level, low, fHigh);
    } else {
        /* cLow and cHigh are not constrants... normal parallel recursion */
        mtbdd_refs_spawn(bdd_constrain_SPAWN(lace, fLow, cLow));
        BDD high = bdd_constrain_CALL(lace, fHigh, cHigh);
        mtbdd_refs_push(high);
        BDD low = mtbdd_refs_sync(bdd_constrain_SYNC(lace));
        mtbdd_refs_pop(1);
        result = mtbdd_make_node(level, low, high);
    }

    if (cache_put3(CACHE_BDD_CONSTRAIN, f, c, 0, result)) sylvan_stats_count(BDD_CONSTRAIN_CACHEDPUT);

    return mark ? bdd_not(result) : result;
}

/**
 * Compute restrict f@c, which uses a heuristic to try and minimize a BDD f with respect to a care function c
 */
TASK(BDD, bdd_restrict_internal, BDD, f, BDD, c)

BDD bdd_restrict_internal_CALL(lace_worker* lace, BDD f, BDD c)
{
    /* Trivial cases */
    if (c == bdd_true) return f;
    if (c == bdd_false) return bdd_false;
    if (bdd_is_leaf(f)) return f;
    if (f == c) return bdd_true;
    if (f == bdd_not(c)) return bdd_false;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_RESTRICT);

    bddnode* nf = MTBDD_GETNODE(f);
    bddnode* nc = MTBDD_GETNODE(c);

    uint32_t vf = bddnode_getvariable(nf);
    uint32_t vc = bddnode_getvariable(nc);
    uint32_t level = vf < vc ? vf : vc;

    /* Make canonical */
    int mark = 0;
    if (BDD_HASMARK(f)) {
        f = BDD_STRIPMARK(f);
        mark = 1;
    }

    /* Consult cache */
    BDD result;
    if (cache_get3(CACHE_BDD_RESTRICT, f, c, 0, &result)) {
        sylvan_stats_count(BDD_RESTRICT_CACHED);
        return mark ? bdd_not(result) : result;
    }

    if (vc < vf) {
        /* f is independent of c, so result is f @ (cLow \/ cHigh) */
        BDD new_c = bdd_not(bdd_and_legacy_CALL(lace, bdd_not(node_low(c, nc)), bdd_not(node_high(c, nc))));
        mtbdd_refs_push(new_c);
        result = bdd_restrict_internal_CALL(lace, f, new_c);
        mtbdd_refs_pop(1);
    } else {
        BDD fLow = node_low(f,nf), fHigh = node_high(f,nf);
        BDD cLow, cHigh;
        if (vf == vc) {
            cLow = node_low(c, nc);
            cHigh = node_high(c, nc);
        } else {
            cLow = cHigh = c;
        }
        if (cLow == bdd_false) {
            /* sibling-substitution */
            result = bdd_restrict_internal_CALL(lace, fHigh, cHigh);
        } else if (cHigh == bdd_false) {
            /* sibling-substitution */
            result = bdd_restrict_internal_CALL(lace, fLow, cLow);
        } else {
            /* parallel recursion */
            mtbdd_refs_spawn(bdd_restrict_internal_SPAWN(lace, fLow, cLow));
            BDD high = bdd_restrict_internal_CALL(lace, fHigh, cHigh);
            mtbdd_refs_push(high);
            BDD low = mtbdd_refs_sync(bdd_restrict_internal_SYNC(lace));
            mtbdd_refs_pop(1);
            result = mtbdd_make_node(level, low, high);
        }
    }

    if (cache_put3(CACHE_BDD_RESTRICT, f, c, 0, result)) sylvan_stats_count(BDD_RESTRICT_CACHEDPUT);

    return mark ? bdd_not(result) : result;
}

BDD
bdd_restrict_CALL(lace_worker* lace, BDD f, BDD c)
{
    BDD result = bdd_restrict_internal_CALL(lace, f, c);
    return mtbdd_node_count(result) <= mtbdd_node_count(f) ? result : f;
}

static int
bdd_is_cube(BDD cube)
{
    while (!bdd_is_leaf(cube)) {
        bddnode *node = MTBDD_GETNODE(cube);
        BDD low = node_low(cube, node);
        BDD high = node_high(cube, node);
        if (low == bdd_false && high != bdd_false) cube = high;
        else if (high == bdd_false && low != bdd_false) cube = low;
        else return 0;
    }
    return cube == bdd_true;
}

BDD
bdd_cofactor(BDD f, BDD cube)
{
    if (!bdd_is_cube(cube)) return mtbdd_invalid;
    return bdd_constrain(f, cube);
}

/**
 * Calculates \exists variables . a
 */
int bdd_exists_CALL(lace_worker* lace, BDD *destination, BDD a, BDD variables)
{
    if (destination == NULL || a == mtbdd_invalid || variables == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminal cases */
    if (a == bdd_true) { *destination = bdd_true; return SYLVAN_OK; }
    if (a == bdd_false) { *destination = bdd_false; return SYLVAN_OK; }
    if (bdd_set_is_empty(variables)) { *destination = a; return SYLVAN_OK; }

    // a != constant
    bddnode* na = MTBDD_GETNODE(a);
    uint32_t level = bddnode_getvariable(na);

    bddnode* nv = MTBDD_GETNODE(variables);
    uint32_t vv = bddnode_getvariable(nv);
    while (vv < level) {
        variables = node_high(variables, nv);
        if (bdd_set_is_empty(variables)) { *destination = a; return SYLVAN_OK; }
        nv = MTBDD_GETNODE(variables);
        vv = bddnode_getvariable(nv);
    }

    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_EXISTS);

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_EXISTS, a, variables, 0, &computed)) {
        sylvan_stats_count(BDD_EXISTS_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    // Get cofactors
    BDD aLow = node_low(a, na);
    BDD aHigh = node_high(a, na);

    BDD low = mtbdd_invalid, high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    int status = SYLVAN_OK;

    if (vv == level) {
        // level is in variable set, perform abstraction
        if (aLow == bdd_true || aHigh == bdd_true || aLow == bdd_not(aHigh)) {
            computed = bdd_true;
        } else {
            BDD _v = bdd_set_next(variables);
            status = bdd_exists_CALL(lace, &low, aLow, _v);
            if (status != SYLVAN_OK) {
                mtbdd_refs_popptr(3);
                return status;
            }
            if (low == bdd_true) {
                computed = bdd_true;
            } else {
                status = bdd_exists_CALL(lace, &high, aHigh, _v);
                if (status != SYLVAN_OK) {
                    mtbdd_refs_popptr(3);
                    return status;
                }
                if (high == bdd_true) {
                    computed = bdd_true;
                } else if (low == bdd_false && high == bdd_false) {
                    computed = bdd_false;
                } else {
                    status = bdd_and_CALL(lace, &computed, bdd_not(low), bdd_not(high));
                    if (status != SYLVAN_OK) {
                        mtbdd_refs_popptr(3);
                        return status;
                    }
                    computed = bdd_not(computed);
                }
            }
        }
    } else {
        // level is not in variable set
        bdd_exists_SPAWN(lace, &high, aHigh, variables);
        int low_status = bdd_exists_CALL(lace, &low, aLow, variables);
        int high_status = bdd_exists_SYNC(lace);
        if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return low_status != SYLVAN_OK ? low_status : high_status;
        }
        status = _mtbdd_try_make_node(&computed, level, low, high);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return status;
        }
    }

    if (cache_put3(CACHE_BDD_EXISTS, a, variables, 0, computed)) sylvan_stats_count(BDD_EXISTS_CACHEDPUT);

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}


/**
 * Calculate projection of <a> unto <v>
 * (Expects Boolean <a>)
 */
int bdd_project_CALL(lace_worker* lace, BDD *destination, BDD a, BDDSET v)
{
    if (destination == NULL || a == mtbdd_invalid || v == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /**
     * Terminal cases
     */
    if (a == bdd_false) { *destination = bdd_false; return SYLVAN_OK; }
    if (a == bdd_true) { *destination = bdd_true; return SYLVAN_OK; }
    if (bdd_set_is_empty(v)) { *destination = bdd_true; return SYLVAN_OK; }

    /**
     * Obtain variables
     */
    const mtbddnode* a_node = MTBDD_GETNODE(a);
    const uint32_t a_var = mtbddnode_getvariable(a_node);

    /**
     * Skip <vars>
     */
    mtbddnode* v_node = MTBDD_GETNODE(v);
    uint32_t v_var = mtbddnode_getvariable(v_node);
    MTBDD v_next = mtbddnode_followhigh(v, v_node);

    while (v_var < a_var) {
        if (bdd_set_is_empty(v_next)) { *destination = bdd_true; return SYLVAN_OK; }
        v = v_next;
        v_node = MTBDD_GETNODE(v);
        v_var = mtbddnode_getvariable(v_node);
        v_next = mtbddnode_followhigh(v, v_node);
    }

    /**
     * Maybe perform garbage collection
     */
    sylvan_gc_test(lace);

    /**
     * Count operation
     */
    sylvan_stats_count(BDD_PROJECT);

    /**
     * Check the cache
     */
    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_PROJECT, a, 0, v, &computed)) {
        sylvan_stats_count(BDD_PROJECT_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /**
     * Get cofactors
     */
    const MTBDD a0 = mtbddnode_followlow(a, a_node);
    const MTBDD a1 = mtbddnode_followhigh(a, a_node);

    /**
     * Compute recursive result
     */
    BDD low = mtbdd_invalid, high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);

    if (v_var == a_var) {
        // variable in projection variables
        bdd_project_SPAWN(lace, &low, a0, v_next);
        int high_status = bdd_project_CALL(lace, &high, a1, v_next);
        int low_status = bdd_project_SYNC(lace);
        if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return high_status != SYLVAN_OK ? high_status : low_status;
        }
        int status = _mtbdd_try_make_node(&computed, a_var, low, high);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return status;
        }
    } else {
        // variable not in projection variables
        bdd_project_SPAWN(lace, &low, a0, v);
        int high_status = bdd_project_CALL(lace, &high, a1, v);
        int low_status = bdd_project_SYNC(lace);
        if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return high_status != SYLVAN_OK ? high_status : low_status;
        }
        int status = bdd_and_CALL(lace, &computed, bdd_not(low), bdd_not(high));
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return status;
        }
        computed = bdd_not(computed);
    }

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_BDD_PROJECT, a, 0, v, computed)) {
        sylvan_stats_count(BDD_PROJECT_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}


/**
 * Calculate exists(a AND b, v)
 */
int bdd_and_exists_CALL(lace_worker* lace, BDD *destination, BDD a, BDD b, BDDSET v)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || v == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Terminal cases */
    if (a == bdd_false || b == bdd_false || a == bdd_not(b)) {
        *destination = bdd_false;
        return SYLVAN_OK;
    }
    if (a == bdd_true && b == bdd_true) { *destination = bdd_true; return SYLVAN_OK; }

    /* Cases that reduce to "exists" and "and" */
    if (a == bdd_true) return bdd_exists_CALL(lace, destination, b, v);
    if (b == bdd_true || a == b) return bdd_exists_CALL(lace, destination, a, v);
    if (bdd_set_is_empty(v)) return bdd_and_CALL(lace, destination, a, b);

    /* At this point, a and b are proper nodes, and v is non-empty */

    /* Improve for caching */
    if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        BDD t = b;
        b = a;
        a = t;
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_AND_EXISTS);

    // a != constant
    bddnode* na = MTBDD_GETNODE(a);
    bddnode* nb = MTBDD_GETNODE(b);
    bddnode* nv = MTBDD_GETNODE(v);

    uint32_t va = bddnode_getvariable(na);
    uint32_t vb = bddnode_getvariable(nb);
    uint32_t vv = bddnode_getvariable(nv);
    uint32_t level = va < vb ? va : vb;

    /* Skip levels in v that are not in a and b */
    while (vv < level) {
        v = node_high(v, nv); // get next variable in conjunction
        if (bdd_set_is_empty(v)) return bdd_and_CALL(lace, destination, a, b);
        nv = MTBDD_GETNODE(v);
        vv = bddnode_getvariable(nv);
    }

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_AND_EXISTS, a, b, v, &computed)) {
        sylvan_stats_count(BDD_AND_EXISTS_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    // Get cofactors
    BDD aLow, aHigh, bLow, bHigh;
    if (level == va) {
        aLow = node_low(a, na);
        aHigh = node_high(a, na);
    } else {
        aLow = a;
        aHigh = a;
    }
    if (level == vb) {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
    } else {
        bLow = b;
        bHigh = b;
    }

    BDD low = mtbdd_invalid, high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    int status = SYLVAN_OK;

    if (level == vv) {
        // level is in variable set, perform abstraction
        BDD _v = node_high(v, nv);
        status = bdd_and_exists_CALL(lace, &low, aLow, bLow, _v);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return status;
        }
        if (low == bdd_true || low == aHigh || low == bHigh) {
            computed = low;
        } else {
            if (low == bdd_not(aHigh)) {
                status = bdd_exists_CALL(lace, &high, bHigh, _v);
            } else if (low == bdd_not(bHigh)) {
                status = bdd_exists_CALL(lace, &high, aHigh, _v);
            } else {
                status = bdd_and_exists_CALL(lace, &high, aHigh, bHigh, _v);
            }
            if (status != SYLVAN_OK) {
                mtbdd_refs_popptr(3);
                return status;
            }
            if (high == bdd_true) {
                computed = bdd_true;
            } else if (high == bdd_false) {
                computed = low;
            } else if (low == bdd_false) {
                computed = high;
            } else {
                status = bdd_and_CALL(lace, &computed, bdd_not(low), bdd_not(high));
                if (status != SYLVAN_OK) {
                    mtbdd_refs_popptr(3);
                    return status;
                }
                computed = bdd_not(computed);
            }
        }
    } else {
        // level is not in variable set
        bdd_and_exists_SPAWN(lace, &high, aHigh, bHigh, v);
        int low_status = bdd_and_exists_CALL(lace, &low, aLow, bLow, v);
        int high_status = bdd_and_exists_SYNC(lace);
        if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return low_status != SYLVAN_OK ? low_status : high_status;
        }
        status = _mtbdd_try_make_node(&computed, level, low, high);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return status;
        }
    }

    if (cache_put3(CACHE_BDD_AND_EXISTS, a, b, v, computed)) sylvan_stats_count(BDD_AND_EXISTS_CACHEDPUT);

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}


/**
 * Calculate projection of (<a> AND <b>) unto <v>
 * (Expects Boolean <a> and <b>)
 */
int bdd_and_project_CALL(lace_worker* lace, BDD *destination, BDD a, BDD b, BDDSET v)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || v == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /**
     * Terminal cases
     */
    if (a == bdd_false || b == bdd_false || a == bdd_not(b)) {
        *destination = bdd_false;
        return SYLVAN_OK;
    }
    if (a == bdd_true && b == bdd_true) { *destination = bdd_true; return SYLVAN_OK; }
    if (bdd_set_is_empty(v)) {
        *destination = bdd_disjoint_CALL(lace, a, b) ? bdd_false : bdd_true;
        return SYLVAN_OK;
    }

    /**
     * Cases that reduce to bdd_project
     */
    if (a == bdd_true) return bdd_project_CALL(lace, destination, b, v);
    if (b == bdd_true || a == b) return bdd_project_CALL(lace, destination, a, v);

    /**
     * Normalization (only for caching)
     */
    if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        BDD t = b;
        b = a;
        a = t;
    }

    /**
     * Maybe perform garbage collection
     */
    sylvan_gc_test(lace);

    /**
     * Count operation
     */
    sylvan_stats_count(BDD_AND_PROJECT);

    /**
     * Obtain variables
     */
    const mtbddnode* a_node = MTBDD_GETNODE(a);
    const mtbddnode* b_node = MTBDD_GETNODE(b);
    const uint32_t a_var = mtbddnode_getvariable(a_node);
    const uint32_t b_var = mtbddnode_getvariable(b_node);
    const uint32_t minvar = a_var < b_var ? a_var : b_var;

    /**
     * Skip <vars>
     */
    mtbddnode* v_node = MTBDD_GETNODE(v);
    uint32_t v_var = mtbddnode_getvariable(v_node);
    MTBDD v_next = mtbddnode_followhigh(v, v_node);

    while (v_var < minvar) {
        if (bdd_set_is_empty(v_next)) {
            *destination = bdd_disjoint_CALL(lace, a, b) ? bdd_false : bdd_true;
            return SYLVAN_OK;
        }
        v = v_next;
        v_node = MTBDD_GETNODE(v);
        v_var = mtbddnode_getvariable(v_node);
        v_next = mtbddnode_followhigh(v, v_node);
    }

    /**
     * Check the cache
     */
    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_AND_PROJECT, a, b, v, &computed)) {
        sylvan_stats_count(BDD_AND_PROJECT_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /**
     * Get cofactors
     */
    const MTBDD a0 = a_var == minvar ? mtbddnode_followlow(a, a_node) : a;
    const MTBDD a1 = a_var == minvar ? mtbddnode_followhigh(a, a_node) : a;
    const MTBDD b0 = b_var == minvar ? mtbddnode_followlow(b, b_node) : b;
    const MTBDD b1 = b_var == minvar ? mtbddnode_followhigh(b, b_node) : b;

    /**
     * Compute recursive result
     */
    BDD low = mtbdd_invalid, high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);

    if (v_var == minvar) {
        // variable in projection variables
        bdd_and_project_SPAWN(lace, &low, a0, b0, v_next);
        int high_status = bdd_and_project_CALL(lace, &high, a1, b1, v_next);
        int low_status = bdd_and_project_SYNC(lace);
        if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return high_status != SYLVAN_OK ? high_status : low_status;
        }
        int status = _mtbdd_try_make_node(&computed, minvar, low, high);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return status;
        }
    } else {
        // variable not in projection variables
        bdd_and_project_SPAWN(lace, &low, a0, b0, v);
        int high_status = bdd_and_project_CALL(lace, &high, a1, b1, v);
        int low_status = bdd_and_project_SYNC(lace);
        if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return high_status != SYLVAN_OK ? high_status : low_status;
        }
        int status = bdd_and_CALL(lace, &computed, bdd_not(low), bdd_not(high));
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(3);
            return status;
        }
        computed = bdd_not(computed);
    }

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_BDD_AND_PROJECT, a, b, v, computed)) {
        sylvan_stats_count(BDD_AND_PROJECT_CACHEDPUT);
    }

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}


BDD bdd_rel_next_CALL(lace_worker* lace, BDD a, BDD b, BDDSET vars)
{
    /* Compute R(s) = \exists x: A(x) \and B(x,s) with support(result) = s, support(A) = s, support(B) = s+t
     * if vars == bdd_false, then every level is in s or t
     * any other levels (outside s,t) in B are ignored / existentially quantified
     */

    /* Terminals */
    if (a == bdd_true && b == bdd_true) return bdd_true;
    if (a == bdd_false) return bdd_false;
    if (b == bdd_false) return bdd_false;
    if (bdd_set_is_empty(vars)) return a;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_RELNEXT);

    /* Determine top level */
    bddnode* na = bdd_is_leaf(a) ? 0 : MTBDD_GETNODE(a);
    bddnode* nb = bdd_is_leaf(b) ? 0 : MTBDD_GETNODE(b);

    uint32_t va = na ? bddnode_getvariable(na) : 0xffffffff;
    uint32_t vb = nb ? bddnode_getvariable(nb) : 0xffffffff;
    uint32_t level = va < vb ? va : vb;

    /* Skip vars */
    int is_s_or_t = 0;
    bddnode* nv = 0;
    if (vars == bdd_false) {
        is_s_or_t = 1;
    } else {
        nv = MTBDD_GETNODE(vars);
        for (;;) {
            /* check if level is s/t */
            uint32_t vv = bddnode_getvariable(nv);
            if (level == vv || (level^1) == vv) {
                is_s_or_t = 1;
                break;
            }
            /* check if level < s/t */
            if (level < vv) break;
            vars = node_high(vars, nv); // get next in vars
            if (bdd_set_is_empty(vars)) return a;
            nv = MTBDD_GETNODE(vars);
        }
    }

    /* Consult cache */
    BDD result;
    if (cache_get3(CACHE_BDD_RELNEXT, a, b, vars, &result)) {
        sylvan_stats_count(BDD_RELNEXT_CACHED);
        return result;
    }

    if (is_s_or_t) {
        /* Get s and t */
        uint32_t s = level & ~UINT32_C(1);
        uint32_t t = s+1;

        BDD a0, a1, b0, b1;
        if (na && va == s) {
            a0 = node_low(a, na);
            a1 = node_high(a, na);
        } else {
            a0 = a1 = a;
        }
        if (nb && vb == s) {
            b0 = node_low(b, nb);
            b1 = node_high(b, nb);
        } else {
            b0 = b1 = b;
        }

        BDD b00, b01, b10, b11;
        if (!bdd_is_leaf(b0)) {
            bddnode* nb0 = MTBDD_GETNODE(b0);
            if (bddnode_getvariable(nb0) == t) {
                b00 = node_low(b0, nb0);
                b01 = node_high(b0, nb0);
            } else {
                b00 = b01 = b0;
            }
        } else {
            b00 = b01 = b0;
        }
        if (!bdd_is_leaf(b1)) {
            bddnode* nb1 = MTBDD_GETNODE(b1);
            if (bddnode_getvariable(nb1) == t) {
                b10 = node_low(b1, nb1);
                b11 = node_high(b1, nb1);
            } else {
                b10 = b11 = b1;
            }
        } else {
            b10 = b11 = b1;
        }

        BDD _vars = vars == bdd_false ? bdd_false : node_high(vars, nv);

        mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a0, b00, _vars));
        mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a1, b10, _vars));
        mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a0, b01, _vars));
        mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a1, b11, _vars));

        BDD f = mtbdd_refs_sync(bdd_rel_next_SYNC(lace)); mtbdd_refs_push(f);
        BDD e = mtbdd_refs_sync(bdd_rel_next_SYNC(lace)); mtbdd_refs_push(e);
        BDD d = mtbdd_refs_sync(bdd_rel_next_SYNC(lace)); mtbdd_refs_push(d);
        BDD c = mtbdd_refs_sync(bdd_rel_next_SYNC(lace)); mtbdd_refs_push(c);

        mtbdd_refs_spawn(bdd_ite_legacy_SPAWN(lace, c, bdd_true, d)); /* a0 b00  \or  a1 b01 */
        mtbdd_refs_spawn(bdd_ite_legacy_SPAWN(lace, e, bdd_true, f)); /* a0 b01  \or  a1 b11 */

        /* R1 */ d = mtbdd_refs_sync(bdd_ite_legacy_SYNC(lace)); mtbdd_refs_push(d);
        /* R0 */ c = mtbdd_refs_sync(bdd_ite_legacy_SYNC(lace)); // not necessary: mtbdd_refs_push(c);

        mtbdd_refs_pop(5);
        result = mtbdd_make_node(s, c, d);
    } else {
        /* Variable not in vars! Take a, quantify b */
        BDD a0, a1, b0, b1;
        if (na && va == level) {
            a0 = node_low(a, na);
            a1 = node_high(a, na);
        } else {
            a0 = a1 = a;
        }
        if (nb && vb == level) {
            b0 = node_low(b, nb);
            b1 = node_high(b, nb);
        } else {
            b0 = b1 = b;
        }

        if (b0 != b1) {
            if (a0 == a1) {
                /* Quantify "b" variables */
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a0, b0, vars));
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a1, b1, vars));

                BDD r1 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r1);
                BDD r0 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r0);
                result = bdd_not(bdd_and_legacy_CALL(lace, bdd_not(r0), bdd_not(r1)));
                mtbdd_refs_pop(2);
            } else {
                /* Quantify "b" variables, but keep "a" variables */
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a0, b0, vars));
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a0, b1, vars));
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a1, b0, vars));
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a1, b1, vars));

                BDD r11 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r11);
                BDD r10 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r10);
                BDD r01 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r01);
                BDD r00 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r00);

                mtbdd_refs_spawn(bdd_ite_legacy_SPAWN(lace, r00, bdd_true, r01));
                mtbdd_refs_spawn(bdd_ite_legacy_SPAWN(lace, r10, bdd_true, r11));

                BDD r1 = mtbdd_refs_sync(bdd_ite_legacy_SYNC(lace));
                mtbdd_refs_push(r1);
                BDD r0 = mtbdd_refs_sync(bdd_ite_legacy_SYNC(lace));
                mtbdd_refs_pop(5);

                result = mtbdd_make_node(level, r0, r1);
            }
        } else {
            /* Keep "a" variables */
            mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a0, b0, vars));
            mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a1, b1, vars));

            BDD r1 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
            mtbdd_refs_push(r1);
            BDD r0 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
            mtbdd_refs_pop(1);
            result = mtbdd_make_node(level, r0, r1);
        }
    }

    if (cache_put3(CACHE_BDD_RELNEXT, a, b, vars, result)) sylvan_stats_count(BDD_RELNEXT_CACHEDPUT);

    return result;
}

BDD bdd_rel_prev_CALL(lace_worker* lace, BDD a, BDD b, BDDSET vars)
{
    /* Compute \exists x: A(s,x) \and B(x,t)
     * if vars == bdd_false, then every level is in s or t
     * any other levels (outside s,t) in A are ignored / existentially quantified
     */

    /* Terminals */
    if (a == bdd_true && b == bdd_true) return bdd_true;
    if (a == bdd_false) return bdd_false;
    if (b == bdd_false) return bdd_false;
    if (bdd_set_is_empty(vars)) return b;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_RELPREV);

    /* Determine top level */
    bddnode* na = bdd_is_leaf(a) ? 0 : MTBDD_GETNODE(a);
    bddnode* nb = bdd_is_leaf(b) ? 0 : MTBDD_GETNODE(b);

    uint32_t va = na ? bddnode_getvariable(na) : 0xffffffff;
    uint32_t vb = nb ? bddnode_getvariable(nb) : 0xffffffff;
    uint32_t level = va < vb ? va : vb;

    /* Skip vars */
    int is_s_or_t = 0;
    bddnode* nv = 0;
    if (vars == bdd_false) {
        is_s_or_t = 1;
    } else {
        nv = MTBDD_GETNODE(vars);
        for (;;) {
            /* check if level is s/t */
            uint32_t vv = bddnode_getvariable(nv);
            if (level == vv || (level^1) == vv) {
                is_s_or_t = 1;
                break;
            }
            /* check if level < s/t */
            if (level < vv) break;
            vars = node_high(vars, nv); // get next in vars
            if (bdd_set_is_empty(vars)) return b;
            nv = MTBDD_GETNODE(vars);
        }
    }

    /* Consult cache */
    BDD result;
    if (cache_get3(CACHE_BDD_RELPREV, a, b, vars, &result)) {
        sylvan_stats_count(BDD_RELPREV_CACHED);
        return result;
    }

    if (is_s_or_t) {
        /* Get s and t */
        uint32_t s = level & ~UINT32_C(1);
        uint32_t t = s+1;

        BDD a0, a1, b0, b1;
        if (na && va == s) {
            a0 = node_low(a, na);
            a1 = node_high(a, na);
        } else {
            a0 = a1 = a;
        }
        if (nb && vb == s) {
            b0 = node_low(b, nb);
            b1 = node_high(b, nb);
        } else {
            b0 = b1 = b;
        }

        BDD a00, a01, a10, a11;
        if (!bdd_is_leaf(a0)) {
            bddnode* na0 = MTBDD_GETNODE(a0);
            if (bddnode_getvariable(na0) == t) {
                a00 = node_low(a0, na0);
                a01 = node_high(a0, na0);
            } else {
                a00 = a01 = a0;
            }
        } else {
            a00 = a01 = a0;
        }
        if (!bdd_is_leaf(a1)) {
            bddnode* na1 = MTBDD_GETNODE(a1);
            if (bddnode_getvariable(na1) == t) {
                a10 = node_low(a1, na1);
                a11 = node_high(a1, na1);
            } else {
                a10 = a11 = a1;
            }
        } else {
            a10 = a11 = a1;
        }

        BDD b00, b01, b10, b11;
        if (!bdd_is_leaf(b0)) {
            bddnode* nb0 = MTBDD_GETNODE(b0);
            if (bddnode_getvariable(nb0) == t) {
                b00 = node_low(b0, nb0);
                b01 = node_high(b0, nb0);
            } else {
                b00 = b01 = b0;
            }
        } else {
            b00 = b01 = b0;
        }
        if (!bdd_is_leaf(b1)) {
            bddnode* nb1 = MTBDD_GETNODE(b1);
            if (bddnode_getvariable(nb1) == t) {
                b10 = node_low(b1, nb1);
                b11 = node_high(b1, nb1);
            } else {
                b10 = b11 = b1;
            }
        } else {
            b10 = b11 = b1;
        }

        BDD _vars;
        if (vars != bdd_false) {
            _vars = node_high(vars, nv);
            if (bdd_set_first(_vars) == t) _vars = bdd_set_next(_vars);
        } else {
            _vars = bdd_false;
        }

        if (b00 == b01) {
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a00, b0, _vars));
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a10, b0, _vars));
        } else {
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a00, b00, _vars));
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a00, b01, _vars));
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a10, b00, _vars));
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a10, b01, _vars));
        }

        if (b10 == b11) {
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a01, b1, _vars));
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a11, b1, _vars));
        } else {
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a01, b10, _vars));
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a01, b11, _vars));
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a11, b10, _vars));
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a11, b11, _vars));
        }

        BDD r00, r01, r10, r11;

        if (b10 == b11) {
            r11 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            r01 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
        } else {
            BDD r111 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            BDD r110 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            r11 = mtbdd_make_node(t, r110, r111);
            mtbdd_refs_pop(2);
            mtbdd_refs_push(r11);
            BDD r011 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            BDD r010 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            r01 = mtbdd_make_node(t, r010, r011);
            mtbdd_refs_pop(2);
            mtbdd_refs_push(r01);
        }

        if (b00 == b01) {
            r10 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            r00 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
        } else {
            BDD r101 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            BDD r100 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            r10 = mtbdd_make_node(t, r100, r101);
            mtbdd_refs_pop(2);
            mtbdd_refs_push(r10);
            BDD r001 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            BDD r000 = mtbdd_refs_push(mtbdd_refs_sync(bdd_rel_prev_SYNC(lace)));
            r00 = mtbdd_make_node(t, r000, r001);
            mtbdd_refs_pop(2);
            mtbdd_refs_push(r00);
         }

        mtbdd_refs_spawn(bdd_and_legacy_SPAWN(lace, bdd_not(r00), bdd_not(r01)));
        mtbdd_refs_spawn(bdd_and_legacy_SPAWN(lace, bdd_not(r10), bdd_not(r11)));

        BDD r1 = bdd_not(mtbdd_refs_push(mtbdd_refs_sync(bdd_and_legacy_SYNC(lace))));
        BDD r0 = bdd_not(mtbdd_refs_sync(bdd_and_legacy_SYNC(lace)));
        mtbdd_refs_pop(5);
        result = mtbdd_make_node(s, r0, r1);
    } else {
        BDD a0, a1, b0, b1;
        if (na && va == level) {
            a0 = node_low(a, na);
            a1 = node_high(a, na);
        } else {
            a0 = a1 = a;
        }
        if (nb && vb == level) {
            b0 = node_low(b, nb);
            b1 = node_high(b, nb);
        } else {
            b0 = b1 = b;
        }

        if (a0 != a1) {
            if (b0 == b1) {
                /* Quantify "a" variables */
                mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a0, b0, vars));
                mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a1, b1, vars));

                BDD r1 = mtbdd_refs_sync(bdd_rel_prev_SYNC(lace));
                mtbdd_refs_push(r1);
                BDD r0 = mtbdd_refs_sync(bdd_rel_prev_SYNC(lace));
                mtbdd_refs_push(r0);
                result = bdd_ite_legacy_CALL(lace, r0, bdd_true, r1);
                mtbdd_refs_pop(2);

            } else {
                /* Quantify "a" variables, but keep "b" variables */
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a0, b0, vars));
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a1, b0, vars));
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a0, b1, vars));
                mtbdd_refs_spawn(bdd_rel_next_SPAWN(lace, a1, b1, vars));

                BDD r11 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r11);
                BDD r01 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r01);
                BDD r10 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r10);
                BDD r00 = mtbdd_refs_sync(bdd_rel_next_SYNC(lace));
                mtbdd_refs_push(r00);

                mtbdd_refs_spawn(bdd_ite_legacy_SPAWN(lace, r00, bdd_true, r10));
                mtbdd_refs_spawn(bdd_ite_legacy_SPAWN(lace, r01, bdd_true, r11));

                BDD r1 = mtbdd_refs_sync(bdd_ite_legacy_SYNC(lace));
                mtbdd_refs_push(r1);
                BDD r0 = mtbdd_refs_sync(bdd_ite_legacy_SYNC(lace));
                mtbdd_refs_pop(5);

                result = mtbdd_make_node(level, r0, r1);
            }
        } else {
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a0, b0, vars));
            mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a1, b1, vars));

            BDD r1 = mtbdd_refs_sync(bdd_rel_prev_SYNC(lace));
            mtbdd_refs_push(r1);
            BDD r0 = mtbdd_refs_sync(bdd_rel_prev_SYNC(lace));
            mtbdd_refs_pop(1);
            result = mtbdd_make_node(level, r0, r1);
        }
    }

    if (cache_put3(CACHE_BDD_RELPREV, a, b, vars, result)) sylvan_stats_count(BDD_RELPREV_CACHEDPUT);

    return result;
}

/**
 * Computes the transitive closure by traversing the BDD recursively.
 * See Y. Matsunaga, P. C. McGeer, R. K. Brayton
 *     On Computing the Transitive Closre of a State Transition Relation
 *     30th ACM Design Automation Conference, 1993.
 */
BDD bdd_transitive_closure_CALL(lace_worker* lace, BDD a)
{
    /* Terminals */
    if (a == bdd_true) return a;
    if (a == bdd_false) return a;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_CLOSURE);

    /* Determine top level */
    bddnode* n = MTBDD_GETNODE(a);
    uint32_t level = bddnode_getvariable(n);

    /* Consult cache */
    BDD result;
    if (cache_get3(CACHE_BDD_CLOSURE, a, 0, 0, &result)) {
        sylvan_stats_count(BDD_CLOSURE_CACHED);
        return result;
    }

    uint32_t s = level & ~UINT32_C(1);
    uint32_t t = s+1;

    BDD a0, a1;
    if (level == s) {
        a0 = node_low(a, n);
        a1 = node_high(a, n);
    } else {
        a0 = a1 = a;
    }

    BDD a00, a01, a10, a11;
    if (!bdd_is_leaf(a0)) {
        bddnode* na0 = MTBDD_GETNODE(a0);
        if (bddnode_getvariable(na0) == t) {
            a00 = node_low(a0, na0);
            a01 = node_high(a0, na0);
        } else {
            a00 = a01 = a0;
        }
    } else {
        a00 = a01 = a0;
    }
    if (!bdd_is_leaf(a1)) {
        bddnode* na1 = MTBDD_GETNODE(a1);
        if (bddnode_getvariable(na1) == t) {
            a10 = node_low(a1, na1);
            a11 = node_high(a1, na1);
        } else {
            a10 = a11 = a1;
        }
    } else {
        a10 = a11 = a1;
    }

    BDD u1 = bdd_transitive_closure_CALL(lace, a11);
    mtbdd_refs_push(u1);
    /* u3 = */ mtbdd_refs_spawn(bdd_rel_prev_SPAWN(lace, a01, u1, bdd_false));
    BDD u2 = bdd_rel_prev_CALL(lace, u1, a10, bdd_false);
    mtbdd_refs_push(u2);
    BDD e = bdd_rel_prev_CALL(lace, a01, u2, bdd_false);
    mtbdd_refs_push(e);
    e = bdd_ite_legacy_CALL(lace, a00, bdd_true, e);
    mtbdd_refs_pop(1);
    mtbdd_refs_push(e);
    e = bdd_transitive_closure_CALL(lace, e);
    mtbdd_refs_pop(1);
    mtbdd_refs_push(e);
    BDD g = bdd_rel_prev_CALL(lace, u2, e, bdd_false);
    mtbdd_refs_push(g);
    BDD u3 = mtbdd_refs_sync(bdd_rel_prev_SYNC(lace));
    mtbdd_refs_push(u3);
    BDD f = bdd_rel_prev_CALL(lace, e, u3, bdd_false);
    mtbdd_refs_push(f);
    BDD h = bdd_rel_prev_CALL(lace, u2, f, bdd_false);
    mtbdd_refs_push(h);
    h = bdd_ite_legacy_CALL(lace, u1, bdd_true, h);
    mtbdd_refs_pop(1);
    mtbdd_refs_push(h);

    BDD r0, r1;
    /* R0 */ r0 = mtbdd_make_node(t, e, f);
    mtbdd_refs_pop(7);
    mtbdd_refs_push(r0);
    /* R1 */ r1 = mtbdd_make_node(t, g, h);
    mtbdd_refs_pop(1);
    result = mtbdd_make_node(s, r0, r1);

    if (cache_put3(CACHE_BDD_CLOSURE, a, 0, 0, result)) sylvan_stats_count(BDD_CLOSURE_CACHEDPUT);

    return result;
}


/**
 * Function composition
 */
BDD bdd_compose_CALL(lace_worker* lace, BDD a, MTBDDMAP map)
{
    /* Trivial cases */
    if (a == bdd_false || a == bdd_true) return a;
    if (mtbdd_map_is_empty(map)) return a;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_COMPOSE);

    /* Determine top level */
    bddnode* n = MTBDD_GETNODE(a);
    uint32_t level = bddnode_getvariable(n);

    /* Skip map */
    bddnode* map_node = MTBDD_GETNODE(map);
    uint32_t map_var = bddnode_getvariable(map_node);
    while (map_var < level) {
        map = node_low(map, map_node);
        if (mtbdd_map_is_empty(map)) return a;
        map_node = MTBDD_GETNODE(map);
        map_var = bddnode_getvariable(map_node);
    }

    /* Consult cache */
    BDD result;
    if (cache_get3(CACHE_BDD_COMPOSE, a, map, 0, &result)) {
        sylvan_stats_count(BDD_COMPOSE_CACHED);
        return result;
    }

    /* Recursively calculate low and high */
    mtbdd_refs_spawn(bdd_compose_SPAWN(lace, node_low(a, n), map));
    BDD high = bdd_compose_CALL(lace, node_high(a, n), map);
    mtbdd_refs_push(high);
    BDD low = mtbdd_refs_sync(bdd_compose_SYNC(lace));
    mtbdd_refs_push(low);

    /* Calculate result */
    BDD root = map_var == level ? node_high(map, map_node) : bdd_var_at_level(level);
    mtbdd_refs_push(root);
    result = bdd_ite_legacy_CALL(lace, root, high, low);
    mtbdd_refs_pop(3);

    if (cache_put3(CACHE_BDD_COMPOSE, a, map, 0, result)) sylvan_stats_count(BDD_COMPOSE_CACHEDPUT);

    return result;
}

/**
 * Calculate the number of distinct paths to True.
 */
double bdd_path_count_CALL(lace_worker* lace, BDD bdd)
{
    /* Trivial cases */
    if (bdd == bdd_false) return 0.0;
    if (bdd == bdd_true) return 1.0;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_PATHCOUNT);

    /* Consult cache */
    double result;
    if (cache_get3(CACHE_BDD_PATHCOUNT, bdd, 0, 0, (uint64_t*)&result)) {
        sylvan_stats_count(BDD_PATHCOUNT_CACHED);
        return result;
    }

    bdd_path_count_SPAWN(lace, mtbdd_node_low(bdd));
    bdd_path_count_SPAWN(lace, mtbdd_node_high(bdd));
    result = bdd_path_count_SYNC(lace);
    result += bdd_path_count_SYNC(lace);

    if (cache_put3(CACHE_BDD_PATHCOUNT, bdd, 0, 0, *(uint64_t*)&result)) sylvan_stats_count(BDD_PATHCOUNT_CACHEDPUT);

    return result;
}

/**
 * Calculate the number of satisfying variable assignments according to <variables>.
 */
double bdd_sat_count_CALL(lace_worker* lace, BDD bdd, BDDSET variables)
{
    /* Trivial cases */
    if (bdd == bdd_false) return 0.0;
    if (bdd == bdd_true) return (double)powl(2.0L, (long double)bdd_set_count(variables));

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_SATCOUNT);

    /* Count variables before var(bdd) */
    size_t skipped = 0;
    uint32_t var = mtbdd_node_variable(bdd);
    bddnode* set_node = MTBDD_GETNODE(variables);
    uint32_t set_var = bddnode_getvariable(set_node);
    while (var != set_var) {
        skipped++;
        variables = node_high(variables, set_node);
        // if this assertion fails, then variables is not the support of <bdd>
        assert(!bdd_set_is_empty(variables));
        set_node = MTBDD_GETNODE(variables);
        set_var = bddnode_getvariable(set_node);
    }

    union {
        double d;
        uint64_t s;
    } hack;

    /* Consult cache */
    if (cache_get3(CACHE_BDD_SATCOUNT, bdd, variables, 0, &hack.s)) {
        sylvan_stats_count(BDD_SATCOUNT_CACHED);
        return (double)((long double)hack.d * powl(2.0L, (long double)skipped));
    }

    bdd_sat_count_SPAWN(lace, mtbdd_node_high(bdd), node_high(variables, set_node));
    double low = bdd_sat_count_CALL(lace, mtbdd_node_low(bdd), node_high(variables, set_node));
    double result = low + bdd_sat_count_SYNC(lace);

    hack.d = result;
    if (cache_put3(CACHE_BDD_SATCOUNT, bdd, variables, 0, hack.s)) sylvan_stats_count(BDD_SATCOUNT_CACHEDPUT);

    return (double)((long double)result * powl(2.0L, (long double)skipped));
}

int
bdd_pick_cube_values(BDD bdd, BDDSET vars, uint8_t *str)
{
    if (bdd == bdd_false) return 0;
    if (str == NULL) return 0;
    if (bdd_set_is_empty(vars)) return 1;

    for (;;) {
        bddnode* n_vars = MTBDD_GETNODE(vars);
        const uint32_t var = bddnode_getvariable(n_vars);
        while (bdd != bdd_true && bddnode_getvariable(MTBDD_GETNODE(bdd)) < var) {
            bddnode *n_bdd = MTBDD_GETNODE(bdd);
            bdd = node_low(bdd, n_bdd) != bdd_false
                ? node_low(bdd, n_bdd)
                : node_high(bdd, n_bdd);
        }
        if (bdd == bdd_true || bddnode_getvariable(MTBDD_GETNODE(bdd)) > var) {
            *str = 2;
        } else {
            bddnode *n_bdd = MTBDD_GETNODE(bdd);
            if (node_low(bdd, n_bdd) != bdd_false) {
                *str = 0;
                bdd = node_low(bdd, n_bdd);
            } else {
                *str = 1;
                bdd = node_high(bdd, n_bdd);
            }
        }
        vars = node_high(vars, n_vars);
        if (bdd_set_is_empty(vars)) break;
        str++;
    }

    return 1;
}

BDD
bdd_pick_minterm(BDD bdd, BDDSET vars)
{
    if (bdd == bdd_false) return bdd_false;
    if (bdd_set_is_empty(vars)) return bdd_true;

    bddnode* n_vars = MTBDD_GETNODE(vars);
    uint32_t var = bddnode_getvariable(n_vars);
    BDD next_vars = node_high(vars, n_vars);
    if (bdd == bdd_true) {
        // take false
        BDD res = bdd_pick_minterm(bdd, next_vars);
        return mtbdd_make_node(var, res, bdd_false);
    }
    bddnode* n_bdd = MTBDD_GETNODE(bdd);
    while (bddnode_getvariable(n_bdd) < var) {
        bdd = node_low(bdd, n_bdd) != bdd_false
            ? node_low(bdd, n_bdd)
            : node_high(bdd, n_bdd);
        if (bdd == bdd_true) return bdd_pick_minterm(bdd, vars);
        n_bdd = MTBDD_GETNODE(bdd);
    }
    if (bddnode_getvariable(n_bdd) != var) {
        assert(bddnode_getvariable(n_bdd)>var);
        // take false
        BDD res = bdd_pick_minterm(bdd, next_vars);
        return mtbdd_make_node(var, res, bdd_false);
    }
    if (node_high(bdd, n_bdd) == bdd_false) {
        // take false
        BDD res = bdd_pick_minterm(node_low(bdd, n_bdd), next_vars);
        return mtbdd_make_node(var, res, bdd_false);
    }
    // take true
    BDD res = bdd_pick_minterm(node_high(bdd, n_bdd), next_vars);
    return mtbdd_make_node(var, bdd_false, res);
}

BDD
bdd_pick_cube(BDD bdd, BDDSET vars)
{
    if (bdd == bdd_false) return bdd_false;
    if (bdd == bdd_true || bdd_set_is_empty(vars)) return bdd_true;

    bddnode* node = MTBDD_GETNODE(bdd);
    bddnode* vars_node = MTBDD_GETNODE(vars);
    uint32_t bdd_var = bddnode_getvariable(node);
    uint32_t var = bddnode_getvariable(vars_node);
    BDDSET next_vars = node_high(vars, vars_node);

    if (var < bdd_var) return bdd_pick_cube(bdd, next_vars);

    BDD low = node_low(bdd, node);
    BDD high = node_high(bdd, node);
    BDD chosen = low != bdd_false ? low : high;

    if (bdd_var < var) return bdd_pick_cube(chosen, vars);

    BDD result = bdd_pick_cube(chosen, next_vars);
    return low != bdd_false
        ? mtbdd_make_node(var, result, bdd_false)
        : mtbdd_make_node(var, bdd_false, result);
}

BDD
bdd_cube(BDDSET vars, uint8_t *cube)
{
    if (bdd_set_is_empty(vars)) return bdd_true;

    bddnode* n = MTBDD_GETNODE(vars);
    uint32_t v = bddnode_getvariable(n);
    vars = node_high(vars, n);

    BDD result = bdd_cube(vars, cube+1);
    if (*cube == 0) {
        result = mtbdd_make_node(v, result, bdd_false);
    } else if (*cube == 1) {
        result = mtbdd_make_node(v, bdd_false, result);
    }

    return result;
}

BDD bdd_or_cube_CALL(lace_worker* lace, BDD bdd, BDDSET vars, uint8_t * cube)
{
    /* Terminal cases */
    if (bdd == bdd_true) return bdd_true;
    if (bdd == bdd_false) return bdd_cube(vars, cube);
    if (bdd_set_is_empty(vars)) return bdd_true;

    bddnode* nv = MTBDD_GETNODE(vars);

    for (;;) {
        if (*cube == 0 || *cube == 1) break;
        // *cube should be 2
        cube++;
        vars = node_high(vars, nv);
        if (bdd_set_is_empty(vars)) return bdd_true;
        nv = MTBDD_GETNODE(vars);
    }

    sylvan_gc_test(lace);

    // missing: SV_CNT_OP FIXME

    bddnode* n = MTBDD_GETNODE(bdd);
    BDD result = bdd;
    uint32_t v = bddnode_getvariable(nv);
    uint32_t n_level = bddnode_getvariable(n);

    if (v < n_level) {
        vars = node_high(vars, nv);
        if (*cube == 0) {
            result = bdd_or_cube_CALL(lace, bdd, vars, cube+1);
            result = mtbdd_make_node(v, result, bdd);
        } else /* *cube == 1 */ {
            result = bdd_or_cube_CALL(lace, bdd, vars, cube+1);
            result = mtbdd_make_node(v, bdd, result);
        }
    } else if (v > n_level) {
        BDD high = node_high(bdd, n);
        BDD low = node_low(bdd, n);
        mtbdd_refs_spawn(bdd_or_cube_SPAWN(lace, high, vars, cube));
        BDD new_low = bdd_or_cube_CALL(lace, low, vars, cube);
        mtbdd_refs_push(new_low);
        BDD new_high = mtbdd_refs_sync(bdd_or_cube_SYNC(lace));
        mtbdd_refs_pop(1);
        if (new_low != low || new_high != high) {
            result = mtbdd_make_node(n_level, new_low, new_high);
        }
    } else /* v == n_level */ {
        vars = node_high(vars, nv);
        BDD high = node_high(bdd, n);
        BDD low = node_low(bdd, n);
        if (*cube == 0) {
            BDD new_low = bdd_or_cube(low, vars, cube+1);
            if (new_low != low) {
                result = mtbdd_make_node(n_level, new_low, high);
            }
        } else /* *cube == 1 */ {
            BDD new_high = bdd_or_cube(high, vars, cube+1);
            if (new_high != high) {
                result = mtbdd_make_node(n_level, low, new_high);
            }
        }
    }

    return result;
}

struct bdd_path
{
    struct bdd_path *prev;
    uint32_t var;
    uint8_t val; // 0=false, 1=true, 2=both
};

void bdd_enum_do_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_enumerate_cb cb, void* context, struct bdd_path* path)
{
    if (bdd == bdd_false) return;

    if (bdd_set_is_empty(vars)) {
        /* bdd should now be true */
        assert(bdd == bdd_true);
        /* compute length of path */
        int i=0;
        struct bdd_path *pp;
        for (pp = path; pp != NULL; pp = pp->prev) i++;
        /* if length is 0 (enum called with empty vars??), return */
        if (i == 0) return;
        /* fill cube and vars with trace */
        void* mark = lace_scratch_mark(lace);
        uint8_t* cube = LACE_SCRATCH_ARRAY(lace, uint8_t, i);
        uint32_t* path_vars = LACE_SCRATCH_ARRAY(lace, uint32_t, i);
        int j=0;
        for (pp = path; pp != NULL; pp = pp->prev) {
            cube[i-j-1] = pp->val;
            path_vars[i-j-1] = pp->var;
            j++;
        }
        /* call callback */
        cb(context, path_vars, cube, i);
        lace_scratch_reset(lace, mark);
        return;
    }

    uint32_t var = mtbdd_node_variable(vars);
    vars = bdd_set_next(vars);
    uint32_t bdd_var = mtbdd_node_variable(bdd);

    /* assert var <= bdd_var */
    if (bdd == bdd_true || var < bdd_var) {
        struct bdd_path pp0 = (struct bdd_path){path, var, 0};
        bdd_enum_do_CALL(lace, bdd, vars, cb, context, &pp0);
        struct bdd_path pp1 = (struct bdd_path){path, var, 1};
        bdd_enum_do_CALL(lace, bdd, vars, cb, context, &pp1);
    } else if (var == bdd_var) {
        struct bdd_path pp0 = (struct bdd_path){path, var, 0};
        bdd_enum_do_CALL(lace, mtbdd_node_low(bdd), vars, cb, context, &pp0);
        struct bdd_path pp1 = (struct bdd_path){path, var, 1};
        bdd_enum_do_CALL(lace, mtbdd_node_high(bdd), vars, cb, context, &pp1);
    } else {
        printf("var %u not expected (expecting %u)!\n", bdd_var, var);
        assert(var <= bdd_var);
    }
}

TASK(void, bdd_enum_par_do, BDD, bdd, BDDSET, vars, bdd_enumerate_cb, cb, void*, context, struct bdd_path*, path)

void bdd_enum_par_do_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_enumerate_cb cb, void* context, struct bdd_path* path)
{
    if (bdd == bdd_false) return;

    if (bdd_set_is_empty(vars)) {
        /* bdd should now be true */
        assert(bdd == bdd_true);
        /* compute length of path */
        int i=0;
        struct bdd_path *pp;
        for (pp = path; pp != NULL; pp = pp->prev) i++;
        /* if length is 0 (enum called with empty vars??), return */
        if (i == 0) return;
        /* fill cube and vars with trace */
        void* mark = lace_scratch_mark(lace);
        uint8_t* cube = LACE_SCRATCH_ARRAY(lace, uint8_t, i);
        uint32_t* path_vars = LACE_SCRATCH_ARRAY(lace, uint32_t, i);
        int j=0;
        for (pp = path; pp != NULL; pp = pp->prev) {
            cube[i-j-1] = pp->val;
            path_vars[i-j-1] = pp->var;
            j++;
        }
        /* call callback */
        cb(context, path_vars, cube, i);
        lace_scratch_reset(lace, mark);
        return;
    }

    uint32_t var = mtbdd_node_variable(vars);
    vars = bdd_set_next(vars);
    uint32_t bdd_var = mtbdd_node_variable(bdd);

    /* assert var <= bdd_var */
    if (var < bdd_var) {
        struct bdd_path pp1 = (struct bdd_path){path, var, 1};
        bdd_enum_par_do_SPAWN(lace, bdd, vars, cb, context, &pp1);
        struct bdd_path pp0 = (struct bdd_path){path, var, 0};
        bdd_enum_par_do_CALL(lace, bdd, vars, cb, context, &pp0);
        bdd_enum_par_do_SYNC(lace);
    } else if (var == bdd_var) {
        struct bdd_path pp1 = (struct bdd_path){path, var, 1};
        bdd_enum_par_do_SPAWN(lace, mtbdd_node_high(bdd), vars, cb, context, &pp1);
        struct bdd_path pp0 = (struct bdd_path){path, var, 0};
        bdd_enum_par_do_CALL(lace, mtbdd_node_low(bdd), vars, cb, context, &pp0);
        bdd_enum_par_do_SYNC(lace);
    } else {
        assert(var <= bdd_var);
    }
}

//FIXME clean this up
void bdd_enumerate_minterms_CALL(lace_worker* lace, BDD bdd, BDDSET  vars, bdd_enumerate_cb cb, void* context)
{
    bdd_enum_do_CALL(lace, bdd, vars, cb, context, 0);
}

void bdd_enumerate_minterms_parallel_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_enumerate_cb cb, void* context)
{
    bdd_enum_par_do_CALL(lace, bdd, vars, cb, context, 0);
}

TASK(BDD, bdd_collect_do, BDD, bdd, BDDSET, vars, bdd_map_reduce_or_cb, cb, void*, context, struct bdd_path*, path)

BDD bdd_collect_do_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_map_reduce_or_cb cb, void* context, struct bdd_path* path)
{
    if (bdd == bdd_false) {
         return bdd_false;
    } else if (bdd_set_is_empty(vars)) {
        /**
         * Compute trace length
         */
        size_t len = 0;
        struct bdd_path *p = path;
        while (p != NULL) {
            len++;
            p = p->prev;
        }
        /**
         * Fill array
         */
        void* mark = lace_scratch_mark(lace);
        uint8_t* arr = LACE_SCRATCH_ARRAY(lace, uint8_t, len);
        for (size_t i=0; i<len; i++) {
            arr[len-i-1] = path->val;
            path = path->prev;
        }
        /**
         * Call callback
         */
        BDD result = cb(context, arr);
        lace_scratch_reset(lace, mark);
        return result;
    } else {
        /**
         * Obtain domain variable
         */
        const uint32_t dom_var = mtbdd_node_variable(vars);
        const BDD dom_next = bdd_set_next(vars);
        /**
         * Obtain cofactors
         */
        BDD bdd0, bdd1;
        if (bdd == bdd_true) {
            bdd0 = bdd1 = bdd;
        } else {
            const uint32_t bdd_var = mtbdd_node_variable(bdd);
            assert(dom_var <= bdd_var);
            if (dom_var < bdd_var) {
                bdd0 = bdd1 = bdd;
            } else {
                bdd0 = mtbdd_node_low(bdd);
                bdd1 = mtbdd_node_high(bdd);
            }
       }
        /**
         * Call recursive functions
         */
        struct bdd_path p0 = (struct bdd_path){path, dom_var, 0};
        struct bdd_path p1 = (struct bdd_path){path, dom_var, 1};
        mtbdd_refs_spawn(bdd_collect_do_SPAWN(lace, bdd1, dom_next, cb, context, &p1));
        BDD low = mtbdd_refs_push(bdd_collect_do_CALL(lace, bdd0, dom_next, cb, context, &p0));
        BDD high = mtbdd_refs_push(mtbdd_refs_sync(bdd_collect_do_SYNC(lace)));
        BDD res = bdd_not(bdd_and_legacy_CALL(lace, bdd_not(low), bdd_not(high)));
        mtbdd_refs_pop(2);
        return res;
    }
}

// FIXME we don't need the extra indirection?
BDD bdd_map_reduce_or_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_map_reduce_or_cb cb, void* context)
{
    return bdd_collect_do_CALL(lace, bdd, vars, cb, context, NULL);
}

/**
 * SERIALIZATION
 */

struct bdd_ser {
    BDD bdd;
    size_t assigned;
};

// Define a AVL tree type with prefix 'bdd_ser' holding
// nodes of struct bdd_ser with the following compare() function...
AVL(bdd_ser, struct bdd_ser)
{
    if (left->bdd > right->bdd) return 1;
    if (left->bdd < right->bdd) return -1;
    return 0;
}

// Define a AVL tree type with prefix 'bdd_ser_reversed' holding
// nodes of struct bdd_ser with the following compare() function...
AVL(bdd_ser_reversed, struct bdd_ser)
{
    if (left->assigned > right->assigned) return 1;
    if (left->assigned < right->assigned) return -1;
    return 0;
}

// Initially, both sets are empty
static avl_node *bdd_ser_set = NULL;
static avl_node *bdd_ser_reversed_set = NULL;

// Start counting (assigning numbers to BDDs) at 1
static size_t bdd_ser_counter = 1;
static size_t bdd_ser_done = 0;


//TODO move mtbdd serialize functions to its own file
//
// Given a BDD, assign unique numbers to all nodes
static size_t
bdd_serialize_assign_rec(BDD bdd)
{
    if (!bdd_is_leaf(bdd)) {
        bddnode* n = MTBDD_GETNODE(bdd);

        struct bdd_ser s, *ss;
        s.bdd = BDD_STRIPMARK(bdd);
        ss = bdd_ser_search(bdd_ser_set, &s);
        if (ss == NULL) {
            // assign dummy value
            s.assigned = 0;
            ss = bdd_ser_put(&bdd_ser_set, &s, NULL);

            // first assign recursively
            bdd_serialize_assign_rec(bddnode_getlow(n));
            bdd_serialize_assign_rec(bddnode_gethigh(n));

            // assign real value
            ss->assigned = bdd_ser_counter++;

            // put a copy in the reversed table
            bdd_ser_reversed_insert(&bdd_ser_reversed_set, ss);
        }

        return ss->assigned;
    }

    return BDD_STRIPMARK(bdd);
}

size_t
bdd_serialize_add(BDD bdd)
{
    return BDD_TRANSFERMARK(bdd, bdd_serialize_assign_rec(bdd));
}

void
bdd_serialize_reset(void)
{
    bdd_ser_free(&bdd_ser_set);
    bdd_ser_free(&bdd_ser_reversed_set);
    bdd_ser_counter = 1;
    bdd_ser_done = 0;
}

size_t
bdd_serialize_get(BDD bdd)
{
    if (bdd_is_leaf(bdd)) return bdd;
    struct bdd_ser s, *ss;
    s.bdd = BDD_STRIPMARK(bdd);
    ss = bdd_ser_search(bdd_ser_set, &s);
    assert(ss != NULL);
    return BDD_TRANSFERMARK(bdd, ss->assigned);
}

BDD
bdd_serialize_get_reversed(size_t value)
{
    if (bdd_is_leaf(value)) return value;
    struct bdd_ser s, *ss;
    s.assigned = BDD_STRIPMARK(value);
    ss = bdd_ser_reversed_search(bdd_ser_reversed_set, &s);
    assert(ss != NULL);
    return BDD_TRANSFERMARK(value, ss->bdd);
}

void
bdd_serialize_totext(FILE *out)
{
    fprintf(out, "[");
    avl_iter_t *it = bdd_ser_reversed_iter(bdd_ser_reversed_set);
    struct bdd_ser *s;

    while ((s=bdd_ser_reversed_iter_next(it))) {
        BDD bdd = s->bdd;
        bddnode* n = MTBDD_GETNODE(bdd);
        fprintf(out, "(%zu,%u,%zu,%zu,%u),", s->assigned,
                                             bddnode_getvariable(n),
                                             (size_t)bddnode_getlow(n),
                                             (size_t)BDD_STRIPMARK(bddnode_gethigh(n)),
                                             BDD_HASMARK(bddnode_gethigh(n)) ? 1 : 0);
    }

    bdd_ser_reversed_iter_free(it);
    fprintf(out, "]");
}

void
bdd_serialize_tofile(FILE *out)
{
    size_t count = avl_count(bdd_ser_reversed_set);
    assert(count >= bdd_ser_done);
    assert(count == bdd_ser_counter-1);
    count -= bdd_ser_done;
    fwrite(&count, sizeof(size_t), 1, out);

    struct bdd_ser *s;
    avl_iter_t *it = bdd_ser_reversed_iter(bdd_ser_reversed_set);

    /* Skip already written entries */
    size_t index = 0;
    while (index < bdd_ser_done && (s=bdd_ser_reversed_iter_next(it))) {
        index++;
        assert(s->assigned == index);
    }

    while ((s=bdd_ser_reversed_iter_next(it))) {
        index++;
        assert(s->assigned == index);

        bddnode* n = MTBDD_GETNODE(s->bdd);

        struct bddnode node;
        bddnode_makenode(&node, bddnode_getvariable(n), bdd_serialize_get(bddnode_getlow(n)), bdd_serialize_get(bddnode_gethigh(n)));

        fwrite(&node, sizeof(struct bddnode), 1, out);
    }

    bdd_ser_done = bdd_ser_counter-1;
    bdd_ser_reversed_iter_free(it);
}

void
bdd_serialize_fromfile(FILE *in)
{
    size_t count, i;
    if (fread(&count, sizeof(size_t), 1, in) != 1) {
        // TODO FIXME return error
        printf("bdd_serialize_fromfile: file format error, giving up\n");
        exit(-1);
    }

    for (i=1; i<=count; i++) {
        struct bddnode node;
        if (fread(&node, sizeof(struct bddnode), 1, in) != 1) {
            // TODO FIXME return error
            printf("bdd_serialize_fromfile: file format error, giving up\n");
            exit(-1);
        }

        BDD low = bdd_serialize_get_reversed(bddnode_getlow(&node));
        BDD high = bdd_serialize_get_reversed(bddnode_gethigh(&node));

        struct bdd_ser s;
        s.bdd = mtbdd_make_node(bddnode_getvariable(&node), low, high);
        s.assigned = ++bdd_ser_done; // starts at 0 but we want 1-based...

        bdd_ser_insert(&bdd_ser_set, &s);
        bdd_ser_reversed_insert(&bdd_ser_reversed_set, &s);
    }
}
