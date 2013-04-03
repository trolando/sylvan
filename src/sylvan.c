#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

#include "config.h"

#include "sylvan.h"
#include "atomics.h"
#include "llmsset.h"
#include "tls.h"
#include "avl.h"

#include "wool.h"

#ifdef HAVE_NUMA_H 
#include <numa.h>
#endif

#if SYLVAN_STATS
#define STATS 1
#endif

#define complementmark 0x8000000000000000

/**
 * Exported BDD constants
 */
const BDD sylvan_true = 0 | complementmark;
const BDD sylvan_false = 0;
const BDD sylvan_invalid = 0x7fffffffffffffff; // uint64_t

#define SYLVAN_PAD(x,b) ( (b) - ( (x) & ((b)-1) ) ) /* b must be power of 2 */

/**
 * Mark handling macros
 */
#define BDD_HASMARK(s)              (s&complementmark?1:0)
#define BDD_TOGGLEMARK(s)           (s^complementmark)
#define BDD_STRIPMARK(s)            (s&~complementmark)
#define BDD_TRANSFERMARK(from, to)  (to ^ (from & complementmark))
#define BDD_ISCONSTANT(s)           (BDD_STRIPMARK(s) == 0)
// Equal under mark
#define BDD_EQUALM(a, b)            ((((a)^(b))&(~complementmark))==0)

static inline uint32_t BDD_GETDATA(BDD s)
{
    return (s>>40) & 0x7fffff;
}

static inline BDD BDD_SETDATA(BDD s, uint32_t data)
{
    return (s & 0x800000ffffffffff) | (((uint64_t)data & 0x7fffff)<<40);
}

__attribute__((packed))
struct bddnode {
    uint64_t high     : 40;
    uint32_t level    : 24;
    uint64_t low      : 40;
    unsigned int data : 23;
    uint8_t comp      : 1;
}; // 16 bytes

typedef struct bddnode* bddnode_t;

/*
__attribute__((packed))
struct pair64 {
    uint64_t one;
    uint64_t two;
};

typedef struct pair64 *pair64_t;
*/
/*
static inline uint64_t BDDNODE_GETLOW(bddnode_t n) 
{
    return n->second & 0x000000ffffffffff;
}

static inline uint64_t BDDNODE_GETHIGH(bddnode_t n) 
{
    return n->first & 0x000000ffffffffff;
}

static inline uint32_t BDDNODE_GETLVL(bddnode_t n) 
{
    return n->first >> 40;
}

static inline unsigned int BDDNODE_GETCOMP(bddnode_t n)
{
    return n->second & 0x8000000000000000 ? 1 : 0;
}

static inline void BDDNODE_SETLOW(bddnode_t n, BDD l)
{
    n->second = (n->second & 0xffffff0000000000) | (l & 0x000000ffffffffff);
}

static inline void BDDNODE_SETHIGH(bddnode_t n, BDD h)
{
    n->first  = (n->first  & 0xffffff0000000000) | (h & 0x000000ffffffffff);
}

static inline void BDDNODE_SETLVL(bddnode_t n, BDDVAR v)
{
    n->first  = (n->first  & 0x000000ffffffffff) | ((uint64_t)v << 40);
}

static inline void BDDNODE_SETCOMP(bddnode_t n, int c)
{
    n->second = (n->second & 0x7fffffffffffffff) | (c ? 0x8000000000000000 : 1);
}

static inline void BDDNODE_MAKE(bddnode_t n, BDDVAR v, BDD l, BDD h, int c) 
{
    n->first  = ((uint64_t)v << 40)          | (h & 0x000000ffffffffff);
    n->second = (c ? 0x8000000000000000 : 0) | (l & 0x000000ffffffffff);
}
*/
static int initialized = 0;

static int granularity = 1; // default

/*
 * Temporary "operations"
 * 0 = ite
 * 1 = relprods
 * 2 = relprods_reversed
 * 3 = count
 * 4 = exists
 * 5 = forall
 * 6 = relprod
 * 7 = substitute
 * Operation numbers are stored in the data field of the first parameter
 */
__attribute__ ((packed))
struct bddcache {
    BDD params[3]; 
    BDD result;
};

typedef struct bddcache* bddcache_t;

#define LLCI_KEYSIZE ((sizeof(struct bddcache) - sizeof(BDD)))
#define LLCI_DATASIZE ((sizeof(struct bddcache)))
#include "llci.h"

static struct {
    llmsset_t data;
    llci_t cache; // operations cache
    int workers;
    int gc;
    unsigned int gccount;
} _bdd;

DECLARE_THREAD_LOCAL(insert_index, uint64_t*);

// Structures for statistics
typedef enum {
  C_cache_new,
  C_cache_exists,
  C_cache_reuse,
  C_cache_overwritten,
  C_gc_user,
  C_gc_hashtable_full,
  C_ite,
  C_exists,
  C_forall,
  C_relprods,
  C_relprods_reversed,
  C_relprod,
  C_substitute,
  C_MAX
} Counters;

#define N_CNT_THREAD 128

struct {
    pthread_t thread_id;
} thread_to_id_map[N_CNT_THREAD];

static int get_thread_id() {
    pthread_t id = pthread_self();
    int i=0;
    for (;i<N_CNT_THREAD;i++) {
        if (thread_to_id_map[i].thread_id == 0) {
            if (cas(&thread_to_id_map[i].thread_id, 0, id)) {
                return i;
            }
        } else if (thread_to_id_map[i].thread_id == id) {
            return i;
        }
    }
    assert(0); // NOT ENOUGH SPACE!!
    return -1;
}

static uint64_t *initialize_insert_index()
{
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    insert_index = (uint64_t*)malloc(LINE_SIZE); 
    size_t my_id = get_thread_id();
    *insert_index = llmsset_get_insertindex_multi(_bdd.data, my_id, _bdd.workers);
    SET_THREAD_LOCAL(insert_index, insert_index);
    return insert_index;
}

/**
 * Very custom random number generator, based on the stack pointer and the OS thread id
 */
static inline size_t rand_1()
{
    register const size_t rsp_alias asm ("rsp");
    size_t id = (size_t)pthread_self();
    id += rsp_alias;
    id *= 1103515245;
    id += 12345;
    return id & 8 ? 1 : 0;
}

struct {
    uint64_t count[C_MAX];
    char pad[SYLVAN_PAD(sizeof(uint64_t)*C_MAX, 64)];
} sylvan_stats[N_CNT_THREAD];

