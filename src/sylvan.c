#include <config.h>

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
#include <barrier.h>
#include <lace.h>
#include <llmsset.h>
#include <sylvan.h>
#include <tls.h>

#if USE_NUMA
#include <numa.h>
#endif

#ifndef SYLVAN_CACHE_STATS
#define SYLVAN_CACHE_STATS 0
#endif

#ifndef SYLVAN_OPERATION_STATS
#define SYLVAN_OPERATION_STATS 0
#endif

#define SYLVAN_STATS SYLVAN_CACHE_STATS || SYLVAN_OPERATION_STATS

#ifndef SYLVAN_REPORT_COLORED
#define SYLVAN_REPORT_COLORED 1
#endif

/**
 * Complement handling macros
 */
#define BDD_HASMARK(s)              (s&sylvan_complement?1:0)
#define BDD_TOGGLEMARK(s)           (s^sylvan_complement)
#define BDD_STRIPMARK(s)            (s&~sylvan_complement)
#define BDD_TRANSFERMARK(from, to)  (to ^ (from & sylvan_complement))
// Equal under mark
#define BDD_EQUALM(a, b)            ((((a)^(b))&(~sylvan_complement))==0)

struct __attribute__((packed)) bddnode {
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

static barrier_t bar;

/**
 * Methods to manipulate the free field inside the BDD type
 */

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

static DECLARE_THREAD_LOCAL(gc_key, gc_tomark_t);

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
 * Operation numbers are stored in the data field of the first parameter
 */
#define CACHE_ITE 0
#define CACHE_RELPRODS 1
#define CACHE_RRELPRODS 2
#define CACHE_COUNT 3
#define CACHE_EXISTS 4
#define CACHE_SATCOUNT 5
#define CACHE_RELPROD 6
#define CACHE_SUBST 7
#define CACHE_CONSTRAIN 8

struct __attribute__((packed)) bddcache {
    BDD params[3];
    BDD result;
};

typedef struct bddcache* bddcache_t;

#define LLCI_KEYSIZE ((sizeof(struct bddcache) - sizeof(BDD)))
#define LLCI_DATASIZE ((sizeof(struct bddcache)))
#include <llci.h>

/** static _bdd struct */

static struct
{
    llmsset_t data;
    llci_t cache; // operations cache
    int workers;
    int gc;
} _bdd;

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
 * Thread-local Insert index for LLMSset
 */
static DECLARE_THREAD_LOCAL(insert_index, uint64_t*);

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
 * External references (note: protected by a spinlock)
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
    if (sylvan_isnode(a)) {
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
    }

    return a;
}

