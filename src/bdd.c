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

int
bdd_eval(BDD *destination, BDD dd, BDDSET variables, const uint8_t *values, size_t count)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;

    BDD evaluated = mtbdd_invalid;
    int status = _mtbdd_eval(&evaluated, dd, variables, values, count);
    if (status != SYLVAN_OK) return status;
    if (evaluated != bdd_false && evaluated != bdd_true) return SYLVAN_ERR_INVALID;

    *destination = evaluated;
    sylvan_stats_count(BDD_EVAL);
    return SYLVAN_OK;
}

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

int
bdd_intersection_witness_CALL(lace_worker* lace, BDD *destination, BDD a, BDD b)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    if (a == bdd_false || b == bdd_false || a == BDD_TOGGLEMARK(b)) {
        *destination = bdd_false;
        return SYLVAN_OK;
    }
    if (a == bdd_true) { *destination = b; return SYLVAN_OK; }
    if (b == bdd_true || a == b) { *destination = a; return SYLVAN_OK; }

    sylvan_gc_test(lace);
    sylvan_stats_count(BDD_INTERSECTION_WITNESS);

    if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        BDD temporary = a;
        a = b;
        b = temporary;
    }

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_INTERSECTION_WITNESS, a, b, 0, &computed)) {
        sylvan_stats_count(BDD_INTERSECTION_WITNESS_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    bddnode* node_a = MTBDD_GETNODE(a);
    bddnode* node_b = MTBDD_GETNODE(b);
    uint32_t level_a = bddnode_getvariable(node_a);
    uint32_t level_b = bddnode_getvariable(node_b);
    uint32_t level = level_a < level_b ? level_a : level_b;

    BDD low_a = level_a == level ? node_low(a, node_a) : a;
    BDD high_a = level_a == level ? node_high(a, node_a) : a;
    BDD low_b = level_b == level ? node_low(b, node_b) : b;
    BDD high_b = level_b == level ? node_high(b, node_b) : b;

    BDD witness = mtbdd_invalid;
    mtbdd_refs_pushptr(&witness);
    int status = bdd_intersection_witness_CALL(lace, &witness, low_a, low_b);
    if (status == SYLVAN_OK && witness != bdd_false) {
        status = _mtbdd_try_make_node(&computed, level, witness, bdd_false);
    } else if (status == SYLVAN_OK) {
        status = bdd_intersection_witness_CALL(lace, &witness, high_a, high_b);
        if (status == SYLVAN_OK && witness != bdd_false) {
            status = _mtbdd_try_make_node(&computed, level, bdd_false, witness);
        } else if (status == SYLVAN_OK) {
            computed = bdd_false;
        }
    }
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(2);
        return status;
    }

    if (cache_put3(CACHE_BDD_INTERSECTION_WITNESS, a, b, 0, computed)) {
        sylvan_stats_count(BDD_INTERSECTION_WITNESS_CACHEDPUT);
    }
    *destination = computed;
    mtbdd_refs_popptr(2);
    return SYLVAN_OK;
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

int
bdd_xnor_CALL(lace_worker *lace, BDD *destination, BDD a, BDD b)
{
    int status = bdd_xor_CALL(lace, destination, a, b);
    if (status == SYLVAN_OK) *destination = bdd_not(*destination);
    return status;
}

int
bdd_or_CALL(lace_worker *lace, BDD *destination, BDD a, BDD b)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }
    int status = bdd_and_CALL(lace, destination, bdd_not(a), bdd_not(b));
    if (status == SYLVAN_OK) *destination = bdd_not(*destination);
    return status;
}

int
bdd_nand_CALL(lace_worker *lace, BDD *destination, BDD a, BDD b)
{
    int status = bdd_and_CALL(lace, destination, a, b);
    if (status == SYLVAN_OK) *destination = bdd_not(*destination);
    return status;
}

int
bdd_nor_CALL(lace_worker *lace, BDD *destination, BDD a, BDD b)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }
    return bdd_and_CALL(lace, destination, bdd_not(a), bdd_not(b));
}

int
bdd_imp_CALL(lace_worker *lace, BDD *destination, BDD a, BDD b)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }
    int status = bdd_and_CALL(lace, destination, a, bdd_not(b));
    if (status == SYLVAN_OK) *destination = bdd_not(*destination);
    return status;
}

int
bdd_diff_CALL(lace_worker *lace, BDD *destination, BDD a, BDD b)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }
    return bdd_and_CALL(lace, destination, a, bdd_not(b));
}

char
bdd_subseteq_CALL(lace_worker *lace, BDD a, BDD b)
{
    return bdd_disjoint_CALL(lace, a, bdd_not(b));
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

/**
 * Compute constrain f@c, also called the generalized co-factor.
 * c is the "care function" - f@c equals f when c evaluates to True.
 */
int bdd_constrain_CALL(lace_worker* lace, BDD *destination, BDD f, BDD c)
{
    if (destination == NULL || f == mtbdd_invalid || c == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Trivial cases */
    if (c == bdd_true) { *destination = f; return SYLVAN_OK; }
    if (c == bdd_false) { *destination = bdd_false; return SYLVAN_OK; }
    if (bdd_is_leaf(f)) { *destination = f; return SYLVAN_OK; }
    if (f == c) { *destination = bdd_true; return SYLVAN_OK; }
    if (f == bdd_not(c)) { *destination = bdd_false; return SYLVAN_OK; }

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

    BDD computed = mtbdd_invalid;
    BDD low = mtbdd_invalid;
    BDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);

    /* Consult cache */
    if (cache_get3(CACHE_BDD_CONSTRAIN, f, c, 0, &computed)) {
        sylvan_stats_count(BDD_CONSTRAIN_CACHED);
        *destination = mark ? bdd_not(computed) : computed;
        mtbdd_refs_popptr(3);
        return SYLVAN_OK;
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

    int status = SYLVAN_OK;

    if (cLow == bdd_false) {
        /* cLow is False, so result equals fHigh @ cHigh */
        if (cHigh == bdd_true) computed = fHigh;
        else status = bdd_constrain_CALL(lace, &computed, fHigh, cHigh);
    } else if (cHigh == bdd_false) {
        /* cHigh is False, so result equals fLow @ cLow */
        if (cLow == bdd_true) computed = fLow;
        else status = bdd_constrain_CALL(lace, &computed, fLow, cLow);
    } else if (cLow == bdd_true) {
        /* cLow is True, so low result equals fLow */
        status = bdd_constrain_CALL(lace, &high, fHigh, cHigh);
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, level, fLow, high);
    } else if (cHigh == bdd_true) {
        /* cHigh is True, so high result equals fHigh */
        status = bdd_constrain_CALL(lace, &low, fLow, cLow);
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, level, low, fHigh);
    } else {
        /* cLow and cHigh are not constrants... normal parallel recursion */
        bdd_constrain_SPAWN(lace, &low, fLow, cLow);
        int high_status = bdd_constrain_CALL(lace, &high, fHigh, cHigh);
        int low_status = bdd_constrain_SYNC(lace);
        if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
            status = high_status != SYLVAN_OK ? high_status : low_status;
        } else {
            status = _mtbdd_try_make_node(&computed, level, low, high);
        }
    }

    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    if (cache_put3(CACHE_BDD_CONSTRAIN, f, c, 0, computed)) sylvan_stats_count(BDD_CONSTRAIN_CACHEDPUT);

    *destination = mark ? bdd_not(computed) : computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