#if COLORSTATS
#define BLACK "\33[22;30m"
#define GRAY "\33[01;30m"
#define RED "\33[22;31m"
#define LRED "\33[01;31m"
#define GREEN "\33[22;32m"
#define LGREEN "\33[01;32m"
#define BLUE "\33[22;34m"
#define LBLUE "\33[01;34m"
#define BROWN "\33[22;33m"
#define YELLOW "\33[01;33m"
#define CYAN "\33[22;36m"
#define LCYAN "\33[22;36m"
#define MAGENTA "\33[22;35m"
#define LMAGENTA "\33[01;35m"
#define NC "\33[0m"
#define BOLD "\33[1m"
#define ULINE "\33[4m" //underline
#define BLINK "\33[5m"
#define INVERT "\33[7m"
#else
#define LRED 
#define NC
#define BOLD
#define ULINE
#define BLUE
#define RED
#endif

void sylvan_reset_counters()
{
    if (initialized == 0) return;

    int i,j;
    for (i=0;i<N_CNT_THREAD;i++) {
        thread_to_id_map[i].thread_id = 0;
        for (j=0;j<C_MAX;j++) {
            sylvan_stats[i].count[j] = 0;
        }
    }
}

void sylvan_report_stats()
{
    if (initialized == 0) return;

    int i,j;

    printf(LRED  "****************\n");
    printf(     "* ");
    printf(NC BOLD"SYLVAN STATS"); 
    printf(NC LRED             " *\n");
    printf(     "****************\n");
    printf(NC ULINE "Memory usage\n" NC LBLUE);
    printf("BDD table:          ");
    llmsset_print_size(_bdd.data, stdout);
    printf("\n");
    printf("Cache:              ");
    llci_print_size(_bdd.cache, stdout);
    printf("\n");

    printf(NC ULINE "Cache\n" NC LBLUE);

    uint64_t totals[C_MAX];
    for (i=0;i<C_MAX;i++) totals[i] = 0;
    for (i=0;i<N_CNT_THREAD;i++) {
        for (j=0;j<C_MAX;j++) totals[j] += sylvan_stats[i].count[j];
    }

    uint64_t total_cache = totals[C_cache_new] + totals[C_cache_exists] + totals[C_cache_reuse];
    printf("New results:         %"PRIu64" of %"PRIu64"\n", totals[C_cache_new], total_cache);
    printf("Existing results:    %"PRIu64" of %"PRIu64"\n", totals[C_cache_exists], total_cache);
    printf("Reused results:      %"PRIu64" of %"PRIu64"\n", totals[C_cache_reuse], total_cache);
    printf("Overwritten results: %"PRIu64" of %"PRIu64"\n", totals[C_cache_overwritten], total_cache);

    printf(NC ULINE "GC\n" NC LBLUE);
    printf("GC user-request:     %"PRIu64"\n", totals[C_gc_user]);
    printf("GC full table:       %"PRIu64"\n", totals[C_gc_hashtable_full]);
    printf(NC ULINE "Call counters (ITE, exists, forall, relprods, reversed relprods, relprod, substitute)\n" NC LBLUE);
    for (i=0;i<N_CNT_THREAD;i++) {
        if (thread_to_id_map[i].thread_id != 0) 
            printf("Thread %02d:           %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64"\n", i, 
                sylvan_stats[i].count[C_ite], sylvan_stats[i].count[C_exists], sylvan_stats[i].count[C_forall],
                sylvan_stats[i].count[C_relprods], sylvan_stats[i].count[C_relprods_reversed],
                sylvan_stats[i].count[C_relprod], sylvan_stats[i].count[C_substitute]);
    }
    printf("Totals:              %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64" %"PRIu64" %"PRIu64"\n",  
        totals[C_ite], totals[C_exists], totals[C_forall],
        totals[C_relprods], totals[C_relprods_reversed],
        totals[C_relprod], totals[C_substitute]);
    printf(LRED  "****************" NC " \n");

    // For bonus point, calculate LLGCSET size...
    printf("BDD Unique table: %zu of %zu buckets filled.\n", llmsset_get_filled(_bdd.data), llmsset_get_size(_bdd.data));
}

#if STATS
int enable_stats=1;
void sylvan_enable_stats() {
    enable_stats = 1;
}
void sylvan_disable_stats() {
    enable_stats = 0;
}
#define SV_CNT(s) {if (enable_stats) {(sylvan_stats[get_thread_id()].count[s]+=1);}}
#else
#define SV_CNT(s) ; /* Empty */
#endif

/**
 * Macro's to convert BDD indices to nodes and vice versa
 */
#define GETNODE(bdd)        ((bddnode_t)llmsset_index_to_ptr(_bdd.data, BDD_STRIPMARK(bdd), sizeof(struct bddnode)))

/* 
 * GARBAGE COLLECTION 
 */

typedef struct gc_tomark
{
    struct gc_tomark *prev;
    BDD bdd;
} *gc_tomark_t;

DECLARE_THREAD_LOCAL(gc_key, gc_tomark_t);

#define TOMARK_INIT \
            gc_tomark_t __tomark_original, __tomark_top; {\
                LOCALIZE_THREAD_LOCAL(gc_key, gc_tomark_t);\
                __tomark_original = __tomark_top = gc_key;}

#define TOMARK_PUSH(p) \
            { gc_tomark_t tomark = (gc_tomark_t)alloca(sizeof(struct gc_tomark));\
              *tomark = (struct gc_tomark){__tomark_top, p};\
              SET_THREAD_LOCAL(gc_key, (__tomark_top = tomark)); }

#define TOMARK_EXIT \
            SET_THREAD_LOCAL(gc_key, __tomark_original);

static void sylvan_mark_rec(BDD bdd)
{
    if (llmsset_mark(_bdd.data, bdd&0x000000ffffffffff)) {
        bddnode_t n = GETNODE(bdd);
        sylvan_mark_rec(n->low);
        sylvan_mark_rec(n->high);
    }
}

/* Not to be inlined */
static void sylvan_gc_participate()
{
    xinc(&_bdd.gccount);
    while (ACCESS_ONCE(_bdd.gc) != 2) cpu_relax(); 

    int my_id = get_thread_id();
    int workers = _bdd.workers;

    // Clear the memoization table
    llci_clear_multi(_bdd.cache, my_id, workers);
   
    // GC phase 1: clear hash table
    llmsset_clear_multi(_bdd.data, my_id, workers);
    xinc(&_bdd.gccount);
    while (ACCESS_ONCE(_bdd.gc) != 3) cpu_relax(); 

    // GC phase 2: mark nodes
    LOCALIZE_THREAD_LOCAL(gc_key, gc_tomark_t);
    gc_tomark_t t = gc_key;
    while (t != NULL) {
        sylvan_mark_rec(t->bdd);
        t = t->prev;
    }
    xinc(&_bdd.gccount);
    while (ACCESS_ONCE(_bdd.gc) != 4) cpu_relax(); 

    // GC phase 3: rehash BDDs
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    if (insert_index == NULL) insert_index = initialize_insert_index();
    *insert_index = llmsset_get_insertindex_multi(_bdd.data, my_id, _bdd.workers);

    llmsset_rehash_multi(_bdd.data, my_id, workers);
    xinc(&_bdd.gccount);
    while (ACCESS_ONCE(_bdd.gc) >= 2) cpu_relax(); // waiting for 0 or 1 
}

