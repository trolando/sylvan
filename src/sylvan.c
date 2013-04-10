#include "config.h"

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomics.h>
#include <avl.h>
#include <lace.h>
#include <llmsset.h>
#include <sylvan.h>
#include <tls.h>

#if USE_NUMA
#include <numa.h>
#endif

#if SYLVAN_STATS
#define STATS 1
#endif

#define complementmark 0x8000000000000000

/**
 * Complement handling macros
 */
#define BDD_HASMARK(s)              (s&complementmark?1:0)
#define BDD_TOGGLEMARK(s)           (s^complementmark)
#define BDD_STRIPMARK(s)            (s&~complementmark)
#define BDD_TRANSFERMARK(from, to)  (to ^ (from & complementmark))
#define BDD_ISCONSTANT(s)           (BDD_STRIPMARK(s) == 0)
// Equal under mark
#define BDD_EQUALM(a, b)            ((((a)^(b))&(~complementmark))==0)

__attribute__((packed))
struct bddnode {
    uint64_t high     : 40;
    uint32_t level    : 24;
    uint64_t low      : 40;
    unsigned int data : 23;
    uint8_t comp      : 1;
}; // 16 bytes

typedef struct bddnode* bddnode_t;

/**
 * Macro's to convert BDD indices to nodes and vice versa
 */
#define GETNODE(bdd) ((bddnode_t)llmsset_index_to_ptr(_bdd.data, BDD_STRIPMARK(bdd), sizeof(struct bddnode)))

/**
 * Exported BDD constants
 */
const BDD sylvan_true = 0 | complementmark;
const BDD sylvan_false = 0;
const BDD sylvan_invalid = 0x7fffffffffffffff; // uint64_t

/**
 * Methods to manipulate the free field inside the BDD type
 */

static inline uint32_t BDD_GETDATA(BDD s)
{
    return (s>>40) & 0x7fffff;
}

static inline BDD BDD_SETDATA(BDD s, uint32_t data)
{
    return (s & 0x800000ffffffffff) | (((uint64_t)data & 0x7fffff)<<40);
}

/**
 * Some essential garbage collection helpers
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

/**
 * Cached operations:
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

static struct
{
    llmsset_t data;
    llci_t cache; // operations cache
    int workers;
    int gc;
    unsigned int gccount;
} _bdd;

/**
 * Thread-local Insert index for LLMSset
 */
DECLARE_THREAD_LOCAL(insert_index, uint64_t*);

static uint64_t*
initialize_insert_index()
{
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    insert_index = (uint64_t*)malloc(LINE_SIZE);
    size_t my_id = LACE_WORKER_ID;
    *insert_index = llmsset_get_insertindex_multi(_bdd.data, my_id, _bdd.workers);
    SET_THREAD_LOCAL(insert_index, insert_index);
    return insert_index;
}

/**
 * Handling references (note: protected by a spinlock)
 */
struct sylvan_ref_s
{
    BDD bdd;
    size_t count;
};

AVL(refset, struct sylvan_ref_s)
{
    return left->bdd - right->bdd;
}

static avl_node_t *sylvan_refs = NULL;
static volatile int sylvan_refs_spinlock = 0;

BDD
sylvan_ref(BDD a)
{
    if (BDD_ISCONSTANT(a)) return a;

    while (!cas(&sylvan_refs_spinlock, 0, 1)) {
        while (sylvan_refs_spinlock != 0) ;
    }

    struct sylvan_ref_s s, *ss;
    s.bdd = BDD_STRIPMARK(a);
    ss = refset_search(sylvan_refs, &s);
    if (ss == NULL) {
        s.count = 0;
        ss = refset_put(&sylvan_refs, &s, 0);
    }
    ss->count++;

    sylvan_refs_spinlock = 0;

    return a;
}

void
sylvan_deref(BDD a)
{
    if (BDD_ISCONSTANT(a)) return;

    while (!cas(&sylvan_refs_spinlock, 0, 1)) {
        while (sylvan_refs_spinlock != 0) ;
    }

    struct sylvan_ref_s s, *ss;
    s.bdd = BDD_STRIPMARK(a);
    ss = refset_search(sylvan_refs, &s);
    assert (ss != NULL);
    assert (ss->count > 0);
    ss->count--;

    sylvan_refs_spinlock = 0;
}

size_t
sylvan_count_refs()
{
    size_t result = 0;

    while (!cas(&sylvan_refs_spinlock, 0, 1)) {
        while (sylvan_refs_spinlock != 0) ;
    }

    struct sylvan_ref_s *ss;
    avl_iter_t *iter = refset_iter(sylvan_refs);
    while ((ss = refset_iter_next(iter))) {
        result += ss->count;
    }
    refset_iter_free(iter);

    sylvan_refs_spinlock = 0;

    return result;
}