void
sylvan_deref(BDD a)
{
    if (sylvan_isnode(a)) {
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

// During garbage collection, after clear, mark stuff again
static void
sylvan_pregc_mark_rec(BDD bdd)
{
    if (!sylvan_isnode(bdd)) return;

    if (llmsset_mark_unsafe(_bdd.data, bdd&0x000000ffffffffff)) {
        bddnode_t n = GETNODE(bdd);
        sylvan_pregc_mark_rec(n->low);
        sylvan_pregc_mark_rec(n->high);
    }
}

// Recursively mark all externally referenced BDDs
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

/** init and quit functions */

static int initialized = 0;
static int granularity = 1; // default

static void sylvan_test_gc();

void
sylvan_init(size_t tablesize, size_t cachesize, int _granularity)
{
    if (initialized != 0) return;
    initialized = 1;

    if (!lace_inited()) {
        fprintf(stderr, "Lace has not been initialized!\n");
        exit(1);
    }

    lace_set_callback(sylvan_test_gc);
    _bdd.workers = lace_workers();
    barrier_init (&bar, lace_workers());

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
        fprintf(stderr, "sylvan_init error: tablesize must be < 40!\n");
        exit(1);
    }

    _bdd.data = llmsset_create(sizeof(struct bddnode), sizeof(struct bddnode), 1LL<<tablesize);

    if (cachesize >= 64) {
        fprintf(stderr, "sylvan_init error: cachesize must be < 64!\n");
        exit(1);
    }

    _bdd.cache = llci_create(1LL<<cachesize);

    _bdd.gc = 0;
}

void
sylvan_quit()
{
    if (initialized == 0) return;
    initialized = 0;

    // TODO: remove lace callback

    llci_free(_bdd.cache);
    llmsset_free(_bdd.data);
    refset_free(&sylvan_refs);
    barrier_destroy (&bar);
}

/**
 * Statistics stuff
 */
#if SYLVAN_STATS
typedef enum {
#if SYLVAN_CACHE_STATS
  C_cache_new,
  C_cache_exists,
  C_cache_reuse,
#endif
  C_gc_user,
  C_gc_hashtable_full,
#if SYLVAN_OPERATION_STATS
  C_ite,
  C_exists,
  C_relprods,
  C_relprods_reversed,
  C_relprod,
  C_substitute,
  C_constrain,
#endif
  C_MAX
} Counters;

#define N_CNT_THREAD 128
#define SYLVAN_PAD(x,b) ( (b) - ( (x) & ((b)-1) ) ) /* b must be power of 2 */
struct {
    uint64_t count[C_MAX];
    char pad[SYLVAN_PAD(sizeof(uint64_t)*C_MAX, 64)];
} sylvan_stats[N_CNT_THREAD];
#endif

#if SYLVAN_REPORT_COLORED
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

void
sylvan_reset_counters()
{
    if (initialized == 0) return;

#if SYLVAN_STATS
    int i,j;
    for (i=0;i<N_CNT_THREAD;i++) {
        for (j=0;j<C_MAX;j++) {
            sylvan_stats[i].count[j] = 0;
        }
    }
#endif
}

void
sylvan_report_stats()
{
    if (initialized == 0) return;

#if SYLVAN_STATS
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

    uint64_t totals[C_MAX];
    for (i=0;i<C_MAX;i++) totals[i] = 0;
    for (i=0;i<N_CNT_THREAD;i++) {
        for (j=0;j<C_MAX;j++) totals[j] += sylvan_stats[i].count[j];
    }

#if SYLVAN_CACHE_STATS
    printf(NC ULINE "Cache\n" NC LBLUE);

    uint64_t total_cache = totals[C_cache_new] + totals[C_cache_exists] + totals[C_cache_reuse];
    printf("New results:         %" PRIu64 "\n", totals[C_cache_new]);
    printf("Existing results:    %" PRIu64 "\n", totals[C_cache_exists]);
    printf("Reused results:      %" PRIu64 "\n", totals[C_cache_reuse]);
    printf("Total results:       %" PRIu64 "\n", total_cache);
#endif

    printf(NC ULINE "GC\n" NC LBLUE);
    printf("GC user-request:     %" PRIu64 "\n", totals[C_gc_user]);
    printf("GC full table:       %" PRIu64 "\n", totals[C_gc_hashtable_full]);

#if SYLVAN_OPERATION_STATS
    printf(NC ULINE "Call counters (ITE, exists, relprods, reversed relprods, relprod, substitute, constrain)\n" NC LBLUE);
    for (i=0;i<_bdd.workers;i++) {
        printf("Worker %02d:           %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n", i,
            sylvan_stats[i].count[C_ite], sylvan_stats[i].count[C_exists], 
            sylvan_stats[i].count[C_relprods], sylvan_stats[i].count[C_relprods_reversed],
            sylvan_stats[i].count[C_relprod], sylvan_stats[i].count[C_substitute], sylvan_stats[i].count[C_constrain]);
    }
    printf("Totals:              %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 ", %" PRIu64 "\n",
        totals[C_ite], totals[C_exists], 
        totals[C_relprods], totals[C_relprods_reversed],
        totals[C_relprod], totals[C_substitute], totals[C_constrain]);
#endif

    printf(LRED  "****************" NC " \n");

    // For bonus point, calculate LLGCSET size...
    printf("BDD Unique table: %zu of %zu buckets filled.\n", llmsset_get_filled(_bdd.data), llmsset_get_size(_bdd.data));
#endif
}

#if SYLVAN_STATS
#define SV_CNT(s) {(sylvan_stats[LACE_WORKER_ID].count[s]+=1);}
#else
#define SV_CNT(s) ; /* Empty */
#endif

#if SYLVAN_CACHE_STATS
#define SV_CNT_CACHE(s) SV_CNT(s)
#else
#define SV_CNT_CACHE(s) /* Empty */
#endif

#if SYLVAN_OPERATION_STATS
#define SV_CNT_OP(s) SV_CNT(s)
#else
#define SV_CNT_OP(s) /* Empty */
#endif

/**
 * Very custom random number generator, based on the stack pointer and the OS thread id
 */
static inline
size_t rand_1()
{
    size_t id = (size_t)pthread_self();
    asm("addq %%rsp, %0" : "+r"(id) : : "cc"); // add RSP to id to increase randomness
    id *= 1103515245;
    id += 12345;
    return id & 8 ? 1 : 0;
}

/**
 * Garbage collection
 */

static int gc_enabled = 1;

void
sylvan_gc_enable()
{
    gc_enabled = 1;
}

void
sylvan_gc_disable()
{
    gc_enabled = 0;
}

/* Not to be inlined */
static void
sylvan_gc_participate()
{
    barrier_wait (&bar);

    int my_id = LACE_WORKER_ID;
    int workers = _bdd.workers;

    // Clear the memoization table
    llci_clear_multi(_bdd.cache, my_id, workers);

    // GC phase 1: clear hash table
    llmsset_clear_multi(_bdd.data, my_id, workers);
    barrier_wait (&bar);

    // GC phase 2: mark nodes
    LOCALIZE_THREAD_LOCAL(gc_key, gc_tomark_t);
    gc_tomark_t t = gc_key;
    while (t != NULL) {
        sylvan_pregc_mark_rec(t->bdd);
        t = t->prev;
    }
    barrier_wait (&bar);

    // GC phase 3: rehash BDDs
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    if (insert_index == NULL) insert_index = initialize_insert_index();
    *insert_index = llmsset_get_insertindex_multi(_bdd.data, my_id, _bdd.workers);

    llmsset_rehash_multi(_bdd.data, my_id, workers);

    barrier_wait (&bar);
}

static void
sylvan_test_gc(void)
{
    if (_bdd.gc) {
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

    barrier_wait (&bar);

    // Clear the memoization table
    llci_clear_multi(_bdd.cache, my_id, workers);

    // GC phase 1: clear hash table
    llmsset_clear_multi(_bdd.data, my_id, workers);

    barrier_wait (&bar);

    // GC phase 2a: mark external refs
    sylvan_pregc_mark_refs();

    // GC phase 2b: mark internal refs
    LOCALIZE_THREAD_LOCAL(gc_key, gc_tomark_t);
    gc_tomark_t t = gc_key;
    while (t != NULL) {
        sylvan_pregc_mark_rec(t->bdd);
        t = t->prev;
    }

    barrier_wait (&bar);

    // GC phase 3: rehash BDDs
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    if (insert_index == NULL) insert_index = initialize_insert_index();
    *insert_index = llmsset_get_insertindex_multi(_bdd.data, my_id, _bdd.workers);

    llmsset_rehash_multi(_bdd.data, my_id, workers);

    ATOMIC_WRITE(_bdd.gc, 0);
    barrier_wait (&bar);
}

/*
 * This method is *often* called *from parallel code* to test if we are
 * entering a garbage collection phase
 */
static inline void
sylvan_gc_test()
{
    while (_bdd.gc) {
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
 * Tiny debug stuff
 */
void
sylvan_fprint(FILE *f, BDD bdd)
{
    sylvan_serialize_reset();
    size_t v = sylvan_serialize_add(bdd);
    fprintf(f, "%s%zu,", bdd&sylvan_complement?"!":"", v);
    sylvan_serialize_totext(f);
}

void
sylvan_print(BDD bdd)
{
    sylvan_fprint(stdout, bdd);
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
        n = (struct bddnode){high, level, low, 0, (uint8_t)(BDD_HASMARK(high) ? 0 : 1)};
    } else {
        mark = 0;
        n = (struct bddnode){high, level, low, 0, (uint8_t)(BDD_HASMARK(high) ? 1 : 0)};
    }

    BDD result;
    uint64_t index;
    int created;
    if (llmsset_lookup(_bdd.data, &n, insert_index, &created, &index) == 0) {
        SV_CNT(C_gc_hashtable_full);

        //size_t before_gc = llmsset_get_filled(_bdd.data);
        if (gc_enabled) sylvan_gc_go();
        //size_t after_gc = llmsset_get_filled(_bdd.data);
        //fprintf(stderr, "GC: %ld to %ld (freed %ld)\n", before_gc, after_gc, before_gc-after_gc);

        if (llmsset_lookup(_bdd.data, &n, insert_index, &created, &index) == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_get_filled(_bdd.data), llmsset_get_size(_bdd.data));
            exit(1);
        }
    }

    result = index;
    return mark ? result | sylvan_complement : result;
}

BDD
sylvan_ithvar(BDDVAR level)
{
    return sylvan_makenode(level, sylvan_false, sylvan_true);
}

BDDVAR
sylvan_var(BDD bdd)
{
    assert(!sylvan_isconst(bdd));
    return GETNODE(bdd)->level;
}

static inline BDD
node_lowedge(bddnode_t node)
{
    return node->low;
}

static inline BDD
node_highedge(bddnode_t node)
{
    return node->high | (node->comp ? sylvan_complement : 0LL);
}

static inline BDD
node_low(BDD bdd, bddnode_t node)
{
    return BDD_TRANSFERMARK(bdd, node_lowedge(node));
}

static inline BDD
node_high(BDD bdd, bddnode_t node)
{
    return BDD_TRANSFERMARK(bdd, node_highedge(node));
}

BDD
sylvan_low(BDD bdd)
{
    if (sylvan_isconst(bdd)) return bdd;
    return node_low(bdd, GETNODE(bdd));
}

BDD
sylvan_high(BDD bdd)
{
    if (sylvan_isconst(bdd)) return bdd;
    return node_high(bdd, GETNODE(bdd));
}

/**
 * Debugging functionality that converts a BDD to one without complemented edges.
 */

BDD
sylvan_makenode_nocomp(BDDVAR level, BDD low, BDD high)
{
    if (low == high) return low;

    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    if (insert_index == NULL) insert_index = initialize_insert_index();

    struct bddnode n = (struct bddnode){high, level, low, 0, 0};

    uint64_t index;
    int created;
    if (llmsset_lookup(_bdd.data, &n, insert_index, &created, &index) == 0) {
        SV_CNT(C_gc_hashtable_full);
        if (gc_enabled) sylvan_gc_go();
        if (llmsset_lookup(_bdd.data, &n, insert_index, &created, &index) == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_get_filled(_bdd.data), llmsset_get_size(_bdd.data));
            exit(1);
        }
    }

    return (BDD)index;
}

BDD
sylvan_bdd_to_nocomp(BDD bdd)
{
    if (bdd == sylvan_true) return sylvan_true_nc;
    if (bdd == sylvan_false) return sylvan_false;

    bddnode_t n = GETNODE(bdd);
    return sylvan_makenode_nocomp(n->level, sylvan_bdd_to_nocomp(node_low(bdd, n)), sylvan_bdd_to_nocomp(node_high(bdd, n)));
}

/**
 * Implementation of unary, binary and if-then-else operators.
 */

TASK_IMPL_4(BDD, sylvan_ite, BDD, a, BDD, b, BDD, c, BDDVAR, prev_level)
{
    /* Terminal cases */
    if (a == sylvan_true) return b;
    if (a == sylvan_false) return c;
    if (a == b) b = sylvan_true;
    if (a == sylvan_not(b)) b = sylvan_false;
    if (a == c) c = sylvan_false;
    if (a == sylvan_not(c)) c = sylvan_true;
    if (b == c) return b;
    if (b == sylvan_true && c == sylvan_false) return a;
    if (b == sylvan_false && c == sylvan_true) return sylvan_not(a);

    /* End terminal cases. Apply rewrite rules to optimize cache use */

    /* At this point, A is not a constant true/false. */

    if (sylvan_isconst(b) && BDD_STRIPMARK(c) < BDD_STRIPMARK(a)) {
        if (b == sylvan_false) {
            // ITE(A,F,C) = ITE(~C,F,~A)
            //            = (A and F) or (~A and C)
            //            = F or (~A and C)
            //            = (~C and F) or (C and ~A)
            //            = ITE(~C,F,~A)
            BDD t = a;
            a = sylvan_not(c);
            c = sylvan_not(t);
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

    if (sylvan_isconst(c) && BDD_STRIPMARK(b) < BDD_STRIPMARK(a)) {
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
            a = sylvan_not(b);
            b = sylvan_not(t);
        }
    }

    if (BDD_STRIPMARK(b) == BDD_STRIPMARK(c)) {
        // At this point, B and C are not constants because that is a terminal case
        // 1. if A then B else ~B = if B then A else ~A
        // 2. if A then ~B else B = if ~B then A else ~A
        if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
            // a > b, exchange:
            b = a;
            a = sylvan_not(c);
            c = sylvan_not(b);
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
    int mark = 0;
    if (BDD_HASMARK(b)) {
        b = sylvan_not(b);
        c = sylvan_not(c);
        mark = 1;
    }

    sylvan_gc_test();

    // The value of a,b,c may be changed, but the reference counters are not changed at this point.

    SV_CNT_OP(C_ite);

    bddnode_t na = sylvan_isconst(a) ? 0 : GETNODE(a);
    bddnode_t nb = sylvan_isconst(b) ? 0 : GETNODE(b);
    bddnode_t nc = sylvan_isconst(c) ? 0 : GETNODE(c);

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
        template_cache_node.params[0] = BDD_SETDATA(a, CACHE_ITE);
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = c;
        template_cache_node.result = sylvan_invalid;

        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD res = template_cache_node.result;
            SV_CNT_CACHE(C_cache_reuse);
            return mark ? sylvan_not(res) : res;
        }
    }

    // Get cofactors
    BDD aLow = a, aHigh = a;
    BDD bLow = b, bHigh = b;
    BDD cLow = c, cHigh = c;
    if (na && level == na->level) {
        aLow = node_low(a, na);
        aHigh = node_high(a, na);
    }
    if (nb && level == nb->level) {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
    }
    if (nc && level == nc->level) {
        cLow = node_low(c, nc);
        cHigh = node_high(c, nc);
    }

    // Recursive computation
    BDD low=sylvan_invalid, high=sylvan_invalid;
    TOMARK_INIT
    if (rand_1()) {
        if (sylvan_isconst(aHigh)) {
            if (aHigh == sylvan_true) high = bHigh;
            else high = cHigh;
            low = CALL(sylvan_ite, aLow, bLow, cLow, level);
            TOMARK_PUSH(low)
        } else {
            SPAWN(sylvan_ite, aHigh, bHigh, cHigh, level);
            low = CALL(sylvan_ite, aLow, bLow, cLow, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_ite);
            TOMARK_PUSH(high)
        }
    } else {
        if (sylvan_isconst(aLow)) {
            if (aLow == sylvan_true) low = bLow;
            else low = cLow;
            high = CALL(sylvan_ite, aHigh, bHigh, cHigh, level);
            TOMARK_PUSH(high)
        } else {
            SPAWN(sylvan_ite, aLow, bLow, cLow, level);
            high = CALL(sylvan_ite, aHigh, bHigh, cHigh, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_ite);
            TOMARK_PUSH(low)
        }
    }

    BDD result = sylvan_makenode(level, low, high);
    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT_CACHE(C_cache_exists);
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT_CACHE(C_cache_new);
        }
    }

    return mark ? sylvan_not(result) : result;
}