inline void sylvan_test_gc(void)
{
    if (ACCESS_ONCE(_bdd.gc)) {
        sylvan_gc_participate();
    }
}

static inline void sylvan_gc_go() 
{
    if (!cas(&_bdd.gc, 0, 1)) {
        sylvan_gc_participate();
        return;
    }

    int my_id = get_thread_id();
    int workers = _bdd.workers;

    while (ACCESS_ONCE(_bdd.gccount) != workers - 1) cpu_relax();
    
    _bdd.gccount = 0;
    _bdd.gc = 2;

    // Clear the memoization table
    llci_clear_multi(_bdd.cache, my_id, workers);

    // GC phase 1: clear hash table
    llmsset_clear_multi(_bdd.data, my_id, workers);

    while (ACCESS_ONCE(_bdd.gccount) != workers - 1) cpu_relax();
    _bdd.gccount = 0;
    _bdd.gc = 3;

    // GC phase 2: mark nodes
    llmsset_refset_t r = _bdd.data->refset;
    while (r != 0) {
        sylvan_mark_rec(r->index);
        r = r->next;
    }
    LOCALIZE_THREAD_LOCAL(gc_key, gc_tomark_t);
    gc_tomark_t t = gc_key;
    while (t != NULL) {
        sylvan_mark_rec(t->bdd);
        t = t->prev;
    }

    while (ACCESS_ONCE(_bdd.gccount) != workers - 1) cpu_relax();
    _bdd.gccount = 0;
    _bdd.gc = 4;

    // GC phase 3: rehash BDDs
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    if (insert_index == NULL) insert_index = initialize_insert_index();
    *insert_index = llmsset_get_insertindex_multi(_bdd.data, my_id, _bdd.workers);

    llmsset_rehash_multi(_bdd.data, my_id, workers);
    while (ACCESS_ONCE(_bdd.gccount) != workers - 1) cpu_relax();

    _bdd.gccount = 0;
    _bdd.gc = 0; 
}

/* 
 * This method is *often* called *from parallel code* to test if we are
 * entering a garbage collection phase
 */
static inline void sylvan_gc_test()
{
    // TODO?: 'unlikely'
    while (ACCESS_ONCE(_bdd.gc)) {
        sylvan_gc_participate();
    }
}

void sylvan_package_init(int workers, int dq_size)
{
    wool_init2(workers, dq_size, dq_size, sylvan_test_gc);

    _bdd.workers = workers;
}

void sylvan_package_exit()
{
    wool_fini();
}


/**
 * Initialize sylvan
 * - datasize / cachesize : number of bits ...
 */
void sylvan_init(size_t tablesize, size_t cachesize, int _granularity)
{
    if (initialized != 0) return;
    initialized = 1;

    INIT_THREAD_LOCAL(gc_key);
    INIT_THREAD_LOCAL(insert_index);
 
#ifdef HAVE_NUMA_H 
    if (numa_available() != -1) {
        numa_set_interleave_mask(numa_all_nodes_ptr);  
    }
#endif

    sylvan_reset_counters();

    granularity = _granularity;
    
    // Sanity check
    if (sizeof(struct bddnode) != 16) {
        fprintf(stderr, "Invalid size of bdd nodes: %ld\n", sizeof(struct bddnode));
        exit(1);
    }

    if (sizeof(struct bddcache) != 32) {
        fprintf(stderr, "Invalid size of bdd cache structure: %ld\n", sizeof(struct bddcache));
        exit(1);
    }
    
    if (tablesize >= 40) {
        fprintf(stderr, "BDD_init error: tablesize must be < 40!\n");
        exit(1);
    }

    _bdd.data = llmsset_create(sizeof(struct bddnode), sizeof(struct bddnode), 1LL<<tablesize);

    if (cachesize >= 64) {
        fprintf(stderr, "BDD_init error: cachesize must be < 64!\n");
        exit(1);
    }
    
    _bdd.cache = llci_create(1LL<<cachesize);

    _bdd.gc = 0;
    _bdd.gccount = 0;
}

void sylvan_quit()
{
    if (initialized == 0) return;
    initialized = 0;

    llci_free(_bdd.cache);
    llmsset_free(_bdd.data);
}

BDD sylvan_ref(BDD a) 
{
    assert(a != sylvan_invalid);
    if (!BDD_ISCONSTANT(a)) llmsset_ref(_bdd.data, BDD_STRIPMARK(a));
    return a;
}

void sylvan_deref(BDD a)
{
    assert(a != sylvan_invalid);
    if (BDD_ISCONSTANT(a)) return;
    llmsset_deref(_bdd.data, BDD_STRIPMARK(a));
}

void sylvan_gc()
{
    if (initialized == 0) return;
    SV_CNT(C_gc_user);
    sylvan_gc_go();
}

/**
 * MAKENODE (level, low, high)
 * Requires ref on low, high.
 * Ensures ref on result node.
 * This will ref the result node. Refs on low and high disappear.
 */
inline BDD sylvan_makenode(BDDVAR level, BDD low, BDD high)
{
    if (low == high) return low;

    // Normalization to keep canonicity
    // low will have no mark

    struct bddnode n;
    int mark;

    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    if (insert_index == NULL) insert_index = initialize_insert_index();

    if (BDD_HASMARK(low)) {
        mark = 1;
        n = (struct bddnode){high, level, low, 0, BDD_HASMARK(high) ? 0 : 1};
    } else {
        mark = 0;
        n = (struct bddnode){high, level, low, 0, BDD_HASMARK(high)};
    }

    BDD result;
    uint64_t index;
    int created;
    if (llmsset_lookup(_bdd.data, &n, insert_index, &created, &index) == 0) {
        SV_CNT(C_gc_hashtable_full);

        //size_t before_gc = llmsset_get_filled(_bdd.data);
        sylvan_gc_go();
        //size_t after_gc = llmsset_get_filled(_bdd.data);
        //fprintf(stderr, "GC: %ld to %ld (freed %ld)\n", before_gc, after_gc, before_gc-after_gc);

        if (llmsset_lookup(_bdd.data, &n, insert_index, &created, &index) == 0) {
            fprintf(stderr, "BDD Unique table full, %ld of %ld buckets filled!\n", llmsset_get_filled(_bdd.data), llmsset_get_size(_bdd.data));
            exit(1);
        }
    }

    result = index;
    return mark ? result | complementmark : result;
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
    return GETNODE(bdd)->level;
}