/**
 * Simplify f with respect to the care function c using Coudert-Madre.
 */
TASK(int, bdd_simplify_internal, BDD*, result, BDD, f, BDD, c)

int bdd_simplify_internal_CALL(lace_worker* lace, BDD *destination, BDD f, BDD c)
{
    if (destination == NULL || f == mtbdd_invalid || c == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Trivial cases */
    if (c == bdd_true) { *destination = f; return SYLVAN_OK; }
    if (c == bdd_false) { *destination = bdd_false; return SYLVAN_OK; }
    if (bdd_is_leaf(f)) { *destination = f; return SYLVAN_OK; }
    if (f == c) { *destination = bdd_true; return SYLVAN_OK; }
    if (f == bdd_not(c)) { *destination = bdd_false; return SYLVAN_OK; }

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_SIMPLIFY);

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

    BDD computed = mtbdd_invalid;
    BDD low = mtbdd_invalid;
    BDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);

    /* Consult cache */
    if (cache_get3(CACHE_BDD_SIMPLIFY, f, c, 0, &computed)) {
        sylvan_stats_count(BDD_SIMPLIFY_CACHED);
        *destination = mark ? bdd_not(computed) : computed;
        mtbdd_refs_popptr(3);
        return SYLVAN_OK;
    }

    int status = SYLVAN_OK;
    if (vc < vf) {
        /* f is independent of c, so result is f @ (cLow \/ cHigh) */
        status = bdd_and_CALL(lace, &low, bdd_not(node_low(c, nc)), bdd_not(node_high(c, nc)));
        if (status == SYLVAN_OK) {
            low = bdd_not(low);
            status = bdd_simplify_internal_CALL(lace, &computed, f, low);
        }
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
            status = bdd_simplify_internal_CALL(lace, &computed, fHigh, cHigh);
        } else if (cHigh == bdd_false) {
            /* sibling-substitution */
            status = bdd_simplify_internal_CALL(lace, &computed, fLow, cLow);
        } else {
            /* parallel recursion */
            bdd_simplify_internal_SPAWN(lace, &low, fLow, cLow);
            int high_status = bdd_simplify_internal_CALL(lace, &high, fHigh, cHigh);
            int low_status = bdd_simplify_internal_SYNC(lace);
            if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
                status = high_status != SYLVAN_OK ? high_status : low_status;
            } else {
                status = _mtbdd_try_make_node(&computed, level, low, high);
            }
        }
    }

    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    if (cache_put3(CACHE_BDD_SIMPLIFY, f, c, 0, computed)) sylvan_stats_count(BDD_SIMPLIFY_CACHEDPUT);

    *destination = mark ? bdd_not(computed) : computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
}

