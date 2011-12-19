#include "runtime.h"
#include "sylvan.h"
#include "llvector.h"
#include "llsched.h"
#include "llgcset.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>

#define complementmark 0x80000000

/**
 * Exported BDD constants
 */
const BDD sylvan_true = 0 | complementmark;
const BDD sylvan_false = 0;
const BDD sylvan_invalid = 0x7fffffff; // uint32_t

/**
 * "volatile" macros
 */
#define atomicsylvan_read(s) atomic32_read(s)
#define atomicsylvan_write(a, b) atomic32_write(a, b)

/**
 * Mark handling macros
 */
#define BDD_HASMARK(s)              (s&complementmark)
#define BDD_TOGGLEMARK(s)           (s^complementmark)
#define BDD_STRIPMARK(s)            (s&~complementmark)
#define BDD_TRANSFERMARK(from, to)  (to ^ (from & complementmark))
#define BDD_ISCONSTANT(s)           (BDD_STRIPMARK(s) == 0)

__attribute__ ((packed))
struct bddnode {
    BDD low;
    BDD high;
    BDDVAR level;
    uint8_t flags; // for marking, e.g. in node_count 
    char pad1[SYLVAN_PAD(sizeof(BDD)*2+sizeof(BDDVAR)+sizeof(uint8_t), 16)];
}; // 16 bytes

typedef struct bddnode* bddnode_t;

#define op_ite 0
#define op_not 1
#define op_substitute 2
#define op_exists 3
#define op_forall 4
#define op_param 5
/*
typedef unsigned char bdd_ops;

__attribute__ ((packed))
struct bddop {
    BDDVAR level; // 2 bytes
    bdd_ops type; // 1 byte
    unsigned char parameters; // 1 byte, the cumulative number of BDD parameters
    union { // 12 bytes max
        struct {
            BDDOP a;
            BDDOP b;
            BDDOP c;
        } param_ite;
        struct {
            BDDOP a;
        } param_not;
        struct {
            BDDOP a;
            BDDVAR from;
            BDD to;
        } param_substitute;
        struct {
            BDDOP a;
            BDDVAR var;
        } param_quantify;
        struct {
            unsigned short param;
        } param_param;
    };
};

typedef struct bddop* bddop_t;
*/
static struct {
    llgcset_t data;
    //llset_t operations; // all operations...
#if CACHE
    llgcset_t cache; // operations cache
#endif
} _bdd;

// max number of parameters (set to: 5, 13, 29 to get bddcache node size 32, 64, 128)
#define MAXPARAM 5

#if CACHE
/*
 * Temporary "operations"
 * 0 = ite
 * 1 = relprods
 * 2 = relprods_reversed
 * 3 = count
 * 4 = exists
 * 5 = forall
 */
__attribute__ ((packed))
struct bddcache {
    BDDOP operation;
    BDD params[MAXPARAM];
    uint32_t parameters; // so we don't have to read <<operation>>
    BDD result;
};

typedef struct bddcache* bddcache_t;
#endif
/**
 * Macro's to convert BDD indices to nodes and vice versa
 */
#define GETNODE(bdd)        ((bddnode_t)llgcset_index_to_ptr(_bdd.data, BDD_STRIPMARK(bdd)))
#if CACHE
#define GETCACHE(bdd)       ((bddcache_t)llgcset_index_to_ptr(_bdd.cache, BDD_STRIPMARK(bdd)))
#define GETCACHEBDD(node)   ((BDD)llgcset_ptr_to_index(_bdd.cache, node))
#endif
/**
 * Hash() and Equals() for the BDD hashtable
 * BDD nodes are 10 bytes in our implementation = 2 * sizeof(BDD) + sizeof(BDDVAR)
 */
uint32_t sylvan_bdd_hash(const void *data_, unsigned int len __attribute__((unused)), uint32_t hash)
{
    return SuperFastHash(data_, 10, hash);
    //return SuperFastHash(data_, len, hash);
}

int sylvan_bdd_equals(const void *a, const void *b, size_t length __attribute__((unused)))
{
    // Compare 10 bytes
    register uint64_t ra = *(uint64_t*)a;
    register uint64_t rb = *(uint64_t*)b;
    if (ra != rb) return 0;
    register uint16_t rc = *(uint8_t*)(a+8);
    register uint16_t rd = *(uint8_t*)(b+8);
    if (rc != rd) return 0;
    return 1;
    //return memcmp(a, b, 10) == 0;
}

/**
 * When a bdd node is deleted, unref the children
 */
void sylvan_bdd_delete(const llgcset_t dbs, const void *a)
{
    bddnode_t node = (bddnode_t)a;
    sylvan_deref(node->low);
    sylvan_deref(node->high);
}

/**
 * Called pre-gc : first, gc the cache to free nodes
 */
void sylvan_bdd_on_full(const llgcset_t dbs) 
{
#if CACHE
    llgcset_gc(_bdd.cache);
#endif
}
#if CACHE
/**
 * Hash() and Equals() for the operations cache
 * Cache nodes are 4 bytes (operation) + n * 4 bytes (every parameter)
 */
uint32_t sylvan_cache_hash(const void *data_, unsigned int len __attribute__((unused)), uint32_t hash)
{
    register unsigned int size = ((bddcache_t)data_)->parameters * 4 + 4;
    return SuperFastHash(data_, size, hash);
}

int sylvan_cache_equals(const void *a, const void *b, size_t length __attribute__((unused)))
{
    register unsigned int size = ((bddcache_t)a)->parameters * 4 + 4;
    if (((bddcache_t)b)->parameters != ((bddcache_t)a)->parameters) return 0;
    return memcmp(a, b, size) == 0;
}