static inline BDD node_highedge(bddnode_t node)
{
    return node->high | (node->comp ? complementmark : 0LL);
}

static inline BDD LOW(BDD n) 
{
    if (BDD_ISCONSTANT(n)) return n;
    return BDD_TRANSFERMARK(n, GETNODE(n)->low);
}

static inline BDD HIGH(BDD n) 
{
    if (BDD_ISCONSTANT(n)) return n;
    return BDD_TRANSFERMARK(n, node_highedge(GETNODE(n)));
}

/**
 * Get the n=0 child.
 * This will ref the result node.
 */
inline BDD sylvan_low(BDD bdd)
{
    if (bdd == sylvan_false || bdd == sylvan_true) return bdd;
    return LOW(bdd);
}

/**
 * Get the n=1 child.
 * This will ref the result node.
 */
inline BDD sylvan_high(BDD bdd)
{
    if (bdd == sylvan_false || bdd == sylvan_true) return bdd;
    return HIGH(bdd);
}

/**
 * Get the complement of the BDD.
 * This will ref the result node.
 */
inline BDD sylvan_not(BDD bdd)
{
    return BDD_TOGGLEMARK(bdd);
}

BDD sylvan_and(BDD a, BDD b)
{
    return sylvan_ite(a, b, sylvan_false);
}

BDD sylvan_xor(BDD a, BDD b)
{
    return sylvan_ite(a, BDD_TOGGLEMARK(b), b);
}

BDD sylvan_or(BDD a, BDD b) 
{
    return sylvan_ite(a, sylvan_true, b);
}

BDD sylvan_nand(BDD a, BDD b)
{
    return sylvan_ite(a, BDD_TOGGLEMARK(b), sylvan_true);
}

BDD sylvan_nor(BDD a, BDD b)
{
    return sylvan_ite(a, sylvan_false, BDD_TOGGLEMARK(b));
}

BDD sylvan_imp(BDD a, BDD b)
{
    return sylvan_ite(a, b, sylvan_true);
}

BDD sylvan_biimp(BDD a, BDD b)
{
    return sylvan_ite(a, b, BDD_TOGGLEMARK(b));
}

BDD sylvan_diff(BDD a, BDD b) 
{
    return sylvan_ite(a, BDD_TOGGLEMARK(b), sylvan_false);
}

BDD sylvan_less(BDD a, BDD b)
{
    return sylvan_ite(a, sylvan_false, b);
}

BDD sylvan_invimp(BDD a, BDD b) 
{
    return sylvan_ite(a, sylvan_false, BDD_TOGGLEMARK(b));
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
    
    // ITE(T,B,C) = B
    // ITE(F,B,C) = C
    if (a == sylvan_true) return b;
    if (a == sylvan_false) return c;
    
    // Normalization to standard triples
    // ITE(A,A,C) = ITE(A,T,C)
    // ITE(A,~A,C) = ITE(A,F,C)
    if (a == b) b = sylvan_true;
    if (a == BDD_TOGGLEMARK(b)) b = sylvan_false;
    
    // ITE(A,B,A) = ITE(A,B,F)
    // ITE(A,B,~A) = ITE(A,B,T)
    if (a == c) c = sylvan_false;
    if (a == BDD_TOGGLEMARK(c)) c = sylvan_true;
    
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
TASK_4(BDD, sylvan_ite_do, BDD, a, BDD, b, BDD, c, BDDVAR, prev_level)
{
    // Standard triples
    BDD r = sylvan_triples(&a, &b, &c);
    if (BDD_STRIPMARK(r) != sylvan_invalid) {
        return r;
    }

    sylvan_gc_test();

    // The value of a,b,c may be changed, but the reference counters are not changed at this point.
    
    SV_CNT(C_ite);

    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
    bddnode_t nc = BDD_ISCONSTANT(c) ? 0 : GETNODE(c);
        
    // Get lowest level
    BDDVAR level = 0xffffffff;
    if (na) level = na->level;
    if (nb && level > nb->level) level = nb->level;
    if (nc && level > nc->level) level = nc->level;

    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != level / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, 0); // ITE operation
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = c;
        template_cache_node.result = sylvan_invalid;
    
        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD res = template_cache_node.result;
            SV_CNT(C_cache_reuse);
            return BDD_TRANSFERMARK(r, res);
        }
    }
    
    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    BDD cLow = c, cHigh = c;
    if (na && level == na->level) {
        aLow = BDD_TRANSFERMARK(a, na->low);
        aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
    }
    if (nb && level == nb->level) {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
    }
    if (nc && level == nc->level) {
        cLow = BDD_TRANSFERMARK(c, nc->low);
        cHigh = BDD_TRANSFERMARK(c, node_highedge(nc));
    }

    // Recursive computation
    BDD low, high;
    TOMARK_INIT
    if (rand_1()) {
        SPAWN(sylvan_ite_do, aHigh, bHigh, cHigh, level);
        low = CALL(sylvan_ite_do, aLow, bLow, cLow, level);
        TOMARK_PUSH(low)
        high = SYNC(sylvan_ite_do);
    } else { 
        SPAWN(sylvan_ite_do, aLow, bLow, cLow, level);
        high = CALL(sylvan_ite_do, aHigh, bHigh, cHigh, level);
        TOMARK_PUSH(high)
        low = SYNC(sylvan_ite_do);
    }
    TOMARK_EXIT

    BDD result = sylvan_makenode(level, low, high);
    
    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT(C_cache_exists);
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            SV_CNT(C_cache_new);
            SV_CNT(C_cache_overwritten);
        }
    }

    return BDD_TRANSFERMARK(r, result);
}

BDD sylvan_ite(BDD a, BDD b, BDD c)
{
    return ROOT_CALL(sylvan_ite_do, a, b, c, 0);
} 
 
/**
 * Calculates \exists variables . a
 * Requires caller has ref on a, variables
 * Ensures caller as ref on a, variables and on result
 */