/**
 * Calculate constrain a @ c
 */
TASK_IMPL_3(BDD, sylvan_constrain, BDD, a, BDD, b, BDDVAR, prev_level)
{
    /* Trivial cases */
    if (b == sylvan_true) return a;
    if (b == sylvan_false) return sylvan_false;
    if (sylvan_isconst(a)) return a;
    if (a == b) return sylvan_true;
    if (a == sylvan_not(b)) return sylvan_false;

    /* Perhaps execute garbage collection */
    sylvan_gc_test();

    /* Count operation */
    SV_CNT_OP(C_constrain);

    // a != constant and b != constant
    bddnode_t na = GETNODE(a);
    bddnode_t nb = GETNODE(b);

    BDDVAR level = na->level < nb->level ? na->level : nb->level;

    // CONSULT CACHE

    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != level / granularity;
    struct bddcache template_cache_node;
    if (cachenow) {
        // TODO: get rid of complement on a for better cache see cudd
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, CACHE_CONSTRAIN);
        template_cache_node.params[1] = b;
        template_cache_node.result = sylvan_invalid;

        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT_CACHE(C_cache_reuse);
            return result;
        }
    }

    // DETERMINE TOP BDDVAR AND COFACTORS

    BDD aLow, aHigh, bLow, bHigh;

    if (na->level == level) {
        aLow = node_low(a, na);
        aHigh = node_high(a, na);
    } else {
        aLow = aHigh = a;
    }

    if (nb->level == level) {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
    } else {
        bLow = bHigh = b;
    }

    BDD result;
    TOMARK_INIT

    BDD low=sylvan_invalid, high=sylvan_invalid;
    if (rand_1()) {
        // Since we already computed bHigh, we can see some trivial results
        if (sylvan_isconst(bHigh)) {
            if (bHigh == sylvan_true) high = aHigh;
            else return CALL(sylvan_constrain, aLow, bLow, level);
        } else {
            SPAWN(sylvan_constrain, aHigh, bHigh, level);
        }

        // Since we already computed bLow, we can see some trivial results
        if (sylvan_isconst(bLow)) {
            if (bLow == sylvan_true) low = bLow;
            else {
                // okay, return aHigh @ bHigh and skip cache
                if (bHigh != sylvan_true) high = SYNC(sylvan_constrain);
                return high;
            }
        } else {
            low = CALL(sylvan_constrain, aLow, bLow, level);
            TOMARK_PUSH(low)
        }

        if (bHigh != sylvan_true) {
            high = SYNC(sylvan_constrain);
            TOMARK_PUSH(high)
        }
    } else {
        // Since we already computed bHigh, we can see some trivial results
        if (sylvan_isconst(bLow)) {
            if (bLow == sylvan_true) low = aLow;
            else return CALL(sylvan_constrain, aHigh, bHigh, level);
        } else {
            SPAWN(sylvan_constrain, aLow, bLow, level);
        }

        // Since we already computed bLow, we can see some trivial results
        if (sylvan_isconst(bHigh)) {
            if (bHigh == sylvan_true) high = bHigh;
            else {
                // okay, return aLow @ bLow and skip cache
                if (bLow != sylvan_true) low = SYNC(sylvan_constrain);
                return low;
            }
        } else {
            high = CALL(sylvan_constrain, aHigh, bHigh, level);
            TOMARK_PUSH(high)
        }

        if (bLow != sylvan_true) {
            low = SYNC(sylvan_constrain);
            TOMARK_PUSH(low)
        }
    }

    result = sylvan_makenode(level, low, high);

    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT_CACHE(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT_CACHE(C_cache_new);
        }
    }

    return result;
}