/**
 * When a cache item is deleted, deref all involved BDDs
 */
void sylvan_cache_delete(const llgcset_t dbs, const void *a)
{
    bddcache_t cache = (bddcache_t)a;
    int i;
    for (i=0;i<cache->parameters;i++) {
        sylvan_deref(cache->params[i]);
    }
    assert (cache->result != sylvan_invalid);
    sylvan_deref(cache->result);
}
#endif
/**
 * Initialize sylvan
 * - datasize / cachesize : number of bits ...
 */
void sylvan_init(int threads, size_t datasize, size_t cachesize, size_t data_gc_size, size_t cache_gc_size)
{
    
    // Sanity check
    if (sizeof(struct bddnode) != 16) {
        fprintf(stderr, "Invalid size of bdd nodes: %ld\n", sizeof(struct bddnode));
        exit(1);
    }
    /*
    if (sizeof(struct bddop) != 16) {
        fprintf(stderr, "Invalid size of bdd operation nodes: %ld\n", sizeof(struct bddop));
        exit(1);
    }*/
#if CACHE
    if (sizeof(struct bddcache) != next_pow2(sizeof(struct bddcache))) {
        fprintf(stderr, "Invalid size of bdd operation cache: %ld\n", sizeof(struct bddcache));
        exit(1);
    }
#endif
    //fprintf(stderr, "Sylvan\n");
    
    if (datasize >= 30) {
        rt_report_and_exit(1, "BDD_init error: datasize must be < 30!");
    }
    if (datasize > 20) {
        //fprintf(stderr, "Data: %d slots, %d bytes per node, %d MB total\n", 1<<datasize, sizeof(struct bddnode), (1<<(datasize-20)) * sizeof(struct bddnode));
    } else {
        //fprintf(stderr, "Data: %d slots, %d bytes per node, %d KB total\n", 1<<datasize, sizeof(struct bddnode), (1<<(datasize-10)) * sizeof(struct bddnode));
    }
    _bdd.data = llgcset_create(sizeof(struct bddnode), datasize, data_gc_size, sylvan_bdd_hash, sylvan_bdd_equals, sylvan_bdd_delete, sylvan_bdd_on_full);
#if CACHE    
    if (cachesize >= 30) {
        rt_report_and_exit(1, "BDD_init error: cachesize must be < 30!");
    }
    if (cachesize > 20) {
        //fprintf(stderr, "Cache: %d slots, %d bytes per node, %d MB total\n", 1<<cachesize, sizeof(struct bddcache), (1<<(cachesize-20)) * sizeof(struct bddcache));
    } else {
        //fprintf(stderr, "Cache: %d slots, %d bytes per node, %d MB total\n", 1<<cachesize, sizeof(struct bddcache), (1<<(cachesize-10)) * sizeof(struct bddcache));
    }
    
    _bdd.cache = llgcset_create(sizeof(struct bddcache), cachesize, cache_gc_size, sylvan_cache_hash, sylvan_cache_equals, sylvan_cache_delete, NULL);
#endif
}

void sylvan_quit()
{
#if CACHE
    llgcset_free(_bdd.cache);
#endif
    llgcset_free(_bdd.data);
}

BDD sylvan_ref(BDD a) 
{
    if (!BDD_ISCONSTANT(a)) llgcset_ref(_bdd.data, BDD_STRIPMARK(a));
    return a;
}

void sylvan_deref(BDD a)
{
    if (BDD_ISCONSTANT(a)) return;
    assert(llgcset_deref(_bdd.data, BDD_STRIPMARK(a)));
}

void sylvan_gc()
{
    llgcset_gc(_bdd.data);
}

/**
 * MAKENODE (level, low, high)
 * Requires ref on low, high.
 * Ensures ref on result node.
 * This will ref the result node. Refs on low and high disappear.
 */
inline BDD sylvan_makenode(BDDVAR level, BDD low, BDD high)
{
    BDD result;
    struct bddnode n;

    if (low == high) {
        sylvan_deref(high);
        return low;
    }
    n.level = level;
    n.flags = 0;

    // Normalization to keep canonicity
    // low will have no mark

    int created;

    if (BDD_HASMARK(low)) {
        // ITE(a,not b,c) == not ITE(a,b,not c)
        n.low = BDD_STRIPMARK(low);
        n.high = BDD_TOGGLEMARK(high);

        if (llgcset_get_or_create(_bdd.data, &n, &created, &result) == 0) {
            rt_report_and_exit(1, "BDD Unique table full!");
        }
        
        if (!created) {
            sylvan_deref(low);
            sylvan_deref(high);
        }

        return result | complementmark;
    } else {
        n.low = low;
        n.high = high;

        if(llgcset_get_or_create(_bdd.data, &n, &created, &result) == 0) {
            rt_report_and_exit(1, "BDD Unique table full!");
        }

        if (!created) {
            sylvan_deref(low);
            sylvan_deref(high);
        }

        return result;
    }
}

inline BDD sylvan_ithvar(BDDVAR level)
{
    return sylvan_makenode(level, sylvan_false, sylvan_true);
}

inline BDD sylvan_nithvar(BDDVAR level)
{
    return sylvan_makenode(level, sylvan_true, sylvan_false);
}

inline BDDVAR sylvan_var(BDD bdd)
{
    assert(!BDD_ISCONSTANT(bdd));
    //if (BDD_ISCONSTANT(bdd)) return sylvan_invalid;
    return GETNODE(bdd)->level;
}

/**
 * Get the n=0 child.
 * This will ref the result node.
 */