TASK_3(BDD, sylvan_exists_do, BDD, a, BDD, variables, BDDVAR, prev_level)
{
    // Trivial cases
    if (BDD_ISCONSTANT(a)) return a;
    if (variables == sylvan_false) return a;
    
    sylvan_gc_test();
    
    SV_CNT(C_exists);
 
    // a != constant    
    bddnode_t na = GETNODE(a);
    BDDVAR level = na->level;
    
    // Get cofactors
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
    
    register int in_x = ({   
        register BDDVAR it_var;
        while (variables != sylvan_false && (it_var=(GETNODE(variables)->level)) < level) {
            variables = BDD_TRANSFERMARK(variables, GETNODE(variables)->low);
        }
        variables == sylvan_false ? 0 : it_var == level;
    });

    if (variables == sylvan_false) return a;

    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != level / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, 4); // exists operation
        template_cache_node.params[1] = variables;
        template_cache_node.result = sylvan_invalid;

        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT(C_cache_reuse);
            return result;
        }
    }

    BDD result;

    TOMARK_INIT

    // variables != sylvan_true (always)
    // variables != sylvan_false
    if (in_x) { 
        // quantify
        BDD low = CALL(sylvan_exists_do, aLow, LOW(variables), level);
        if (low == sylvan_true) {
            result = sylvan_true;
        } else {
            TOMARK_PUSH(low)
            BDD high = CALL(sylvan_exists_do, aHigh, LOW(variables), level);
            if (high == sylvan_true) {
                result = sylvan_true;
            }
            else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            }
            else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite_do, low, sylvan_true, high, 0); // or 
            }
        }
    } else {
        // no quantify
        BDD low, high;
        if (rand_1()) {
            SPAWN(sylvan_exists_do, aHigh, variables, level);
            low = CALL(sylvan_exists_do, aLow, variables, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_exists_do);
        } else {
            SPAWN(sylvan_exists_do, aLow, variables, level);
            high = CALL(sylvan_exists_do, aHigh, variables, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_exists_do);
        }
        result = sylvan_makenode(level, low, high);
    }

    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            SV_CNT(C_cache_new);
            SV_CNT(C_cache_overwritten);
        }
    }

    return result;
}

BDD sylvan_exists(BDD a, BDD variables)
{
    return ROOT_CALL(sylvan_exists_do, a, variables, 0);
}

/**
 * Calculates \forall variables . a
 * Requires ref on a, variables
 * Ensures ref on a, variables, result
 */
TASK_3(BDD, sylvan_forall_do, BDD, a, BDD, variables, BDDVAR, prev_level)
{
    // Trivial cases
    if (BDD_ISCONSTANT(a)) return a;
    if (variables == sylvan_false) return a;

    sylvan_gc_test();

    SV_CNT(C_forall);
 
    // a != constant
    bddnode_t na = GETNODE(a);
    BDDVAR level = na->level;

    // Get cofactors    
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
   
    register int in_x = ({   
        register BDDVAR it_var;
        while (variables != sylvan_false && (it_var=(GETNODE(variables)->level)) < level) {
            variables = BDD_TRANSFERMARK(variables, GETNODE(variables)->low);
        }
        variables == sylvan_false ? 0 : it_var == level;
    });

    if (variables == sylvan_false) return a;
 
    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != level / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, 5); // forall operation
        template_cache_node.params[1] = variables;
        template_cache_node.result = sylvan_invalid;

        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT(C_cache_reuse);
            return result;
        }
    }

    BDD result;
    
    // variables != sylvan_true (always)
    // variables != sylvan_false

    TOMARK_INIT
    
    if (in_x) {
        // quantify
        BDD low = CALL(sylvan_forall_do, aLow, LOW(variables), level);
        if (low == sylvan_false) {
            result = sylvan_false;
        } else {
            TOMARK_PUSH(low)
            BDD high = CALL(sylvan_forall_do, aHigh, LOW(variables), level);
            if (high == sylvan_false) {
                result = sylvan_false;
            }
            else if (low == sylvan_true && high == sylvan_true) {
                result = sylvan_true;
            }
            else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite_do, low, high, sylvan_false, 0); // and
            }
        }
    } else {
        // no quantify
        BDD low, high;
        if (rand_1()) {
            SPAWN(sylvan_forall_do, aHigh, variables, level);
            low = CALL(sylvan_forall_do, aLow, variables, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_forall_do);
        } else {
            SPAWN(sylvan_forall_do, aLow, variables, level);
            high = CALL(sylvan_forall_do, aHigh, variables, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_forall_do);
        }
        result = sylvan_makenode(level, low, high);
    }

    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            SV_CNT(C_cache_new);
            SV_CNT(C_cache_overwritten);
        }
    }

    return result;
}

BDD sylvan_forall(BDD a, BDD variables)
{
    return ROOT_CALL(sylvan_forall_do, a, variables, 0);
}

/**
 * RelProd. Calculates \exists X (A/\B)
 * NOTE: only for variables in x that are even...
 */
TASK_4(BDD, sylvan_relprod_do, BDD, a, BDD, b, BDD, x, BDDVAR, prev_level)
{
    /* 
     * Normalization and trivial cases
     */
    
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;
    
    // A /\ A = A /\ 1
    if (a == b) b = sylvan_true;
    
    // A /\ -A = 0; 
    else if (BDD_EQUALM(a, b)) return sylvan_false;

    // A /\ B = B /\ A         (exchange when b < a)
    if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        register BDD _b = b;
        b = a;
        a = _b;
    }

    // Note: the above also takes care of (A, 1)

    /*
     * END of Normalization and trivial cases
     */

    sylvan_gc_test();

    SV_CNT(C_relprod);

    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
        
    // Get lowest level and cofactors
    BDDVAR level;
    BDD aLow=a, aHigh=a, bLow=b, bHigh=b;
    if (na) {
        if (nb && na->level > nb->level) {
            level = nb->level;
            bLow = BDD_TRANSFERMARK(b, nb->low);
            bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
        } else if (nb && na->level == nb->level) {
            level = na->level;
            aLow = BDD_TRANSFERMARK(a, na->low);
            aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
            bLow = BDD_TRANSFERMARK(b, nb->low);
            bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
        } else {
            level = na->level;
            aLow = BDD_TRANSFERMARK(a, na->low);
            aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
        }
    } else {
        level = nb->level;
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
    }

    register int in_x = ({   
        register BDDVAR it_var = -1;
        while (x != sylvan_false && (it_var=(GETNODE(x)->level)) < level) {
            x = BDD_TRANSFERMARK(x, GETNODE(x)->low);
        }
        x == sylvan_false ? 0 : it_var == level;
    });
 
    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != level / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, 6); // RelProd operation
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = x;
        template_cache_node.result = sylvan_invalid;
        
        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT(C_cache_reuse);
            return result;
        }
    }
   
    // Recursive computation
    BDD low, high, result;

    TOMARK_INIT
    
    if (in_x) {
        low = CALL(sylvan_relprod_do, aLow, bLow, x, level);
        // variable in X: quantify
        if (low == sylvan_true) {
            result = sylvan_true;
        } else {
            TOMARK_PUSH(low)
            high = CALL(sylvan_relprod_do, aHigh, bHigh, x, level);
            // Calculate low \/ high
            if (high == sylvan_true) {
                result = sylvan_true;
            }
            else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            }
            else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite_do, low, sylvan_true, high, 0); // or
            }
        }
    } else {
        if (rand_1()) {
            SPAWN(sylvan_relprod_do, aLow, bLow, x, level);
            high = CALL(sylvan_relprod_do, aHigh, bHigh, x, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_relprod_do);
        } else {
            SPAWN(sylvan_relprod_do, aHigh, bHigh, x, level);
            low = CALL(sylvan_relprod_do, aLow, bLow, x, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_relprod_do);
        }
        result = sylvan_makenode(level, low, high);
    }

    TOMARK_EXIT
    
    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            SV_CNT(C_cache_new);
            SV_CNT(C_cache_overwritten);
        } 
    }

    return result;
}

