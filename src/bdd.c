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

#include <sylvan/internal/internal.h>

#include <inttypes.h>
#include <math.h>
#include <string.h>

#include <avl.h>

/**
 * Implementation of unary, binary and if-then-else operators.
 */
BDD
bdd_and_CALL(lace_worker* lace, BDD a, BDD b)
{
    /* Terminal cases */
    if (a == mtbdd_true) return b;
    if (b == mtbdd_true) return a;
    if (a == mtbdd_false) return mtbdd_false;
    if (b == mtbdd_false) return mtbdd_false;
    if (a == b) return a;
    if (a == BDD_TOGGLEMARK(b)) return mtbdd_false;

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

    BDDVAR va = bddnode_getvariable(na);
    BDDVAR vb = bddnode_getvariable(nb);
    BDDVAR level = va < vb ? va : vb;

    BDD result;
    if (cache_get3(CACHE_BDD_AND, a, b, mtbdd_false, &result)) {
        sylvan_stats_count(BDD_AND_CACHED);
        return result;
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
    BDD low = mtbdd_invalid;
    BDD high = mtbdd_invalid;

    int n=0;

    if (aHigh == mtbdd_true) {
        high = bHigh;
    } else if (aHigh == mtbdd_false || bHigh == mtbdd_false) {
        high = mtbdd_false;
    } else if (bHigh == mtbdd_true) {
        high = aHigh;
    } else {
        mtbdd_refs_spawn(bdd_and_SPAWN(lace, aHigh, bHigh));
        n=1;
    }

    if (aLow == mtbdd_true) {
        low = bLow;
    } else if (aLow == mtbdd_false || bLow == mtbdd_false) {
        low = mtbdd_false;
    } else if (bLow == mtbdd_true) {
        low = aLow;
    } else {
        low = bdd_and_CALL(lace, aLow, bLow);
    }

    if (n) {
        mtbdd_refs_push(low);
        high = mtbdd_refs_sync(bdd_and_SYNC(lace));
        mtbdd_refs_pop(1);
    }

    result = mtbdd_makenode(level, low, high);

    if (cache_put3(CACHE_BDD_AND, a, b, mtbdd_false, result)) sylvan_stats_count(BDD_AND_CACHEDPUT);

    return result;
}

// FIXME improve documentation...
/*
    bdd_disjoint could be implemented as "bdd_and(a,b)==mtbdd_false",
    but this implementation avoids building new nodes and allows more short-circuitry.
*/
char bdd_disjoint_CALL(lace_worker* lace, BDD a, BDD b)
{
    /* Terminal cases */
    if (a == mtbdd_false || b == mtbdd_false) return 1; 
    if (a == mtbdd_true || b == mtbdd_true) return 0; /* since a,b != mtbdd_false */
    if (a == b) return 0; /* since a,b != mtbdd_false */
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

    BDDVAR va = bddnode_getvariable(na);
    BDDVAR vb = bddnode_getvariable(nb);
    BDDVAR level = va < vb ? va : vb;

    {
        BDD result;
        if (cache_get3(CACHE_BDD_DISJOINT, a, b, mtbdd_false, &result)) {
            sylvan_stats_count(BDD_DISJOINT_CACHED);
            return (result==mtbdd_false ? 0 : 1);
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

    if (aHigh == mtbdd_false || bHigh == mtbdd_false) {
        high = 1;
    } else if (aHigh == mtbdd_true || bHigh == mtbdd_true) {
        high = 0; /* since none of them is mtbdd_false */
    } else if (aHigh == bHigh) {
        high = 0; /* since none of them is mtbdd_false */
    } else if (aHigh == BDD_TOGGLEMARK(bHigh)) {
        high = 1;
    }

    if (aLow == mtbdd_false || bLow == mtbdd_false) {
        low = 1;
    } else if (aLow == mtbdd_true || bLow == mtbdd_true) {
        low = 0; /* since none of them is mtbdd_false */
    } else if (aLow == bLow) {
        low = 0; /* since none of them is mtbdd_false */
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
        BDD to_cache = (result ? mtbdd_true : mtbdd_false);
        if (cache_put3(CACHE_BDD_DISJOINT, a, b, mtbdd_false, to_cache)) {
            sylvan_stats_count(BDD_DISJOINT_CACHEDPUT);
        }
    }

    return result;
}

BDD bdd_xor_CALL(lace_worker* lace, BDD a, BDD b)
{
    /* Terminal cases */
    if (a == mtbdd_false) return b;
    if (b == mtbdd_false) return a;
    if (a == mtbdd_true) return bdd_not(b);
    if (b == mtbdd_true) return bdd_not(a);
    if (a == b) return mtbdd_false;
    if (a == bdd_not(b)) return mtbdd_true;

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

    BDDVAR va = bddnode_getvariable(na);
    BDDVAR vb = bddnode_getvariable(nb);
    BDDVAR level = va < vb ? va : vb;

    {
        BDD result;
        if (cache_get3(CACHE_BDD_XOR, a, b, mtbdd_false, &result)) {
            sylvan_stats_count(BDD_XOR_CACHED);
            return result;
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

    // Recursive computation
    BDD low, high, result;

    mtbdd_refs_spawn(bdd_xor_SPAWN(lace, aHigh, bHigh));
    low = bdd_xor_CALL(lace, aLow, bLow);
    mtbdd_refs_push(low);
    high = mtbdd_refs_sync(bdd_xor_SYNC(lace));
    mtbdd_refs_pop(1);

    result = mtbdd_makenode(level, low, high);

    if (cache_put3(CACHE_BDD_XOR, a, b, mtbdd_false, result)) sylvan_stats_count(BDD_XOR_CACHEDPUT);

    return result;
}


BDD bdd_ite_CALL(lace_worker *lace, BDD a, BDD b, BDD c)
{
    /* Terminal cases */
    if (a == mtbdd_true) return b;
    if (a == mtbdd_false) return c;
    if (a == b) b = mtbdd_true;
    if (a == bdd_not(b)) b = mtbdd_false;
    if (a == c) c = mtbdd_false;
    if (a == bdd_not(c)) c = mtbdd_true;
    if (b == c) return b;
    if (b == mtbdd_true && c == mtbdd_false) return a;
    if (b == mtbdd_false && c == mtbdd_true) return bdd_not(a);

    /* Cases that reduce to AND and XOR */

    // ITE(A,B,0) => AND(A,B)
    if (c == mtbdd_false) return bdd_and_CALL(lace, a, b);

    // ITE(A,1,C) => ~AND(~A,~C)
    if (b == mtbdd_true) return bdd_not(bdd_and_CALL(lace, bdd_not(a), bdd_not(c)));

    // ITE(A,0,C) => AND(~A,C)
    if (b == mtbdd_false) return bdd_and_CALL(lace, bdd_not(a), c);

    // ITE(A,B,1) => ~AND(A,~B)
    if (c == mtbdd_true) return bdd_not(bdd_and_CALL(lace, a, bdd_not(b)));

    // ITE(A,B,~B) => XOR(A,~B)
    if (b == bdd_not(c)) return bdd_xor_CALL(lace, a, c);

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

    BDDVAR va = bddnode_getvariable(na);
    BDDVAR vb = bddnode_getvariable(nb);
    BDDVAR vc = bddnode_getvariable(nc);

    // Get lowest level
    BDDVAR level = vb < vc ? vb : vc;

    // Fast case
    if (va < level && node_low(a, na) == mtbdd_false && node_high(a, na) == mtbdd_true) {
        BDD result = mtbdd_makenode(va, c, b);
        return mark ? bdd_not(result) : result;
    }

    if (va < level) level = va;

    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_ITE);

    {
        BDD result;
        if (cache_get3(CACHE_BDD_ITE, a, b, c, &result)) {
            sylvan_stats_count(BDD_ITE_CACHED);
            return mark ? bdd_not(result) : result;
        }
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
    BDD low=mtbdd_invalid, high=mtbdd_invalid, result;

    int n=0;

    if (aHigh == mtbdd_true) {
        high = bHigh;
    } else if (aHigh == mtbdd_false) {
        high = cHigh;
    } else {
        mtbdd_refs_spawn(bdd_ite_SPAWN(lace, aHigh, bHigh, cHigh));
        n=1;
    }

    if (aLow == mtbdd_true) {
        low = bLow;
    } else if (aLow == mtbdd_false) {
        low = cLow;
    } else {
        low = bdd_ite_CALL(lace, aLow, bLow, cLow);
    }

    if (n) {
        mtbdd_refs_push(low);
        high = mtbdd_refs_sync(bdd_ite_SYNC(lace));
        mtbdd_refs_pop(1);
    }

    result = mtbdd_makenode(level, low, high);

    if (cache_put3(CACHE_BDD_ITE, a, b, c, result)) sylvan_stats_count(BDD_ITE_CACHEDPUT);

    return mark ? bdd_not(result) : result;
}

/**
 * Compute constrain f@c, also called the generalized co-factor.
 * c is the "care function" - f@c equals f when c evaluates to True.
 */
BDD bdd_constrain_CALL(lace_worker* lace, BDD f, BDD c)
{
    /* Trivial cases */
    if (c == mtbdd_true) return f;
    if (c == mtbdd_false) return mtbdd_false;
    if (bdd_isconst(f)) return f;
    if (f == c) return mtbdd_true;
    if (f == bdd_not(c)) return mtbdd_false;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_CONSTRAIN);

    bddnode* nf = MTBDD_GETNODE(f);
    bddnode* nc = MTBDD_GETNODE(c);

    BDDVAR vf = bddnode_getvariable(nf);
    BDDVAR vc = bddnode_getvariable(nc);
    BDDVAR level = vf < vc ? vf : vc;

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

    if (cLow == mtbdd_false) {
        /* cLow is False, so result equals fHigh @ cHigh */
        if (cHigh == mtbdd_true) result = fHigh;
        else result = bdd_constrain_CALL(lace, fHigh, cHigh);
    } else if (cHigh == mtbdd_false) {
        /* cHigh is False, so result equals fLow @ cLow */
        if (cLow == mtbdd_true) result = fLow;
        else result = bdd_constrain_CALL(lace, fLow, cLow);
    } else if (cLow == mtbdd_true) {
        /* cLow is True, so low result equals fLow */
        BDD high = bdd_constrain_CALL(lace, fHigh, cHigh);
        result = mtbdd_makenode(level, fLow, high);
    } else if (cHigh == mtbdd_true) {
        /* cHigh is True, so high result equals fHigh */
        BDD low = bdd_constrain_CALL(lace, fLow, cLow);
        result = mtbdd_makenode(level, low, fHigh);
    } else {
        /* cLow and cHigh are not constrants... normal parallel recursion */
        mtbdd_refs_spawn(bdd_constrain_SPAWN(lace, fLow, cLow));
        BDD high = bdd_constrain_CALL(lace, fHigh, cHigh);
        mtbdd_refs_push(high);
        BDD low = mtbdd_refs_sync(bdd_constrain_SYNC(lace));
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(level, low, high);
    }

    if (cache_put3(CACHE_BDD_CONSTRAIN, f, c, 0, result)) sylvan_stats_count(BDD_CONSTRAIN_CACHEDPUT);

    return mark ? bdd_not(result) : result;
}

/**
 * Compute restrict f@c, which uses a heuristic to try and minimize a BDD f with respect to a care function c
 */
BDD bdd_restrict_CALL(lace_worker* lace, BDD f, BDD c)
{
    /* Trivial cases */
    if (c == mtbdd_true) return f;
    if (c == mtbdd_false) return mtbdd_false;
    if (bdd_isconst(f)) return f;
    if (f == c) return mtbdd_true;
    if (f == bdd_not(c)) return mtbdd_false;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_RESTRICT);

    bddnode* nf = MTBDD_GETNODE(f);
    bddnode* nc = MTBDD_GETNODE(c);

    BDDVAR vf = bddnode_getvariable(nf);
    BDDVAR vc = bddnode_getvariable(nc);
    BDDVAR level = vf < vc ? vf : vc;

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
        BDD new_c = bdd_not(bdd_and_CALL(lace, bdd_not(node_low(c, nc)), bdd_not(node_high(c, nc))));
        mtbdd_refs_push(new_c);
        result = bdd_restrict_CALL(lace, f, new_c);
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
        if (cLow == mtbdd_false) {
            /* sibling-substitution */
            result = bdd_restrict_CALL(lace, fHigh, cHigh);
        } else if (cHigh == mtbdd_false) {
            /* sibling-substitution */
            result = bdd_restrict_CALL(lace, fLow, cLow);
        } else {
            /* parallel recursion */
            mtbdd_refs_spawn(bdd_restrict_SPAWN(lace, fLow, cLow));
            BDD high = bdd_restrict_CALL(lace, fHigh, cHigh);
            mtbdd_refs_push(high);
            BDD low = mtbdd_refs_sync(bdd_restrict_SYNC(lace));
            mtbdd_refs_pop(1);
            result = mtbdd_makenode(level, low, high);
        }
    }

    if (cache_put3(CACHE_BDD_RESTRICT, f, c, 0, result)) sylvan_stats_count(BDD_RESTRICT_CACHEDPUT);

    return mark ? bdd_not(result) : result;
}

/**
 * Calculates \exists variables . a
 */
BDD bdd_exists_CALL(lace_worker* lace, BDD a, BDD variables)
{
    /* Terminal cases */
    if (a == mtbdd_true) return mtbdd_true;
    if (a == mtbdd_false) return mtbdd_false;
    if (mtbdd_set_isempty(variables)) return a;

    // a != constant
    bddnode* na = MTBDD_GETNODE(a);
    BDDVAR level = bddnode_getvariable(na);

    bddnode* nv = MTBDD_GETNODE(variables);
    BDDVAR vv = bddnode_getvariable(nv);
    while (vv < level) {
        variables = node_high(variables, nv);
        if (mtbdd_set_isempty(variables)) return a;
        nv = MTBDD_GETNODE(variables);
        vv = bddnode_getvariable(nv);
    }

    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_EXISTS);

    BDD result;
    if (cache_get3(CACHE_BDD_EXISTS, a, variables, 0, &result)) {
        sylvan_stats_count(BDD_EXISTS_CACHED);
        return result;
    }

    // Get cofactors
    BDD aLow = node_low(a, na);
    BDD aHigh = node_high(a, na);

    if (vv == level) {
        // level is in variable set, perform abstraction
        if (aLow == mtbdd_true || aHigh == mtbdd_true || aLow == bdd_not(aHigh)) {
            result = mtbdd_true;
        } else {
            BDD _v = mtbdd_set_next(variables);
            BDD low = bdd_exists_CALL(lace, aLow, _v);
            if (low == mtbdd_true) {
                result = mtbdd_true;
            } else {
                mtbdd_refs_push(low);
                BDD high = bdd_exists_CALL(lace, aHigh, _v);
                if (high == mtbdd_true) {
                    result = mtbdd_true;
                    mtbdd_refs_pop(1);
                } else if (low == mtbdd_false && high == mtbdd_false) {
                    result = mtbdd_false;
                    mtbdd_refs_pop(1);
                } else {
                    mtbdd_refs_push(high);
                    result = bdd_not(bdd_and_CALL(lace, bdd_not(low), bdd_not(high))); // low or high
                    mtbdd_refs_pop(2);
                }
            }
        }
    } else {
        // level is not in variable set
        BDD low, high;
        mtbdd_refs_spawn(bdd_exists_SPAWN(lace, aHigh, variables));
        low = bdd_exists_CALL(lace, aLow, variables);
        mtbdd_refs_push(low);
        high = mtbdd_refs_sync(bdd_exists_SYNC(lace));
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(level, low, high);
    }

    if (cache_put3(CACHE_BDD_EXISTS, a, variables, 0, result)) sylvan_stats_count(BDD_EXISTS_CACHEDPUT);

    return result;
}


/**
 * Calculate projection of <a> unto <v>
 * (Expects Boolean <a>)
 */
BDD bdd_project_CALL(lace_worker* lace, BDD a, BDDSET v)
{
    /**
     * Terminal cases
     */
    if (a == mtbdd_false) return mtbdd_false;
    if (a == mtbdd_true) return mtbdd_true;
    if (mtbdd_set_isempty(v)) return mtbdd_true;

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
        if (mtbdd_set_isempty(v_next)) return mtbdd_true;
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
    MTBDD result;
    if (cache_get3(CACHE_BDD_PROJECT, a, 0, v, &result)) {
        sylvan_stats_count(BDD_PROJECT_CACHED);
        return result;
    }

    /**
     * Get cofactors
     */
    const MTBDD a0 = mtbddnode_followlow(a, a_node);
    const MTBDD a1 = mtbddnode_followhigh(a, a_node);

    /**
     * Compute recursive result
     */
    if (v_var == a_var) {
        // variable in projection variables
        mtbdd_refs_spawn(bdd_project_SPAWN(lace, a0, v_next));
        const MTBDD high = mtbdd_refs_push(bdd_project_CALL(lace, a1, v_next));
        const MTBDD low = mtbdd_refs_sync(bdd_project_SYNC(lace));
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(a_var, low, high);
    } else {
        // variable not in projection variables
        mtbdd_refs_spawn(bdd_project_SPAWN(lace, a0, v));
        const MTBDD high = mtbdd_refs_push(bdd_project_CALL(lace, a1, v));
        const MTBDD low = mtbdd_refs_push(mtbdd_refs_sync(bdd_project_SYNC(lace)));
        result = bdd_not(bdd_and_CALL(lace, bdd_not(low), bdd_not(high)));
        mtbdd_refs_pop(2);
    }

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_BDD_PROJECT, a, 0, v, result)) {
        sylvan_stats_count(BDD_PROJECT_CACHEDPUT);
    }

    return result;
}