inline BDD sylvan_low(BDD bdd)
{
    if (bdd == sylvan_false || bdd == sylvan_true) return bdd;
    BDD low = GETNODE(bdd)->low;
    sylvan_ref(low);
    return BDD_TRANSFERMARK(bdd, low);
}

// Macro for internal use (no ref)
#define LOW(a) ((BDD_ISCONSTANT(a))?a:BDD_TRANSFERMARK(a, GETNODE(a)->low))

/**
 * Get the n=1 child.
 * This will ref the result node.
 */
inline BDD sylvan_high(BDD bdd)
{
    if (bdd == sylvan_false || bdd == sylvan_true) return bdd;
    BDD high = GETNODE(bdd)->high;
    sylvan_ref(high);
    return BDD_TRANSFERMARK(bdd, high);
}

// Macro for internal use (no ref)
#define HIGH(a) ((BDD_ISCONSTANT(a))?a:BDD_TRANSFERMARK(a, GETNODE(a)->high))

/**
 * Get the complement of the BDD.
 * This will ref the result node.
 */
inline BDD sylvan_not(BDD bdd)
{
    sylvan_ref(bdd);
    return BDD_TOGGLEMARK(bdd);
}

BDD sylvan_and(BDD a, BDD b)
{
    return sylvan_ite(a, b, sylvan_false);
}

BDD sylvan_xor(BDD a, BDD b)
{
    BDD not_b = sylvan_not(b);
    BDD result = sylvan_ite(a, not_b, b);
    sylvan_deref(not_b);
    return result;
}

BDD sylvan_or(BDD a, BDD b) 
{
    return sylvan_ite(a, sylvan_true, b);
}

BDD sylvan_nand(BDD a, BDD b)
{
    BDD not_b = sylvan_not(b);
    BDD result = sylvan_ite(a, not_b, sylvan_true);
    sylvan_deref(not_b);
    return result;
}

BDD sylvan_nor(BDD a, BDD b)
{
    BDD not_b = sylvan_not(b);
    BDD result = sylvan_ite(a, sylvan_false, not_b);
    sylvan_deref(not_b);
    return result;
}

BDD sylvan_imp(BDD a, BDD b)
{
    return sylvan_ite(a, b, sylvan_true);
}

BDD sylvan_biimp(BDD a, BDD b)
{
    BDD not_b = sylvan_not(b);
    BDD result = sylvan_ite(a, b, not_b);
    sylvan_deref(not_b);
    return result;
}

BDD sylvan_diff(BDD a, BDD b) 
{
    BDD not_b = sylvan_not(b);
    BDD result = sylvan_ite(a, not_b, sylvan_false);
    sylvan_deref(not_b);
    return result;
}

BDD sylvan_less(BDD a, BDD b)
{
    return sylvan_ite(a, sylvan_false, b);
}

BDD sylvan_invimp(BDD a, BDD b) 
{
    BDD not_b = sylvan_not(b);
    BDD result = sylvan_ite(a, sylvan_false, b);
    sylvan_deref(not_b);
    return result;
}

/**
 * Calculate standard triples. Find trivial cases.
 * Returns either
 * - sylvan_invalid | complement
 * - sylvan_invalid
 * - a result BDD
 * This function does not alter reference counters.
 */
static BDD sylvan_triples(BDD *_a, BDD *_b, BDD *_c)
{
    BDD a=*_a, b=*_b, c=*_c;
    
    // TERMINAL CASE (attempt 1)
    // ITE(T,B,C) = B
    // ITE(F,B,C) = C
    if (a == sylvan_true) return b;
    if (a == sylvan_false) return c;
    
    // Normalization to standard triples
    // ITE(A,A,C) = ITE(A,T,C)
    // ITE(A,~A,C) = ITE(A,F,C)
    if (BDD_STRIPMARK(a) == BDD_STRIPMARK(b)) {
        if (a == b) b = sylvan_true;
        else b = sylvan_false;
    }
    
    // ITE(A,B,A) = ITE(A,B,T)
    // ITE(A,B,~A) = ITE(A,B,F)
    if (BDD_STRIPMARK(a) == BDD_STRIPMARK(c)) {
        if (a != c) c = sylvan_true;
        else c = sylvan_false;
    }
    
    if (b == c) return b;
    if (b == sylvan_true && c == sylvan_false) return a;
    if (b == sylvan_false && c == sylvan_true) return BDD_TOGGLEMARK(a);
    
    if (BDD_ISCONSTANT(b) && BDD_STRIPMARK(c) < BDD_STRIPMARK(a)) {
        if (b == sylvan_false) {
            // ITE(A,F,C) = ITE(~C,F,~A)
            //            = (A and F) or (~A and C)
            //            = F or (~A and C)
            //            = (~C and F) or (C and ~A)
            //            = ITE(~C,F,~A)
            BDD t = a;
            a = BDD_TOGGLEMARK(c);
            c = BDD_TOGGLEMARK(t);
        } else {
            // ITE(A,T,C) = ITE(C,T,A)
            //            = (A and T) or (~A and C)
            //            = A or (~A and C)
            //            = C or (~C and A)
            //            = (C and T) or (~C and A)
            //            = ITE(C,T,A)
            BDD t = a;
            a = c;
            c = t;
        }
    }
    
    if (BDD_ISCONSTANT(c) && BDD_STRIPMARK(b) < BDD_STRIPMARK(a)) {
        if (c == sylvan_false) {
            // ITE(A,B,F) = ITE(B,A,F)
            //            = (A and B) or (~A and F)
            //            = (A and B) or F
            //            = (B and A) or (~B and F)
            BDD t = a;
            a = b;
            b = t;
        } else {
            // ITE(A,B,T) = ITE(~B,~A,T)
            //            = (A and B) or (~A and T)
            //            = (A and B) or ~A
            //            = (~B and ~A) or B
            //            = (~B and ~A) or (B and T)
            //            = ITE(~B,~A,T)
            BDD t = a;
            a = BDD_TOGGLEMARK(b);
            b = BDD_TOGGLEMARK(t);
        }
    }
    
    if (BDD_STRIPMARK(b) == BDD_STRIPMARK(c)) {
        // b and c not constants...
        // 1. if A then B else not-B = if B then A else not-A
        // 2. if A then not-B else B = if not-B then A else not-A
        if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
            // a > b, exchange:
            b = a;
            a = BDD_TOGGLEMARK(c);
            c = BDD_TOGGLEMARK(b); // (old a)
        }
    }
    
    // TERMINAL CASE
    // ITE(~A,B,C) = ITE(A,C,B)
    if (BDD_HASMARK(a)) {
        a = BDD_STRIPMARK(a);
        BDD t = c;
        c = b;
        b = t;
    }
    
    /**
     * Apply De Morgan: ITE(A,B,C) = ~ITE(A,~B,~C)
     *
     * Proof:
     *   ITE(A,B,C) = (A and B) or (~A and C)
     *              = (A or C) and (~A or B)
     *              = ~(~(A or C) or ~(~A or B))
     *              = ~((~A and ~C) or (A and ~B))
     *              = ~((A and ~B) or (~A and ~C))
     *              = ~ITE(A,~B,~C)
     */
    if (BDD_HASMARK(b)) {
        b = BDD_TOGGLEMARK(b);
        c = BDD_TOGGLEMARK(c);
        *_a=a;
        *_b=b;
        *_c=c;
        return sylvan_invalid | complementmark;
    }
    
    *_a=a;
    *_b=b;
    *_c=c;
    return sylvan_invalid;
}