BDD sylvan_relprod(BDD a, BDD b, BDD x) 
{
    return ROOT_CALL(sylvan_relprod_do, a, b, x, 0);
}


/**
 * Specialized substitute, substitutes variables 'x' \in vars by 'x-1'
 */
TASK_3(BDD, sylvan_substitute_do, BDD, a, BDD, vars, BDDVAR, prev_level)
{
    // Trivial cases
    if (BDD_ISCONSTANT(a)) return a;
    if (vars == sylvan_false) return a;

    SV_CNT(C_substitute);

    sylvan_gc_test();

    bddnode_t na = GETNODE(a);
    BDDVAR level = na->level;
    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
     
    register int in_vars = ({   
        register BDDVAR it_var;
        while (vars != sylvan_false && (it_var=(GETNODE(vars)->level)) < level) {
            vars = BDD_TRANSFERMARK(vars, GETNODE(vars)->low);
        }
        vars == sylvan_false ? 0 : it_var == level;
    });

    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != level / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, 7); // Substitute operation
        template_cache_node.params[1] = vars;
        template_cache_node.params[2] = 0;
        template_cache_node.result = sylvan_invalid;
        
        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT(C_cache_reuse);
            return result;
        }
    }

    TOMARK_INIT
   
    // Recursive computation
    BDD low, high;
    if (rand_1()) {
        SPAWN(sylvan_substitute_do, aLow, vars, level);
        high = CALL(sylvan_substitute_do, aHigh, vars, level);
        TOMARK_PUSH(high)
        low = SYNC(sylvan_substitute_do);
    } else {
        SPAWN(sylvan_substitute_do, aHigh, vars, level);
        low = CALL(sylvan_substitute_do, aLow, vars, level);
        TOMARK_PUSH(low)
        high = SYNC(sylvan_substitute_do);
    }

    TOMARK_EXIT

    BDD result;
    
    if (in_vars) {
        result = sylvan_makenode(level-1, low, high);
    } else {
        if (low == aLow && high == aHigh) {
            result = a;
        }
        else result = sylvan_makenode(level, low, high);
    }
    
    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            SV_CNT(C_cache_new);
            SV_CNT(C_cache_overwritten);
        } 
    }

    return result;
}

BDD sylvan_substitute(BDD a, BDD vars)
{
    return ROOT_CALL(sylvan_substitute_do, a, vars, 0);
}

int sylvan_relprods_analyse(BDD a, BDD b, void_cb cb_in, void_cb cb_out)
{
    if (a == sylvan_true && b == sylvan_true) return 0;
    if (a == sylvan_false || b == sylvan_false) return 0;
    if (a == b) b = sylvan_true;
    else if (BDD_EQUALM(a, b)) return 0;

    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
        
    BDD aLow=a, aHigh=a, bLow=b, bHigh=b;
    if (na) {
        if (nb && na->level > nb->level) {
            bLow = BDD_TRANSFERMARK(b, nb->low);
            bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
        } else if (nb && na->level == nb->level) {
            aLow = BDD_TRANSFERMARK(a, na->low);
            aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
            bLow = BDD_TRANSFERMARK(b, nb->low);
            bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
        } else {
            aLow = BDD_TRANSFERMARK(a, na->low);
            aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
        }
    } else {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
    }

    cb_in();
    int d1 = sylvan_relprods_analyse(aLow, bLow, cb_in, cb_out);
    int d2 = sylvan_relprods_analyse(aHigh, bHigh, cb_in, cb_out);
    cb_out();

    return d1>d2?d1+1:d2+1;
}

/**
 * Very specialized RelProdS. Calculates ( \exists X (A /\ B) ) [X'/X]
 * Assumptions on variables: 
 * - 'vars' is the union of set of variables in X and X'
 * - A is defined on variables not in X'
 * - variables in X are even (0, 2, 4)
 * - variables in X' are odd (1, 3, 5)
 * - the substitution X/X' substitutes 0 by 1, 2 by 3, etc.
 */

TASK_4(BDD, sylvan_relprods_do, BDD, a, BDD, b, BDD, vars, BDDVAR, prev_level)
{
    /* 
     * Normalization and trivial cases
     */
    
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;
    
    // A /\ A = A /\ 1
    if (a == b) b = sylvan_true;
    
    // A /\ -A = 0; 
    else if (BDD_EQUALM(a, b)) return sylvan_false;

    // A /\ B = B /\ A         (exchange when b < a)
    if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        register BDD _b = b;
        b = a;
        a = _b;
    }

    // Note: the above also takes care of (A, 1)

    /*
     * END of Normalization and trivial cases
     */

    sylvan_gc_test();

    SV_CNT(C_relprods);

    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
        
    // Get lowest level and cofactors
    BDDVAR level;
    BDD aLow=a, aHigh=a, bLow=b, bHigh=b;
    if (na) {
        if (nb && na->level > nb->level) {
            level = nb->level;
            bLow = BDD_TRANSFERMARK(b, nb->low);
            bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
        } else if (nb && na->level == nb->level) {
            level = na->level;
            aLow = BDD_TRANSFERMARK(a, na->low);
            aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
            bLow = BDD_TRANSFERMARK(b, nb->low);
            bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
        } else {
            level = na->level;
            aLow = BDD_TRANSFERMARK(a, na->low);
            aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
        }
    } else {
        level = nb->level;
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
    }

    register int in_vars = vars == sylvan_true ? 1 : ({   
        register BDDVAR it_var = -1;
        while (vars != sylvan_false && (it_var=(GETNODE(vars)->level)) < level) {
            vars = BDD_TRANSFERMARK(vars, GETNODE(vars)->low);
        }
        vars == sylvan_false ? 0 : it_var == level;
    });
 
    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != level / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, 1); // RelProdS operation
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = vars;
        template_cache_node.result = sylvan_invalid;
        
        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT(C_cache_reuse);
            return result;
        }
    }

    // Recursive computation
    BDD low, high, result;

    TOMARK_INIT
    
    if (in_vars && 0==(level&1)) {
        // variable in X: quantify
        low = CALL(sylvan_relprods_do, aLow, bLow, vars, level);
        if (low == sylvan_true) {
            result = sylvan_true;
        }
        else {
            TOMARK_PUSH(low)
            high = CALL(sylvan_relprods_do, aHigh, bHigh, vars, level);
            if (high == sylvan_true) {
                result = sylvan_true;
            }
            else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            }
            else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite_do, low, sylvan_true, high, 0);
            }
        }
    } 
    else {
        if (rand_1()) {
            SPAWN(sylvan_relprods_do, aLow, bLow, vars, level);
            high = CALL(sylvan_relprods_do, aHigh, bHigh, vars, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_relprods_do);
        } else {
            SPAWN(sylvan_relprods_do, aHigh, bHigh, vars, level);
            low = CALL(sylvan_relprods_do, aLow, bLow, vars, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_relprods_do);
        }

        // variable in X': substitute
        if (in_vars == 1) result = sylvan_makenode(level-1, low, high);

        // variable not in X or X': normal behavior
        else result = sylvan_makenode(level, low, high);
    }

    TOMARK_EXIT
    
    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            SV_CNT(C_cache_new);
            SV_CNT(C_cache_overwritten);
        } 
    }

    return result;
}