/**
 * Calculate exists(a AND b, v)
 */
BDD bdd_and_exists_CALL(lace_worker* lace, BDD a, BDD b, BDDSET v)
{
    /* Terminal cases */
    if (a == mtbdd_false) return mtbdd_false;
    if (b == mtbdd_false) return mtbdd_false;
    if (a == bdd_not(b)) return mtbdd_false;
    if (a == mtbdd_true && b == mtbdd_true) return mtbdd_true;

    /* Cases that reduce to "exists" and "and" */
    if (a == mtbdd_true) return bdd_exists_CALL(lace, b, v);
    if (b == mtbdd_true) return bdd_exists_CALL(lace, a, v);
    if (a == b) return bdd_exists_CALL(lace, a, v);
    if (mtbdd_set_isempty(v)) return bdd_and_CALL(lace, a, b);

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

    BDDVAR va = bddnode_getvariable(na);
    BDDVAR vb = bddnode_getvariable(nb);
    BDDVAR vv = bddnode_getvariable(nv);
    BDDVAR level = va < vb ? va : vb;

    /* Skip levels in v that are not in a and b */
    while (vv < level) {
        v = node_high(v, nv); // get next variable in conjunction
        if (mtbdd_set_isempty(v)) return bdd_and_CALL(lace, a, b);
        nv = MTBDD_GETNODE(v);
        vv = bddnode_getvariable(nv);
    }

    BDD result;
    if (cache_get3(CACHE_BDD_AND_EXISTS, a, b, v, &result)) {
        sylvan_stats_count(BDD_AND_EXISTS_CACHED);
        return result;
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

    if (level == vv) {
        // level is in variable set, perform abstraction
        BDD _v = node_high(v, nv);
        BDD low = bdd_and_exists_CALL(lace, aLow, bLow, _v);
        if (low == mtbdd_true || low == aHigh || low == bHigh) {
            result = low;
        } else {
            mtbdd_refs_push(low);
            BDD high;
            if (low == bdd_not(aHigh)) {
                high = bdd_exists_CALL(lace, bHigh, _v);
            } else if (low == bdd_not(bHigh)) {
                high = bdd_exists_CALL(lace, aHigh, _v);
            } else {
                high = bdd_and_exists_CALL(lace, aHigh, bHigh, _v);
            }
            if (high == mtbdd_true) {
                result = mtbdd_true;
                mtbdd_refs_pop(1);
            } else if (high == mtbdd_false) {
                result = low;
                mtbdd_refs_pop(1);
            } else if (low == mtbdd_false) {
                result = high;
                mtbdd_refs_pop(1);
            } else {
                mtbdd_refs_push(high);
                result = bdd_not(bdd_and_CALL(lace, bdd_not(low), bdd_not(high)));
                mtbdd_refs_pop(2);
            }
        }
    } else {
        // level is not in variable set
        mtbdd_refs_spawn(bdd_and_exists_SPAWN(lace, aHigh, bHigh, v));
        BDD low = bdd_and_exists_CALL(lace, aLow, bLow, v);
        mtbdd_refs_push(low);
        BDD high = mtbdd_refs_sync(bdd_and_exists_SYNC(lace));
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(level, low, high);
    }

    if (cache_put3(CACHE_BDD_AND_EXISTS, a, b, v, result)) sylvan_stats_count(BDD_AND_EXISTS_CACHEDPUT);

    return result;
}


/**
 * Calculate projection of (<a> AND <b>) unto <v>
 * (Expects Boolean <a> and <b>)
 */
MTBDD bdd_and_project_CALL(lace_worker* lace, MTBDD a, MTBDD b, MTBDD v)
{
    /**
     * Terminal cases
     */
    if (a == mtbdd_false) return mtbdd_false;
    if (b == mtbdd_false) return mtbdd_false;
    if (a == bdd_not(b)) return mtbdd_false;
    if (a == mtbdd_true && b == mtbdd_true) return mtbdd_true;
    if (mtbdd_set_isempty(v)) return mtbdd_true;

    /**
     * Cases that reduce to bdd_project
     */
    if (a == mtbdd_true || b == mtbdd_true || a == b) return bdd_project(b, v);

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
        if (mtbdd_set_isempty(v_next)) return mtbdd_true;
        v = v_next;
        v_node = MTBDD_GETNODE(v);
        v_var = mtbddnode_getvariable(v_node);
        v_next = mtbddnode_followhigh(v, v_node);
    }

    /**
     * Check the cache
     */
    MTBDD result;
    if (cache_get3(CACHE_BDD_AND_PROJECT, a, b, v, &result)) {
        sylvan_stats_count(BDD_AND_PROJECT_CACHED);
        return result;
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
    if (v_var == minvar) {
        // variable in projection variables
        mtbdd_refs_spawn(bdd_and_project_SPAWN(lace, a0, b0, v_next));
        const MTBDD high = mtbdd_refs_push(bdd_and_project_CALL(lace, a1, b1, v_next));
        const MTBDD low = mtbdd_refs_sync(bdd_and_project_SYNC(lace));
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(minvar, low, high);
    } else {
        // variable not in projection variables
        mtbdd_refs_spawn(bdd_and_project_SPAWN(lace, a0, b0, v));
        const MTBDD high = mtbdd_refs_push(bdd_and_project_CALL(lace, a1, b1, v));
        const MTBDD low = mtbdd_refs_push(mtbdd_refs_sync(bdd_and_project_SYNC(lace)));
        result = bdd_not(bdd_and_CALL(lace, bdd_not(low), bdd_not(high)));
        mtbdd_refs_pop(2);
    }

    /**
     * Put in cache
     */
    if (cache_put3(CACHE_BDD_AND_PROJECT, a, b, v, result)) {
        sylvan_stats_count(BDD_AND_PROJECT_CACHEDPUT);
    }

    return result;
}


BDD bdd_relnext_CALL(lace_worker* lace, BDD a, BDD b, BDDSET vars)
{
    /* Compute R(s) = \exists x: A(x) \and B(x,s) with support(result) = s, support(A) = s, support(B) = s+t
     * if vars == mtbdd_false, then every level is in s or t
     * any other levels (outside s,t) in B are ignored / existentially quantified
     */

    /* Terminals */
    if (a == mtbdd_true && b == mtbdd_true) return mtbdd_true;
    if (a == mtbdd_false) return mtbdd_false;
    if (b == mtbdd_false) return mtbdd_false;
    if (mtbdd_set_isempty(vars)) return a;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_RELNEXT);

    /* Determine top level */
    bddnode* na = bdd_isconst(a) ? 0 : MTBDD_GETNODE(a);
    bddnode* nb = bdd_isconst(b) ? 0 : MTBDD_GETNODE(b);

    BDDVAR va = na ? bddnode_getvariable(na) : 0xffffffff;
    BDDVAR vb = nb ? bddnode_getvariable(nb) : 0xffffffff;
    BDDVAR level = va < vb ? va : vb;

    /* Skip vars */
    int is_s_or_t = 0;
    bddnode* nv = 0;
    if (vars == mtbdd_false) {
        is_s_or_t = 1;
    } else {
        nv = MTBDD_GETNODE(vars);
        for (;;) {
            /* check if level is s/t */
            BDDVAR vv = bddnode_getvariable(nv);
            if (level == vv || (level^1) == vv) {
                is_s_or_t = 1;
                break;
            }
            /* check if level < s/t */
            if (level < vv) break;
            vars = node_high(vars, nv); // get next in vars
            if (mtbdd_set_isempty(vars)) return a;
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
        BDDVAR s = level & (~1);
        BDDVAR t = s+1;

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
        if (!bdd_isconst(b0)) {
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
        if (!bdd_isconst(b1)) {
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

        BDD _vars = vars == mtbdd_false ? mtbdd_false : node_high(vars, nv);

        mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a0, b00, _vars));
        mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a1, b10, _vars));
        mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a0, b01, _vars));
        mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a1, b11, _vars));

        BDD f = mtbdd_refs_sync(bdd_relnext_SYNC(lace)); mtbdd_refs_push(f);
        BDD e = mtbdd_refs_sync(bdd_relnext_SYNC(lace)); mtbdd_refs_push(e);
        BDD d = mtbdd_refs_sync(bdd_relnext_SYNC(lace)); mtbdd_refs_push(d);
        BDD c = mtbdd_refs_sync(bdd_relnext_SYNC(lace)); mtbdd_refs_push(c);

        mtbdd_refs_spawn(bdd_ite_SPAWN(lace, c, mtbdd_true, d)); /* a0 b00  \or  a1 b01 */
        mtbdd_refs_spawn(bdd_ite_SPAWN(lace, e, mtbdd_true, f)); /* a0 b01  \or  a1 b11 */

        /* R1 */ d = mtbdd_refs_sync(bdd_ite_SYNC(lace)); mtbdd_refs_push(d);
        /* R0 */ c = mtbdd_refs_sync(bdd_ite_SYNC(lace)); // not necessary: mtbdd_refs_push(c);

        mtbdd_refs_pop(5);
        result = mtbdd_makenode(s, c, d);
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
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a0, b0, vars));
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a1, b1, vars));

                BDD r1 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r1);
                BDD r0 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r0);
                result = bdd_not(bdd_and_CALL(lace, bdd_not(r0), bdd_not(r1)));
                mtbdd_refs_pop(2);
            } else {
                /* Quantify "b" variables, but keep "a" variables */
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a0, b0, vars));
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a0, b1, vars));
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a1, b0, vars));
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a1, b1, vars));

                BDD r11 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r11);
                BDD r10 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r10);
                BDD r01 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r01);
                BDD r00 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r00);

                mtbdd_refs_spawn(bdd_ite_SPAWN(lace, r00, mtbdd_true, r01));
                mtbdd_refs_spawn(bdd_ite_SPAWN(lace, r10, mtbdd_true, r11));

                BDD r1 = mtbdd_refs_sync(bdd_ite_SYNC(lace));
                mtbdd_refs_push(r1);
                BDD r0 = mtbdd_refs_sync(bdd_ite_SYNC(lace));
                mtbdd_refs_pop(5);

                result = mtbdd_makenode(level, r0, r1);
            }
        } else {
            /* Keep "a" variables */
            mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a0, b0, vars));
            mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a1, b1, vars));

            BDD r1 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
            mtbdd_refs_push(r1);
            BDD r0 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
            mtbdd_refs_pop(1);
            result = mtbdd_makenode(level, r0, r1);
        }
    }

    if (cache_put3(CACHE_BDD_RELNEXT, a, b, vars, result)) sylvan_stats_count(BDD_RELNEXT_CACHEDPUT);

    return result;
}