/**
 * At entry, all BDDs should be ref'd by caller.
 * At exit, they still are ref'd by caller, and the result it ref'd, and any items in the OC are ref'd.
 */
BDD sylvan_ite(BDD a, BDD b, BDD c)
{
    // Standard triples
    BDD r = sylvan_triples(&a, &b, &c);
    if (BDD_STRIPMARK(r) != sylvan_invalid) {
        return sylvan_ref(r);
    }
    
    // The value of a,b,c may be changed, but the reference counters are not changed at this point.
#if CACHE 
    // Check cache
    struct bddcache template_cache_node;
    template_cache_node.operation = 0; // to be done
    template_cache_node.parameters = 3;
    template_cache_node.params[0] = a;
    template_cache_node.params[1] = b;
    template_cache_node.params[2] = c;
    template_cache_node.result = sylvan_invalid;
    
    int created;
    uint32_t idx;
    bddcache_t ptr = llgcset_get_or_create(_bdd.cache, &template_cache_node, &created, &idx);
    if (ptr == 0) rt_report_and_exit(1, "Operations cache full!");
    
    if (created) {
        sylvan_ref(a);
        sylvan_ref(b);
        sylvan_ref(c);
    }
    
    // Did cache return result?
    if (ptr->result != sylvan_invalid) {
        BDD res = ptr->result;
        sylvan_ref(res); // Ref again for result.
        llgcset_deref(_bdd.cache, idx);
        return BDD_TRANSFERMARK(r, res);
    }
#endif
    
    // No result, so we need to calculate...
    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
    bddnode_t nc = BDD_ISCONSTANT(c) ? 0 : GETNODE(c);
        
    // Get lowest level
    BDDVAR level = 0xffff;
    if (na && level > na->level) level = na->level;
    if (nb && level > nb->level) level = nb->level;
    if (nc && level > nc->level) level = nc->level;
    
    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    BDD cLow = c, cHigh = c;
    if (na && level == na->level) {
        aLow = BDD_TRANSFERMARK(a, na->low);
        aHigh = BDD_TRANSFERMARK(a, na->high);
    }
    if (nb && level == nb->level) {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, nb->high);
    }
    if (nc && level == nc->level) {
        cLow = BDD_TRANSFERMARK(c, nc->low);
        cHigh = BDD_TRANSFERMARK(c, nc->high);
    }
    
    // Recursive computation
    BDD low = sylvan_ite(aLow, bLow, cLow);
    BDD high = sylvan_ite(aHigh, bHigh, cHigh);
    BDD result = sylvan_makenode(level, low, high);
    
    /*
     * We gained ref on low, high
     * We exchanged ref on low, high for ref on result
     * Ref it again for the result.
     */

#if CACHE
    ptr->result = sylvan_ref(result);
    llgcset_deref(_bdd.cache, idx);
#endif

    return BDD_TRANSFERMARK(r, result);
}

/**
 * Calculates \exists variables . a
 * Requires caller has ref on a, variables
 * Ensures caller as ref on a, variables and on result
 */