BDD sylvan_relprods(BDD a, BDD b, BDD vars) 
{
    return ROOT_CALL(sylvan_relprods_do, a, b, vars, 0);
}

/**
 * Very specialized reversed RelProdS. Calculates \exists X' (A[X/X'] /\ B)
 * Assumptions:
 * - 'vars' is the union of set of variables in X and X'
 * - A is defined on variables not in X'
 * - variables in X are even (0, 2, 4)
 * - variables in X' are odd (1, 3, 5)
 * - the substitution X/X' substitutes 0 by 1, 2 by 3, etc.
 */
TASK_4(BDD, sylvan_relprods_reversed_do, BDD, a, BDD, b, BDD, vars, BDDVAR, prev_level) 
{
    /* 
     * Normalization and trivial cases
     */
    
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;
    
    /*
     * END of Normalization and trivial cases
     */

    sylvan_gc_test();

    SV_CNT(C_relprods_reversed);

    bddnode_t na = BDD_ISCONSTANT(a) ? 0 : GETNODE(a);
    bddnode_t nb = BDD_ISCONSTANT(b) ? 0 : GETNODE(b);
        
    BDDVAR x_a=-1, x_b=-1, S_x_a=-1, x=0xFFFF;
    if (na) {
        x_a = na->level;
        S_x_a = x_a;
        x = x_a;
    }
    if (nb) {
        x_b = nb->level;
        if (x > x_b) x = x_b;
    }

    // x = Top(x_a, x_b)

    register int in_vars = vars == sylvan_true ? 1 : ({
        register BDDVAR it_var;
        while (vars != sylvan_false && (it_var=(GETNODE(vars)->level)) < x) {
            vars = BDD_TRANSFERMARK(vars, GETNODE(vars)->low);
        }
        vars == sylvan_false ? 0 : it_var == x;
    });

    if (in_vars) {
        S_x_a = x_a + 1;
        if (x_b != x) x += 1;
    }

    // x = Top(S_x_a, x_b)

    // OK , so now S_x_a = S(x_a) properly, and 
    // if S_x_a == x then x_a == S'(x)

    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    if (na && x == S_x_a) {
        aLow = BDD_TRANSFERMARK(a, na->low);
        aHigh = BDD_TRANSFERMARK(a, node_highedge(na));
    }
    
    if (nb && x == x_b) {
        bLow = BDD_TRANSFERMARK(b, nb->low);
        bHigh = BDD_TRANSFERMARK(b, node_highedge(nb));
    }
    
    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != x / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, 2); // RelProdS reversed operation
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = vars;
        template_cache_node.result = sylvan_invalid;
    
        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT(C_cache_reuse);
            return result;
        }
    }

    BDD low, high, result;

    TOMARK_INIT

    // if x \in X'
    if ((x&1) == 1 && in_vars) {
        low = CALL(sylvan_relprods_reversed_do, aLow, bLow, vars, x);
        // variable in X': quantify
        if (low == sylvan_true) {
            result = sylvan_true;
        } else {
            TOMARK_PUSH(low)
            high = CALL(sylvan_relprods_reversed_do, aHigh, bHigh, vars, x);
            if (high == sylvan_true) {
                result = sylvan_true;
            } else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            } else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite_do, low, sylvan_true, high, 0);
            }
        }
    } 
    // if x \in X OR if excluded (works in either case)
    else {
        if (rand_1()) {
            SPAWN(sylvan_relprods_reversed_do, aLow, bLow, vars, x);
            high = CALL(sylvan_relprods_reversed_do, aHigh, bHigh, vars, x);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_relprods_reversed_do);
        } else {
            SPAWN(sylvan_relprods_reversed_do, aHigh, bHigh, vars, x);
            low = CALL(sylvan_relprods_reversed_do, aLow, bLow, vars, x);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_relprods_reversed_do);
        }
        result = sylvan_makenode(x, low, high);
    }

    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT(C_cache_new);
        } else if (cache_res == 2) {
            // Replaced existing!
            SV_CNT(C_cache_new);
            SV_CNT(C_cache_overwritten);
        }
    }

    return result;
}

BDD sylvan_relprods_reversed(BDD a, BDD b, BDD vars) 
{
    return ROOT_CALL(sylvan_relprods_reversed_do, a, b, vars, 0);
}

void sylvan_nodecount_levels_do_1(BDD bdd, uint32_t *variables)
{
    if (BDD_ISCONSTANT(bdd)) return;
    bddnode_t na = GETNODE(bdd);
    if (na->data & 1) return;
    variables[na->level]++;
    na->data |= 1; // mark
    sylvan_nodecount_levels_do_1(na->low, variables);
    sylvan_nodecount_levels_do_1(na->high, variables);
}

void sylvan_nodecount_levels_do_2(BDD bdd)
{
    if (BDD_ISCONSTANT(bdd)) return;
    bddnode_t na = GETNODE(bdd);
    if (!(na->data & 1)) return;
    na->data &= ~1; // unmark
    sylvan_nodecount_levels_do_2(na->low);
    sylvan_nodecount_levels_do_2(na->high);
}

void sylvan_nodecount_levels(BDD bdd, uint32_t *variables)
{
    sylvan_nodecount_levels_do_1(bdd, variables);
    sylvan_nodecount_levels_do_2(bdd);
}