BDD bdd_relprev_CALL(lace_worker* lace, BDD a, BDD b, BDDSET vars)
{
    /* Compute \exists x: A(s,x) \and B(x,t)
     * if vars == mtbdd_false, then every level is in s or t
     * any other levels (outside s,t) in A are ignored / existentially quantified
     */

    /* Terminals */
    if (a == mtbdd_true && b == mtbdd_true) return mtbdd_true;
    if (a == mtbdd_false) return mtbdd_false;
    if (b == mtbdd_false) return mtbdd_false;
    if (mtbdd_set_isempty(vars)) return b;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_RELPREV);

    /* Determine top level */
    bddnode* na = bdd_isconst(a) ? 0 : MTBDD_GETNODE(a);
    bddnode* nb = bdd_isconst(b) ? 0 : MTBDD_GETNODE(b);

    BDDVAR va = na ? bddnode_getvariable(na) : 0xffffffff;
    BDDVAR vb = nb ? bddnode_getvariable(nb) : 0xffffffff;
    BDDVAR level = va < vb ? va : vb;

    /* Skip vars */
    int is_s_or_t = 0;
    bddnode* nv = 0;
    if (vars == mtbdd_false) {
        is_s_or_t = 1;
    } else {
        nv = MTBDD_GETNODE(vars);
        for (;;) {
            /* check if level is s/t */
            BDDVAR vv = bddnode_getvariable(nv);
            if (level == vv || (level^1) == vv) {
                is_s_or_t = 1;
                break;
            }
            /* check if level < s/t */
            if (level < vv) break;
            vars = node_high(vars, nv); // get next in vars
            if (mtbdd_set_isempty(vars)) return b;
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
        BDDVAR s = level & (~1);
        BDDVAR t = s+1;

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
        if (!bdd_isconst(a0)) {
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
        if (!bdd_isconst(a1)) {
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
        if (!bdd_isconst(b0)) {
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
        if (!bdd_isconst(b1)) {
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
        if (vars != mtbdd_false) {
            _vars = node_high(vars, nv);
            if (mtbdd_set_first(_vars) == t) _vars = mtbdd_set_next(_vars);
        } else {
            _vars = mtbdd_false;
        }

        if (b00 == b01) {
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a00, b0, _vars));
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a10, b0, _vars));
        } else {
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a00, b00, _vars));
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a00, b01, _vars));
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a10, b00, _vars));
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a10, b01, _vars));
        }

        if (b10 == b11) {
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a01, b1, _vars));
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a11, b1, _vars));
        } else {
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a01, b10, _vars));
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a01, b11, _vars));
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a11, b10, _vars));
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a11, b11, _vars));
        }

        BDD r00, r01, r10, r11;

        if (b10 == b11) {
            r11 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            r01 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
        } else {
            BDD r111 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            BDD r110 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            r11 = mtbdd_makenode(t, r110, r111);
            mtbdd_refs_pop(2);
            mtbdd_refs_push(r11);
            BDD r011 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            BDD r010 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            r01 = mtbdd_makenode(t, r010, r011);
            mtbdd_refs_pop(2);
            mtbdd_refs_push(r01);
        }

        if (b00 == b01) {
            r10 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            r00 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
        } else {
            BDD r101 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            BDD r100 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            r10 = mtbdd_makenode(t, r100, r101);
            mtbdd_refs_pop(2);
            mtbdd_refs_push(r10);
            BDD r001 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            BDD r000 = mtbdd_refs_push(mtbdd_refs_sync(bdd_relprev_SYNC(lace)));
            r00 = mtbdd_makenode(t, r000, r001);
            mtbdd_refs_pop(2);
            mtbdd_refs_push(r00);
         }

        mtbdd_refs_spawn(bdd_and_SPAWN(lace, bdd_not(r00), bdd_not(r01)));
        mtbdd_refs_spawn(bdd_and_SPAWN(lace, bdd_not(r10), bdd_not(r11)));

        BDD r1 = bdd_not(mtbdd_refs_push(mtbdd_refs_sync(bdd_and_SYNC(lace))));
        BDD r0 = bdd_not(mtbdd_refs_sync(bdd_and_SYNC(lace)));
        mtbdd_refs_pop(5);
        result = mtbdd_makenode(s, r0, r1);
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
                mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a0, b0, vars));
                mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a1, b1, vars));

                BDD r1 = mtbdd_refs_sync(bdd_relprev_SYNC(lace));
                mtbdd_refs_push(r1);
                BDD r0 = mtbdd_refs_sync(bdd_relprev_SYNC(lace));
                mtbdd_refs_push(r0);
                result = bdd_ite_CALL(lace, r0, mtbdd_true, r1);
                mtbdd_refs_pop(2);

            } else {
                /* Quantify "a" variables, but keep "b" variables */
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a0, b0, vars));
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a1, b0, vars));
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a0, b1, vars));
                mtbdd_refs_spawn(bdd_relnext_SPAWN(lace, a1, b1, vars));

                BDD r11 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r11);
                BDD r01 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r01);
                BDD r10 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r10);
                BDD r00 = mtbdd_refs_sync(bdd_relnext_SYNC(lace));
                mtbdd_refs_push(r00);

                mtbdd_refs_spawn(bdd_ite_SPAWN(lace, r00, mtbdd_true, r10));
                mtbdd_refs_spawn(bdd_ite_SPAWN(lace, r01, mtbdd_true, r11));

                BDD r1 = mtbdd_refs_sync(bdd_ite_SYNC(lace));
                mtbdd_refs_push(r1);
                BDD r0 = mtbdd_refs_sync(bdd_ite_SYNC(lace));
                mtbdd_refs_pop(5);

                result = mtbdd_makenode(level, r0, r1);
            }
        } else {
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a0, b0, vars));
            mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a1, b1, vars));

            BDD r1 = mtbdd_refs_sync(bdd_relprev_SYNC(lace));
            mtbdd_refs_push(r1);
            BDD r0 = mtbdd_refs_sync(bdd_relprev_SYNC(lace));
            mtbdd_refs_pop(1);
            result = mtbdd_makenode(level, r0, r1);
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
BDD bdd_closure_CALL(lace_worker* lace, BDD a)
{
    /* Terminals */
    if (a == mtbdd_true) return a;
    if (a == mtbdd_false) return a;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_CLOSURE);

    /* Determine top level */
    bddnode* n = MTBDD_GETNODE(a);
    BDDVAR level = bddnode_getvariable(n);

    /* Consult cache */
    BDD result;
    if (cache_get3(CACHE_BDD_CLOSURE, a, 0, 0, &result)) {
        sylvan_stats_count(BDD_CLOSURE_CACHED);
        return result;
    }

    BDDVAR s = level & (~1);
    BDDVAR t = s+1;

    BDD a0, a1;
    if (level == s) {
        a0 = node_low(a, n);
        a1 = node_high(a, n);
    } else {
        a0 = a1 = a;
    }

    BDD a00, a01, a10, a11;
    if (!bdd_isconst(a0)) {
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
    if (!bdd_isconst(a1)) {
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

    BDD u1 = bdd_closure_CALL(lace, a11);
    mtbdd_refs_push(u1);
    /* u3 = */ mtbdd_refs_spawn(bdd_relprev_SPAWN(lace, a01, u1, mtbdd_false));
    BDD u2 = bdd_relprev_CALL(lace, u1, a10, mtbdd_false);
    mtbdd_refs_push(u2);
    BDD e = bdd_relprev_CALL(lace, a01, u2, mtbdd_false);
    mtbdd_refs_push(e);
    e = bdd_ite_CALL(lace, a00, mtbdd_true, e);
    mtbdd_refs_pop(1);
    mtbdd_refs_push(e);
    e = bdd_closure_CALL(lace, e);
    mtbdd_refs_pop(1);
    mtbdd_refs_push(e);
    BDD g = bdd_relprev_CALL(lace, u2, e, mtbdd_false);
    mtbdd_refs_push(g);
    BDD u3 = mtbdd_refs_sync(bdd_relprev_SYNC(lace));
    mtbdd_refs_push(u3);
    BDD f = bdd_relprev_CALL(lace, e, u3, mtbdd_false);
    mtbdd_refs_push(f);
    BDD h = bdd_relprev_CALL(lace, u2, f, mtbdd_false);
    mtbdd_refs_push(h);
    h = bdd_ite_CALL(lace, u1, mtbdd_true, h);
    mtbdd_refs_pop(1);
    mtbdd_refs_push(h);

    BDD r0, r1;
    /* R0 */ r0 = mtbdd_makenode(t, e, f);
    mtbdd_refs_pop(7);
    mtbdd_refs_push(r0);
    /* R1 */ r1 = mtbdd_makenode(t, g, h);
    mtbdd_refs_pop(1);
    result = mtbdd_makenode(s, r0, r1);

    if (cache_put3(CACHE_BDD_CLOSURE, a, 0, 0, result)) sylvan_stats_count(BDD_CLOSURE_CACHEDPUT);

    return result;
}


/**
 * Function composition
 */
BDD bdd_compose_CALL(lace_worker* lace, BDD a, BDDMAP map)
{
    /* Trivial cases */
    if (a == mtbdd_false || a == mtbdd_true) return a;
    if (mtbdd_map_isempty(map)) return a;

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_COMPOSE);

    /* Determine top level */
    bddnode* n = MTBDD_GETNODE(a);
    BDDVAR level = bddnode_getvariable(n);

    /* Skip map */
    bddnode* map_node = MTBDD_GETNODE(map);
    BDDVAR map_var = bddnode_getvariable(map_node);
    while (map_var < level) {
        map = node_low(map, map_node);
        if (mtbdd_map_isempty(map)) return a;
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
    BDD root = map_var == level ? node_high(map, map_node) : mtbdd_ithvar(level);
    mtbdd_refs_push(root);
    result = bdd_ite_CALL(lace, root, high, low);
    mtbdd_refs_pop(3);

    if (cache_put3(CACHE_BDD_COMPOSE, a, map, 0, result)) sylvan_stats_count(BDD_COMPOSE_CACHEDPUT);

    return result;
}

/**
 * Calculate the number of distinct paths to True.
 */
double bdd_pathcount_CALL(lace_worker* lace, BDD bdd)
{
    /* Trivial cases */
    if (bdd == mtbdd_false) return 0.0;
    if (bdd == mtbdd_true) return 1.0;

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

    bdd_pathcount_SPAWN(lace, mtbdd_getlow(bdd));
    bdd_pathcount_SPAWN(lace, mtbdd_gethigh(bdd));
    result = bdd_pathcount_SYNC(lace);
    result += bdd_pathcount_SYNC(lace);

    if (cache_put3(CACHE_BDD_PATHCOUNT, bdd, 0, 0, *(uint64_t*)&result)) sylvan_stats_count(BDD_PATHCOUNT_CACHEDPUT);

    return result;
}

/**
 * Calculate the number of satisfying variable assignments according to <variables>.
 */
double bdd_satcount_CALL(lace_worker* lace, BDD bdd, BDDSET variables)
{
    /* Trivial cases */
    if (bdd == mtbdd_false) return 0.0;
    if (bdd == mtbdd_true) return powl(2.0L, mtbdd_set_count(variables));

    /* Perhaps execute garbage collection */
    sylvan_gc_test(lace);

    /* Count operation */
    sylvan_stats_count(BDD_SATCOUNT);

    /* Count variables before var(bdd) */
    size_t skipped = 0;
    BDDVAR var = mtbdd_getvar(bdd);
    bddnode* set_node = MTBDD_GETNODE(variables);
    BDDVAR set_var = bddnode_getvariable(set_node);
    while (var != set_var) {
        skipped++;
        variables = node_high(variables, set_node);
        // if this assertion fails, then variables is not the support of <bdd>
        assert(!mtbdd_set_isempty(variables));
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
        return hack.d * powl(2.0L, skipped);
    }

    bdd_satcount_SPAWN(lace, mtbdd_gethigh(bdd), node_high(variables, set_node));
    double low = bdd_satcount_CALL(lace, mtbdd_getlow(bdd), node_high(variables, set_node));
    double result = low + bdd_satcount_SYNC(lace);

    hack.d = result;
    if (cache_put3(CACHE_BDD_SATCOUNT, bdd, variables, 0, hack.s)) sylvan_stats_count(BDD_SATCOUNT_CACHEDPUT);

    return result * powl(2.0L, skipped);
}

int
bdd_sat_one(BDD bdd, BDDSET vars, uint8_t *str)
{
    if (bdd == mtbdd_false) return 0;
    if (str == NULL) return 0;
    if (mtbdd_set_isempty(vars)) return 1;

    for (;;) {
        bddnode* n_vars = MTBDD_GETNODE(vars);
        if (bdd == mtbdd_true) {
            *str = 0;
        } else {
            bddnode* n_bdd = MTBDD_GETNODE(bdd);
            if (bddnode_getvariable(n_bdd) != bddnode_getvariable(n_vars)) {
                *str = 0;
            } else {
                if (node_low(bdd, n_bdd) == mtbdd_false) {
                    // take high edge
                    *str = 1;
                    bdd = node_high(bdd, n_bdd);
                } else {
                    // take low edge
                    *str = 0;
                    bdd = node_low(bdd, n_bdd);
                }
            }
        }
        vars = node_high(vars, n_vars);
        if (mtbdd_set_isempty(vars)) break;
        str++;
    }

    return 1;
}

BDD
bdd_sat_single(BDD bdd, BDDSET vars)
{
    if (bdd == mtbdd_false) return mtbdd_false;
    if (mtbdd_set_isempty(vars)) {
        assert(bdd == mtbdd_true);
        return mtbdd_true;
    }

    bddnode* n_vars = MTBDD_GETNODE(vars);
    uint32_t var = bddnode_getvariable(n_vars);
    BDD next_vars = node_high(vars, n_vars);
    if (bdd == mtbdd_true) {
        // take false
        BDD res = bdd_sat_single(bdd, next_vars);
        return mtbdd_makenode(var, res, mtbdd_false);
    }
    bddnode* n_bdd = MTBDD_GETNODE(bdd);
    if (bddnode_getvariable(n_bdd) != var) {
        assert(bddnode_getvariable(n_bdd)>var);
        // take false
        BDD res = bdd_sat_single(bdd, next_vars);
        return mtbdd_makenode(var, res, mtbdd_false);
    }
    if (node_high(bdd, n_bdd) == mtbdd_false) {
        // take false
        BDD res = bdd_sat_single(node_low(bdd, n_bdd), next_vars);
        return mtbdd_makenode(var, res, mtbdd_false);
    }
    // take true
    BDD res = bdd_sat_single(node_high(bdd, n_bdd), next_vars);
    return mtbdd_makenode(var, mtbdd_false, res);
}

BDD
bdd_sat_one_bdd(BDD bdd)
{
    if (bdd == mtbdd_false) return mtbdd_false;
    if (bdd == mtbdd_true) return mtbdd_true;

    bddnode* node = MTBDD_GETNODE(bdd);
    BDD low = node_low(bdd, node);
    BDD high = node_high(bdd, node);

    BDD m;

    BDD result;
    if (low == mtbdd_false) {
        m = bdd_sat_one_bdd(high);
        result = mtbdd_makenode(bddnode_getvariable(node), mtbdd_false, m);
    } else if (high == mtbdd_false) {
        m = bdd_sat_one_bdd(low);
        result = mtbdd_makenode(bddnode_getvariable(node), m, mtbdd_false);
    } else {
        if (rand() & 0x2000) {
            m = bdd_sat_one_bdd(low);
            result = mtbdd_makenode(bddnode_getvariable(node), m, mtbdd_false);
        } else {
            m = bdd_sat_one_bdd(high);
            result = mtbdd_makenode(bddnode_getvariable(node), mtbdd_false, m);
        }
    }

    return result;
}

BDD
bdd_cube(BDDSET vars, uint8_t *cube)
{
    if (mtbdd_set_isempty(vars)) return mtbdd_true;

    bddnode* n = MTBDD_GETNODE(vars);
    BDDVAR v = bddnode_getvariable(n);
    vars = node_high(vars, n);

    BDD result = bdd_cube(vars, cube+1);
    if (*cube == 0) {
        result = mtbdd_makenode(v, result, mtbdd_false);
    } else if (*cube == 1) {
        result = mtbdd_makenode(v, mtbdd_false, result);
    }

    return result;
}

BDD bdd_union_cube_CALL(lace_worker* lace, BDD bdd, BDDSET vars, uint8_t * cube)
{
    /* Terminal cases */
    if (bdd == mtbdd_true) return mtbdd_true;
    if (bdd == mtbdd_false) return bdd_cube(vars, cube);
    if (mtbdd_set_isempty(vars)) return mtbdd_true;

    bddnode* nv = MTBDD_GETNODE(vars);

    for (;;) {
        if (*cube == 0 || *cube == 1) break;
        // *cube should be 2
        cube++;
        vars = node_high(vars, nv);
        if (mtbdd_set_isempty(vars)) return mtbdd_true;
        nv = MTBDD_GETNODE(vars);
    }

    sylvan_gc_test(lace);

    // missing: SV_CNT_OP FIXME

    bddnode* n = MTBDD_GETNODE(bdd);
    BDD result = bdd;
    BDDVAR v = bddnode_getvariable(nv);
    BDDVAR n_level = bddnode_getvariable(n);

    if (v < n_level) {
        vars = node_high(vars, nv);
        if (*cube == 0) {
            result = bdd_union_cube_CALL(lace, bdd, vars, cube+1);
            result = mtbdd_makenode(v, result, bdd);
        } else /* *cube == 1 */ {
            result = bdd_union_cube_CALL(lace, bdd, vars, cube+1);
            result = mtbdd_makenode(v, bdd, result);
        }
    } else if (v > n_level) {
        BDD high = node_high(bdd, n);
        BDD low = node_low(bdd, n);
        mtbdd_refs_spawn(bdd_union_cube_SPAWN(lace, high, vars, cube));
        BDD new_low = bdd_union_cube_CALL(lace, low, vars, cube);
        mtbdd_refs_push(new_low);
        BDD new_high = mtbdd_refs_sync(bdd_union_cube_SYNC(lace));
        mtbdd_refs_pop(1);
        if (new_low != low || new_high != high) {
            result = mtbdd_makenode(n_level, new_low, new_high);
        }
    } else /* v == n_level */ {
        vars = node_high(vars, nv);
        BDD high = node_high(bdd, n);
        BDD low = node_low(bdd, n);
        if (*cube == 0) {
            BDD new_low = bdd_union_cube(low, vars, cube+1);
            if (new_low != low) {
                result = mtbdd_makenode(n_level, new_low, high);
            }
        } else /* *cube == 1 */ {
            BDD new_high = bdd_union_cube(high, vars, cube+1);
            if (new_high != high) {
                result = mtbdd_makenode(n_level, low, new_high);
            }
        }
    }

    return result;
}

struct bdd_path
{
    struct bdd_path *prev;
    BDDVAR var;
    int8_t val; // 0=false, 1=true, 2=both
};

void bdd_enum_do_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_enum_cb cb, void* context, struct bdd_path* path)
{
    if (bdd == mtbdd_false) return;

    if (mtbdd_set_isempty(vars)) {
        /* bdd should now be true */
        assert(bdd == mtbdd_true);
        /* compute length of path */
        int i=0;
        struct bdd_path *pp;
        for (pp = path; pp != NULL; pp = pp->prev) i++;
        /* if length is 0 (enum called with empty vars??), return */
        if (i == 0) return;
        /* fill cube and vars with trace */
        uint8_t* cube = SYLVAN_ALLOCA(uint8_t, i);
        BDDVAR *vars = SYLVAN_ALLOCA(BDDVAR, i);
        int j=0;
        for (pp = path; pp != NULL; pp = pp->prev) {
            cube[i-j-1] = pp->val;
            vars[i-j-1] = pp->var;
            j++;
        }
        /* call callback */
        cb(context, vars, cube, i);
        return;
    }

    BDDVAR var = mtbdd_getvar(vars);
    vars = mtbdd_set_next(vars);
    BDDVAR bdd_var = mtbdd_getvar(bdd);

    /* assert var <= bdd_var */
    if (bdd == mtbdd_true || var < bdd_var) {
        struct bdd_path pp0 = (struct bdd_path){path, var, 0};
        bdd_enum_do_CALL(lace, bdd, vars, cb, context, &pp0);
        struct bdd_path pp1 = (struct bdd_path){path, var, 1};
        bdd_enum_do_CALL(lace, bdd, vars, cb, context, &pp1);
    } else if (var == bdd_var) {
        struct bdd_path pp0 = (struct bdd_path){path, var, 0};
        bdd_enum_do_CALL(lace, mtbdd_getlow(bdd), vars, cb, context, &pp0);
        struct bdd_path pp1 = (struct bdd_path){path, var, 1};
        bdd_enum_do_CALL(lace, mtbdd_gethigh(bdd), vars, cb, context, &pp1);
    } else {
        printf("var %u not expected (expecting %u)!\n", bdd_var, var);
        assert(var <= bdd_var);
    }
}