int
bdd_simplify_CALL(lace_worker* lace, BDD *destination, BDD f, BDD c)
{
    if (destination == NULL || f == mtbdd_invalid || c == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    int status = bdd_simplify_internal_CALL(lace, &computed, f, c);
    if (status == SYLVAN_OK) {
        *destination = mtbdd_node_count(computed) <= mtbdd_node_count(f) ? computed : f;
    }
    mtbdd_refs_popptr(1);
    return status;
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

int
bdd_cofactor(BDD *destination, BDD f, BDD cube)
{
    if (destination == NULL || f == mtbdd_invalid || cube == mtbdd_invalid || !bdd_is_cube(cube)) {
        return SYLVAN_ERR_INVALID;
    }
    return bdd_constrain(destination, f, cube);
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

int
bdd_forall_CALL(lace_worker *lace, BDD *destination, BDD dd, BDDSET variables)
{
    if (destination == NULL || dd == mtbdd_invalid ||
        variables == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }
    int status = bdd_exists_CALL(lace, destination, bdd_not(dd), variables);
    if (status == SYLVAN_OK) *destination = bdd_not(*destination);
    return status;
}

/**
 * Calculate the parity abstraction of <a> over <variables>.
 */
int
bdd_unique_CALL(lace_worker* lace, BDD *destination, BDD a, BDDSET variables)
{
    if (destination == NULL || a == mtbdd_invalid || variables == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    if (bdd_set_is_empty(variables)) { *destination = a; return SYLVAN_OK; }
    if (a == bdd_false || a == bdd_true) { *destination = bdd_false; return SYLVAN_OK; }

    bddnode* na = MTBDD_GETNODE(a);
    uint32_t level = bddnode_getvariable(na);
    uint32_t variable = bdd_set_first(variables);

    /* An absent quantified variable contributes two equal cofactors. */
    if (variable < level) { *destination = bdd_false; return SYLVAN_OK; }

    sylvan_gc_test(lace);
    sylvan_stats_count(BDD_UNIQUE);

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_UNIQUE, a, variables, 0, &computed)) {
        sylvan_stats_count(BDD_UNIQUE_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    BDD low = mtbdd_invalid;
    BDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);

    BDDSET next_variables = variable == level ? bdd_set_next(variables) : variables;
    bdd_unique_SPAWN(lace, &high, node_high(a, na), next_variables);
    int low_status = bdd_unique_CALL(lace, &low, node_low(a, na), next_variables);
    int high_status = bdd_unique_SYNC(lace);
    if (low_status != SYLVAN_OK || high_status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return low_status != SYLVAN_OK ? low_status : high_status;
    }

    int status;
    if (variable == level) {
        status = bdd_xor_CALL(lace, &computed, low, high);
    } else {
        status = _mtbdd_try_make_node(&computed, level, low, high);
    }
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    if (cache_put3(CACHE_BDD_UNIQUE, a, variables, 0, computed)) {
        sylvan_stats_count(BDD_UNIQUE_CACHEDPUT);
    }

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

static int
bdd_apply_operator_CALL(lace_worker *lace, BDD *destination, BDD a, BDD b,
                        bdd_apply_operator apply)
{
    int status;
    switch (apply) {
    case BDD_APPLY_AND:
        return bdd_and_CALL(lace, destination, a, b);
    case BDD_APPLY_XOR:
        return bdd_xor_CALL(lace, destination, a, b);
    case BDD_APPLY_OR:
        status = bdd_and_CALL(lace, destination, bdd_not(a), bdd_not(b));
        if (status == SYLVAN_OK) *destination = bdd_not(*destination);
        return status;
    case BDD_APPLY_XNOR:
        status = bdd_xor_CALL(lace, destination, a, b);
        if (status == SYLVAN_OK) *destination = bdd_not(*destination);
        return status;
    case BDD_APPLY_NAND:
        status = bdd_and_CALL(lace, destination, a, b);
        if (status == SYLVAN_OK) *destination = bdd_not(*destination);
        return status;
    case BDD_APPLY_NOR:
        return bdd_and_CALL(lace, destination, bdd_not(a), bdd_not(b));
    case BDD_APPLY_IMP:
        status = bdd_and_CALL(lace, destination, a, bdd_not(b));
        if (status == SYLVAN_OK) *destination = bdd_not(*destination);
        return status;
    case BDD_APPLY_DIFF:
        return bdd_and_CALL(lace, destination, a, bdd_not(b));
    default:
        return SYLVAN_ERR_INVALID;
    }
}

static int
bdd_apply_is_commutative(bdd_apply_operator apply)
{
    return apply == BDD_APPLY_AND || apply == BDD_APPLY_XOR ||
           apply == BDD_APPLY_OR || apply == BDD_APPLY_XNOR ||
           apply == BDD_APPLY_NAND || apply == BDD_APPLY_NOR;
}

int
bdd_apply_abstract_CALL(lace_worker *lace, BDD *destination, BDD a, BDD b,
                        BDDSET variables, bdd_apply_operator apply,
                        bdd_abstract_operator abstract)
{
    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid ||
        variables == mtbdd_invalid ||
        apply < BDD_APPLY_AND || apply > BDD_APPLY_DIFF ||
        abstract < BDD_ABSTRACT_EXISTS || abstract > BDD_ABSTRACT_UNIQUE) {
        return SYLVAN_ERR_INVALID;
    }

    if (bdd_set_is_empty(variables)) {
        return bdd_apply_operator_CALL(lace, destination, a, b, apply);
    }

    if (bdd_apply_is_commutative(apply) &&
        BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        const BDD swap = a;
        a = b;
        b = swap;
    }

    if ((a == bdd_false || a == bdd_true) &&
        (b == bdd_false || b == bdd_true)) {
        if (abstract == BDD_ABSTRACT_UNIQUE) {
            *destination = bdd_false;
            return SYLVAN_OK;
        }
        return bdd_apply_operator_CALL(lace, destination, a, b, apply);
    }

    bddnode *a_node =
        (a == bdd_false || a == bdd_true) ? NULL : MTBDD_GETNODE(a);
    bddnode *b_node =
        (b == bdd_false || b == bdd_true) ? NULL : MTBDD_GETNODE(b);
    const uint32_t a_level =
        a_node == NULL ? UINT32_MAX : bddnode_getvariable(a_node);
    const uint32_t b_level =
        b_node == NULL ? UINT32_MAX : bddnode_getvariable(b_node);
    const uint32_t level = a_level < b_level ? a_level : b_level;

    while (!bdd_set_is_empty(variables) &&
           bdd_set_first(variables) < level) {
        if (abstract == BDD_ABSTRACT_UNIQUE) {
            *destination = bdd_false;
            return SYLVAN_OK;
        }
        variables = bdd_set_next(variables);
    }
    if (bdd_set_is_empty(variables)) {
        return bdd_apply_operator_CALL(lace, destination, a, b, apply);
    }

    sylvan_gc_test(lace);
    sylvan_stats_count(BDD_APPLY_ABSTRACT);

    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    const uint64_t operation =
        ((uint64_t)(unsigned)abstract << 8) | (uint64_t)(unsigned)apply;
    if (cache_get4(CACHE_BDD_APPLY_ABSTRACT, a, b, variables, operation,
                   &computed)) {
        sylvan_stats_count(BDD_APPLY_ABSTRACT_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    const BDD a_low = a_level == level ? node_low(a, a_node) : a;
    const BDD a_high = a_level == level ? node_high(a, a_node) : a;
    const BDD b_low = b_level == level ? node_low(b, b_node) : b;
    const BDD b_high = b_level == level ? node_high(b, b_node) : b;
    const int quantify = bdd_set_first(variables) == level;
    const BDDSET next_variables =
        quantify ? bdd_set_next(variables) : variables;

    BDD low = mtbdd_invalid;
    BDD high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);
    bdd_apply_abstract_SPAWN(
        lace, &high, a_high, b_high, next_variables, apply, abstract);
    int status = bdd_apply_abstract_CALL(
        lace, &low, a_low, b_low, next_variables, apply, abstract);
    const int high_status = bdd_apply_abstract_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;

    if (status == SYLVAN_OK && quantify) {
        switch (abstract) {
        case BDD_ABSTRACT_EXISTS:
            status = bdd_apply_operator_CALL(
                lace, &computed, low, high, BDD_APPLY_OR);
            break;
        case BDD_ABSTRACT_FORALL:
            status = bdd_and_CALL(lace, &computed, low, high);
            break;
        case BDD_ABSTRACT_UNIQUE:
            status = bdd_xor_CALL(lace, &computed, low, high);
            break;
        default:
            status = SYLVAN_ERR_INVALID;
            break;
        }
    } else if (status == SYLVAN_OK) {
        status = _mtbdd_try_make_node(&computed, level, low, high);
    }

    if (status == SYLVAN_OK &&
        cache_put4(CACHE_BDD_APPLY_ABSTRACT, a, b, variables, operation,
                   computed)) {
        sylvan_stats_count(BDD_APPLY_ABSTRACT_CACHEDPUT);
    }
    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(3);
    return status;
}


int bdd_rel_next_CALL(lace_worker* lace, BDD *destination, BDD a, BDD b, BDDSET vars)
{
    /* Compute R(s) = \exists x: A(x) \and B(x,s) with support(result) = s, support(A) = s, support(B) = s+t
     * if vars == bdd_false, then every level is in s or t
     * any other levels (outside s,t) in B are ignored / existentially quantified
     */

    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || vars == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Terminals */
    if (a == bdd_true && b == bdd_true) { *destination = bdd_true; return SYLVAN_OK; }
    if (a == bdd_false || b == bdd_false) { *destination = bdd_false; return SYLVAN_OK; }
    if (bdd_set_is_empty(vars)) { *destination = a; return SYLVAN_OK; }

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
            if (bdd_set_is_empty(vars)) { *destination = a; return SYLVAN_OK; }
            nv = MTBDD_GETNODE(vars);
        }
    }

    /* Consult cache */
    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_RELNEXT, a, b, vars, &computed)) {
        sylvan_stats_count(BDD_RELNEXT_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
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

        BDD c = mtbdd_invalid, d = mtbdd_invalid, e = mtbdd_invalid, f = mtbdd_invalid;
        BDD low = mtbdd_invalid, high = mtbdd_invalid;
        mtbdd_refs_pushptr(&c);
        mtbdd_refs_pushptr(&d);
        mtbdd_refs_pushptr(&e);
        mtbdd_refs_pushptr(&f);
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);

        bdd_rel_next_SPAWN(lace, &c, a0, b00, _vars);
        bdd_rel_next_SPAWN(lace, &d, a1, b10, _vars);
        bdd_rel_next_SPAWN(lace, &e, a0, b01, _vars);
        bdd_rel_next_SPAWN(lace, &f, a1, b11, _vars);

        int status = SYLVAN_OK;
        for (int i = 0; i < 4; i++) {
            int child_status = bdd_rel_next_SYNC(lace);
            if (status == SYLVAN_OK && child_status != SYLVAN_OK) status = child_status;
        }
        if (status == SYLVAN_OK) {
            bdd_ite_SPAWN(lace, &high, e, bdd_true, f);
            status = bdd_ite_CALL(lace, &low, c, bdd_true, d);
            int high_status = bdd_ite_SYNC(lace);
            if (status == SYLVAN_OK) status = high_status;
        }
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, s, low, high);
        mtbdd_refs_popptr(6);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(1);
            return status;
        }
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
                BDD r0 = mtbdd_invalid, r1 = mtbdd_invalid;
                mtbdd_refs_pushptr(&r0);
                mtbdd_refs_pushptr(&r1);
                bdd_rel_next_SPAWN(lace, &r1, a1, b1, vars);
                int status = bdd_rel_next_CALL(lace, &r0, a0, b0, vars);
                int r1_status = bdd_rel_next_SYNC(lace);
                if (status == SYLVAN_OK) status = r1_status;
                if (status == SYLVAN_OK) {
                    status = bdd_and_CALL(lace, &computed, bdd_not(r0), bdd_not(r1));
                    if (status == SYLVAN_OK) computed = bdd_not(computed);
                }
                mtbdd_refs_popptr(2);
                if (status != SYLVAN_OK) {
                    mtbdd_refs_popptr(1);
                    return status;
                }
            } else {
                /* Quantify "b" variables, but keep "a" variables */
                BDD r00 = mtbdd_invalid, r01 = mtbdd_invalid;
                BDD r10 = mtbdd_invalid, r11 = mtbdd_invalid;
                BDD r0 = mtbdd_invalid, r1 = mtbdd_invalid;
                mtbdd_refs_pushptr(&r00);
                mtbdd_refs_pushptr(&r01);
                mtbdd_refs_pushptr(&r10);
                mtbdd_refs_pushptr(&r11);
                mtbdd_refs_pushptr(&r0);
                mtbdd_refs_pushptr(&r1);

                bdd_rel_next_SPAWN(lace, &r00, a0, b0, vars);
                bdd_rel_next_SPAWN(lace, &r01, a0, b1, vars);
                bdd_rel_next_SPAWN(lace, &r10, a1, b0, vars);
                bdd_rel_next_SPAWN(lace, &r11, a1, b1, vars);
                int status = SYLVAN_OK;
                for (int i = 0; i < 4; i++) {
                    int child_status = bdd_rel_next_SYNC(lace);
                    if (status == SYLVAN_OK && child_status != SYLVAN_OK) status = child_status;
                }
                if (status == SYLVAN_OK) {
                    bdd_ite_SPAWN(lace, &r1, r10, bdd_true, r11);
                    status = bdd_ite_CALL(lace, &r0, r00, bdd_true, r01);
                    int r1_status = bdd_ite_SYNC(lace);
                    if (status == SYLVAN_OK) status = r1_status;
                }
                if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, level, r0, r1);
                mtbdd_refs_popptr(6);
                if (status != SYLVAN_OK) {
                    mtbdd_refs_popptr(1);
                    return status;
                }
            }
        } else {
            /* Keep "a" variables */
            BDD r0 = mtbdd_invalid, r1 = mtbdd_invalid;
            mtbdd_refs_pushptr(&r0);
            mtbdd_refs_pushptr(&r1);
            bdd_rel_next_SPAWN(lace, &r1, a1, b1, vars);
            int status = bdd_rel_next_CALL(lace, &r0, a0, b0, vars);
            int r1_status = bdd_rel_next_SYNC(lace);
            if (status == SYLVAN_OK) status = r1_status;
            if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, level, r0, r1);
            mtbdd_refs_popptr(2);
            if (status != SYLVAN_OK) {
                mtbdd_refs_popptr(1);
                return status;
            }
        }
    }

    if (cache_put3(CACHE_BDD_RELNEXT, a, b, vars, computed)) sylvan_stats_count(BDD_RELNEXT_CACHEDPUT);

    *destination = computed;
    mtbdd_refs_popptr(1);
    return SYLVAN_OK;
}