uint64_t sylvan_nodecount_do_1(BDD a) 
{
    if (BDD_ISCONSTANT(a)) return 0;
    bddnode_t na = GETNODE(a);
    if (na->data & 1) return 0;
    na->data |= 1; // mark
    uint64_t result = 1;
    result += sylvan_nodecount_do_1(na->low);
    result += sylvan_nodecount_do_1(na->high);
    return result;
}

void sylvan_nodecount_do_2(BDD a) 
{
    if (BDD_ISCONSTANT(a)) return;
    bddnode_t na = GETNODE(a);
    if (!(na->data & 1)) return;
    na->data &= ~1; // unmark
    sylvan_nodecount_do_2(na->low);
    sylvan_nodecount_do_2(na->high);
}

uint64_t sylvan_nodecount(BDD a) 
{
    uint32_t result = sylvan_nodecount_do_1(a);
    sylvan_nodecount_do_2(a);
    return result;
}

long double sylvan_pathcount(BDD bdd)
{
    if (bdd == sylvan_false) return 0.0;
    if (bdd == sylvan_true) return 1.0;
    long double high = sylvan_pathcount(HIGH(bdd));
    long double low = sylvan_pathcount(LOW(bdd));
    return high+low;
}


long double sylvan_satcount_do(BDD bdd, BDD variables)
{
    if (bdd == sylvan_false) return 0.0;
    if (bdd == sylvan_true) {
        long double result = 1.0L;
        while (variables != sylvan_false) {
            variables = LOW(variables);
            result *= 2.0L;
        }
        return result;
    }
    if (variables == sylvan_false) {
        fprintf(stderr, "ERROR in sylvan_satcount: 'bdd' contains variable %d not in 'variables'!\n", sylvan_var(bdd));
        assert(0); 
    }
    if (variables == sylvan_true) {
        fprintf(stderr, "ERROR in sylvan_satcount: invalid 'variables'!\n");
        assert(0);
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

static void sylvan_fprint_1(FILE *out, BDD bdd)
{
    if (bdd==sylvan_invalid) return;
    if (BDD_ISCONSTANT(bdd)) return;
    bddnode_t n = GETNODE(bdd);
    if (n->data & 0x2) return;
    n->data |= 0x2;

    fprintf(out, "%016llX: (%u, low=%016llX, high=%s%016llX) %s\n", 
        bdd, n->level, 
        (uint64_t)n->low,
        n->comp ? "~" : "", 
        (uint64_t)n->high,
        n->data & 0x1?"*":"");
        
    sylvan_fprint_1(out, BDD_STRIPMARK(n->low));
    sylvan_fprint_1(out, BDD_STRIPMARK(n->high));
}

static void sylvan_print_2(BDD bdd)
{
    if (bdd==sylvan_invalid) return;
    if (BDD_ISCONSTANT(bdd)) return;
    bddnode_t n = GETNODE(bdd);
    if (n->data & 0x2) {
        n->data &= ~0x2;
        sylvan_print_2(n->low);
        sylvan_print_2(n->high);
    }
}

void sylvan_print(BDD bdd)
{
    sylvan_fprint(stdout, bdd);
}

void sylvan_fprint(FILE *out, BDD bdd)
{
    if (bdd == sylvan_invalid) return;
    fprintf(out, "Dump of %016llX:\n", bdd);
    sylvan_fprint_1(out, bdd);
    sylvan_print_2(bdd);
}

llmsset_t __sylvan_get_internal_data() 
{
    return _bdd.data;
}

llci_t __sylvan_get_internal_cache() 
{
    return _bdd.cache;
}

long long sylvan_count_refs()
{
    long long result = 0;

    llmsset_refset_t r = _bdd.data->refset;
    while (r != 0) {
        result += r->count;
        r = r->next;
    }
    
    return result;
}

/**
 * SAVE and LOAD routines
 */

struct sylvan_save_dict_type {
    BDD bdd;
    uint64_t assigned;
};

AVL(save, sylvan_save_dict_type)
{
    return left->bdd - right->bdd;
}

static AVL_ROOT(sylvan_save_dict);
static uint64_t ser_count = 0;

/** COUNTER MANAGEMENT **/
static long ser_offset;

static void sylvan_save_reset()
{
    save_free(&sylvan_save_dict);
    ser_count = 0;
}

static void sylvan_save_dummy(FILE *f)
{
    ser_offset = ftell(f);
    fwrite(&ser_count, 8, 1, f);
}

static void sylvan_save_update(FILE *f)
{
    long off = ftell(f);
    fseek(f, ser_offset, SEEK_SET);
    fwrite(&ser_count, 8, 1, f);
    fseek(f, off, SEEK_SET);
}
/** END COUNTER MANAGEMENT **/

uint64_t sylvan_save_bdd(FILE* f, BDD bdd) 
{
    if (BDD_ISCONSTANT(bdd)) return bdd;

    bddnode_t n = GETNODE(bdd);

    struct sylvan_save_dict_type e;
    e.bdd = BDD_STRIPMARK(bdd);

    if (!save_search(sylvan_save_dict, &e)) {
        uint64_t low = sylvan_save_bdd(f, n->low);
        uint64_t high = sylvan_save_bdd(f, n->high);
    
        if (ser_count == 0) sylvan_save_dummy(f);
        ser_count++;
        e.assigned = ser_count;
        save_insert(&sylvan_save_dict, &e);

        struct bddnode node;
        node.high = high;
        node.low = low;
        node.level = n->level;
        node.data = 0;
        node.comp = n->comp;

        fwrite(&node, sizeof(struct bddnode), 1, f);
    }

    return BDD_TRANSFERMARK(bdd, e.assigned);
}

void sylvan_save_done(FILE *f)
{
    sylvan_save_update(f);
    sylvan_save_reset();
}

static BDD *ser_arr; // serialize array

void sylvan_load(FILE *f) 
{
    fread(&ser_count, 8, 1, f);
    ser_arr = (BDD*)malloc(sizeof(BDD) * ser_count);

    unsigned long i;
    for (i=1;i<=ser_count;i++) {
        struct bddnode node;
        fread(&node, sizeof(struct bddnode), 1, f);

        assert(node.high < i);
        assert(node.low  < i);

        BDD low = node.low == 0 ? sylvan_false : ser_arr[node.low-1];
        BDD high = node.high == 0 ? sylvan_false : ser_arr[node.high-1];
        if (node.comp) high |= complementmark;

        ser_arr[i-1] = sylvan_makenode(node.level, low, high);
    }
}

BDD sylvan_load_translate(uint64_t bdd) 
{
    if (BDD_ISCONSTANT(bdd)) return bdd;
    return BDD_TRANSFERMARK(bdd, ser_arr[BDD_STRIPMARK(bdd)-1]);
}

void sylvan_load_done()
{
    free(ser_arr);
}