BDD sylvan_exists(BDD a, BDD variables)
{
    // Trivial cases
    if (BDD_ISCONSTANT(a)) return a;
#if CACHE
    // Check cache
    struct bddcache template_cache_node;
    template_cache_node.operation = 4; // to be done
    template_cache_node.parameters = 2;
    template_cache_node.params[0] = a;
    template_cache_node.params[1] = variables;
    template_cache_node.result = sylvan_invalid;
    
    int created;
    uint32_t idx;
    bddcache_t ptr = llgcset_get_or_create(_bdd.cache, &template_cache_node, &created, &idx);
    if (ptr == 0) rt_report_and_exit(1, "Operations cache full!");
    
    if (created) {
        sylvan_ref(a);
        sylvan_ref(variables);
    }
    
    // Did cache return result?
    if (ptr->result != sylvan_invalid) {
        BDD result = sylvan_ref(ptr->result);
        llgcset_deref(_bdd.cache, idx);
        return result;
    }
#endif
    // a != constant    
    bddnode_t na = GETNODE(a);
        
    // Get lowest level
    BDDVAR level = na->level;
    
    // Get cofactors
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, na->high);
    
    // Skip variables not in a
    while (variables != sylvan_false && sylvan_var(variables) < level) {
        // Without increasing ref counter..
        variables = BDD_TRANSFERMARK(variables, GETNODE(variables)->low);
    }

    if (variables == sylvan_false) {
#if CACHE
        ptr->result = sylvan_ref(a);
        llgcset_deref(_bdd.cache, idx);
#endif
        return sylvan_ref(a);
    }
    // variables != sylvan_true (always)
    // variables != sylvan_false
    
    if (sylvan_var(variables) == level) {
        // quantify
        BDD low = sylvan_exists(aLow, LOW(variables));
        if (low == sylvan_true) {
#if CACHE
            ptr->result = sylvan_true;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_true;
        }
        BDD high = sylvan_exists(aHigh, LOW(variables));
        if (high == sylvan_true) {
            sylvan_deref(low);
#if CACHE
            ptr->result = sylvan_true;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_true;
        }
        if (low == sylvan_false && high == sylvan_false) {
#if CACHE
            ptr->result = sylvan_false;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_false;
        }

        BDD result = sylvan_or(low, high);
        sylvan_deref(low);
        sylvan_deref(high);
#if CACHE
        ptr->result = sylvan_ref(result);
        llgcset_deref(_bdd.cache, idx);
#endif
        return result;
    } else {
        // no quantify
        BDD low = sylvan_exists(aLow, variables);
        BDD high = sylvan_exists(aHigh, variables);
        BDD result = sylvan_makenode(level, low, high);
#if CACHE
        ptr->result = sylvan_ref(result);
        llgcset_deref(_bdd.cache, idx);
#endif
        return result;
    }
}

/**
 * Calculates \forall variables . a
 * Requires ref on a, variables
 * Ensures ref on a, variables, result
 */
BDD sylvan_forall(BDD a, BDD variables)
{
    // Trivial cases
    if (BDD_ISCONSTANT(a)) return a;
#if CACHE
    // Check cache
    struct bddcache template_cache_node;
    template_cache_node.operation = 5; // to be done
    template_cache_node.parameters = 2;
    template_cache_node.params[0] = a;
    template_cache_node.params[1] = variables;
    template_cache_node.result = sylvan_invalid;
    
    int created;
    uint32_t idx;
    bddcache_t ptr = llgcset_get_or_create(_bdd.cache, &template_cache_node, &created, &idx);
    if (ptr == 0) rt_report_and_exit(1, "Operations cache full!");
    
    if (created) {
        sylvan_ref(a);
        sylvan_ref(variables);
    }
    
    // Did cache return result?
    if (ptr->result != sylvan_invalid) {
        BDD result = sylvan_ref(ptr->result);
        llgcset_deref(_bdd.cache, idx);
        return result;
    }
#endif
    // a != constant
    bddnode_t na = GETNODE(a);
        
    // Get lowest level
    BDDVAR level = na->level;
    
    // Get cofactors
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, na->high);
    
    // Skip variables not in a
    while (variables != sylvan_false && sylvan_var(variables) < level) {
        // Custom code to not modify the ref counters
        variables = BDD_TRANSFERMARK(variables, GETNODE(variables)->low);
    }

    if (variables == sylvan_false) {
#if CACHE
        ptr->result = sylvan_ref(a);
        llgcset_deref(_bdd.cache, idx);
#endif
        return sylvan_ref(a);
    }
    // variables != sylvan_true (always)
    // variables != sylvan_false

    if (level == sylvan_var(variables)) {
        // quantify
        BDD low = sylvan_forall(aLow, LOW(variables));
        if (low == sylvan_false) {
#if CACHE
            ptr->result = sylvan_false;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_false;        
        }
        BDD high = sylvan_forall(aHigh, LOW(variables));
        if (high == sylvan_false) {
            sylvan_deref(low);
#if CACHE
            ptr->result = sylvan_false;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_false;
        }
        if (low == sylvan_true && high == sylvan_true) {
#if CACHE
            ptr->result = sylvan_true;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_true;        
        }
        BDD result = sylvan_and(low, high);
        sylvan_deref(low);
        sylvan_deref(high);
#if CACHE
        ptr->result = sylvan_ref(result);
        llgcset_deref(_bdd.cache, idx);
#endif
        return result;
    } else {
        // no quantify
        BDD low = sylvan_forall(aLow, variables);
        BDD high = sylvan_forall(aHigh, variables);
        BDD result = sylvan_makenode(level, low, high);
#if CACHE
        ptr->result = sylvan_ref(result);
        llgcset_deref(_bdd.cache, idx);
#endif
        return result;
    }    
}

BDD sylvan_relprods(BDD a, BDD b) 
{
    return sylvan_relprods_partial(a, b, sylvan_false);
}

/**
 * Very specialized RelProdS. Calculates ( \exists X (A /\ B) ) [X'/X]
 * Assumptions on variables: 
 * - every variable 0, 2, 4 etc is in X except if in excluded_variables
 * - every variable 1, 3, 5 etc is in X' (except if in excluded_variables)
 * - (excluded_variables should really only contain variables from X...)
 * - the substitution X'/X substitutes 1 by 0, 3 by 2, etc.
 */