int bdd_rel_prev_CALL(lace_worker* lace, BDD *destination, BDD a, BDD b, BDDSET vars)
{
    /* Compute \exists x: A(s,x) \and B(x,t)
     * if vars == bdd_false, then every level is in s or t
     * any other levels (outside s,t) in A are ignored / existentially quantified
     */

    if (destination == NULL || a == mtbdd_invalid || b == mtbdd_invalid || vars == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Terminals */
    if (a == bdd_true && b == bdd_true) { *destination = bdd_true; return SYLVAN_OK; }
    if (a == bdd_false || b == bdd_false) { *destination = bdd_false; return SYLVAN_OK; }
    if (bdd_set_is_empty(vars)) { *destination = b; return SYLVAN_OK; }

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
            if (bdd_set_is_empty(vars)) { *destination = b; return SYLVAN_OK; }
            nv = MTBDD_GETNODE(vars);
        }
    }

    /* Consult cache */
    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_RELPREV, a, b, vars, &computed)) {
        sylvan_stats_count(BDD_RELPREV_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
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

        BDD r00 = mtbdd_invalid, r01 = mtbdd_invalid;
        BDD r10 = mtbdd_invalid, r11 = mtbdd_invalid;
        BDD r000 = mtbdd_invalid, r001 = mtbdd_invalid;
        BDD r100 = mtbdd_invalid, r101 = mtbdd_invalid;
        BDD r010 = mtbdd_invalid, r011 = mtbdd_invalid;
        BDD r110 = mtbdd_invalid, r111 = mtbdd_invalid;
        BDD r0 = mtbdd_invalid, r1 = mtbdd_invalid;
        mtbdd_refs_pushptr(&r00);
        mtbdd_refs_pushptr(&r01);
        mtbdd_refs_pushptr(&r10);
        mtbdd_refs_pushptr(&r11);
        mtbdd_refs_pushptr(&r000);
        mtbdd_refs_pushptr(&r001);
        mtbdd_refs_pushptr(&r100);
        mtbdd_refs_pushptr(&r101);
        mtbdd_refs_pushptr(&r010);
        mtbdd_refs_pushptr(&r011);
        mtbdd_refs_pushptr(&r110);
        mtbdd_refs_pushptr(&r111);
        mtbdd_refs_pushptr(&r0);
        mtbdd_refs_pushptr(&r1);

        int spawned = 0;
        if (b00 == b01) {
            bdd_rel_prev_SPAWN(lace, &r00, a00, b0, _vars); spawned++;
            bdd_rel_prev_SPAWN(lace, &r10, a10, b0, _vars); spawned++;
        } else {
            bdd_rel_prev_SPAWN(lace, &r000, a00, b00, _vars); spawned++;
            bdd_rel_prev_SPAWN(lace, &r001, a00, b01, _vars); spawned++;
            bdd_rel_prev_SPAWN(lace, &r100, a10, b00, _vars); spawned++;
            bdd_rel_prev_SPAWN(lace, &r101, a10, b01, _vars); spawned++;
        }

        if (b10 == b11) {
            bdd_rel_prev_SPAWN(lace, &r01, a01, b1, _vars); spawned++;
            bdd_rel_prev_SPAWN(lace, &r11, a11, b1, _vars); spawned++;
        } else {
            bdd_rel_prev_SPAWN(lace, &r010, a01, b10, _vars); spawned++;
            bdd_rel_prev_SPAWN(lace, &r011, a01, b11, _vars); spawned++;
            bdd_rel_prev_SPAWN(lace, &r110, a11, b10, _vars); spawned++;
            bdd_rel_prev_SPAWN(lace, &r111, a11, b11, _vars); spawned++;
        }

        int status = SYLVAN_OK;
        while (spawned-- > 0) {
            int child_status = bdd_rel_prev_SYNC(lace);
            if (status == SYLVAN_OK && child_status != SYLVAN_OK) status = child_status;
        }

        if (status == SYLVAN_OK && b00 != b01) status = _mtbdd_try_make_node(&r00, t, r000, r001);
        if (status == SYLVAN_OK && b00 != b01) status = _mtbdd_try_make_node(&r10, t, r100, r101);
        if (status == SYLVAN_OK && b10 != b11) status = _mtbdd_try_make_node(&r01, t, r010, r011);
        if (status == SYLVAN_OK && b10 != b11) status = _mtbdd_try_make_node(&r11, t, r110, r111);

        if (status == SYLVAN_OK) {
            bdd_and_SPAWN(lace, &r1, bdd_not(r10), bdd_not(r11));
            status = bdd_and_CALL(lace, &r0, bdd_not(r00), bdd_not(r01));
            int r1_status = bdd_and_SYNC(lace);
            if (status == SYLVAN_OK) status = r1_status;
            if (status == SYLVAN_OK) {
                r0 = bdd_not(r0);
                r1 = bdd_not(r1);
                status = _mtbdd_try_make_node(&computed, s, r0, r1);
            }
        }

        mtbdd_refs_popptr(14);
        if (status != SYLVAN_OK) {
            mtbdd_refs_popptr(1);
            return status;
        }
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
                BDD r0 = mtbdd_invalid, r1 = mtbdd_invalid;
                mtbdd_refs_pushptr(&r0);
                mtbdd_refs_pushptr(&r1);
                bdd_rel_prev_SPAWN(lace, &r1, a1, b1, vars);
                int status = bdd_rel_prev_CALL(lace, &r0, a0, b0, vars);
                int r1_status = bdd_rel_prev_SYNC(lace);
                if (status == SYLVAN_OK) status = r1_status;
                if (status == SYLVAN_OK) status = bdd_ite_CALL(lace, &computed, r0, bdd_true, r1);
                mtbdd_refs_popptr(2);
                if (status != SYLVAN_OK) {
                    mtbdd_refs_popptr(1);
                    return status;
                }
            } else {
                /* Quantify "a" variables, but keep "b" variables */
                BDD r00 = mtbdd_invalid, r01 = mtbdd_invalid;
                BDD r10 = mtbdd_invalid, r11 = mtbdd_invalid;
                BDD r0 = mtbdd_invalid, r1 = mtbdd_invalid;
                mtbdd_refs_pushptr(&r00);
                mtbdd_refs_pushptr(&r01);
                mtbdd_refs_pushptr(&r10);
                mtbdd_refs_pushptr(&r11);
                mtbdd_refs_pushptr(&r0);
                mtbdd_refs_pushptr(&r1);

                bdd_rel_next_SPAWN(lace, &r00, a0, b0, vars);
                bdd_rel_next_SPAWN(lace, &r10, a1, b0, vars);
                bdd_rel_next_SPAWN(lace, &r01, a0, b1, vars);
                bdd_rel_next_SPAWN(lace, &r11, a1, b1, vars);
                int status = SYLVAN_OK;
                for (int i = 0; i < 4; i++) {
                    int child_status = bdd_rel_next_SYNC(lace);
                    if (status == SYLVAN_OK && child_status != SYLVAN_OK) status = child_status;
                }
                if (status == SYLVAN_OK) {
                    bdd_ite_SPAWN(lace, &r1, r01, bdd_true, r11);
                    status = bdd_ite_CALL(lace, &r0, r00, bdd_true, r10);
                    int r1_status = bdd_ite_SYNC(lace);
                    if (status == SYLVAN_OK) status = r1_status;
                }
                if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, level, r0, r1);
                mtbdd_refs_popptr(6);
                if (status != SYLVAN_OK) {
                    mtbdd_refs_popptr(1);
                    return status;
                }
            }
        } else {
            BDD r0 = mtbdd_invalid, r1 = mtbdd_invalid;
            mtbdd_refs_pushptr(&r0);
            mtbdd_refs_pushptr(&r1);
            bdd_rel_prev_SPAWN(lace, &r1, a1, b1, vars);
            int status = bdd_rel_prev_CALL(lace, &r0, a0, b0, vars);
            int r1_status = bdd_rel_prev_SYNC(lace);
            if (status == SYLVAN_OK) status = r1_status;
            if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, level, r0, r1);
            mtbdd_refs_popptr(2);
            if (status != SYLVAN_OK) {
                mtbdd_refs_popptr(1);
                return status;
            }
        }
    }

    if (cache_put3(CACHE_BDD_RELPREV, a, b, vars, computed)) sylvan_stats_count(BDD_RELPREV_CACHEDPUT);

    *destination = computed;
    mtbdd_refs_popptr(1);
    return SYLVAN_OK;
}