TASK(void, bdd_enum_par_do, BDD, bdd, BDDSET, vars, bdd_enum_cb, cb, void*, context, struct bdd_path*, path)

void bdd_enum_par_do_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_enum_cb cb, void* context, struct bdd_path* path)
{
    if (bdd == mtbdd_false) return;

    if (mtbdd_set_isempty(vars)) {
        /* bdd should now be true */
        assert(bdd == mtbdd_true);
        /* compute length of path */
        int i=0;
        struct bdd_path *pp;
        for (pp = path; pp != NULL; pp = pp->prev) i++;
        /* if length is 0 (enum called with empty vars??), return */
        if (i == 0) return;
        /* fill cube and vars with trace */
        uint8_t* cube = SYLVAN_ALLOCA(uint8_t, i);
        BDDVAR* vars = SYLVAN_ALLOCA(BDDVAR, i);
        int j=0;
        for (pp = path; pp != NULL; pp = pp->prev) {
            cube[i-j-1] = pp->val;
            vars[i-j-1] = pp->var;
            j++;
        }
        /* call callback */
        cb(context, vars, cube, i);
        return;
    }

    BDD var = mtbdd_getvar(vars);
    vars = mtbdd_set_next(vars);
    BDD bdd_var = mtbdd_getvar(bdd);

    /* assert var <= bdd_var */
    if (var < bdd_var) {
        struct bdd_path pp1 = (struct bdd_path){path, var, 1};
        bdd_enum_par_do_SPAWN(lace, bdd, vars, cb, context, &pp1);
        struct bdd_path pp0 = (struct bdd_path){path, var, 0};
        bdd_enum_par_do_CALL(lace, bdd, vars, cb, context, &pp0);
        bdd_enum_par_do_SYNC(lace);
    } else if (var == bdd_var) {
        struct bdd_path pp1 = (struct bdd_path){path, var, 1};
        bdd_enum_par_do_SPAWN(lace, mtbdd_gethigh(bdd), vars, cb, context, &pp1);
        struct bdd_path pp0 = (struct bdd_path){path, var, 0};
        bdd_enum_par_do_CALL(lace, mtbdd_getlow(bdd), vars, cb, context, &pp0);
        bdd_enum_par_do_SYNC(lace);
    } else {
        assert(var <= bdd_var);
    }
}