BDD sylvan_relprods_partial(BDD a, BDD b, BDD excluded_variables)
{
    // Trivial case
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;

#if CACHE    
    // Check cache
    struct bddcache template_cache_node;
    template_cache_node.operation = 1; // to be done
    template_cache_node.parameters = 3;
    template_cache_node.params[0] = a;
    template_cache_node.params[1] = b;
    template_cache_node.params[2] = excluded_variables;
    template_cache_node.result = sylvan_invalid;
    
    int created;
    uint32_t idx;
    bddcache_t ptr = llgcset_get_or_create(_bdd.cache, &template_cache_node, &created, &idx);
    if (ptr == 0) rt_report_and_exit(1, "Operations cache full!");
    
    if (created) {
        sylvan_ref(a);
        sylvan_ref(b);
        sylvan_ref(excluded_variables);
    }
    
    // Did cache return result?
    if (ptr->result != sylvan_invalid) {
        BDD result = sylvan_ref(ptr->result);
        llgcset_deref(_bdd.cache, idx);
        return result;
    }
#endif    
    // No result, so we need to calculate...
    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
        
    // Get lowest level
    BDDVAR level = 0xffff;
    if (na && level > na->level) level = na->level;
    if (nb && level > nb->level) level = nb->level;
    
    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    if (na && level == na->level) {
        aLow = BDD_TRANSFERMARK(a, na->low);
        aHigh = BDD_TRANSFERMARK(a, na->high);
    }
    if (nb && level == nb->level) {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, nb->high);
    }
    
    // Check if excluded variable
    int is_excluded = 0;
    while (excluded_variables != sylvan_false) {
        BDDVAR var = sylvan_var(excluded_variables);
        if (var == level) {
            is_excluded = 1;
            break;
        }
        else if (var > level) {
            break;
        }
        // var < level
        // do not mess with ref counts...
        else excluded_variables = BDD_TRANSFERMARK(excluded_variables, GETNODE(excluded_variables)->low);
    }
    
    // Recursive computation
    BDD low = sylvan_relprods_partial(aLow, bLow, excluded_variables);
    
    if (0==(level&1) && is_excluded == 0) {
        // variable in X: quantify
        if (low == sylvan_true) {
#if CACHE
            ptr->result = sylvan_true;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_true;
        }
        BDD high = sylvan_relprods_partial(aHigh, bHigh, excluded_variables);
        if (high == sylvan_true) {
            sylvan_deref(low);
#if CACHE
            ptr->result = sylvan_true;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_true;
        }
        if (low == sylvan_false && high == sylvan_false) {
#if CACHE
            ptr->result = sylvan_false;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_false;
        }
        BDD result = sylvan_or(low, high);
        sylvan_deref(low);
        sylvan_deref(high);
#if CACHE
        ptr->result = sylvan_ref(result);
        llgcset_deref(_bdd.cache, idx);
#endif
        return result;
    }
    
    BDD high = sylvan_relprods_partial(aHigh, bHigh, excluded_variables);
    BDD result;

    // variable in X': substitute
    if (is_excluded == 0) result = sylvan_makenode(level-1, low, high);

    // variable not in X or X': normal behavior
    else result = sylvan_makenode(level, low, high);
    
#if CACHE
    ptr->result = sylvan_ref(result);
    llgcset_deref(_bdd.cache, idx);
#endif
    return result;
}

/**
 * Very specialized RelProdS. Calculates \exists X' (A[X/X'] /\ B)
 * Assumptions:
 * - A is only defined on variables in X
 * - every variable 0, 2, 4 etc is in X (exclude)
 * - every variable 1, 3, 5 etc is in X' (exclude)
 * - variables in exclude_variables are not in X or X'
 * - the substitution X/X' substitutes 0 by 1, 2 by 3, etc.
 */