/**
 * Computes the transitive closure by traversing the BDD recursively.
 * See Y. Matsunaga, P. C. McGeer, R. K. Brayton
 *     On Computing the Transitive Closre of a State Transition Relation
 *     30th ACM Design Automation Conference, 1993.
 */
int bdd_transitive_closure_CALL(lace_worker* lace, BDD *destination, BDD a)
{
    if (destination == NULL || a == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Terminals */
    if (a == bdd_true || a == bdd_false) {
        *destination = a;
        return SYLVAN_OK;
    }

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_CLOSURE);

    /* Determine top level */
    bddnode* n = MTBDD_GETNODE(a);
    uint32_t level = bddnode_getvariable(n);

    /* Consult cache */
    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_CLOSURE, a, 0, 0, &computed)) {
        sylvan_stats_count(BDD_CLOSURE_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
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

    BDD u1 = mtbdd_invalid, u2 = mtbdd_invalid, u3 = mtbdd_invalid;
    BDD e = mtbdd_invalid, f = mtbdd_invalid, g = mtbdd_invalid, h = mtbdd_invalid;
    BDD r0 = mtbdd_invalid, r1 = mtbdd_invalid;
    mtbdd_refs_pushptr(&u1);
    mtbdd_refs_pushptr(&u2);
    mtbdd_refs_pushptr(&u3);
    mtbdd_refs_pushptr(&e);
    mtbdd_refs_pushptr(&f);
    mtbdd_refs_pushptr(&g);
    mtbdd_refs_pushptr(&h);
    mtbdd_refs_pushptr(&r0);
    mtbdd_refs_pushptr(&r1);

    int status = bdd_transitive_closure_CALL(lace, &u1, a11);
    if (status == SYLVAN_OK) {
        status = bdd_rel_prev_CALL(lace, &u3, a01, u1, bdd_false);
    }
    if (status == SYLVAN_OK) status = bdd_rel_prev_CALL(lace, &u2, u1, a10, bdd_false);
    if (status == SYLVAN_OK) status = bdd_rel_prev_CALL(lace, &e, a01, u2, bdd_false);
    if (status == SYLVAN_OK) status = bdd_ite_CALL(lace, &e, a00, bdd_true, e);
    if (status == SYLVAN_OK) status = bdd_transitive_closure_CALL(lace, &e, e);
    if (status == SYLVAN_OK) status = bdd_rel_prev_CALL(lace, &g, u2, e, bdd_false);
    if (status == SYLVAN_OK) status = bdd_rel_prev_CALL(lace, &f, e, u3, bdd_false);
    if (status == SYLVAN_OK) status = bdd_rel_prev_CALL(lace, &h, u2, f, bdd_false);
    if (status == SYLVAN_OK) status = bdd_ite_CALL(lace, &h, u1, bdd_true, h);
    if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&r0, t, e, f);
    if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&r1, t, g, h);
    if (status == SYLVAN_OK) status = _mtbdd_try_make_node(&computed, s, r0, r1);

    mtbdd_refs_popptr(9);
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(1);
        return status;
    }

    if (cache_put3(CACHE_BDD_CLOSURE, a, 0, 0, computed)) sylvan_stats_count(BDD_CLOSURE_CACHEDPUT);

    *destination = computed;
    mtbdd_refs_popptr(1);
    return SYLVAN_OK;
}