/**
 * Calculates \exists variables . a
 */
TASK_IMPL_3(BDD, sylvan_exists, BDD, a, BDD, variables, BDDVAR, prev_level)
{
    /* Trivial cases */
    if (sylvan_isconst(a)) return a;
    if (sylvan_set_isempty(variables)) return a;

    sylvan_gc_test();

    SV_CNT_OP(C_exists);

    // a != constant
    bddnode_t na = GETNODE(a);
    BDDVAR level = na->level;

    // Get cofactors
    BDD aLow = node_low(a, na);
    BDD aHigh = node_high(a, na);

    while (!sylvan_set_isempty(variables) && sylvan_var(variables) < level) {
        // Skip variables before x
        variables = sylvan_set_next(variables);
    }

    if (sylvan_set_isempty(variables)) return a; // again, trivial case

    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != level / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, CACHE_EXISTS);
        template_cache_node.params[1] = variables;
        template_cache_node.result = sylvan_invalid;

        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT_CACHE(C_cache_reuse);
            return result;
        }
    }

    BDD result;
    TOMARK_INIT

    if (sylvan_set_var(variables) == level) {
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
            TOMARK_PUSH(high)
        } else {
            SPAWN(sylvan_exists, aLow, variables, level);
            high = CALL(sylvan_exists, aHigh, variables, level);
            TOMARK_PUSH(high)
            low = SYNC(sylvan_exists);
            TOMARK_PUSH(low)
        }
        result = sylvan_makenode(level, low, high);
    }

    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT_CACHE(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT_CACHE(C_cache_new);
        }
    }

    return result;
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

    SV_CNT_OP(C_relprod);

    bddnode_t na = sylvan_isconst(a) ? 0 : GETNODE(a);
    bddnode_t nb = sylvan_isconst(b) ? 0 : GETNODE(b);

    // Get lowest level and cofactors
    BDDVAR level;
    BDD aLow=a, aHigh=a, bLow=b, bHigh=b;
    if (na) {
        if (nb && na->level > nb->level) {
            level = nb->level;
            bLow = node_low(b, nb);
            bHigh = node_high(b, nb);
        } else if (nb && na->level == nb->level) {
            level = na->level;
            aLow = node_low(a, na);
            aHigh = node_high(a, na);
            bLow = node_low(b, nb);
            bHigh = node_high(b, nb);
        } else {
            level = na->level;
            aLow = node_low(a, na);
            aHigh = node_high(a, na);
        }
    } else {
        level = nb->level;
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
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
        template_cache_node.params[0] = BDD_SETDATA(a, CACHE_RELPROD);
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = x;
        template_cache_node.result = sylvan_invalid;

        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT_CACHE(C_cache_reuse);
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
            TOMARK_PUSH(low)
        } else {
            SPAWN(sylvan_relprod, aHigh, bHigh, x, level);
            low = CALL(sylvan_relprod, aLow, bLow, x, level);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_relprod);
            TOMARK_PUSH(high)
        }
        result = sylvan_makenode(level, low, high);
    }

    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT_CACHE(C_cache_exists);
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT_CACHE(C_cache_new);
        }
    }

    return result;
}

/**
 * Specialized substitute, substitutes variables 'x' \in vars by 'x-1'
 */
TASK_IMPL_3(BDD, sylvan_substitute, BDD, a, BDD, vars, BDDVAR, prev_level)
{
    // Trivial cases
    if (sylvan_isconst(a)) return a;

    SV_CNT_OP(C_substitute);

    sylvan_gc_test();

    bddnode_t na = GETNODE(a);
    BDDVAR level = na->level;

    BDD aLow = node_low(a, na);
    BDD aHigh = node_high(a, na);

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
        template_cache_node.params[0] = BDD_SETDATA(a, CACHE_SUBST);
        template_cache_node.params[1] = vars;
        template_cache_node.params[2] = 0;
        template_cache_node.result = sylvan_invalid;

        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT_CACHE(C_cache_reuse);
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
        TOMARK_PUSH(low)
    } else {
        SPAWN(sylvan_substitute, aHigh, vars, level);
        low = CALL(sylvan_substitute, aLow, vars, level);
        TOMARK_PUSH(low)
        high = SYNC(sylvan_substitute);
        TOMARK_PUSH(high)
    }

    BDD result;

    if (!sylvan_set_isempty(vars) && sylvan_var(vars) == level) {
        result = sylvan_makenode(level-1, low, high);
    } else {
        if (low == aLow && high == aHigh) result = a;
        else result = sylvan_makenode(level, low, high);
    }

    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT_CACHE(C_cache_exists);
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT_CACHE(C_cache_new);
        }
    }

    return result;
}