//FIXME clean this up
void bdd_enum_CALL(lace_worker* lace, BDD bdd, BDDSET  vars, bdd_enum_cb cb, void* context)
{
    bdd_enum_do_CALL(lace, bdd, vars, cb, context, 0);
}

void bdd_enum_par_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_enum_cb cb, void* context)
{
    bdd_enum_par_do_CALL(lace, bdd, vars, cb, context, 0);
}

TASK(BDD, bdd_collect_do, BDD, bdd, BDDSET, vars, bdd_collect_cb, cb, void*, context, struct bdd_path*, path)

BDD bdd_collect_do_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_collect_cb cb, void* context, struct bdd_path* path)
{
    if (bdd == mtbdd_false) {
         return mtbdd_false;
    } else if (mtbdd_set_isempty(vars)) {
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
        uint8_t* arr = SYLVAN_ALLOCA(uint8_t, len);
        for (size_t i=0; i<len; i++) {
            arr[len-i-1] = path->val;
            path = path->prev;
        }
        /**
         * Call callback
         */
        return cb(context, arr);
    } else {
        /**
         * Obtain domain variable
         */
        const uint32_t dom_var = mtbdd_getvar(vars);
        const BDD dom_next = mtbdd_set_next(vars);
        /**
         * Obtain cofactors
         */
        BDD bdd0, bdd1;
        if (bdd == mtbdd_true) {
            bdd0 = bdd1 = bdd;
        } else {
            const uint32_t bdd_var = mtbdd_getvar(bdd);
            assert(dom_var <= bdd_var);
            if (dom_var < bdd_var) {
                bdd0 = bdd1 = bdd;
            } else {
                bdd0 = mtbdd_getlow(bdd);
                bdd1 = mtbdd_gethigh(bdd);
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
        BDD res = bdd_not(bdd_and_CALL(lace, bdd_not(low), bdd_not(high)));
        mtbdd_refs_pop(2);
        return res;
    }
}

// FIXME we don't need the extra indirection?
BDD bdd_collect_CALL(lace_worker* lace, BDD bdd, BDDSET vars, bdd_collect_cb cb, void* context)
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
    if (bdd_isnode(bdd)) {
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
    if (!bdd_isnode(bdd)) return bdd;
    struct bdd_ser s, *ss;
    s.bdd = BDD_STRIPMARK(bdd);
    ss = bdd_ser_search(bdd_ser_set, &s);
    assert(ss != NULL);
    return BDD_TRANSFERMARK(bdd, ss->assigned);
}

BDD
bdd_serialize_get_reversed(size_t value)
{
    if (!bdd_isnode(value)) return value;
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
        s.bdd = mtbdd_makenode(bddnode_getvariable(&node), low, high);
        s.assigned = ++bdd_ser_done; // starts at 0 but we want 1-based...

        bdd_ser_insert(&bdd_ser_set, &s);
        bdd_ser_reversed_insert(&bdd_ser_reversed_set, &s);
    }
}