/**
 * Function composition
 */
int bdd_compose_CALL(lace_worker* lace, BDD *destination, BDD a, MTBDDMAP map)
{
    if (destination == NULL || a == mtbdd_invalid || map == mtbdd_invalid) return SYLVAN_ERR_INVALID;

    /* Trivial cases */
    if (a == bdd_false || a == bdd_true || mtbdd_map_is_empty(map)) {
        *destination = a;
        return SYLVAN_OK;
    }

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
        if (mtbdd_map_is_empty(map)) {
            *destination = a;
            return SYLVAN_OK;
        }
        map_node = MTBDD_GETNODE(map);
        map_var = bddnode_getvariable(map_node);
    }

    /* Consult cache */
    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&computed);
    if (cache_get3(CACHE_BDD_COMPOSE, a, map, 0, &computed)) {
        sylvan_stats_count(BDD_COMPOSE_CACHED);
        *destination = computed;
        mtbdd_refs_popptr(1);
        return SYLVAN_OK;
    }

    /* Recursively calculate low and high */
    BDD low = mtbdd_invalid, high = mtbdd_invalid;
    mtbdd_refs_pushptr(&low);
    mtbdd_refs_pushptr(&high);

    bdd_compose_SPAWN(lace, &low, node_low(a, n), map);
    int high_status = bdd_compose_CALL(lace, &high, node_high(a, n), map);
    int low_status = bdd_compose_SYNC(lace);
    if (high_status != SYLVAN_OK || low_status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return high_status != SYLVAN_OK ? high_status : low_status;
    }

    /* Calculate result */
    int status;
    if (map_var == level) {
        BDD root = node_high(map, map_node);
        status = bdd_ite_CALL(lace, &computed, root, high, low);
    } else {
        status = _mtbdd_try_make_node(&computed, level, low, high);
    }
    if (status != SYLVAN_OK) {
        mtbdd_refs_popptr(3);
        return status;
    }

    if (cache_put3(CACHE_BDD_COMPOSE, a, map, 0, computed)) sylvan_stats_count(BDD_COMPOSE_CACHEDPUT);

    *destination = computed;
    mtbdd_refs_popptr(3);
    return SYLVAN_OK;
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

static int
bdd_count_shift_u64(uint64_t *result, uint64_t value, size_t shift)
{
    if (value == 0) {
        *result = 0;
        return SYLVAN_OK;
    }
    if (shift >= 64 || value > (UINT64_MAX >> shift)) return SYLVAN_ERR_OVERFLOW;
    *result = value << shift;
    return SYLVAN_OK;
}

/**
 * Calculate the exact number of satisfying variable assignments according to
 * <variables>.
 */