int
sylvan_relprods_analyse(BDD a, BDD b, void_cb cb_in, void_cb cb_out)
{
    if (a == sylvan_true && b == sylvan_true) return 0;
    if (a == sylvan_false || b == sylvan_false) return 0;
    if (a == b) b = sylvan_true;
    else if (BDD_EQUALM(a, b)) return 0;

    bddnode_t na = sylvan_isconst(a) ? 0 : GETNODE(a);
    bddnode_t nb = sylvan_isconst(b) ? 0 : GETNODE(b);

    BDD aLow=a, aHigh=a, bLow=b, bHigh=b;
    if (na) {
        if (nb && na->level > nb->level) {
            bLow = node_low(b, nb);
            bHigh = node_high(b, nb);
        } else if (nb && na->level == nb->level) {
            aLow = node_low(a, na);
            aHigh = node_high(a, na);
            bLow = node_low(b, nb);
            bHigh = node_high(b, nb);
        } else {
            aLow = node_low(a, na);
            aHigh = node_high(a, na);
        }
    } else {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
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
    /* Trivial cases */
    if (a == sylvan_true && b == sylvan_true) return sylvan_true;
    if (a == sylvan_false || b == sylvan_false) return sylvan_false;
    if (a == b) b = sylvan_true;
    else if (a == sylvan_not(b)) return sylvan_false;


    /* Perhaps execute garbage collection */
    sylvan_gc_test();

    /* Rewrite to improve cache utilisation */
    if (a > b || b == sylvan_true) {
        BDD _b = b;
        b = a;
        a = _b;
    }

    /* Count operation */
    SV_CNT_OP(C_relprods);

    /* Determine top level */
    bddnode_t na = sylvan_isconst(a) ? 0 : GETNODE(a);
    bddnode_t nb = GETNODE(b);

    BDDVAR level = nb->level;
    if (na && na->level < level) level = na->level;

    // Determine if level \in vars
    int in_vars = vars == sylvan_true;
    if (!in_vars) while (!sylvan_set_isempty(vars)) {
        BDD it_var = sylvan_set_var(vars);
        if (it_var < level) {
            vars = sylvan_set_next(vars);
            continue;
        }
        if (it_var == level) {
            vars = sylvan_set_next(vars);
            in_vars = 1;
        }
        break;
    }

    /* Consult cache */
    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != level / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, CACHE_RELPRODS);
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = vars;
        template_cache_node.result = sylvan_invalid;

        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT_CACHE(C_cache_reuse);
            return result;
        }
    }

    /* Determine cofactors */
    BDD aLow=a, aHigh=a, bLow=b, bHigh=b;
    if (na && na->level == level) {
        aLow = node_low(a, na);
        aHigh = node_high(a, na);
    }
    if (nb->level == level) {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
    }

    /* Recursively calculate low and high */
    BDD low=sylvan_invalid, high=sylvan_invalid, result;

    // Save old internal reference stack top
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
            int spawned = 0;
            // Get rid of trivial cases
            if (aLow == sylvan_true && bLow == sylvan_true) low = sylvan_true;
            else if (aLow == sylvan_false || bLow == sylvan_false) low = sylvan_false;
            else if (aLow == bLow) low = sylvan_true;
            else if (BDD_EQUALM(aLow, bLow)) low = sylvan_false;
            else {
                SPAWN(sylvan_relprods, aLow, bLow, vars, level);
                spawned = 1;
            }
            high = CALL(sylvan_relprods, aHigh, bHigh, vars, level);
            TOMARK_PUSH(high)
            if (spawned) {
                low = SYNC(sylvan_relprods);
                TOMARK_PUSH(low)
            }
        } else {
            int spawned = 0;
            // Get rid of trivial cases
            if (aHigh == sylvan_true && bHigh == sylvan_true) high = sylvan_true;
            else if (aHigh == sylvan_false || bHigh == sylvan_false) high = sylvan_false;
            else if (aHigh == bHigh) high = sylvan_true;
            else if (BDD_EQUALM(aHigh, bHigh)) high = sylvan_false;
            else {
                SPAWN(sylvan_relprods, aHigh, bHigh, vars, level);
                spawned = 1;
            }
            low = CALL(sylvan_relprods, aLow, bLow, vars, level);
            TOMARK_PUSH(low)
            if (spawned) {
                high = SYNC(sylvan_relprods);
                TOMARK_PUSH(high)
            }
        }

        // variable in X': substitute
        if (in_vars) result = sylvan_makenode(level-1, low, high);

        // variable not in X or X': normal behavior
        else result = sylvan_makenode(level, low, high);
    }

    // Drop references added after TOMARK_INIT
    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT_CACHE(C_cache_exists);
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT_CACHE(C_cache_new);
        }
    }

    return result;
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

    SV_CNT_OP(C_relprods_reversed);

    bddnode_t na = sylvan_isconst(a) ? 0 : GETNODE(a);
    bddnode_t nb = sylvan_isconst(b) ? 0 : GETNODE(b);

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
        while (!sylvan_set_isempty(vars) && (it_var=(GETNODE(vars)->level)) < x) {
            vars = sylvan_set_next(vars);
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
        aLow = node_low(a, na);
        aHigh = node_high(a, na);
    }

    if (nb && x == x_b) {
        bLow = node_low(b, nb);
        bHigh = node_high(b, nb);
    }

    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != x / granularity;

    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(a, CACHE_RRELPRODS);
        template_cache_node.params[1] = b;
        template_cache_node.params[2] = vars;
        template_cache_node.result = sylvan_invalid;

        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            BDD result = template_cache_node.result;
            SV_CNT_CACHE(C_cache_reuse);
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
            TOMARK_PUSH(low)
        } else {
            SPAWN(sylvan_relprods_reversed, aHigh, bHigh, vars, x);
            low = CALL(sylvan_relprods_reversed, aLow, bLow, vars, x);
            TOMARK_PUSH(low)
            high = SYNC(sylvan_relprods_reversed);
            TOMARK_PUSH(high)
        }
        result = sylvan_makenode(x, low, high);
    }

    TOMARK_EXIT

    if (cachenow) {
        template_cache_node.result = result;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT_CACHE(C_cache_exists);
            // No need to ref
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT_CACHE(C_cache_new);
        }
    }

    return result;
}

/**
 * Count number of nodes for each level
 */

// TODO: use AVL

void sylvan_nodecount_levels_do_1(BDD bdd, uint32_t *variables)
{
    if (!sylvan_isnode(bdd)) return;

    bddnode_t na = GETNODE(bdd);
    if (na->data & 1) return;
    variables[na->level]++;
    na->data |= 1; // mark
    sylvan_nodecount_levels_do_1(na->low, variables);
    sylvan_nodecount_levels_do_1(na->high, variables);
}

