#include "runtime.h"
#include "sylvan.h"
#include "llvector.h"
#include "llsched.h"
#include "llset.h"
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
//const BDD quant_exists = -1;     // 0xffffffff
//const BDD quant_forall = -2;     // 0xfffffffe
//const BDD quant_unique = -3;     // 0xfffffffd
//const BDD ite_ex_second = -4;       // 0xfffffffc

/**
 * Internal BDD constants
 */
//const BDD bddhandled = 0xffffffff; // being handled by operation
//const BDD bddinternal = 0x40000000; // ITE* node marker (on "a")
//const uint8_t bddcommand_quit = 1;
//const uint8_t bddcommand_ite_ex = 2;
//const uint8_t bddcommand_ite = 3;

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
    uint8_t flags; // temporary 
    char pad1[PAD(sizeof(BDD)*2+sizeof(BDDVAR)+sizeof(uint8_t), 16)];
}; // 16 bytes

typedef struct bddnode* bddnode_t;

#define op_ite 0
#define op_not 1
#define op_substitute 2
#define op_exists 3
#define op_forall 4
#define op_param 5

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

static struct {
    llset_t data;
    //llset_t operations; // all operations...
    llset_t cache; // operations cache
} _bdd;

// max number of parameters (set to: 5, 13, 29 to get bddcache node size 32, 64, 128)
#define MAXPARAM 5

/*
 * Temporary "operations"
 * 0 = ite
 * 1 = relprods
 * 2 = relprods_reversed
 * 3 = count
 */
__attribute__ ((packed))
struct bddcache {
    BDDOP operation;
    BDD params[MAXPARAM];
    uint32_t parameters; // so we don't have to read <<operation>>
    BDD result;
};

typedef struct bddcache* bddcache_t;

/**
 * Macro's to convert BDD indices to nodes and vice versa
 */
#define GETNODE(bdd)        ((bddnode_t)llset_index_to_ptr(_bdd.data, BDD_STRIPMARK(bdd)))
#define GETCACHE(bdd)       ((bddcache_t)llset_index_to_ptr(_bdd.cache, BDD_STRIPMARK(bdd)))
#define GETCACHEBDD(node)   ((BDD)llset_ptr_to_index(_bdd.cache, node))

// one template cache node for each thread...
// static bddcache_t *template_cache_node;

/**
 * Internal methods
 */
//#define sylvan_makeite(t,a,b,c,x,y) sylvan_makeite_ex(t,a,b,c,x,y,0)
//static BDD sylvan_makeite_ex(int thread, BDD a, BDD b, BDD c, int *created, int *cached, int is_ex);
//static void sylvan_execute_ite(const int thread);
//static void sylvan_execute_ite_ex(const int thread);
//static inline void sylvan_handle_ite_parents(const int thread, bddcache_t node, BDD node_c);
//static void *sylvan_thread(void *data);
//void sylvan_wait_for_threads();
//void sylvan_print_cache(BDD root);
//static inline void sylvan_printbdd(const char *s, BDD bdd);

/**
 * Hash() and Equals() for the BDD hashtable
 * BDD nodes are 10 bytes in our implementation = 2 * sizeof(BDD) + sizeof(BDDVAR)
 */
uint32_t sylvan_bdd_hash(const void *data_, unsigned int len __attribute__((unused)), uint32_t hash)
{
    return hash_128_swapc(data_, 10, hash);
}

int sylvan_bdd_equals(const void *a, const void *b, size_t length __attribute__((unused)))
{
    return memcmp(a, b, 10) == 0;
}

/**
 * Hash() and Equals() for the operations cache
 * Cache nodes are 4 bytes (operation) + n * 4 bytes (every parameter)
 */
uint32_t sylvan_cache_hash(const void *data_, unsigned int len __attribute__((unused)), uint32_t hash)
{
    register unsigned int size = ((bddcache_t)data_)->parameters * 4 + 4;
    return hash_128_swapc(data_, size, hash);
}

int sylvan_cache_equals(const void *a, const void *b, size_t length __attribute__((unused)))
{
    register unsigned int size = ((bddcache_t)a)->parameters * 4 + 4;
    if (((bddcache_t)b)->parameters != ((bddcache_t)a)->parameters) return 0;
    return memcmp(a, b, size) == 0;
}