static void
sylvan_pregc_mark_rec(BDD bdd)
{
    if (llmsset_mark_unsafe(_bdd.data, bdd&0x000000ffffffffff)) {
        bddnode_t n = GETNODE(bdd);
        sylvan_pregc_mark_rec(n->low);
        sylvan_pregc_mark_rec(n->high);
    }
}

static void
sylvan_pregc_mark_refs()
{
    while (!cas(&sylvan_refs_spinlock, 0, 1)) {
        while (sylvan_refs_spinlock != 0) ;
    }

    struct sylvan_ref_s *ss;
    avl_iter_t *iter = refset_iter(sylvan_refs);
    while ((ss = refset_iter_next(iter))) {
        if (ss->count > 0) sylvan_pregc_mark_rec(ss->bdd);
    }
    refset_iter_free(iter);

    sylvan_refs_spinlock = 0;
}

static int initialized = 0;
static int granularity = 1; // default

static void sylvan_test_gc();
static void sylvan_reset_counters();

void
sylvan_package_init(int workers, int dq_size)
{
    lace_init(workers, dq_size, 0);
    lace_set_callback(sylvan_test_gc);

    _bdd.workers = workers;
}

void
sylvan_package_exit()
{
    lace_exit();
}

void
sylvan_init(size_t tablesize, size_t cachesize, int _granularity)
{
    if (initialized != 0) return;
    initialized = 1;

    INIT_THREAD_LOCAL(gc_key);
    INIT_THREAD_LOCAL(insert_index);

#if USE_NUMA
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

void
sylvan_quit()
{
    if (initialized == 0) return;
    initialized = 0;

    llci_free(_bdd.cache);
    llmsset_free(_bdd.data);
    refset_free(&sylvan_refs);
}

/**
 * Statistics stuff
 */
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

#define SYLVAN_PAD(x,b) ( (b) - ( (x) & ((b)-1) ) ) /* b must be power of 2 */

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


static void
sylvan_reset_counters()
{
    if (initialized == 0) return;

    int i,j;
    for (i=0;i<N_CNT_THREAD;i++) {
        for (j=0;j<C_MAX;j++) {
            sylvan_stats[i].count[j] = 0;
        }
    }
}

void
sylvan_report_stats()
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
    for (i=0;i<_bdd.workers;i++) {
        printf("Worker %02d:           %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64", %"PRIu64"\n", i,
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
#define SV_CNT(s) {if (enable_stats) {(sylvan_stats[LACE_WORKER_ID].count[s]+=1);}}
#else
#define SV_CNT(s) ; /* Empty */
#endif

/**
 * Very custom random number generator, based on the stack pointer and the OS thread id
 */
static inline
size_t rand_1()
{
    register const size_t rsp_alias asm ("rsp");
    size_t id = (size_t)pthread_self();
    id += rsp_alias;
    id *= 1103515245;
    id += 12345;
    return id & 8 ? 1 : 0;
}



/* Not to be inlined */
static void
sylvan_gc_participate()
{
    xinc(&_bdd.gccount);
    while (atomic_read(&_bdd.gc) != 2) ;

    int my_id = LACE_WORKER_ID;
    int workers = _bdd.workers;

    // Clear the memoization table
    llci_clear_multi(_bdd.cache, my_id, workers);

    // GC phase 1: clear hash table
    llmsset_clear_multi(_bdd.data, my_id, workers);
    xinc(&_bdd.gccount);
    while (atomic_read(&_bdd.gc) != 3) ;

    // GC phase 2: mark nodes
    LOCALIZE_THREAD_LOCAL(gc_key, gc_tomark_t);
    gc_tomark_t t = gc_key;
    while (t != NULL) {
        sylvan_pregc_mark_rec(t->bdd);
        t = t->prev;
    }
    xinc(&_bdd.gccount);
    while (atomic_read(&_bdd.gc) != 4) ;

    // GC phase 3: rehash BDDs
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    if (insert_index == NULL) insert_index = initialize_insert_index();
    *insert_index = llmsset_get_insertindex_multi(_bdd.data, my_id, _bdd.workers);

    llmsset_rehash_multi(_bdd.data, my_id, workers);
    xinc(&_bdd.gccount);
    while (atomic_read(&_bdd.gc) >= 2) ; // waiting for 0 or 1
}

static void
sylvan_test_gc(void)
{
    if (atomic_read(&_bdd.gc)) {
        sylvan_gc_participate();
    }
}

static inline
void sylvan_gc_go()
{
    if (!cas(&_bdd.gc, 0, 1)) {
        sylvan_gc_participate();
        return;
    }

    int my_id = LACE_WORKER_ID;
    unsigned int workers = _bdd.workers;

    while (atomic_read(&_bdd.gccount) != workers - 1) ;

    _bdd.gccount = 0;
    _bdd.gc = 2;

    // Clear the memoization table
    llci_clear_multi(_bdd.cache, my_id, workers);

    // GC phase 1: clear hash table
    llmsset_clear_multi(_bdd.data, my_id, workers);

    while (atomic_read(&_bdd.gccount) != workers - 1) ;
    _bdd.gccount = 0;
    _bdd.gc = 3;

    // GC phase 2a: mark external refs
    sylvan_pregc_mark_refs();

    // GC phase 2b: mark internal refs
    LOCALIZE_THREAD_LOCAL(gc_key, gc_tomark_t);
    gc_tomark_t t = gc_key;
    while (t != NULL) {
        sylvan_pregc_mark_rec(t->bdd);
        t = t->prev;
    }

    while (atomic_read(&_bdd.gccount) != workers - 1) ;
    _bdd.gccount = 0;
    _bdd.gc = 4;

    // GC phase 3: rehash BDDs
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    if (insert_index == NULL) insert_index = initialize_insert_index();
    *insert_index = llmsset_get_insertindex_multi(_bdd.data, my_id, _bdd.workers);

    llmsset_rehash_multi(_bdd.data, my_id, workers);
    while (atomic_read(&_bdd.gccount) != workers - 1) ;

    _bdd.gccount = 0;
    _bdd.gc = 0;
}

/*
 * This method is *often* called *from parallel code* to test if we are
 * entering a garbage collection phase
 */
static inline void
sylvan_gc_test()
{
    // TODO?: 'unlikely'
    while (atomic_read(&_bdd.gc)) {
        sylvan_gc_participate();
    }
}

void sylvan_gc()
{
    if (initialized == 0) return;
    SV_CNT(C_gc_user);
    sylvan_gc_go();
}

/**
 * Core BDD operations
 */

BDD
sylvan_makenode(BDDVAR level, BDD low, BDD high)
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

inline BDD
sylvan_ithvar(BDDVAR level)
{
    return sylvan_makenode(level, sylvan_false, sylvan_true);
}

inline BDD
sylvan_nithvar(BDDVAR level)
{
    return sylvan_makenode(level, sylvan_true, sylvan_false);
}

inline BDDVAR
sylvan_var(BDD bdd)
{
    assert(!BDD_ISCONSTANT(bdd));
    return GETNODE(bdd)->level;
}

static inline
BDD node_lowedge(bddnode_t node)
{
    return node->low;
}

static inline
BDD node_highedge(bddnode_t node)
{
    return node->high | (node->comp ? complementmark : 0LL);
}

inline BDD
sylvan_low(BDD bdd)
{
    if (BDD_ISCONSTANT(bdd)) return bdd;
    return BDD_TRANSFERMARK(bdd, node_lowedge(GETNODE(bdd)));
}

inline BDD
sylvan_high(BDD bdd)
{
    if (BDD_ISCONSTANT(bdd)) return bdd;
    return BDD_TRANSFERMARK(bdd, node_highedge(GETNODE(bdd)));
}

inline BDD
sylvan_not(BDD bdd)
{
    return BDD_TOGGLEMARK(bdd);
}

BDD
sylvan_and(BDD a, BDD b)
{
    return sylvan_ite(a, b, sylvan_false);
}

BDD
sylvan_xor(BDD a, BDD b)
{
    return sylvan_ite(a, BDD_TOGGLEMARK(b), b);
}

BDD
sylvan_or(BDD a, BDD b)
{
    return sylvan_ite(a, sylvan_true, b);
}

BDD
sylvan_nand(BDD a, BDD b)
{
    return sylvan_ite(a, BDD_TOGGLEMARK(b), sylvan_true);
}

BDD
sylvan_nor(BDD a, BDD b)
{
    return sylvan_ite(a, sylvan_false, BDD_TOGGLEMARK(b));
}

BDD
sylvan_imp(BDD a, BDD b)
{
    return sylvan_ite(a, b, sylvan_true);
}

BDD
sylvan_biimp(BDD a, BDD b)
{
    return sylvan_ite(a, b, BDD_TOGGLEMARK(b));
}

BDD
sylvan_diff(BDD a, BDD b)
{
    return sylvan_ite(a, BDD_TOGGLEMARK(b), sylvan_false);
}

BDD
sylvan_less(BDD a, BDD b)
{
    return sylvan_ite(a, sylvan_false, b);
}

BDD
sylvan_invimp(BDD a, BDD b)
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
static BDD
sylvan_triples(BDD *_a, BDD *_b, BDD *_c)
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
TASK_IMPL_4(BDD, sylvan_ite, BDD, a, BDD, b, BDD, c, BDDVAR, prev_level)
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
        SPAWN(sylvan_ite, aHigh, bHigh, cHigh, level);
        low = CALL(sylvan_ite, aLow, bLow, cLow, level);
        TOMARK_PUSH(low)
        high = SYNC(sylvan_ite);
    } else {
        SPAWN(sylvan_ite, aLow, bLow, cLow, level);
        high = CALL(sylvan_ite, aHigh, bHigh, cHigh, level);
        TOMARK_PUSH(high)
        low = SYNC(sylvan_ite);
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

BDD
sylvan_ite(BDD a, BDD b, BDD c)
{
    return CALL(sylvan_ite, a, b, c, 0);
}

/**
 * Calculates \exists variables . a
 * Requires caller has ref on a, variables
 * Ensures caller as ref on a, variables and on result
 */
TASK_IMPL_3(BDD, sylvan_exists, BDD, a, BDD, variables, BDDVAR, prev_level)
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

    while (!sylvan_set_isempty(variables) && sylvan_var(variables) < level) {
        // Skip variables before x
        variables = sylvan_set_next(variables);
    }

    if (sylvan_set_isempty(variables)) return a;

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

    if (sylvan_var(variables) == level) {
        // level is in variable set, perform abstraction
        BDD low = CALL(sylvan_exists, aLow, sylvan_set_next(variables), level);
        if (low == sylvan_true) {
            result = sylvan_true;
        } else {
            TOMARK_PUSH(low)
            BDD high = CALL(sylvan_exists, aHigh, sylvan_set_next(variables), level);
            if (high == sylvan_true) {
                result = sylvan_true;
            }
            else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            }
            else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite, low, sylvan_true, high, 0); // or
            }
        }
    } else {
        // level is not in variable set
        BDD low, high;
        if (rand_1()) {
            SPAWN(sylvan_exists, aHigh, variables, level);
            low = CALL(sylvan_exists, aLow, variables, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_exists);
        } else {
            SPAWN(sylvan_exists, aLow, variables, level);
            high = CALL(sylvan_exists, aHigh, variables, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_exists);
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

BDD
sylvan_exists(BDD a, BDD variables)
{
    return CALL(sylvan_exists, a, variables, 0);
}

/**
 * Calculates \forall variables . a
 * Requires ref on a, variables
 * Ensures ref on a, variables, result
 */
TASK_IMPL_3(BDD, sylvan_forall, BDD, a, BDD, variables, BDDVAR, prev_level)
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

    while (!sylvan_set_isempty(variables) && sylvan_var(variables) < level) {
        // Skip variables before x
        variables = sylvan_set_next(variables);
    }

    if (sylvan_set_isempty(variables)) return a;

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

    TOMARK_INIT

    if (sylvan_var(variables) == level) {
        // level is in variable set, perform abstraction
        BDD low = CALL(sylvan_forall, aLow, sylvan_set_next(variables), level);
        if (low == sylvan_false) {
            result = sylvan_false;
        } else {
            TOMARK_PUSH(low)
            BDD high = CALL(sylvan_forall, aHigh, sylvan_set_next(variables), level);
            if (high == sylvan_false) {
                result = sylvan_false;
            }
            else if (low == sylvan_true && high == sylvan_true) {
                result = sylvan_true;
            }
            else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite, low, high, sylvan_false, 0); // and
            }
        }
    } else {
        // level is not in variable set
        BDD low, high;
        if (rand_1()) {
            SPAWN(sylvan_forall, aHigh, variables, level);
            low = CALL(sylvan_forall, aLow, variables, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_forall);
        } else {
            SPAWN(sylvan_forall, aLow, variables, level);
            high = CALL(sylvan_forall, aHigh, variables, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_forall);
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

BDD
sylvan_forall(BDD a, BDD variables)
{
    return CALL(sylvan_forall, a, variables, 0);
}

/**
 * RelProd. Calculates \exists X (A/\B)
 * NOTE: only for variables in x that are even...
 */
TASK_IMPL_4(BDD, sylvan_relprod, BDD, a, BDD, b, BDD, x, BDDVAR, prev_level)
{
    /*
     * Normalization and trivial cases
     */

    // 1 and 1 => 1
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;

    // A and 0 => 0, 0 and B => 0
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;

    // A and A => A and 1
    if (a == b) b = sylvan_true;

    // A and not A => 0;
    else if (BDD_EQUALM(a, b)) return sylvan_false;

    // A and B => B and A         (exchange when b < a)
    // (Also does A and 1 => 1 and A)
    if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
        register BDD _b = b;
        b = a;
        a = _b;
    }

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

    while (!sylvan_set_isempty(x) && sylvan_var(x) < level) {
        // Skip variables before x
        x = sylvan_set_next(x);
    }

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

    if (!sylvan_set_isempty(x) && sylvan_var(x) == level) {
        // variable level in variables, perform abstraction
        low = CALL(sylvan_relprod, aLow, bLow, sylvan_set_next(x), level);
        if (low == sylvan_true) {
            result = sylvan_true;
        } else {
            TOMARK_PUSH(low)
            high = CALL(sylvan_relprod, aHigh, bHigh, sylvan_set_next(x), level);
            if (high == sylvan_true) {
                result = sylvan_true;
            }
            else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            }
            else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite, low, sylvan_true, high, 0); // or
            }
        }
    } else {
        if (rand_1()) {
            SPAWN(sylvan_relprod, aLow, bLow, x, level);
            high = CALL(sylvan_relprod, aHigh, bHigh, x, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_relprod);
        } else {
            SPAWN(sylvan_relprod, aHigh, bHigh, x, level);
            low = CALL(sylvan_relprod, aLow, bLow, x, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_relprod);
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

BDD
sylvan_relprod(BDD a, BDD b, BDD x)
{
    return CALL(sylvan_relprod, a, b, x, 0);
}

/**
 * Specialized substitute, substitutes variables 'x' \in vars by 'x-1'
 */
TASK_IMPL_3(BDD, sylvan_substitute, BDD, a, BDD, vars, BDDVAR, prev_level)
{
    // Trivial cases
    if (BDD_ISCONSTANT(a)) return a;

    SV_CNT(C_substitute);

    sylvan_gc_test();

    bddnode_t na = GETNODE(a);
    BDDVAR level = na->level;

    BDD aLow = BDD_TRANSFERMARK(a, na->low);
    BDD aHigh = BDD_TRANSFERMARK(a, node_highedge(na));

    while (!sylvan_set_isempty(vars) && sylvan_var(vars) < level) {
        // Skip variables before x
        vars = sylvan_set_next(vars);
    }

    if (sylvan_set_isempty(vars)) return a;

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
        SPAWN(sylvan_substitute, aLow, vars, level);
        high = CALL(sylvan_substitute, aHigh, vars, level);
        TOMARK_PUSH(high)
        low = SYNC(sylvan_substitute);
    } else {
        SPAWN(sylvan_substitute, aHigh, vars, level);
        low = CALL(sylvan_substitute, aLow, vars, level);
        TOMARK_PUSH(low)
        high = SYNC(sylvan_substitute);
    }

    TOMARK_EXIT

    BDD result;

    if (!sylvan_set_isempty(vars) && sylvan_var(vars) == level) {
        result = sylvan_makenode(level-1, low, high);
    } else {
        if (low == aLow && high == aHigh) result = a;
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

BDD
sylvan_substitute(BDD a, BDD vars)
{
    return CALL(sylvan_substitute, a, vars, 0);
}

int
sylvan_relprods_analyse(BDD a, BDD b, void_cb cb_in, void_cb cb_out)
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

TASK_IMPL_4(BDD, sylvan_relprods, BDD, a, BDD, b, BDD, vars, BDDVAR, prev_level)
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
        low = CALL(sylvan_relprods, aLow, bLow, vars, level);
        if (low == sylvan_true) {
            result = sylvan_true;
        }
        else {
            TOMARK_PUSH(low)
            high = CALL(sylvan_relprods, aHigh, bHigh, vars, level);
            if (high == sylvan_true) {
                result = sylvan_true;
            }
            else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            }
            else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite, low, sylvan_true, high, 0);
            }
        }
    }
    else {
        if (rand_1()) {
            SPAWN(sylvan_relprods, aLow, bLow, vars, level);
            high = CALL(sylvan_relprods, aHigh, bHigh, vars, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_relprods);
        } else {
            SPAWN(sylvan_relprods, aHigh, bHigh, vars, level);
            low = CALL(sylvan_relprods, aLow, bLow, vars, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_relprods);
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

BDD
sylvan_relprods(BDD a, BDD b, BDD vars)
{
    return CALL(sylvan_relprods, a, b, vars, 0);
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
TASK_IMPL_4(BDD, sylvan_relprods_reversed, BDD, a, BDD, b, BDD, vars, BDDVAR, prev_level)
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
        low = CALL(sylvan_relprods_reversed, aLow, bLow, vars, x);
        // variable in X': quantify
        if (low == sylvan_true) {
            result = sylvan_true;
        } else {
            TOMARK_PUSH(low)
            high = CALL(sylvan_relprods_reversed, aHigh, bHigh, vars, x);
            if (high == sylvan_true) {
                result = sylvan_true;
            } else if (low == sylvan_false && high == sylvan_false) {
                result = sylvan_false;
            } else {
                TOMARK_PUSH(high)
                result = CALL(sylvan_ite, low, sylvan_true, high, 0);
            }
        }
    }
    // if x \in X OR if excluded (works in either case)
    else {
        if (rand_1()) {
            SPAWN(sylvan_relprods_reversed, aLow, bLow, vars, x);
            high = CALL(sylvan_relprods_reversed, aHigh, bHigh, vars, x);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_relprods_reversed);
        } else {
            SPAWN(sylvan_relprods_reversed, aHigh, bHigh, vars, x);
            low = CALL(sylvan_relprods_reversed, aLow, bLow, vars, x);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_relprods_reversed);
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

BDD
sylvan_relprods_reversed(BDD a, BDD b, BDD vars)
{
    return CALL(sylvan_relprods_reversed, a, b, vars, 0);
}

/**
 * Count number of nodes for each level
 */
// TODO: use AVL

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

/**
 * Count number of nodes in BDD
 */

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

size_t sylvan_nodecount(BDD a)
{
    uint32_t result = sylvan_nodecount_do_1(a);
    sylvan_nodecount_do_2(a);
    return result;
}

/**
 * CALCULATE NUMBER OF DISTINCT PATHS TO TRUE
 */

TASK_IMPL_1(long double, sylvan_pathcount, BDD, bdd)
{
    if (bdd == sylvan_false) return 0.0;
    if (bdd == sylvan_true) return 1.0;
    SPAWN(sylvan_pathcount, sylvan_low(bdd));
    SPAWN(sylvan_pathcount, sylvan_high(bdd));
    return SYNC(sylvan_pathcount) + SYNC(sylvan_pathcount);
}

long double sylvan_pathcount(BDD bdd)
{
    return CALL(sylvan_pathcount, bdd);
}

/**
 * CALCULATE NUMBER OF VAR ASSIGNMENTS THAT YIELD TRUE
 */

static long double
sylvan_satcount_rec(BDD bdd, BDD variables)
{
    if (bdd == sylvan_false) return 0.0;
    if (bdd == sylvan_true) return powl(2.0L, sylvan_set_count(variables));

    // Skip all variables before level(bdd)
    size_t skipped = 0;
    while (sylvan_var(bdd) > sylvan_var(variables)) {
        skipped++;
        variables = sylvan_set_next(variables);
        if (sylvan_set_isempty(variables)) break;
    }

    // We now expect sylvan_var(variables) == sylvan_var(bdd)
    if (sylvan_set_isempty(variables) || sylvan_var(variables) != sylvan_var(bdd)) {
        fprintf(stderr, "[sylvan_satcount] bdd contains unexpected level %d!\n", sylvan_var(bdd));
        assert(0);
    }

    long double high = sylvan_satcount_rec(sylvan_high(bdd), sylvan_set_next(variables));
    long double low = sylvan_satcount_rec(sylvan_low(bdd), sylvan_set_next(variables));
    return (high+low) * powl(2.0L, skipped);
}

long double
sylvan_satcount(BDD bdd, BDD variables)
{
    return sylvan_satcount_rec(bdd, variables);
}

/**
 * IMPLEMENTATION OF BDD-AS-SET
 */

int
sylvan_set_isempty(BDD set)
{
    return set == sylvan_false ? 1 : 0;
}

BDD
sylvan_set_empty()
{
    return sylvan_false;
}

BDD
sylvan_set_add(BDD set, BDDVAR level)
{
    return sylvan_or(set, sylvan_ithvar(level));
}

BDD
sylvan_set_remove(BDD set, BDDVAR level)
{
    return sylvan_exists(set, sylvan_ithvar(level));
}

int
sylvan_set_in(BDD set, BDDVAR level)
{
    while (!BDD_ISCONSTANT(set)) {
        bddnode_t n = GETNODE(set);
        if (n->level == level) return 1;
        if (n->level > level) break; // BDDs are ordered
        set = n->low;
    }

    return 0;
}

BDD
sylvan_set_next(BDD set)
{
    if (BDD_ISCONSTANT(set)) return sylvan_false;
    return GETNODE(set)->low;
}

size_t
sylvan_set_count(BDD set)
{
    size_t result = 0;
    while (set != sylvan_false) {
        result++;
        set = sylvan_set_next(set);
    }
    return result;
}

void
sylvan_set_toarray(BDD set, BDDVAR *arr)
{
    size_t i = 0;
    while (set != sylvan_false) {
        arr[i++] = GETNODE(set)->level;
        set = sylvan_set_next(set);
    }
}

BDD
sylvan_set_fromarray(BDDVAR* arr, size_t length)
{
    BDD result = sylvan_set_empty();
    size_t i;
    for (i=0; i<length; i++) result = sylvan_or(result, sylvan_ithvar(arr[i]));
    return result;
}

TASK_IMPL_1(BDD, sylvan_support, BDD, bdd)
{
    if (BDD_ISCONSTANT(bdd)) return sylvan_false;
    bddnode_t n = GETNODE(bdd);
    BDD high, low, set, result;

    TOMARK_INIT
    SPAWN(sylvan_support, n->low);
    high = CALL(sylvan_support, n->high);
    TOMARK_PUSH(high);
    low = SYNC(sylvan_support);
    TOMARK_PUSH(low);
    set = CALL(sylvan_ite, high, sylvan_true, low, 0);
    TOMARK_PUSH(set);
    result = CALL(sylvan_ite, sylvan_ithvar(n->level), sylvan_true, set, 0);
    TOMARK_EXIT

    return result;
}

BDD sylvan_support(BDD bdd)
{
    return CALL(sylvan_support, bdd);
}

/**
 * GENERIC MARK/UNMARK (DATA FIELD IN BDD NODE) METHODS
 */

static inline int
sylvan_mark(bddnode_t node, unsigned int mark)
{
    if (node->data & mark) return 0;
    node->data |= mark;
    return 1;
}

static inline int
sylvan_unmark(bddnode_t node, unsigned int mark)
{
    if (node->data & mark) {
        node->data &= ~mark;
        return 1;
    } else {
        return 0;
    }
}

static __attribute__((unused)) void
sylvan_mark_rec(bddnode_t node, unsigned int mark)
{
    if (sylvan_mark(node, mark)) {
        if (!BDD_ISCONSTANT(node->low)) sylvan_mark_rec(GETNODE(node->low), mark);
        if (!BDD_ISCONSTANT(node->high)) sylvan_mark_rec(GETNODE(node->high), mark);
    }
}

static __attribute__((unused)) void
sylvan_unmark_rec(bddnode_t node, unsigned int mark)
{
    if (sylvan_unmark(node, mark)) {
        if (!BDD_ISCONSTANT(node->low)) sylvan_unmark_rec(GETNODE(node->low), mark);
        if (!BDD_ISCONSTANT(node->high)) sylvan_unmark_rec(GETNODE(node->high), mark);
    }
}

/*************
 * DOT OUTPUT
*************/

/***
 * We keep a set [level -> [node]] using AVLset
 */
struct level_to_nodeset {
    BDDVAR level;
    avl_node_t *set;
};

AVL(level_to_nodeset, struct level_to_nodeset)
{
    return left->level - right->level;
}

AVL(nodeset, BDD)
{
    return *left - *right;
}

static void __attribute__((noinline))
sylvan_dothelper_register(avl_node_t **set, BDD bdd)
{
    struct level_to_nodeset s, *ss;
    bddnode_t node = GETNODE(bdd);
    s.level = node->level;
    ss = level_to_nodeset_search(*set, &s);
    if (ss == NULL) {
        s.set = NULL;
        ss = level_to_nodeset_put(set, &s, NULL);
    }
    assert(ss != NULL);
    bdd = BDD_STRIPMARK(bdd);
    nodeset_insert(&ss->set, &bdd);
}

static void
sylvan_fprintdot_rec(FILE *out, BDD bdd, avl_node_t **levels)
{
    if (bdd == sylvan_invalid || BDD_ISCONSTANT(bdd)) return;

    bdd = BDD_STRIPMARK(bdd);
    bddnode_t n = GETNODE(bdd);
    if (!sylvan_mark(n, 1)) return;

    sylvan_dothelper_register(levels, bdd);

    fprintf(out, "%llu [label=\"%d\"];\n", bdd, n->level);

    sylvan_fprintdot_rec(out, n->low, levels);
    sylvan_fprintdot_rec(out, n->high, levels);

    fprintf(out, "%llu -> %llu [style=dashed];\n", bdd, (BDD)n->low);
    fprintf(out, "%llu -> %llu [style=solid dir=both arrowtail=%s];\n", bdd, (BDD)n->high, n->comp ? "dot" : "none");
}

void
sylvan_fprintdot(FILE *out, BDD bdd)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    fprintf(out, "0 [shape=box, label=\"0\", style=filled, shape=box, height=0.3, width=0.3];\n");
    avl_node_t *levels = NULL;
    sylvan_fprintdot_rec(out, bdd, &levels);

    size_t levels_count = avl_count(levels);
    struct level_to_nodeset *arr = level_to_nodeset_toarray(levels);
    size_t i;
    for (i=0;i<levels_count;i++) {
        fprintf(out, "{ rank=same; ");
        size_t node_count = avl_count(arr[i].set);
        size_t j;
        BDD *arr_j = nodeset_toarray(arr[i].set);
        for (j=0;j<node_count;j++) {
            fprintf(out, "%llu; ", arr_j[j]);
        }
        fprintf(out, "}\n");
    }
    level_to_nodeset_free(&levels);

    fprintf(out, "}\n");
    sylvan_unmark_rec(GETNODE(bdd), 1);
}

void
sylvan_printdot(BDD bdd)
{
    sylvan_fprintdot(stdout, bdd);
}

/**
 * END DOT OUTPUT
 */


llmsset_t
__sylvan_get_internal_data()
{
    return _bdd.data;
}

llci_t
__sylvan_get_internal_cache()
{
    return _bdd.cache;
}

/**
 * SERIALIZATION helpers
 */

struct sylvan_ser {
    BDD bdd;
    size_t assigned;
};

AVL(sylvan_ser, struct sylvan_ser)
{
    return left->bdd - right->bdd;
}

AVL(sylvan_ser_reversed, struct sylvan_ser)
{
    return left->assigned - right->assigned;
}

static avl_node_t *sylvan_ser_set = NULL;
static avl_node_t *sylvan_ser_reversed_set = NULL;
static size_t sylvan_ser_counter = 1;

static void
sylvan_serialize_assign_rec(BDD bdd)
{
    if (!BDD_ISCONSTANT(bdd)) {
        bddnode_t n = GETNODE(bdd);

        struct sylvan_ser s, *ss;
        s.bdd = BDD_STRIPMARK(bdd);
        ss = sylvan_ser_search(sylvan_ser_set, &s);
        if (ss == NULL) {
            s.assigned = 0; // dummy value
            ss = sylvan_ser_put(&sylvan_ser_set, &s, NULL);

            sylvan_serialize_assign_rec(n->low);
            sylvan_serialize_assign_rec(n->high);

            ss->assigned = sylvan_ser_counter++;
            sylvan_ser_reversed_insert(&sylvan_ser_reversed_set, ss); // put a copy in the reversed table...
        }
    }
}

void
sylvan_serialize_add(BDD bdd)
{
    sylvan_serialize_assign_rec(bdd);
}

void
sylvan_serialize_reset()
{
    sylvan_ser_free(&sylvan_ser_set);
    sylvan_ser_free(&sylvan_ser_reversed_set);
    sylvan_ser_counter = 1;
}

size_t
sylvan_serialize_get(BDD bdd)
{
    if (BDD_ISCONSTANT(bdd)) return bdd;
    struct sylvan_ser s, *ss;
    s.bdd = BDD_STRIPMARK(bdd);
    ss = sylvan_ser_search(sylvan_ser_set, &s);
    assert(ss != NULL);
    return BDD_TRANSFERMARK(bdd, ss->assigned);
}

BDD
sylvan_serialize_get_reversed(size_t value)
{
    if (BDD_ISCONSTANT(value)) return value;
    struct sylvan_ser s, *ss;
    s.assigned = BDD_STRIPMARK(value);
    ss = sylvan_ser_reversed_search(sylvan_ser_reversed_set, &s);
    assert(ss != NULL);
    return BDD_TRANSFERMARK(value, ss->bdd);
}

void
sylvan_serialize_totext(FILE *out)
{
    fprintf(out, "[");
    avl_iter_t *it = sylvan_ser_reversed_iter(sylvan_ser_reversed_set);
    struct sylvan_ser *s;
    while ((s=sylvan_ser_reversed_iter_next(it))) {
        BDD bdd = s->bdd;
        bddnode_t n = GETNODE(bdd);
        fprintf(out, "(%zu,%u,%zu,%zu,%u),", s->assigned,
                                             n->level,
                                             sylvan_serialize_get(n->low),
                                             sylvan_serialize_get(n->high),
                                             n->comp);
    }
    sylvan_ser_reversed_iter_free(it);
    fprintf(out, "]");
}

void
sylvan_serialize_tofile(FILE *out)
{
    size_t count = avl_count(sylvan_ser_reversed_set);
    fwrite(&count, sizeof(size_t), 1, out);

    struct sylvan_ser *s;
    avl_iter_t *it = sylvan_ser_reversed_iter(sylvan_ser_reversed_set);
    while ((s=sylvan_ser_reversed_iter_next(it))) {
        bddnode_t n = GETNODE(s->bdd);

        struct bddnode node;
        node.high = sylvan_serialize_get(n->high);
        node.low = sylvan_serialize_get(n->low);
        node.level = n->level;
        node.data = 0;
        node.comp = n->comp;

        fwrite(&node, sizeof(struct bddnode), 1, out);
    }
    sylvan_ser_reversed_iter_free(it);
}

void
sylvan_serialize_fromfile(FILE *in)
{
    sylvan_serialize_reset();
    size_t count, i;
    fread(&count, 8, 1, in);

    for (i=1; i<=count; i++) {
        struct bddnode node;
        fread(&node, sizeof(struct bddnode), 1, in);

        BDD low = sylvan_serialize_get_reversed(node.low);
        BDD high = sylvan_serialize_get_reversed(node.high);
        if (node.comp) high |= complementmark;

        struct sylvan_ser s;

        s.bdd = sylvan_makenode(node.level, low, high);
        s.assigned = i;

        sylvan_ser_insert(&sylvan_ser_set, &s);
        sylvan_ser_reversed_insert(&sylvan_ser_reversed_set, &s);
    }
}