void sylvan_nodecount_levels_do_2(BDD bdd)
{
    if (!sylvan_isnode(bdd)) return;

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
    if (sylvan_isconst(a)) return 0;
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
    if (sylvan_isconst(a)) return;
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
    long double res1 = SYNC(sylvan_pathcount);
    return res1 + SYNC(sylvan_pathcount);
}

/**
 * CALCULATE NUMBER OF VAR ASSIGNMENTS THAT YIELD TRUE
 */

TASK_IMPL_3(sylvan_satcount_double_t, sylvan_satcount_cached, BDD, bdd, BDDSET, variables, BDDVAR, prev_level)
{
    /* Trivial cases */
    if (bdd == sylvan_false) return 0.0;
    if (bdd == sylvan_true) return powl(2.0L, sylvan_set_count(variables));

    /* Perhaps execute garbage collection */
    sylvan_gc_test();

    /* Count variables before var(bdd) */
    size_t skipped = 0;
    BDDVAR var = sylvan_var(bdd);
    bddnode_t set_node = GETNODE(variables);
    while (var != set_node->level) {
        skipped++;
        variables = node_low(variables, set_node);
        // if this assertion fails, then variables is not the support of <bdd>
        assert(!sylvan_set_isempty(variables));
        set_node = GETNODE(variables);
    }

    /* Count operation */
    // SV_CNT_OP(C_satcount);
    
    union {
        sylvan_satcount_double_t d;
        size_t s;
    } hack;

    /* Consult cache */
    int cachenow = granularity < 2 || prev_level == 0 ? 1 : prev_level / granularity != var / granularity;
    struct bddcache template_cache_node;
    if (cachenow) {
        // Check cache
        memset(&template_cache_node, 0, sizeof(struct bddcache));
        template_cache_node.params[0] = BDD_SETDATA(bdd, CACHE_SATCOUNT);
        template_cache_node.params[1] = variables;
        if (llci_get_tag(_bdd.cache, &template_cache_node)) {
            SV_CNT_CACHE(C_cache_reuse);
            hack.s = template_cache_node.result;
            return hack.d * powl(2.0L, skipped);
        }
    }

    SPAWN(sylvan_satcount_cached, sylvan_high(bdd), node_low(variables, set_node), var);
    sylvan_satcount_double_t low = CALL(sylvan_satcount_cached, sylvan_low(bdd), node_low(variables, set_node), var);
    sylvan_satcount_double_t result = (low + SYNC(sylvan_satcount_cached));

    if (cachenow) {
        hack.d = result;
        template_cache_node.result = hack.s;
        int cache_res = llci_put_tag(_bdd.cache, &template_cache_node);
        if (cache_res == 0) {
            // It existed!
            SV_CNT_CACHE(C_cache_exists);
        } else if (cache_res == 1) {
            // Created new!
            SV_CNT_CACHE(C_cache_new);
        }
    }

    return result * powl(2.0L, skipped);
}

TASK_IMPL_2(long double, sylvan_satcount, BDD, bdd, BDD, variables)
{
    /* Trivial cases */
    if (bdd == sylvan_false) return 0.0;
    if (bdd == sylvan_true) return powl(2.0L, sylvan_set_count(variables));

    /* Perhaps execute garbage collection */
    sylvan_gc_test();

    /* Count variables before var(bdd) */
    size_t skipped = 0;
    BDDVAR var = sylvan_var(bdd);
    bddnode_t set_node = GETNODE(variables);
    while (var != set_node->level) {
        skipped++;
        variables = node_low(variables, set_node);
        // if this assertion fails, then variables is not the support of <bdd>
        assert(!sylvan_set_isempty(variables));
        set_node = GETNODE(variables);
    }

    /* Count operation */
    // SV_CNT_OP(C_satcount);

    SPAWN(sylvan_satcount, sylvan_high(bdd), node_low(variables, set_node));
    long double low = CALL(sylvan_satcount, sylvan_low(bdd), node_low(variables, set_node));
    return (low + SYNC(sylvan_satcount)) * powl(2.0L, skipped);
}

static void
gnomesort_bddvars(BDDVAR* arr, size_t size)
{
    size_t i=0;
    while (i<size) {
        if (i == 0 || arr[i-1] <= arr[i]) i++;
        else { BDDVAR tmp = arr[i]; arr[i] = arr[i-1]; arr[--i] = tmp; }
    }
}

int
sylvan_sat_one(BDD bdd, BDDVAR *vars, size_t cnt, char* str)
{
    if (bdd == sylvan_false) return 0;
    if (str == NULL) return 0;

    BDDVAR *sorted_vars = (BDDVAR*)alloca(sizeof(BDDVAR)*cnt);
    memcpy(sorted_vars, vars, sizeof(BDDVAR)*cnt);
    gnomesort_bddvars(sorted_vars, cnt);

    size_t i = 0;
    for (i=0; i<cnt; i++) {
        BDDVAR var = sorted_vars[i];

        size_t idx=0;
        for (idx=0; vars[idx]!=var; idx++) {}

        if (bdd != sylvan_true) {
            bddnode_t node = GETNODE(bdd);
            if (node->level == var) {
                BDD lowedge = node_low(bdd, node);
                BDD highedge = node_high(bdd, node);
                if (highedge == sylvan_false) {
                    // take low edge
                    bdd = lowedge;
                    str[idx++] = 0;
                } else if (lowedge == sylvan_false) {
                    // take high edge
                    bdd = highedge;
                    str[idx++] = 1;
                } else {
                    // take random edge
                    if (rand() & 0x2000) {
                        bdd = lowedge;
                        str[idx++] = 0;
                    } else {
                        bdd = highedge;
                        str[idx++] = 1;
                    }
                }
                continue;
            }
        }
        str[idx++] = 2;
    }

    return 1;
}

BDD
sylvan_sat_one_bdd(BDD bdd)
{
    if (bdd == sylvan_false) return sylvan_false;
    if (bdd == sylvan_true) return sylvan_true;

    bddnode_t node = GETNODE(bdd);
    BDD low = node_low(bdd, node);
    BDD high = node_high(bdd, node);

    LOCALIZE_THREAD_LOCAL(gc_key, gc_tomark_t);

    struct gc_tomark m;
    m.prev = gc_key;
    m.bdd = sylvan_invalid;

    SET_THREAD_LOCAL(gc_key, &m);

    BDD result;
    if (low == sylvan_false) {
        m.bdd = sylvan_sat_one_bdd(high);
        result = sylvan_makenode(node->level, sylvan_false, m.bdd);
    } else if (high == sylvan_false) {
        m.bdd = sylvan_sat_one_bdd(low);
        result = sylvan_makenode(node->level, m.bdd, sylvan_false);
    } else {
        if (rand() & 0x2000) {
            m.bdd = sylvan_sat_one_bdd(low);
            result = sylvan_makenode(node->level, m.bdd, sylvan_false);
        } else {
            m.bdd = sylvan_sat_one_bdd(high);
            result = sylvan_makenode(node->level, sylvan_false, m.bdd);
        }
    }

    SET_THREAD_LOCAL(gc_key, m.prev);
    return result;
}