/**
 * Initialize sylvan
 * - datasize / cachesize : number of bits ...
 */
void sylvan_init(int threads, size_t datasize, size_t cachesize)
{
    if (datasize >= 30) {
        rt_report_and_exit(1, "BDD_init error: datasize must be < 30!");
    }
    if (cachesize >= 30) {
        rt_report_and_exit(1, "BDD_init error: cachesize must be < 30!");
    }
    
    // Sanity check
    if (sizeof(struct bddnode) != 16) {
        printf("Invalid size of bdd nodes: %ld\n", sizeof(struct bddnode));
        exit(0);
    }
    if (sizeof(struct bddop) != 16) {
        printf("Invalid size of bdd operation nodes: %ld\n", sizeof(struct bddop));
        exit(0);
    }
    if (sizeof(struct bddcache) != next_pow2(sizeof(struct bddcache))) {
        printf("Invalid size of bdd operation cache: %ld\n", sizeof(struct bddcache));
        exit(0);
    }

    /*
    if (datasize > 20) {
      printf("BDD_init\nData: %d times %d bytes = %d MB\n", 1<<datasize, datal, (1<<(datasize-20)) * datal);
      printf("Cache: %d times %d bytes = %d MB\n\n", 1<<cachesize, acl, (1<<(cachesize-20)) * acl);
    } else {
      printf("BDD_init\nData: %d times %d bytes = %d KB\n", 1<<datasize, datal, (1<<(datasize-10)) * datal);
      printf("Cache: %d times %d bytes = %d KB\n\n", 1<<cachesize, acl, (1<<(cachesize-10)) * acl);
    }*/
    _bdd.data = llset_create(sizeof(struct bddnode), datasize, sylvan_bdd_hash, sylvan_bdd_equals);
    _bdd.cache = llset_create(sizeof(struct bddcache), cachesize, sylvan_cache_hash, sylvan_cache_equals);

    /*template_cache_node = malloc(sizeof(bddcache_t) * threads);
    for (int i=0; i<threads; i++) {
        template_cache_node[i] = malloc(sizeof(struct bddcache));
        memset(template_cache_node[i], 0, sizeof(struct bddcache));
        template_cache_node[i]->result = sylvan_invalid;
    }*/

    //_bdd.sched = llsched_create(threads, sizeof(BDD));
    //_bdd.threadCount = threads;
    //_bdd.threads = rt_align(CACHE_LINE_SIZE, sizeof(pthread_t) * threads);
    //_bdd.flags = rt_align(CACHE_LINE_SIZE, sizeof(uint8_t) * threads);
    //memset(_bdd.flags, 0, sizeof(uint8_t) * threads);
    //llvector_init(&_bdd.leaves, sizeof(bddcache_t));
    //_bdd.replaceBy = 0;
    //_bdd.replaceLast = 0;
    /// Start threads
    //for (int i=1;i<threads;i++) {
//    pthread_create(&_bdd.threads[i], NULL, sylvan_thread, (void*)i);
    //}
}

void sylvan_quit()
{
    /*for (int i=1; i<_bdd.threadCount; i++) {
      atomic8_write(&_bdd.flags[i], bddcommand_quit);
    }
    for (int i=1; i<_bdd.threadCount; i++) {
      void *tmp;
      pthread_join(_bdd.threads[i], &tmp);
    }*/
    //llvector_deinit(&_bdd.leaves);
    //free(_bdd.flags);
    //free(_bdd.threads);
    //llsched_free(_bdd.sched);
/*
    for (int i=0;i<_bdd.threadCount;i++) free(template_apply_node[i]);
    free(template_apply_node);
*/
    llset_free(_bdd.cache);
    llset_free(_bdd.data);
}

/********************************
 * MAKENODE (level, low, high)
 * and
 * ITHVAR (level)
 * NITHVAR (level)
 * VAR (bdd)
 * LOW (bdd)
 * HIGH (bdd)
 * NOT (bdd)
 ********************************/