int
bdd_sat_count_u64_CALL(lace_worker* lace, uint64_t *destination, BDD bdd, BDDSET variables)
{
    if (destination == NULL || bdd == mtbdd_invalid || variables == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    if (bdd == bdd_false) {
        *destination = 0;
        return SYLVAN_OK;
    }
    if (bdd == bdd_true) {
        uint64_t result;
        int status = bdd_count_shift_u64(&result, 1, bdd_set_count(variables));
        if (status == SYLVAN_OK) *destination = result;
        return status;
    }
    if (mtbdd_is_leaf(bdd)) return SYLVAN_ERR_INVALID;

    sylvan_stats_count(BDD_SAT_COUNT_U64);

    size_t skipped = 0;
    const uint32_t variable = mtbdd_node_variable(bdd);
    while (!bdd_set_is_empty(variables) && bdd_set_first(variables) < variable) {
        skipped++;
        variables = bdd_set_next(variables);
    }
    if (bdd_set_is_empty(variables) || bdd_set_first(variables) != variable) {
        return SYLVAN_ERR_INVALID;
    }

    uint64_t cached;
    if (cache_get3(CACHE_BDD_SAT_COUNT_U64, bdd, variables, 0, &cached)) {
        sylvan_stats_count(BDD_SAT_COUNT_U64_CACHED);
        uint64_t result;
        int status = bdd_count_shift_u64(&result, cached, skipped);
        if (status == SYLVAN_OK) *destination = result;
        return status;
    }

    const BDDSET next = bdd_set_next(variables);
    uint64_t low, high;
    bdd_sat_count_u64_SPAWN(lace, &high, mtbdd_node_high(bdd), next);
    int low_status = bdd_sat_count_u64_CALL(lace, &low, mtbdd_node_low(bdd), next);
    int high_status = bdd_sat_count_u64_SYNC(lace);
    if (low_status != SYLVAN_OK) return low_status;
    if (high_status != SYLVAN_OK) return high_status;
    if (UINT64_MAX - low < high) return SYLVAN_ERR_OVERFLOW;

    const uint64_t sum = low + high;
    if (cache_put3(CACHE_BDD_SAT_COUNT_U64, bdd, variables, 0, sum)) {
        sylvan_stats_count(BDD_SAT_COUNT_U64_CACHEDPUT);
    }

    uint64_t result;
    int status = bdd_count_shift_u64(&result, sum, skipped);
    if (status == SYLVAN_OK) *destination = result;
    return status;
}

/**
 * Calculate an approximate number of satisfying variable assignments according
 * to <variables>.
 */
double
bdd_sat_count_double_CALL(lace_worker* lace, BDD bdd, BDDSET variables)
{
    if (bdd == mtbdd_invalid || variables == mtbdd_invalid) return NAN;

    /* Trivial cases */
    if (bdd == bdd_false) return 0.0;
    if (bdd == bdd_true) return (double)powl(2.0L, (long double)bdd_set_count(variables));
    if (mtbdd_is_leaf(bdd)) return NAN;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_SAT_COUNT_DOUBLE);

    /* Count variables before var(bdd) */
    size_t skipped = 0;
    uint32_t var = mtbdd_node_variable(bdd);
    while (!bdd_set_is_empty(variables) && bdd_set_first(variables) < var) {
        skipped++;
        variables = bdd_set_next(variables);
    }
    if (bdd_set_is_empty(variables) || bdd_set_first(variables) != var) return NAN;

    union {
        double d;
        uint64_t s;
    } hack;

    /* Consult cache */
    if (cache_get3(CACHE_BDD_SAT_COUNT_DOUBLE, bdd, variables, 0, &hack.s)) {
        sylvan_stats_count(BDD_SAT_COUNT_DOUBLE_CACHED);
        return (double)((long double)hack.d * powl(2.0L, (long double)skipped));
    }

    const BDDSET next = bdd_set_next(variables);
    bdd_sat_count_double_SPAWN(lace, mtbdd_node_high(bdd), next);
    double low = bdd_sat_count_double_CALL(lace, mtbdd_node_low(bdd), next);
    double result = low + bdd_sat_count_double_SYNC(lace);

    hack.d = result;
    if (cache_put3(CACHE_BDD_SAT_COUNT_DOUBLE, bdd, variables, 0, hack.s)) {
        sylvan_stats_count(BDD_SAT_COUNT_DOUBLE_CACHEDPUT);
    }

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

int
bdd_pick_minterm_CALL(lace_worker* lace, BDD *destination, BDD bdd, BDDSET vars)
{
    if (destination == NULL || bdd == mtbdd_invalid || vars == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    if (bdd == bdd_false) { *destination = bdd_false; return SYLVAN_OK; }
    if (bdd_set_is_empty(vars)) { *destination = bdd_true; return SYLVAN_OK; }

    bddnode* n_vars = MTBDD_GETNODE(vars);
    uint32_t var = bddnode_getvariable(n_vars);
    BDDSET next_vars = node_high(vars, n_vars);

    BDD child = mtbdd_invalid;
    mtbdd_refs_pushptr(&child);
    int status;

    if (bdd == bdd_true) {
        status = bdd_pick_minterm_CALL(lace, &child, bdd, next_vars);
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(destination, var, child, bdd_false);
        mtbdd_refs_popptr(1);
        return status;
    }

    bddnode* n_bdd = MTBDD_GETNODE(bdd);
    while (bddnode_getvariable(n_bdd) < var) {
        bdd = node_low(bdd, n_bdd) != bdd_false
            ? node_low(bdd, n_bdd)
            : node_high(bdd, n_bdd);
        if (bdd == bdd_true) {
            mtbdd_refs_popptr(1);
            return bdd_pick_minterm_CALL(lace, destination, bdd, vars);
        }
        n_bdd = MTBDD_GETNODE(bdd);
    }

    if (bddnode_getvariable(n_bdd) != var) {
        assert(bddnode_getvariable(n_bdd) > var);
        status = bdd_pick_minterm_CALL(lace, &child, bdd, next_vars);
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(destination, var, child, bdd_false);
    } else if (node_high(bdd, n_bdd) == bdd_false) {
        status = bdd_pick_minterm_CALL(lace, &child, node_low(bdd, n_bdd), next_vars);
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(destination, var, child, bdd_false);
    } else {
        status = bdd_pick_minterm_CALL(lace, &child, node_high(bdd, n_bdd), next_vars);
        if (status == SYLVAN_OK) status = _mtbdd_try_make_node(destination, var, bdd_false, child);
    }

    mtbdd_refs_popptr(1);
    return status;
}

int
bdd_pick_cube_CALL(lace_worker* lace, BDD *destination, BDD bdd, BDDSET vars)
{
    if (destination == NULL || bdd == mtbdd_invalid || vars == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    if (bdd == bdd_false) { *destination = bdd_false; return SYLVAN_OK; }
    if (bdd == bdd_true || bdd_set_is_empty(vars)) { *destination = bdd_true; return SYLVAN_OK; }

    bddnode* node = MTBDD_GETNODE(bdd);
    bddnode* vars_node = MTBDD_GETNODE(vars);
    uint32_t bdd_var = bddnode_getvariable(node);
    uint32_t var = bddnode_getvariable(vars_node);
    BDDSET next_vars = node_high(vars, vars_node);

    if (var < bdd_var) return bdd_pick_cube_CALL(lace, destination, bdd, next_vars);

    BDD low = node_low(bdd, node);
    BDD high = node_high(bdd, node);
    BDD chosen = low != bdd_false ? low : high;

    if (bdd_var < var) return bdd_pick_cube_CALL(lace, destination, chosen, vars);

    BDD child = mtbdd_invalid;
    mtbdd_refs_pushptr(&child);
    int status = bdd_pick_cube_CALL(lace, &child, chosen, next_vars);
    if (status == SYLVAN_OK) {
        status = low != bdd_false
            ? _mtbdd_try_make_node(destination, var, child, bdd_false)
            : _mtbdd_try_make_node(destination, var, bdd_false, child);
    }
    mtbdd_refs_popptr(1);
    return status;
}

int
bdd_cube_CALL(lace_worker* lace, BDD *destination, BDDSET vars, const uint8_t *cube)
{
    if (destination == NULL || vars == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    if (bdd_set_is_empty(vars)) { *destination = bdd_true; return SYLVAN_OK; }
    if (cube == NULL) return SYLVAN_ERR_INVALID;
    if (*cube > 2) return SYLVAN_ERR_INVALID;

    bddnode* n = MTBDD_GETNODE(vars);
    uint32_t var = bddnode_getvariable(n);
    BDDSET next_vars = node_high(vars, n);

    BDD child = mtbdd_invalid;
    mtbdd_refs_pushptr(&child);
    int status = bdd_cube_CALL(lace, &child, next_vars, cube + 1);
    if (status == SYLVAN_OK) {
        if (*cube == 0) status = _mtbdd_try_make_node(destination, var, child, bdd_false);
        else if (*cube == 1) status = _mtbdd_try_make_node(destination, var, bdd_false, child);
        else *destination = child;
    }
    mtbdd_refs_popptr(1);
    return status;
}

int
bdd_or_cube_CALL(lace_worker* lace, BDD *destination, BDD bdd, BDDSET vars, const uint8_t *cube)
{
    if (destination == NULL || bdd == mtbdd_invalid || vars == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }

    /* Terminal cases */
    if (bdd == bdd_true) { *destination = bdd_true; return SYLVAN_OK; }
    if (bdd_set_is_empty(vars)) { *destination = bdd_true; return SYLVAN_OK; }
    if (cube == NULL) return SYLVAN_ERR_INVALID;
    if (bdd == bdd_false) return bdd_cube_CALL(lace, destination, vars, cube);

    bddnode* nv = MTBDD_GETNODE(vars);
    for (;;) {
        if (*cube > 2) return SYLVAN_ERR_INVALID;
        if (*cube == 0 || *cube == 1) break;
        cube++;
        vars = node_high(vars, nv);
        if (bdd_set_is_empty(vars)) { *destination = bdd_true; return SYLVAN_OK; }
        nv = MTBDD_GETNODE(vars);
    }

    sylvan_gc_test(lace);

    // missing: SV_CNT_OP FIXME

    bddnode* n = MTBDD_GETNODE(bdd);
    BDD computed = bdd;
    mtbdd_refs_pushptr(&computed);
    uint32_t var = bddnode_getvariable(nv);
    uint32_t bdd_var = bddnode_getvariable(n);
    int status = SYLVAN_OK;

    if (var < bdd_var) {
        BDD child = mtbdd_invalid;
        mtbdd_refs_pushptr(&child);
        BDDSET next_vars = node_high(vars, nv);
        status = bdd_or_cube_CALL(lace, &child, bdd, next_vars, cube + 1);
        if (status == SYLVAN_OK) {
            status = *cube == 0
                ? _mtbdd_try_make_node(&computed, var, child, bdd)
                : _mtbdd_try_make_node(&computed, var, bdd, child);
        }
        mtbdd_refs_popptr(1);
    } else if (var > bdd_var) {
        BDD high = node_high(bdd, n);
        BDD low = node_low(bdd, n);
        BDD new_low = mtbdd_invalid, new_high = mtbdd_invalid;
        mtbdd_refs_pushptr(&new_low);
        mtbdd_refs_pushptr(&new_high);

        bdd_or_cube_SPAWN(lace, &new_high, high, vars, cube);
        int low_status = bdd_or_cube_CALL(lace, &new_low, low, vars, cube);
        int high_status = bdd_or_cube_SYNC(lace);
        status = low_status != SYLVAN_OK ? low_status : high_status;
        if (status == SYLVAN_OK && (new_low != low || new_high != high)) {
            status = _mtbdd_try_make_node(&computed, bdd_var, new_low, new_high);
        }

        mtbdd_refs_popptr(2);
    } else {
        BDD high = node_high(bdd, n);
        BDD low = node_low(bdd, n);
        BDD child = mtbdd_invalid;
        mtbdd_refs_pushptr(&child);
        BDDSET next_vars = node_high(vars, nv);

        if (*cube == 0) {
            status = bdd_or_cube_CALL(lace, &child, low, next_vars, cube + 1);
            if (status == SYLVAN_OK && child != low) {
                status = _mtbdd_try_make_node(&computed, bdd_var, child, high);
            }
        } else {
            status = bdd_or_cube_CALL(lace, &child, high, next_vars, cube + 1);
            if (status == SYLVAN_OK && child != high) {
                status = _mtbdd_try_make_node(&computed, bdd_var, low, child);
            }
        }

        mtbdd_refs_popptr(1);
    }

    if (status == SYLVAN_OK) *destination = computed;
    mtbdd_refs_popptr(1);
    return status;
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

TASK(int, bdd_collect_do, BDD*, result, BDD, bdd, BDDSET, vars, bdd_map_reduce_or_cb, cb, void*, context, struct bdd_path*, path)

int bdd_collect_do_CALL(lace_worker* lace, BDD *destination, BDD bdd, BDDSET vars, bdd_map_reduce_or_cb cb, void* context, struct bdd_path* path)
{
    if (destination == NULL || bdd == mtbdd_invalid || vars == mtbdd_invalid || cb == NULL) {
        return SYLVAN_ERR_INVALID;
    }

    if (bdd == bdd_false) {
        *destination = bdd_false;
        return SYLVAN_OK;
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
        BDD computed = cb(context, arr);
        lace_scratch_reset(lace, mark);
        if (computed == mtbdd_invalid) return SYLVAN_ERR_CALLBACK;
        *destination = computed;
        return SYLVAN_OK;
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
        BDD low = mtbdd_invalid, high = mtbdd_invalid, computed = mtbdd_invalid;
        mtbdd_refs_pushptr(&low);
        mtbdd_refs_pushptr(&high);
        mtbdd_refs_pushptr(&computed);

        bdd_collect_do_SPAWN(lace, &high, bdd1, dom_next, cb, context, &p1);
        int status = bdd_collect_do_CALL(lace, &low, bdd0, dom_next, cb, context, &p0);
        int high_status = bdd_collect_do_SYNC(lace);
        if (status == SYLVAN_OK) status = high_status;
        if (status == SYLVAN_OK) {
            status = bdd_and_CALL(lace, &computed, bdd_not(low), bdd_not(high));
            if (status == SYLVAN_OK) computed = bdd_not(computed);
        }
        if (status == SYLVAN_OK) *destination = computed;
        mtbdd_refs_popptr(3);
        return status;
    }
}

int bdd_map_reduce_or_CALL(lace_worker* lace, BDD *destination, BDD bdd, BDDSET vars, bdd_map_reduce_or_cb cb, void* context)
{
    return bdd_collect_do_CALL(lace, destination, bdd, vars, cb, context, NULL);
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