BDD
sylvan_cube(BDDVAR* vars, size_t cnt, char* cube)
{
    assert(cube != NULL);

    BDDVAR *sorted_vars = (BDDVAR*)alloca(sizeof(BDDVAR)*cnt);
    memcpy(sorted_vars, vars, sizeof(BDDVAR)*cnt);
    gnomesort_bddvars(sorted_vars, cnt);

    LOCALIZE_THREAD_LOCAL(gc_key, gc_tomark_t);

    struct gc_tomark m;
    m.prev = gc_key;
    m.bdd = sylvan_true; 

    SET_THREAD_LOCAL(gc_key, &m);

    size_t i;
    for (i=0; i<cnt; i++) {
        BDDVAR var = sorted_vars[cnt-i-1];
        size_t idx=0;
        for (idx=0; vars[idx]!=var; idx++) {}
        if (cube[idx] == 0) {
            m.bdd = sylvan_makenode(var, m.bdd, sylvan_false);
        } else if (cube[idx] == 1) {
            m.bdd = sylvan_makenode(var, sylvan_false, m.bdd);
        } else {
            m.bdd = sylvan_makenode(var, m.bdd, m.bdd); // actually: this skips
        }
    }

    SET_THREAD_LOCAL(gc_key, m.prev);
    return m.bdd;
}

/**
 * IMPLEMENTATION OF BDDSET
 */

int
sylvan_set_in(BDDSET set, BDDVAR level)
{
    while (!sylvan_set_isempty(set)) {
        bddnode_t n = GETNODE(set);
        if (n->level == level) return 1;
        if (n->level > level) return 0; // BDDs are ordered
        set = node_low(set, n);
    }

    return 0;
}

size_t
sylvan_set_count(BDDSET set)
{
    size_t result = 0;
    for (;!sylvan_set_isempty(set);set = sylvan_set_next(set)) result++;
    return result;
}

void
sylvan_set_toarray(BDDSET set, BDDVAR *arr)
{
    size_t i = 0;
    while (!sylvan_set_isempty(set)) {
        bddnode_t n = GETNODE(set);
        arr[i++] = n->level;
        set = node_low(set, n);
    }
}

BDDSET
sylvan_set_fromarray(BDDVAR* arr, size_t length)
{
    if (length == 0) return sylvan_set_empty();
    TOMARK_INIT
    BDDSET sub = sylvan_set_fromarray(arr+1, length-1);
    TOMARK_PUSH(sub)
    BDDSET result = sylvan_set_add(sub, *arr);
    TOMARK_EXIT
    return result;
}

/**
 * Determine the support of a BDD (all variables used in the BDD)
 */

TASK_IMPL_1(BDD, sylvan_support, BDD, bdd)
{
    if (!sylvan_isnode(bdd)) return sylvan_false;

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
        if (!sylvan_isconst(node->low)) sylvan_mark_rec(GETNODE(node->low), mark);
        if (!sylvan_isconst(node->high)) sylvan_mark_rec(GETNODE(node->high), mark);
    }
}

static __attribute__((unused)) void
sylvan_unmark_rec(bddnode_t node, unsigned int mark)
{
    if (sylvan_unmark(node, mark)) {
        if (!sylvan_isconst(node->low)) sylvan_unmark_rec(GETNODE(node->low), mark);
        if (!sylvan_isconst(node->high)) sylvan_unmark_rec(GETNODE(node->high), mark);
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
    if (!sylvan_isnode(bdd)) return;

    bdd = BDD_STRIPMARK(bdd);
    bddnode_t n = GETNODE(bdd);
    if (!sylvan_mark(n, 1)) return;

    sylvan_dothelper_register(levels, bdd);

    fprintf(out, "%" PRIu64 " [label=\"%d\"];\n", bdd, n->level);

    sylvan_fprintdot_rec(out, n->low, levels);
    sylvan_fprintdot_rec(out, n->high, levels);

    fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=dashed];\n", bdd, (BDD)n->low);
    fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n", bdd, (BDD)n->high, n->comp ? "dot" : "none");
}

void
sylvan_fprintdot(FILE *out, BDD bdd)
{
    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    fprintf(out, "0 [shape=box, label=\"0\", style=filled, shape=box, height=0.3, width=0.3];\n");
    fprintf(out, "root [style=invis];\n");
    fprintf(out, "root -> %" PRIu64 " [style=solid dir=both arrowtail=%s];\n", BDD_STRIPMARK(bdd), BDD_HASMARK(bdd) ? "dot" : "none");

    avl_node_t *levels = NULL;
    sylvan_fprintdot_rec(out, bdd, &levels);

    if (levels != NULL) {
        size_t levels_count = avl_count(levels);
        struct level_to_nodeset *arr = level_to_nodeset_toarray(levels);
        size_t i;
        for (i=0;i<levels_count;i++) {
            fprintf(out, "{ rank=same; ");
            size_t node_count = avl_count(arr[i].set);
            size_t j;
            BDD *arr_j = nodeset_toarray(arr[i].set);
            for (j=0;j<node_count;j++) {
                fprintf(out, "%" PRIu64 "; ", arr_j[j]);
            }
            fprintf(out, "}\n");
        }
        level_to_nodeset_free(&levels);
    }

    fprintf(out, "}\n");
    if (!sylvan_isconst(bdd)) sylvan_unmark_rec(GETNODE(bdd), 1);
}

void
sylvan_printdot(BDD bdd)
{
    sylvan_fprintdot(stdout, bdd);
}

static void __attribute__((noinline))
sylvan_dothelper_nocomp_register(avl_node_t **set, BDD bdd)
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
    nodeset_insert(&ss->set, &bdd);
}

static void
sylvan_fprintdot_nocomp_rec(FILE *out, BDD bdd, avl_node_t **levels)
{
    if (!sylvan_isnode(bdd)) return;

    bddnode_t n = GETNODE(bdd);
    if (!sylvan_mark(n, 1)) return;

    sylvan_dothelper_nocomp_register(levels, bdd);

    fprintf(out, "%" PRIu64 " [label=\"%d\"];\n", bdd, n->level);

    sylvan_fprintdot_nocomp_rec(out, n->low, levels);
    sylvan_fprintdot_nocomp_rec(out, n->high, levels);

    fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=dashed];\n", bdd, (BDD)n->low);
    fprintf(out, "%" PRIu64 " -> %" PRIu64 " [style=solid];\n", bdd, (BDD)n->high);
}