inline BDD sylvan_makenode(BDDVAR level, BDD low, BDD high)
{
    BDD result;
    struct bddnode n;
    if (low == high) return low;
    n.level = level;
    n.flags = 0;
    // Normalization to keep canonicity
    // low will have no mark
    if (BDD_HASMARK(low)) {
        // ITE(a,not b,c) == not ITE(a,b,not c)
        n.low = BDD_STRIPMARK(low);
        n.high = BDD_TOGGLEMARK(high);
        if (llset_get_or_create(_bdd.data, &n, NULL, &result) == 0) {
            rt_report_and_exit(1, "BDD Unique table full!");
        }
        return result | complementmark;
    } else {
        n.low = low;
        n.high = high;
        if(llset_get_or_create(_bdd.data, &n, NULL, &result) == 0) {
            rt_report_and_exit(1, "BDD Unique table full!");
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
    return GETNODE(bdd)->level;
}

inline BDD sylvan_low(BDD bdd)
{
    if (bdd == sylvan_false || bdd == sylvan_true) return bdd;
    return BDD_TRANSFERMARK(bdd, GETNODE(bdd)->low);
}

inline BDD sylvan_high(BDD bdd)
{
    if (bdd == sylvan_false || bdd == sylvan_true) return bdd;
    return BDD_TRANSFERMARK(bdd, GETNODE(bdd)->high);
}

inline BDD sylvan_not(BDD bdd)
{
    return BDD_TOGGLEMARK(bdd);
}

/**
 * Calculate standard triples. Find trivial cases.
 * Returns either
 * - sylvan_invalid | complement
 * - sylvan_invalid
 * - a result BDD
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

BDD sylvan_ite(BDD a, BDD b, BDD c)
{
    struct bddcache template_cache_node;
    
    // Standard triples
    BDD r = sylvan_triples(&a, &b, &c);
    if (BDD_STRIPMARK(r) != sylvan_invalid) return r;
    
    // Check cache
    template_cache_node.operation = 0; // to be done
    template_cache_node.parameters = 3;
    template_cache_node.params[0] = a;
    template_cache_node.params[1] = b;
    template_cache_node.params[2] = c;
    template_cache_node.result = sylvan_invalid;
    
    bddcache_t ptr = llset_get_or_create(_bdd.cache, &template_cache_node, 0, 0);
    if (ptr == 0) rt_report_and_exit(1, "Operations cache full!");
    
    // Did cache return result?
    if (ptr->result != sylvan_invalid) return BDD_TRANSFERMARK(r, ptr->result);
    
    // No result, so we need to calculate...
    bddnode_t restrict na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t restrict nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
    bddnode_t restrict nc = BDD_ISCONSTANT(c) ? 0 : GETNODE(c);
        
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
    ptr->result = sylvan_makenode(level, low, high);
    
    return BDD_TRANSFERMARK(r, ptr->result);
}

BDD sylvan_exists(BDD a, BDD variables)
{
    // Trivial cases
    if (BDD_ISCONSTANT(a)) return a;

    // a != constant    
    bddnode_t restrict na = GETNODE(a);
        
    // Get lowest level
    BDDVAR level = na->level;
    
    // Get cofactors
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, na->high);
    
    // Skip variables not in a
    while (variables != sylvan_false && sylvan_var(variables) < level) {
        variables = sylvan_low(variables);
    }

    if (variables == sylvan_false) return a;
    // variables != sylvan_true (always)
    // variables != sylvan_false
    
    if (sylvan_var(variables) == level) {
        // quantify
        BDD low = sylvan_exists(aLow, sylvan_low(variables));
        if (low == sylvan_true) return sylvan_true;
        BDD high = sylvan_exists(aHigh, sylvan_low(variables));
        if (high == sylvan_true) return sylvan_true;
        if (low == sylvan_false && high == sylvan_false) return sylvan_false;
        return sylvan_or(low, high);
    } else {
        // no quantify
        BDD low = sylvan_exists(aLow, variables);
        BDD high = sylvan_exists(aHigh, variables);
        return sylvan_makenode(level, low, high);
    }
}
/*
BDD sylvan_exists(BDD a, BDDVAR* variables, int size)
{
    // Trivial case
    if (BDD_ISCONSTANT(a)) return a;
    
    bddnode_t restrict na = GETNODE(a);
        
    // Get lowest level
    BDDVAR level = na->level;
    
    // Get cofactors
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, na->high);
    
    int i;
    for (i=0;i<size;i++) {
        if (level == variables[i]) {
            BDD low = sylvan_exists(aLow, variables, size);
            if (low == sylvan_true) return sylvan_true;
            BDD high = sylvan_exists(aHigh, variables, size);
            if (high == sylvan_true) return sylvan_true;
            if (low == sylvan_false && high == sylvan_false) return sylvan_false;
            return sylvan_or(low, high);
        }
    }
    
    // Recursive computation
    BDD low = sylvan_exists(aLow, variables, size);
    BDD high = sylvan_exists(aHigh, variables, size);
    return sylvan_makenode(level, low, high);
}
*/
BDD sylvan_forall(BDD a, BDDVAR* variables, int size)
{
    // Trivial case
    if (BDD_ISCONSTANT(a)) return a;
    
    bddnode_t restrict na = GETNODE(a);
        
    // Get lowest level
    BDDVAR level = na->level;
    
    // Get cofactors
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, na->high);
    
    int i;
    for (i=0;i<size;i++) {
        if (level == variables[i]) {
            BDD low = sylvan_forall(aLow, variables, size);
            if (low == sylvan_false) return sylvan_false;
            BDD high = sylvan_forall(aHigh, variables, size);
            if (high == sylvan_false) return sylvan_false;
            if (low == sylvan_true && high == sylvan_true) return sylvan_true;
            return sylvan_and(low, high);
        }
    }
    
    // Recursive computation
    BDD low = sylvan_forall(aLow, variables, size);
    BDD high = sylvan_forall(aHigh, variables, size);
    return sylvan_makenode(level, low, high);
}

extern BDD sylvan_relprods(BDD a, BDD b) 
{
    return sylvan_relprods_partial(a, b, sylvan_false);
}

/**
 * Very specialized RelProdS. Calculates ( \exists X (A /\ B) ) [X'/X]
 * Assumptions on variables: 
 * - every variable 0, 2, 4 etc is in X except if in excluded_variables
 * - every variable 1, 3, 5 etc is in X' (except if in excluded_variables)
 * - excluded_variables should really only contain variables from X...
 * - the substitution X'/X substitutes 1 by 0, 3 by 2, etc.
 */
BDD sylvan_relprods_partial(BDD a, BDD b, BDD excluded_variables)
{
    struct bddcache template_cache_node;

    // Trivial case
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;
    
    // Check cache
    template_cache_node.operation = 1; // to be done
    template_cache_node.parameters = 2;
    template_cache_node.params[0] = a;
    template_cache_node.params[1] = b;
    template_cache_node.result = sylvan_invalid;
    
    bddcache_t ptr = llset_get_or_create(_bdd.cache, &template_cache_node, 0, 0);
    if (ptr == 0) rt_report_and_exit(1, "Operations cache full!");
    
    // Did cache return result?
    if (ptr->result != sylvan_invalid) return ptr->result;
    
    // No result, so we need to calculate...
    bddnode_t restrict na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t restrict nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
        
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
        if (var > level) {
            break;
        }
        // var < level
        excluded_variables = sylvan_low(excluded_variables);
    }
    
    // Recursive computation
    BDD low = sylvan_relprods_partial(aLow, bLow, excluded_variables);
    
    if (0==(level&1) && is_excluded == 0) {
        // variable in X: quantify
        if (low == sylvan_true) return sylvan_true;
        BDD high = sylvan_relprods_partial(aHigh, bHigh, excluded_variables);
        if (high == sylvan_true) return sylvan_true;
        if (low == sylvan_false && high == sylvan_false) return sylvan_false;
        return sylvan_ite(low, sylvan_true, high);
    }
    
    BDD high = sylvan_relprods_partial(aHigh, bHigh, excluded_variables);

    // variable in X': substitute
    if (is_excluded == 0) ptr->result = sylvan_makenode(level-1, low, high);
    // variable not in X or X': normal behavior
    ptr->result = sylvan_makenode(level, low, high);
    
    return ptr->result;
}