BDD sylvan_relprods_reversed_partial(BDD a, BDD b, BDD excluded_variables) 
{
    // Trivial case
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;
#if CACHE    
    // Check cache
    struct bddcache template_cache_node;
    template_cache_node.operation = 2; // to be done
    template_cache_node.parameters = 3;
    template_cache_node.params[0] = a;
    template_cache_node.params[1] = b;
    template_cache_node.params[2] = excluded_variables;
    template_cache_node.result = sylvan_invalid;
    
    int created;
    uint32_t idx;
    bddcache_t ptr = llgcset_get_or_create(_bdd.cache, &template_cache_node, &created, &idx);
    if (ptr == 0) rt_report_and_exit(1, "Operations cache full!");
    
    if (created) {
        sylvan_ref(a);
        sylvan_ref(b);
        sylvan_ref(excluded_variables);
    }
    
    // Did cache return result?
    if (ptr->result != sylvan_invalid) {
        BDD res = sylvan_ref(ptr->result);
        llgcset_deref(_bdd.cache, idx);
        return res;
    }
#endif    
    // No result, so we need to calculate...
    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
    
    // Replace level in a, but only if not excluded!
    
    // Get lowest level
    BDDVAR level = 0xffff;
    if (na && level > na->level) level = na->level;
    if (nb && level > nb->level) level = nb->level;
    
    // Check if excluded variable
    int is_excluded = 0;
    while (excluded_variables != sylvan_false) {
        BDDVAR var = sylvan_var(excluded_variables);
        if (var == level) {
            is_excluded = 1;
            break;
        }
        else if (var > level) {
            break;
        }
        // var < level
        // do not modify ref counters
        else excluded_variables = BDD_TRANSFERMARK(excluded_variables, GETNODE(excluded_variables)->low);
    }
    
    // raise_a means: we are at a's level and it will be raised. 
    int raise_a = (!is_excluded && na && level == na->level) ? 1 : 0;
    // ignore_a means: we raise A but B is at the same level as A.
    int ignore_a = (raise_a && nb && level == nb->level) ? 1 : 0;
    
    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    if (!ignore_a && na && level == na->level) {
        aLow = BDD_TRANSFERMARK(a, na->low);
        aHigh = BDD_TRANSFERMARK(a, na->high);
    }
    
    // if raise_a and not ignore_a then A[X/X'] == {level+1, aLow, aHigh} and B == {y>level, ..., ...}
    // so, increase level, and we get A[X/X'] == {level, aLow, aHigh} and B == {y>=level, ..., ...}
    // note that all variables in A are by definition in X,
    //   so aLow and aHigh would never contain odd variables.
    if (raise_a && !ignore_a) level++;
    
    // it is now possible/likely that A[X/X'] and B are at the same level
    if (nb && level == nb->level) {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, nb->high);
    }
    
    // there are three cases: either ignore_a, or raise_a, or nothing
    // in all three cases, we must have aLow = A[X/X'](x=0) and aHigh = A[X/X'](x=1)
    //                              and bLow = B(x=0)       and bHigh = B(x=1)
   
    // if ignore_a: then A[X/X'](x=v) = A[X/X'] and bLow and bHigh are set to the low/high edges
    // else if raise_a, then bLow/bHigh is either set to low/high edges or set to b; a is correct.
    // else then no substitution occurred...
    
    // Recursive computation
    BDD low = sylvan_relprods_reversed_partial(aLow, bLow, excluded_variables);
    
    if (1==(level&1) && is_excluded == 0) {
        // note that variables in X' should not be excluded!!
        // variable in X': quantify
        if (low == sylvan_true) {
#if CACHE
            ptr->result = sylvan_true;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_true;
        }
        BDD high = sylvan_relprods_reversed_partial(aHigh, bHigh, excluded_variables);
        if (high == sylvan_true) {
            sylvan_deref(low);
#if CACHE            
            ptr->result = sylvan_true;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_true;
        }
        if (low == sylvan_false && high == sylvan_false) {
#if CACHE
            ptr->result = sylvan_false;
            llgcset_deref(_bdd.cache, idx);
#endif
            return sylvan_false;
        }
        BDD result = sylvan_or(low, high);
        sylvan_deref(low);
        sylvan_deref(high);
#if CACHE
        ptr->result = sylvan_ref(result);
        llgcset_deref(_bdd.cache, idx);
#endif
        return result;
    }
    
    // variable not in X'
    BDD high = sylvan_relprods_reversed_partial(aHigh, bHigh, excluded_variables);
    BDD result = sylvan_makenode(level, low, high);
#if CACHE    
    ptr->result = sylvan_ref(result);
    llgcset_deref(_bdd.cache, idx);
#endif
    return result;
}

BDD sylvan_relprods_reversed(BDD a, BDD b) 
{
    return sylvan_relprods_reversed_partial(a, b, sylvan_false);
}

uint32_t sylvan_nodecount_do_1(BDD a) 
{
    uint32_t result = 0;
    if (BDD_ISCONSTANT(a)) return 0;
    bddnode_t na = GETNODE(a);
    if (na->flags & 1) return 0;
    na->flags |= 1; // mark
    result = 1;
    result += sylvan_nodecount_do_1(na->low);
    result += sylvan_nodecount_do_1(na->high);
    return result;
}

void sylvan_nodecount_do_2(BDD a) 
{
    if (BDD_ISCONSTANT(a)) return;
    bddnode_t na = GETNODE(a);
    if (!(na->flags & 1)) return;
    na->flags &= ~1; // unmark
    sylvan_nodecount_do_2(na->low);
    sylvan_nodecount_do_2(na->high);
}

uint32_t sylvan_nodecount(BDD a) 
{
    uint32_t result = sylvan_nodecount_do_1(a);
    sylvan_nodecount_do_2(a);
    return result;
}

long double sylvan_satcount_do(BDD bdd, BDD variables)
{
    if (bdd == sylvan_false) return 0.0;
    if (variables == sylvan_false) {
        if (bdd == sylvan_true) return 1.0;
        // bdd != sylvan_false
        fprintf(stderr, "ERROR in sylvan_satcount: 'bdd' contains variable %d not in 'variables'!\n", sylvan_var(bdd));
        exit(0); 
    }
    if (variables == sylvan_true) {
        fprintf(stderr, "ERROR in sylvan_satcount: invalid 'variables'!\n");
        exit(0);
    }
    // bdd != false
    // variables != constant
    if (bdd == sylvan_true) {
        // TODO: just run a loop on variables...
        return 2.0L * sylvan_satcount_do(bdd, LOW(variables));
    }
    // bdd != constant
    // variables != constant
    if (sylvan_var(bdd) > sylvan_var(variables)) {
        return 2.0L * sylvan_satcount_do(bdd, LOW(variables));
    } else {
        long double high = sylvan_satcount_do(HIGH(bdd), LOW(variables));
        long double low = sylvan_satcount_do(LOW(bdd), LOW(variables));
        return high+low;
    }
}

long double sylvan_satcount(BDD bdd, BDD variables)
{
    return sylvan_satcount_do(bdd, variables);
}

static inline void sylvan_printbdd(const char *s, BDD bdd)
{
    if (bdd==sylvan_invalid/* || bdd==bddhandled*/) {
        printf("-1");
        return;
    }
    printf("%s%s%u", /*bdd&bddinternal?"!":*/"", BDD_HASMARK(bdd)?"~":"",
           bdd&~(/*bddinternal|*/complementmark));
}