void
sylvan_fprintdot_nocomp(FILE *out, BDD bdd)
{
    // Bye comp
    bdd = sylvan_bdd_to_nocomp(bdd);

    fprintf(out, "digraph \"DD\" {\n");
    fprintf(out, "graph [dpi = 300];\n");
    fprintf(out, "center = true;\n");
    fprintf(out, "edge [dir = forward];\n");
    if (bdd != sylvan_true_nc) fprintf(out, "0 [shape=box, label=\"0\", style=filled, shape=box, height=0.3, width=0.3];\n");
    if (bdd != sylvan_false) fprintf(out, "%" PRIu64 " [shape=box, label=\"1\", style=filled, shape=box, height=0.3, width=0.3];\n", sylvan_true_nc);
    fprintf(out, "root [style=invis];\n");
    fprintf(out, "root -> %" PRIu64 " [style=solid];\n", bdd);

    avl_node_t *levels = NULL;
    sylvan_fprintdot_nocomp_rec(out, bdd, &levels);

    if (levels != NULL) {
        size_t levels_count = avl_count(levels);
        struct level_to_nodeset *arr = level_to_nodeset_toarray(levels);
        size_t i;
        for (i=0;i<levels_count;i++) {
            fprintf(out, "{ rank=same; ");
            size_t node_count = avl_count(arr[i].set);
            size_t j;
            BDD *arr_j = nodeset_toarray(arr[i].set);
            for (j=0;j<node_count;j++) {
                fprintf(out, "%" PRIu64 "; ", arr_j[j]);
            }
            fprintf(out, "}\n");
        }
        level_to_nodeset_free(&levels);
    }

    if (!sylvan_isconst(bdd)) fprintf(out, "{ rank=same; 0; %" PRIu64 "; }\n", sylvan_true_nc);

    fprintf(out, "}\n");
    if (!sylvan_isconst(bdd)) sylvan_unmark_rec(GETNODE(bdd), 1);
}

void
sylvan_printdot_nocomp(BDD bdd)
{
    sylvan_fprintdot_nocomp(stdout, bdd);
}

/**
 * SERIALIZATION
 */

struct sylvan_ser {
    BDD bdd;
    size_t assigned;
};

// Define a AVL tree type with prefix 'sylvan_ser' holding
// nodes of struct sylvan_ser with the following compare() function...
AVL(sylvan_ser, struct sylvan_ser)
{
    return left->bdd - right->bdd;
}

// Define a AVL tree type with prefix 'sylvan_ser_reversed' holding 
// nodes of struct sylvan_ser with the following compare() function...
AVL(sylvan_ser_reversed, struct sylvan_ser)
{
    return left->assigned - right->assigned;
}

// Initially, both sets are empty
static avl_node_t *sylvan_ser_set = NULL;
static avl_node_t *sylvan_ser_reversed_set = NULL;

// Start counting (assigning numbers to BDDs) at 1
static size_t sylvan_ser_counter = 1;
static size_t sylvan_ser_done = 0;

// Given a BDD, assign unique numbers to all nodes
static size_t
sylvan_serialize_assign_rec(BDD bdd)
{
    if (sylvan_isnode(bdd)) {
        bddnode_t n = GETNODE(bdd);

        struct sylvan_ser s, *ss;
        s.bdd = BDD_STRIPMARK(bdd);
        ss = sylvan_ser_search(sylvan_ser_set, &s);
        if (ss == NULL) {
            // assign dummy value
            s.assigned = 0;
            ss = sylvan_ser_put(&sylvan_ser_set, &s, NULL);

            // first assign recursively
            sylvan_serialize_assign_rec(n->low);
            sylvan_serialize_assign_rec(n->high);

            // assign real value
            ss->assigned = sylvan_ser_counter++;

            // put a copy in the reversed table
            sylvan_ser_reversed_insert(&sylvan_ser_reversed_set, ss);
        }

        return ss->assigned;
    }

    return BDD_STRIPMARK(bdd);
}

size_t
sylvan_serialize_add(BDD bdd)
{
    return BDD_TRANSFERMARK(bdd, sylvan_serialize_assign_rec(bdd));
}

void
sylvan_serialize_reset()
{
    sylvan_ser_free(&sylvan_ser_set);
    sylvan_ser_free(&sylvan_ser_reversed_set);
    sylvan_ser_counter = 1;
    sylvan_ser_done = 0;
}

size_t
sylvan_serialize_get(BDD bdd)
{
    if (!sylvan_isnode(bdd)) return bdd;
    struct sylvan_ser s, *ss;
    s.bdd = BDD_STRIPMARK(bdd);
    ss = sylvan_ser_search(sylvan_ser_set, &s);
    assert(ss != NULL);
    return BDD_TRANSFERMARK(bdd, ss->assigned);
}

BDD
sylvan_serialize_get_reversed(size_t value)
{
    if (!sylvan_isnode(value)) return value;
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

    /* Skip already written entries */
    size_t index = 0;
    while (index < sylvan_ser_done && (s=sylvan_ser_reversed_iter_next(it))) index++;

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
    assert(count >= sylvan_ser_done);
    assert(count == sylvan_ser_counter-1);
    count -= sylvan_ser_done;
    fwrite(&count, sizeof(size_t), 1, out);

    struct sylvan_ser *s;
    avl_iter_t *it = sylvan_ser_reversed_iter(sylvan_ser_reversed_set);

    /* Skip already written entries */
    size_t index = 0;
    while (index < sylvan_ser_done && (s=sylvan_ser_reversed_iter_next(it))) {
        index++;
        assert(s->assigned == index);
    }

    while ((s=sylvan_ser_reversed_iter_next(it))) {
        index++;
        assert(s->assigned == index);

        bddnode_t n = GETNODE(s->bdd);

        struct bddnode node;
        node.high = sylvan_serialize_get(n->high);
        node.low = sylvan_serialize_get(n->low);
        node.level = n->level;
        node.data = 0;
        node.comp = n->comp;

        assert(node.high < index);
        assert(node.low < index);

        fwrite(&node, sizeof(struct bddnode), 1, out);
    }

    sylvan_ser_done = sylvan_ser_counter-1;
    sylvan_ser_reversed_iter_free(it);
}

void
sylvan_serialize_fromfile(FILE *in)
{
    size_t count, i;
    assert(fread(&count, sizeof(size_t), 1, in) == 1);

    for (i=1; i<=count; i++) {
        struct bddnode node;
        assert(fread(&node, sizeof(struct bddnode), 1, in) == 1);

        assert(node.low <= sylvan_ser_done);
        assert(node.high <= sylvan_ser_done);

        BDD low = sylvan_serialize_get_reversed(node.low);
        BDD high = sylvan_serialize_get_reversed(node.high);
        if (node.comp) high |= sylvan_complement;

        struct sylvan_ser s;
        s.bdd = sylvan_makenode(node.level, low, high);
        s.assigned = ++sylvan_ser_done; // starts at 0 but we want 1-based...

        sylvan_ser_insert(&sylvan_ser_set, &s);
        sylvan_ser_reversed_insert(&sylvan_ser_reversed_set, &s);
    }
}