/**
 * Very specialized RelProdS. Calculates \exists X' (A[X/X'] /\ B)
 * Assumptions:
 * - A is only defined on variables in X
 * - every variable 0, 2, 4 etc is in X
 * - every variable 1, 3, 5 etc is in X'
 * - the substitution X/X' substitutes 0 by 1, 2 by 3, etc.
 */
BDD sylvan_relprods_reversed(BDD a, BDD b) 
{
    struct bddcache template_cache_node;

    // Trivial case
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;
    
    // Check cache
    template_cache_node.operation = 2; // to be done
    template_cache_node.parameters = 2;
    template_cache_node.params[0] = a;
    template_cache_node.params[1] = b;
    template_cache_node.result = sylvan_invalid;
    
    bddcache_t ptr = llset_get_or_create(_bdd.cache, &template_cache_node, 0, 0);
    if (ptr == 0) rt_report_and_exit(1, "Operations cache full!");
    
    // Did cache return result?
    if (ptr->result != sylvan_invalid) return ptr->result;
    
    // No result, so we need to calculate...
    bddnode_t restrict na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t restrict nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
    
    // Replace level in a.
    
    // Get lowest level
    BDDVAR level = 0xffff;
    if (na && level > (na->level+1)) level = (na->level+1);
    if (nb && level > nb->level) level = nb->level;
    
    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    if (na && level == (na->level+1)) {
        aLow = BDD_TRANSFERMARK(a, na->low);
        aHigh = BDD_TRANSFERMARK(a, na->high);
    }
    if (nb && level == nb->level) {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, nb->high);
    }
    
    // Recursive computation
    BDD low = sylvan_relprods_reversed(aLow, bLow);
    
    if (1==(level&1)) {
        // variable in X': quantify
        if (low == sylvan_true) return sylvan_true;
        BDD high = sylvan_relprods_reversed(aHigh, bHigh);
        if (high == sylvan_true) return sylvan_true;
        if (low == sylvan_false && high == sylvan_false) return sylvan_false;
        return sylvan_or(low, high);
    }
    
    // variable in X
    BDD high = sylvan_relprods_reversed(aHigh, bHigh);
    ptr->result = sylvan_makenode(level, low, high);
    
    return ptr->result;
}