void sylvan_print(BDD bdd)
{
    if (bdd == sylvan_invalid) return;
    printf("Dump of ");
    sylvan_printbdd("%u", bdd);
    printf("\n");
    bdd = BDD_STRIPMARK(bdd);
    if (bdd < 2) return;
    llvector_t v = llvector_create(sizeof(BDD));
    llgcset_t s = llgcset_create(sizeof(BDD), 17, 10, NULL, NULL, NULL, NULL);
    int created;
    llvector_push(v, &bdd);
    if (llgcset_get_or_create(s, &bdd, &created, NULL) == 0) {
        rt_report_and_exit(1, "Temp hash table full!");
    }
    while (llvector_pop(v, &bdd)) {
        sylvan_printbdd("% 10d", bdd);
        printf(": %u low=", sylvan_var(bdd));
        sylvan_printbdd("%u", LOW(bdd));
        printf(" high=");
        sylvan_printbdd("%u", HIGH(bdd));
        printf("\n");
        BDD low = BDD_STRIPMARK(LOW(bdd));
        BDD high = BDD_STRIPMARK(HIGH(bdd));
        if (low >= 2) {
            if (llgcset_get_or_create(s, &low, &created, NULL) == 0) {
                rt_report_and_exit(1, "Temp hash table full!");
            }
            if (created) llvector_push(v, &low);
        }
        if (high >= 2) {
            if (llgcset_get_or_create(s, &high, &created, NULL) == 0) {
                rt_report_and_exit(1, "Temp hash table full!");
            }
            if (created) llvector_push(v, &high);
        }
    }
    llvector_free(v);
    llgcset_free(s);
}

llgcset_t __sylvan_get_internal_data() 
{
    return _bdd.data;
}
#if CACHE
llgcset_t __sylvan_get_internal_cache() 
{
    return _bdd.cache;
}
#endif

long long sylvan_count_refs()
{
    long long result = 0;
    
    int i;
    for (i=0;i<_bdd.data->size;i++) {
        uint32_t c = _bdd.data->table[i];
        if (c == 0) continue; // not in use (never used)
        if (c == 0x7fffffff) continue; // not in use (tombstone)
        
        c &= 0x0000ffff;
        assert (c!=0x0000ffff); // "about to be deleted" should not be visible here
        
        assert (c!=0x0000fffe); // If this fails, implement behavior for saturated nodes
        
        result += c; // for now, ignore saturated...
        
        bddnode_t n = GETNODE(i);

        //fprintf(stderr, "Node %08X var=%d low=%08X high=%08X rc=%d\n", i, n->level, n->low, n->high, c);
        
        if (!BDD_ISCONSTANT(n->low)) result--; // dont include internals
        if (!BDD_ISCONSTANT(n->high)) result--; // dont include internals
    }
    
    for (i=0;i<_bdd.cache->size;i++) {
        uint32_t c = _bdd.cache->table[i];
        if (c == 0) continue;
        if (c == 0x7fffffff) continue;
        
        bddcache_t n = GETCACHE(i);
        
        //fprintf(stderr, "Cache %08X ", i);
        
        int j;
        for (j=0; j<n->parameters; j++) {
            //fprintf(stderr, "%d=%08X ", j, n->params[j]);
            if (BDD_ISCONSTANT(n->params[j])) continue;
            result--;
        }
                
        //fprintf(stderr, "res=%08X\n", n->result);
        
        if (n->result != sylvan_invalid && (!BDD_ISCONSTANT(n->result))) result--;
    }
    
    return result;
}

/*
void sylvan_print_cache_node(bddcache_t node) {
  BDD node_c = GETCACHEBDD(node);
  printf("%u: a=", GETCACHEBDD(node));
  sylvan_printbdd("%u", node->a);
  printf(", b=");
  sylvan_printbdd("%u", node->b);
  printf(", c=");
  sylvan_printbdd("%u", node->c);
  printf(", r=%u low=", node->root);
  sylvan_printbdd("%u", node->low);
  printf(" high=");
  sylvan_printbdd("%u", node->high);
  printf(" la=");
  sylvan_printbdd("%u", node->cache_low);
  printf(" ha=");
  sylvan_printbdd("%u", node->cache_high);
  printf(" parents={");
  int i=0;
  BDD prnt = node->first_parent;
  while (prnt != 0) {
    if (i>0) printf(",");
    printf("%u", prnt);
    if (BDD_STRIPMARK(GETCACHE(prnt)->cache_low) == node_c)
      prnt = GETCACHE(prnt)->next_low_parent;
    else
      prnt = GETCACHE(prnt)->next_high_parent;
    i++;
  }
  printf("}, r=%x\n", node->result);
}
void sylvan_print_cache(BDD root) {
  llvector_t v = llvector_create(sizeof(BDD));
  llset_t s = llset_create(sizeof(BDD), 13, NULL, NULL);
  int created;
  printf("Dump of cache ");
  sylvan_printbdd("%x\n", root);
  llvector_push(v, &root);
  if (llset_get_or_create(s, &root, &created, NULL) == 0) {
    rt_report_and_exit(1, "Temp hash table full!");
  }
  while (llvector_pop(v, &root)) {
    sylvan_print_cache_node(GETCACHE(root));
    BDD low = GETCACHE(root)->cache_low;
    BDD high = GETCACHE(root)->cache_high;
    if (low) {
      if (llset_get_or_create(s, &low, &created, NULL) == 0) {
        rt_report_and_exit(1, "Temp hash table full!");
      }
      if (created) llvector_push(v, &low);
    }
    if (high) {
      if (llset_get_or_create(s, &high, &created, NULL) == 0) {
        rt_report_and_exit(1, "Temp hash table full!");
      }
      if (created) llvector_push(v, &high);
    }
  }
  llvector_free(v);
  llset_free(s);
}
*/