uint32_t sylvan_nodecount_do_1(BDD a) 
{
    uint32_t result = 0;
    if (BDD_ISCONSTANT(a)) return 0;
    bddnode_t restrict na = GETNODE(a);
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
    bddnode_t restrict na = GETNODE(a);
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
        fprintf(stderr, "ERROR in sylvan_satcount: 'bdd' contains variables not in 'variables'!\n");
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
        return 2.0L * sylvan_satcount_do(bdd, sylvan_low(variables));
    }
    // bdd != constant
    // variables != constant
    if (sylvan_var(bdd) > sylvan_var(variables)) {
        return 2.0L * sylvan_satcount_do(bdd, sylvan_low(variables));
    } else {
        long double high = sylvan_satcount_do(sylvan_high(bdd), sylvan_low(variables));
        long double low = sylvan_satcount_do(sylvan_low(bdd), sylvan_low(variables));
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
    llset_t s = llset_create(sizeof(BDD), 17, NULL, NULL);
    int created;
    llvector_push(v, &bdd);
    if (llset_get_or_create(s, &bdd, &created, NULL) == 0) {
        rt_report_and_exit(1, "Temp hash table full!");
    }
    while (llvector_pop(v, &bdd)) {
        sylvan_printbdd("% 10d", bdd);
        printf(": %u low=", sylvan_var(bdd));
        sylvan_printbdd("%u", sylvan_low(bdd));
        printf(" high=");
        sylvan_printbdd("%u", sylvan_high(bdd));
        printf("\n");
        BDD low = BDD_STRIPMARK(sylvan_low(bdd));
        BDD high = BDD_STRIPMARK(sylvan_high(bdd));
        if (low >= 2) {
            if (llset_get_or_create(s, &low, &created, NULL) == 0) {
                rt_report_and_exit(1, "Temp hash table full!");
            }
            if (created) llvector_push(v, &low);
        }
        if (high >= 2) {
            if (llset_get_or_create(s, &high, &created, NULL) == 0) {
                rt_report_and_exit(1, "Temp hash table full!");
            }
            if (created) llvector_push(v, &high);
        }
    }
    llvector_free(v);
    llset_free(s);
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
