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

/**
 * Exported BDD constants
 */

const BDD sylvan_true = 1;
const BDD sylvan_false = 0;
const BDD sylvan_invalid = 0x7fffffff; // uint32_t

const BDD quant_exists = -1;     // 0xffffffff
const BDD quant_forall = -2;     // 0xfffffffe
const BDD quant_unique = -3;     // 0xfffffffd
const BDD sylvan_last = -4;       // 0xfffffffc

/**
 * Internal BDD constants
 */
const BDD bddmark = 0x80000000;
const BDD bddhandled = 0xffffffff; // being handled by operation
const BDD bddinternal = 0x40000000; // ITE* node marker (on "a")

const uint8_t bddcommand_quit = 1;
const uint8_t bddcommand_ite_down = 2;
const uint8_t bddcommand_ite = 3;

/**
 * "volatile" macros
 */
#define atomicsylvan_read(s) atomic32_read(s)
#define atomicsylvan_write(a, b) atomic32_write(a, b)

/**
 * Mark handling macros
 */

#define BDD_HASMARK(s) (s&bddmark)
#define BDD_TOGGLEMARK(s) (s<2?1-s:s^bddmark)
#define BDD_STRIPMARK(s) (s&~bddmark)
#define BDD_NORMALIZE(s) (s<2?1:s&~bddmark)
//#define BDD_TRANSFERMARK(from, to) (to<2 ? (from & bddmark ? 1-to : to) : (to ^ (from & bddmark)))
#define BDD_TRANSFERMARK(from, to) (from & bddmark ? BDD_TOGGLEMARK(to) : to)

// DEBUG MACROS
#if 1
#define REPORT_RESULT(a,b,c) printf("Set result of " #a " %d to %s%d\n", b, c&bddmark?"~":"", c&~bddmark);
#else
#define REPORT_RESULT(a,b,c)
#endif

struct bddnode
{
    BDDLEVEL level;
    BDD low;
    BDD high;
};

typedef struct bddnode* bddnode_t;

static struct {
    /// Main UNIQUE table
    llset_t data;

    /// ITE
    llset_t cache;
    struct llvector leaves;

    /// ITE*
    struct llvector leaves2;
    BDD *replaceBy;
    BDD replaceLast;

    /// THREAD information
    llsched_t sched;
    int threadCount;
    pthread_t *threads;
    uint8_t *flags;
} _bdd;

struct bddcache {
    /// Following variables are the KEY for the unique table
    BDD a; // if
    BDD b; // then (set to sylvan_invalid for replace nodes)
    BDD c; // else (set to sylvan_invalid for replace nodes)
    /// Stuff
    BDD root; // new if
    BDD high; // new then
    BDD low; // new else
    BDD cache_low; // index to bddcache else
    BDD cache_high; // index to bddcache then
    struct llvector parents; // parents in the current operation
    /// The cache of calculated BDD values
    BDD result; // sylvan_invalid after creation, bddhandled when added to the job queue
};

typedef struct bddcache* bddcache_t;

/**
 * Macro's to convert BDD indices to nodes and vice versa
 */
#define GETNODE(bdd)   ((bddnode_t)llset_index_to_ptr(_bdd.data, BDD_STRIPMARK(bdd)))
#define GETCACHE(bdd)   ((bddcache_t)llset_index_to_ptr(_bdd.cache, BDD_STRIPMARK(bdd)))
#define GETCACHEBDD(node)   ((BDD)llset_ptr_to_index(_bdd.cache, node))

static bddcache_t *template_apply_node;


/**
 * Internal methods
 */
static BDD sylvan_makeite(int thread, BDD a, BDD b, BDD c, int *created, int *cached);
static inline void sylvan_move_parents(bddcache_t from, BDD to_cs);
static inline int sylvan_process_ite_ex(int thread, bddcache_t node, int queue_new_nodes);
static void sylvan_execute_ite(const int thread);
static void sylvan_execute_ite_down(const int thread);
static inline void sylvan_handle_ite_parents(const int thread, bddcache_t node, BDD node_c);

static void *sylvan_thread(void *data);
void sylvan_wait_for_threads();

void sylvan_print_cache(BDD root);
static inline void sylvan_printbdd(const char *s, BDD bdd);

/**
 * Hash() and Equals() for the ITE cache
 */
uint32_t sylvan_cache_hash(const void *data_, int len __attribute__((unused)), uint32_t hash) {
    return SuperFastHash(data_, sizeof(bddnode_t)*3, hash);
}

int sylvan_cache_equals(const void *a, const void *b, size_t length __attribute__((unused))) {
    return memcmp(a, b, sizeof(BDD)*3) == 0;
}

/**
 * IMPLEMENTATION
 */


void sylvan_init(int threads, size_t datasize, size_t cachesize) {
    if (datasize >= 30) {
        printf("BDD_init error: datasize must be < 30!");
        exit(1);
    }

    if (cachesize >= 30) {
        printf("BDD_init error: cachesize must be < 30!");
        exit(1);
    }

    size_t datal = sizeof(struct bddnode);
    size_t acl = sizeof(struct bddcache);

    if (datasize > 20) {
        printf("BDD_init\nData: %d times %d bytes = %d MB\n", 1<<datasize, datal, (1<<(datasize-20)) * datal);
        printf("Cache: %d times %d bytes = %d MB\n\n", 1<<cachesize, acl, (1<<(cachesize-20)) * acl);
    } else {
        printf("BDD_init\nData: %d times %d bytes = %d KB\n", 1<<datasize, datal, (1<<(datasize-10)) * datal);
        printf("Cache: %d times %d bytes = %d KB\n\n", 1<<cachesize, acl, (1<<(cachesize-10)) * acl);
    }

    _bdd.data = llset_create(sizeof(struct bddnode), datasize, NULL, NULL);
    _bdd.cache = llset_create(sizeof(struct bddcache), cachesize, sylvan_cache_hash, sylvan_cache_equals);

    _bdd.sched = llsched_create(threads, sizeof(BDD));
    _bdd.threadCount = threads;
    _bdd.threads = rt_align(CACHE_LINE_SIZE, sizeof(pthread_t) * threads);
    _bdd.flags = rt_align(CACHE_LINE_SIZE, sizeof(uint8_t) * threads);

    memset(_bdd.flags, 0, sizeof(uint8_t) * threads);

    llvector_init(&_bdd.leaves, sizeof(bddcache_t));
    llvector_init(&_bdd.leaves2, sizeof(bddcache_t));

    _bdd.replaceBy = 0;
    _bdd.replaceLast = 0;

    template_apply_node = malloc(sizeof(bddcache_t) * threads);
    for (int i=0;i<threads;i++) {
        template_apply_node[i] = malloc(sizeof(struct bddcache));
        memset(template_apply_node[i], 0, sizeof(struct bddcache));
        template_apply_node[i]->result = sylvan_invalid;
        llvector_init(&template_apply_node[i]->parents, sizeof(bddcache_t)); // this does not call malloc!
    }

    /// Start threads
    for (int i=1;i<threads;i++) {
        pthread_create(&_bdd.threads[i], NULL, sylvan_thread, (void*)i);
    }
}

void sylvan_quit()
{
    for (int i=1; i<_bdd.threadCount; i++) {
        atomic8_write(&_bdd.flags[i], bddcommand_quit);
    }

    for (int i=1; i<_bdd.threadCount; i++) {
        void *tmp;
        pthread_join(_bdd.threads[i], &tmp);
    }

    for (int i=0;i<_bdd.threadCount;i++) free(template_apply_node[i]);
    free(template_apply_node);

    llvector_deinit(&_bdd.leaves);
    free(_bdd.flags);
    free(_bdd.threads);
    llsched_free(_bdd.sched);
    llset_free(_bdd.cache);
    llset_free(_bdd.data);
}

inline BDD sylvan_makenode(BDD level, BDD low, BDD high) {
    BDD result;

    struct bddnode n;
    n.level = level;

    // Normalization
    // low will have no mark and will not be "true"

    if (low == sylvan_true) {
        // ITE(a,b,true) == not ITE(a,not b,false)
        n.low = sylvan_false;
        n.high = BDD_TOGGLEMARK(high);
        llset_get_or_create(_bdd.data, &n, NULL, &result);
        return result | bddmark;
    } else if (BDD_HASMARK(low)) {
        // ITE(a,b,not c) == not ITE(a,not b, c)
        n.low = BDD_STRIPMARK(low);
        n.high = BDD_TOGGLEMARK(high);
        llset_get_or_create(_bdd.data, &n, NULL, &result);
        return result | bddmark;
    } else {
        n.low = low;
        n.high = high;
        llset_get_or_create(_bdd.data, &n, NULL, &result);
        return result;
    }
}

inline BDD sylvan_ithvar(BDD level) {
    return sylvan_makenode(level, sylvan_false, sylvan_true);
}

inline BDD sylvan_nithvar(BDD level) {
    return sylvan_makenode(level, sylvan_true, sylvan_false);
}

inline BDD sylvan_var(BDD bdd) {
    return GETNODE(bdd)->level;
}

inline BDD sylvan_low(BDD bdd) {
    if (bdd<2) return bdd;
    return BDD_TRANSFERMARK(bdd, GETNODE(bdd)->low);
}

inline BDD sylvan_high(BDD bdd) {
    if (bdd<2) return bdd;
    return BDD_TRANSFERMARK(bdd, GETNODE(bdd)->high);
}

inline BDD sylvan_not(BDD bdd) {
    return BDD_TOGGLEMARK(bdd);
}

BDD sylvan_apply(BDD a, BDD b, sylvan_operator op) {
    switch (op) {
    case operator_and:
        return sylvan_ite(a, b, sylvan_false);
    case operator_xor:
        return sylvan_ite(a, sylvan_not(b), b);
    case operator_or:
        return sylvan_ite(a, sylvan_true, b);
    case operator_nand:
        return sylvan_ite(a, sylvan_not(b), sylvan_true);
    case operator_nor:
        return sylvan_ite(a, sylvan_false, sylvan_not(b));
    case operator_imp:
        return sylvan_ite(a, b, sylvan_true);
    case operator_biimp:
        return sylvan_ite(a, b, sylvan_not(b));
    case operator_diff:
        return sylvan_ite(a, sylvan_not(b), sylvan_false);
    case operator_less:
        return sylvan_ite(a, sylvan_false, b);
    case operator_invimp:
        return sylvan_ite(a, sylvan_true, sylvan_not(b));
    default:
        assert(0);
    }
}

BDD sylvan_apply_ex(BDD a, BDD b, sylvan_operator op, const BDD* pairs, size_t n)
{
    switch (op) {
    case operator_and:
        return sylvan_ite_ex(a, b, sylvan_false, pairs, n);
    case operator_xor:
        return sylvan_ite_ex(a, sylvan_not(b), b, pairs, n);
    case operator_or:
        return sylvan_ite_ex(a, sylvan_true, b, pairs, n);
    case operator_nand:
        return sylvan_ite_ex(a, sylvan_not(b), sylvan_true, pairs, n);
    case operator_nor:
        return sylvan_ite_ex(a, sylvan_false, sylvan_not(b), pairs, n);
    case operator_imp:
        return sylvan_ite_ex(a, b, sylvan_true, pairs, n);
    case operator_biimp:
        return sylvan_ite_ex(a, b, sylvan_not(b), pairs, n);
    case operator_diff:
        return sylvan_ite_ex(a, sylvan_not(b), sylvan_false, pairs, n);
    case operator_less:
        return sylvan_ite_ex(a, sylvan_false, b, pairs, n);
    case operator_invimp:
        return sylvan_ite_ex(a, sylvan_true, sylvan_not(b), pairs, n);
    default:
        assert(0);
    }
}

static BDD sylvan_makeite(int thread, BDD a, BDD b, BDD c, int *created, int *cached)
{
    if (a & bddinternal) assert(0);
    if (b & bddinternal) assert(0);
    if (c & bddinternal) assert(0);
/*
    printf("MakeIte(%s%d,%s%d,%s%d) =", a&bddmark?"~":"",BDD_STRIPMARK(a),
                                        b&bddmark?"~":"",BDD_STRIPMARK(b),
                                        c&bddmark?"~":"",BDD_STRIPMARK(c));
*/     
    // TERMINAL CASE (attempt 1)

    // ITE(T,B,C) = B
    // ITE(F,B,C) = C
    if (a < 2) {
        BDD result = (a == sylvan_true ? b : c);
        if (created!=0) *created = 0;
        if (cached!=0) *cached = 1;
        //printf(" %d\n",result);
        return result;
    }

    // Normalization to standard triples

    // ITE(A,A,C) = ITE(A,T,C)
    // ITE(A,~A,C) = ITE(A,F,C)
    if (BDD_STRIPMARK(a) == BDD_STRIPMARK(b)) {
        // faster STRIPMARK means we don't detect constants
        if (a == b) b = sylvan_true;
        else b = sylvan_false;
    }

    // ITE(A,B,A) = ITE(A,B,T)
    // ITE(A,B,~A) = ITE(A,B,F)
    if (BDD_STRIPMARK(a) == BDD_STRIPMARK(c)) {
        // faster STRIPMARK means we don't detect constants
        if (a != c) c = sylvan_true;
        else c = sylvan_false;
    }

    if (b < 2 && BDD_STRIPMARK(c) < BDD_STRIPMARK(a)) {
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

    if (c < 2 && BDD_STRIPMARK(b) < BDD_STRIPMARK(a)) {
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

    // Now, if b == constant then a<c (a may be constant)
    //      if c == constant then a<b (a may be constant)
    //      if both constant then a will be constant

    if (BDD_NORMALIZE(b) == BDD_NORMALIZE(c)) {
        // NORMALIZE: like STRIPMARK but also turns "false" into "true"
        if (b == c) {
            // trivially equal to b (=c)
            BDD result = b;
            if (created!=0) *created = 0;
            if (cached!=0) *cached = 1;
            return result;
        } else {
            // Trivial case: they are constants
            if (b<2) {
                assert(a<2);
                // if they are different, then either
                // true then true else false === true
                // true then false else true === false
                // false then true else false === false
                // false then false else true === true
                BDD result = (a == sylvan_true?b:c);
                //if (a == b) result = sylvan_true;
                //else result = sylvan_false;

                if (created!=0) *created = 0;
                if (cached!=0) *cached = 1;
                return result;
            }

            // b and c not constants...
            // 1. if A then B else not-B
            // 2. if A then not-B else B
            if (BDD_STRIPMARK(a) > BDD_STRIPMARK(b)) {
                // a > b, exchange:
                // 1. if B then A else not-A
                // 2. if not-B then A else not-A
                b = a;
                a = BDD_TOGGLEMARK(c);
                c = BDD_TOGGLEMARK(b);
            }
        }
    }

    // TERMINAL CASE

    // ITE(T,B,C) = B
    // ITE(F,B,C) = C
    if (a < 2) {
        BDD result = (a == sylvan_true ? b : c);
        if (created!=0) *created = 0;
        if (cached!=0) *cached = 1;
        //printf(" %d\n",result);
        return result;
    }

    // ITE(~A,B,C) = ITE(A,C,B)
    if (BDD_HASMARK(a)) {
        a = BDD_STRIPMARK(a);
        BDD t = c;
        c = b;
        b = t;
    }

    BDD mark = 0;

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

    if (BDD_HASMARK(b) || b == sylvan_false) {
        //printf(" (%d) ",sylvan_var(a));
        // a then -b else c
        // (A and -B) or (-A and C)
        // (-A or -B) and (A or C)
        // -(A and B) and -(-A and -C)
        // - ( (A and B) or (-A and -C) )
        // not ( a then b else not-c )
        mark = bddmark;
        b = BDD_TOGGLEMARK(b);
        c = BDD_TOGGLEMARK(c);
        //printf(" not if %d then %d else %d\n", a,b,c);
    }

    template_apply_node[thread]->a = a;
    template_apply_node[thread]->b = b;
    template_apply_node[thread]->c = c;

    /**
     * "if" is not a constant and is not complemented
     * "then" is not complemented
     */

    int _created;
    BDD result;
    bddcache_t ptr = llset_get_or_create(_bdd.cache, template_apply_node[thread], &_created, &result);

    if (!_created && BDD_STRIPMARK(ptr->result) != sylvan_invalid) {
        if (created!=0) *created = 0;
        if (cached!=0) *cached = 1;
        return BDD_TRANSFERMARK(mark, ptr->result);
    }
  
    if (created!=0) *created = _created;
    if (cached!=0) *cached = 0;
    return BDD_TRANSFERMARK(mark, result);
}

/**
 * <to> takes over the parents of <from>
 * This means that the cache_low and cache_high values will be updated with to_cs
 * This method is ONLY for parents of ITE* nodes
 * ITE* nodes have no mark
 * Moves from ITE* to ITE node
 */
static inline void sylvan_move_parents(bddcache_t from, BDD to_c)
{
    BDD from_c = GETCACHEBDD(from);
    bddcache_t to = GETCACHE(to_c);

	printf("Moving parents from %d to %s%d\n", from_c,
			to_c&bddmark?"~":"",to_c&~bddmark);

    bddcache_t parent;
    while (llvector_pop(&from->parents, &parent)) {
        // Check that the parent is also ITE* node
        assert(parent->a & bddinternal);

        if (parent->cache_low == from_c) {
            parent->cache_low = to_c;
        }

        if (parent->cache_high == from_c) {
            parent->cache_high = to_c;
        }

        llvector_push(&to->parents, &parent);
    }

    // free empty vector
    llvector_deinit(&from->parents);
}

/**
 * Process a ITE* node that has low and high set
 * If it requires additional ITE calculation, the ITE node is added to leaves2
 * If the ITE* value is calculated, returns 1 (else 0)
 * Also returns 0 if it was a root node
 */
static inline int sylvan_process_ite_ex(int thread, bddcache_t node, int queue_new_nodes)
{
    BDD node_c = GETCACHEBDD(node);

    // Verify it is a ITE* node
    assert(node->a & bddinternal);

    if (node->root == sylvan_last) {
        // Root node
        node->result = node->low; // or node->high
        REPORT_RESULT(Root ITE*, node_c, node->result);
        return 0;
    }

    // Create ITE node
    int created, cached;
    BDD result;

    if (node->root == quant_forall) {
        // ForAll: ITE(low, high, F)
        result = sylvan_makeite(thread, node->low, node->high, sylvan_false, &created, &cached);
    } else if (node->root == quant_exists) {
        // Exists: ITE(low, T, high)
        result = sylvan_makeite(thread, node->low, sylvan_true, node->high, &created, &cached);
    } else if (node->root == quant_unique) {
        // Unique: ITE(low, sylvan_not(high), high)
        result = sylvan_makeite(thread, node->low, BDD_TOGGLEMARK(node->low), node->high, &created, &cached);
    } else if (node->low == node->high) {
        // Trivial: low == high
        result = node->low;
        created = 0;
        cached = 1;
    } else {
        result = sylvan_makeite(thread, node->root, node->high, node->low, &created, &cached);
    }

    int is_not_root_node = (llvector_count(&node->parents) > 0)?1:0;

    // if it is cached... store result and return
    if (cached) {
        node->result = result;
        REPORT_RESULT(Cached ITE*, node_c, node->result);

        // if it is not the root node return 1
        return is_not_root_node;
    }

    // it is not cached

    if (is_not_root_node == 0) {
        // it is the root node
        node->root = sylvan_last;
        node->cache_low = node->cache_high = result;
        llvector_push(&GETCACHE(result)->parents, &node);
    } else {
        // it is not the root node, replace and delete it
        // This function is only valid on ITE* nodes.
        // Move all parents from ITE* node to ITE result
        sylvan_move_parents(node, result);
        llset_delete(_bdd.cache, node_c);
    }

    // if it is created, add it to future leaves
    if (created) {
        if (queue_new_nodes==0) {
            llvector_push(&_bdd.leaves2, &result);
        } else {
            llsched_push(_bdd.sched, thread, &result);
        }
    } else {
        // check if the result has been calculated by now
        bddcache_t result_node = GETCACHE(result);
        __sync_synchronize();
        if (atomicsylvan_read(&result_node->result) != sylvan_invalid) {
            while (atomicsylvan_read(&result_node->result) == bddhandled) {}
            sylvan_handle_ite_parents(thread, result_node, BDD_STRIPMARK(result));
            return is_not_root_node;
        }
    }

    return 0;
}

/**
 * This method is called when <low> and <high> are set.
 * First, it will attempt to acquire the node for processing (CAS)
 * Then, it will calculate the result
 * - ITE nodes will be added to the queue
 * - ITE* nodes will be either replaced by ITE nodes, or will spawn ITE nodes, or
 *   will be added to the queue
 */
static inline void sylvan_calculate_result(int thread, bddcache_t node, BDD node_c)
{
    if (!__sync_bool_compare_and_swap(&node->result, sylvan_invalid, bddhandled)) {
        // Another thread has acquired the parent OR this is the root ITE* node
    	if (node->a & bddinternal && node->root == sylvan_last) {
			node->result = node->low;
    	}
        return;
    }

    if ((node->a & bddinternal) == 0) {
        // Normal ITE node
        if (node->low == node->high) {
            node->result = node->low;
        } else {
            node->result = sylvan_makenode(node->root, node->low, node->high);
        }

        REPORT_RESULT(ITE, node_c, node->result);

        // We calculated the result, add it to the queue
        llsched_push(_bdd.sched, thread, &node_c);
    } else {
		// ITE* node
		if (sylvan_process_ite_ex(thread, node, 1)) {
			// Returns 1 if result is calculated AND it's not a root node
			llsched_push(_bdd.sched, thread, &node_c);
		}
    }
}


static inline void sylvan_handle_ite_parents(const int thread, bddcache_t node, BDD node_c)
{
    // Multiple threads may be working here at once!
    BDD q = node->result;

    bddcache_t parent;
    while (llvector_pop(&node->parents, &parent)) {
        BDD parent_c = GETCACHEBDD(parent);

		if (BDD_STRIPMARK(parent->cache_low) == node_c) {
			parent->low = BDD_TRANSFERMARK(parent->cache_low, q);
			parent->cache_low = 0;
		}

		if (BDD_STRIPMARK(parent->cache_high) == node_c) {
			parent->high = BDD_TRANSFERMARK(parent->cache_high, q);
			parent->cache_high = 0;
		}

		// Memory barrier
		__sync_synchronize();

		// Use volatiles!
		if (atomicsylvan_read(&parent->low) != sylvan_invalid &&
			atomicsylvan_read(&parent->high) != sylvan_invalid) {
	        sylvan_calculate_result(thread, parent, parent_c);
		}

    }

    // free empty vector
    llvector_deinit(&node->parents);

    // if ITE* node, delete it
    if (node->a & bddinternal) llset_delete(_bdd.cache, node_c);
}

static inline void sylvan_prepare_ite(const int thread, bddcache_t node, BDD node_c)
{
    // We're in a fresh ITE node and we need to calculate the dependencies

    bddnode_t restrict a = node->a < 2 ? 0 : GETNODE(node->a);
    bddnode_t restrict b = node->b < 2 ? 0 : GETNODE(node->b);
    bddnode_t restrict c = node->c < 2 ? 0 : GETNODE(node->c);

    // Step 1: figure out what our level is

    BDD level = 0xffffffff;
    if (a && level > a->level) level = a->level;
    if (b && level > b->level) level = b->level;
    if (c && level > c->level) level = c->level;

    node->root = level;

    // Step 2: calculate {a,b,c}_low and {a,b,c}_high

    BDD aLow, aHigh, bLow, bHigh, cLow, cHigh;

    if (a && level == a->level) {
        aLow = BDD_TRANSFERMARK(node->a, a->low);
        aHigh = BDD_TRANSFERMARK(node->a, a->high);
    } else {
        aLow = node->a;
        aHigh = node->a;
    }

    if (b && level == b->level) {
        bLow = BDD_TRANSFERMARK(node->b, b->low);
        bHigh = BDD_TRANSFERMARK(node->b, b->high);
    } else {
        bLow = node->b;
        bHigh = node->b;
    }

    if (c && level == c->level) {
        cLow = BDD_TRANSFERMARK(node->c, c->low);
        cHigh = BDD_TRANSFERMARK(node->c, c->high);
    } else {
        cLow = node->c;
        cHigh = node->c;
    }

    // Step 3: create low and high ITE nodes

    int created, cached;

    // These are necessary!
    node->low = sylvan_invalid;
    node->high = sylvan_invalid;

    BDD low = sylvan_makeite(thread, aLow, bLow, cLow, &created, &cached);
    if (cached) {
        node->low = low;
        node->cache_low = 0;
    } else {
    	bddcache_t low_node = GETCACHE(low);

        node->low = sylvan_invalid;
        node->cache_low = low;
        llvector_push(&low_node->parents, &node);

        if (created) llsched_push(_bdd.sched, thread, &low);

        else {
            // Occasionally, low just got set and we missed it!
        	__sync_synchronize();

        	// The problem here is that it may have missed that we added the parent...

			if (atomicsylvan_read(&low_node->result) != sylvan_invalid) {
				while (atomicsylvan_read(&low_node->result) == bddhandled) {}
				sylvan_handle_ite_parents(thread, low_node, BDD_STRIPMARK(low));
			}
		}
    }

    BDD high = sylvan_makeite(thread, aHigh, bHigh, cHigh, &created, &cached);

    if (cached) {
        node->high = high;
        node->cache_high = 0;
    } else {
    	bddcache_t high_node = GETCACHE(high);

        node->high = sylvan_invalid;
        node->cache_high = high;
        llvector_push(&high_node->parents, &node);

        if (created) llsched_push(_bdd.sched, thread, &high);
        else {
        	__sync_synchronize();

        	// The problem here is that it may have missed that we added the parent...

        	if (atomicsylvan_read(&high_node->result) != sylvan_invalid) {
				while (atomicsylvan_read(&high_node->result) == bddhandled) {}
				sylvan_handle_ite_parents(thread, high_node, BDD_STRIPMARK(high));
			}
        }
    }

    //__sync_synchronize();

    // Use volatiles!
    if (atomicsylvan_read(&node->low) != sylvan_invalid &&
        atomicsylvan_read(&node->high) != sylvan_invalid) {
    	sylvan_calculate_result(thread, node, node_c);
    }
}

static void sylvan_execute_ite(const int thread) {
    BDD node_c;
    bddcache_t node;
    while (llsched_pop(_bdd.sched, thread, &node_c) == 1) {
        node_c = BDD_STRIPMARK(node_c);
        node = GETCACHE(node_c);

        // Results are calculated in sylvan_prepare_ite 
        // and sylvan_handle_ite_parents

        if (node->result != sylvan_invalid) {
            sylvan_handle_ite_parents(thread, node, node_c);
        } else {
            sylvan_prepare_ite(thread, node, node_c);
        }
    }
}

BDD sylvan_ite(BDD a, BDD b, BDD c) {
    assert(a != sylvan_invalid);
    assert(b != sylvan_invalid);
    assert(c != sylvan_invalid);

    // Wait until all threads are ready and waiting
    sylvan_wait_for_threads();

    int cached;
    BDD ptr = sylvan_makeite(0, a, b, c, NULL, &cached);

    // Check if it is cached
    if (cached) return ptr;

    // Start other threads
    for (int i=1; i<_bdd.threadCount; i++) {
        atomic8_write(&_bdd.flags[i], bddcommand_ite);
    }

    // Push root node to scheduler
    llsched_push(_bdd.sched, 0, &ptr);

    // Start this thread
    sylvan_execute_ite(0);

    BDD result = GETCACHE(ptr)->result;
    if (result == sylvan_invalid ) {
        sylvan_print_cache(ptr);
    }

    assert(result != sylvan_invalid);
    assert(result != bddhandled);

    return BDD_TRANSFERMARK(ptr, result);
}

static void sylvan_execute_ite_down(const int thread) {
    // Phase 1: create calculation tree (ITE* nodes)
    BDD node_c;
    while (llsched_pop(_bdd.sched, thread, &node_c) == 1) {
        assert((node_c & bddmark) == 0);

        //node_c = BDD_STRIPMARK(node_c);
        bddcache_t node = GETCACHE(node_c);

        // Verify that this is an ITE* node
        assert(node->a & bddinternal);

        BDD na = (node->a & ~bddinternal);

        // Determine nodes a,b,c
        bddnode_t restrict a = na < 2 ? 0 : GETNODE(na);
        bddnode_t restrict b = node->b < 2 ? 0 : GETNODE(node->b);
        bddnode_t restrict c = node->c < 2 ? 0 : GETNODE(node->c);

        // Calculate level
        BDD level = 0xffffffff;
        if (a && level>a->level) level = a->level;
        if (b && level>b->level) level = b->level;
        if (c && level>c->level) level = c->level;

        // Set root level to replacement
        node->root = _bdd.replaceBy[level];

        // Determine {a,b,c}-low and {a,b,c}-high
        BDD aLow, aHigh, bLow, bHigh, cLow, cHigh;

        if (a && level == a->level) {
            aLow = BDD_TRANSFERMARK(na, a->low);
            aHigh = BDD_TRANSFERMARK(na, a->high);
        } else {
            aLow = na;
            aHigh = na;
        }

        if (b && level == b->level) {
            bLow = BDD_TRANSFERMARK(node->b, b->low);
            bHigh = BDD_TRANSFERMARK(node->b, b->high);
        } else {
            bLow = node->b;
            bHigh = node->b;
        }

        if (c && level == c->level) {
            cLow = BDD_TRANSFERMARK(node->c, c->low);
            cHigh = BDD_TRANSFERMARK(node->c, c->high);
        } else {
            cLow = node->c;
            cHigh = node->c;
        }

        int n = 0;

        // Determine if we need child ITE* nodes
        if (level < _bdd.replaceLast) {
            int created;
            uint32_t idx;
            bddcache_t ptr;

            template_apply_node[thread]->a = aLow | bddinternal;
            template_apply_node[thread]->b = bLow;
            template_apply_node[thread]->c = cLow;

            ptr = llset_get_or_create(_bdd.cache, template_apply_node[thread], &created, &idx);
            if (created) {
                // Add new ITE* nodes to queue
                llsched_push(_bdd.sched, thread, &idx);
            }

            node->cache_low = idx;
            node->low = sylvan_invalid;
            llvector_push(&ptr->parents, &node);

            template_apply_node[thread]->a = aHigh | bddinternal;
            template_apply_node[thread]->b = bHigh;
            template_apply_node[thread]->c = cHigh;

            ptr = llset_get_or_create(_bdd.cache, template_apply_node[thread], &created, &idx);
            if (created) {
                // Add new ITE* nodes to queue
                llsched_push(_bdd.sched, thread, &idx);
            }

            node->cache_high = idx;
            node->high = sylvan_invalid;
            llvector_push(&ptr->parents, &node);

        } else {
            // we need a child ITE nodes
            int created, cached;
            BDD result;

            result = sylvan_makeite(thread, aLow, bLow, cLow, &created, &cached);
            if (cached) {
                node->cache_low = 0;
                node->low = result;
                ++n;
            } else {
                if (created) {
                    // Uncached ITE nodes must be calculated
                    llvector_push(&_bdd.leaves2, &result);
                }
                node->cache_low = result;
                node->low = sylvan_invalid;
                llvector_push(&GETCACHE(result)->parents, &node);
            }

            result = sylvan_makeite(thread, aHigh, bHigh, cHigh, &created, &cached);
            if (cached) {
                node->cache_high = 0;
                node->high = result;
                printf("Set %d high to cached %s%d\n", node_c,
                		bddmark&node->high?"~":"", node->high&~bddmark);
                ++n;
            } else {
                if (created) {
                    // Uncached ITE nodes must be calculated
                    llvector_push(&_bdd.leaves2, &result);
                }
                node->cache_high = result;
                node->high = sylvan_invalid;
                llvector_push(&GETCACHE(result)->parents, &node);
            }
        }

        if (n==2) {
            // ITE* leaf: two ITE nodes that are cached
            llvector_push(&_bdd.leaves, &node_c);
        }
    }

    // PART 2
    if (thread == 0) {
        llsched_setupwait(_bdd.sched);
        while (llvector_pop(&_bdd.leaves, &node_c)) {
            llsched_push(_bdd.sched, 0, &node_c);
        }
    }

    while (llsched_pop(_bdd.sched, thread, &node_c) == 1) {
        assert((node_c & bddmark)==0);

        // Here we process ITE* nodes that have two ITE nodes cached
        // as well as ITE* nodes that have two ITE* nodes resolved

        //node_c = BDD_STRIPMARK(node_c);
        bddcache_t node = GETCACHE(node_c);

        // This will spawn ITE nodes...
        // if it returns 1, then the result is calculated
        if (!sylvan_process_ite_ex(thread, node, 0)) {
            // Returns 0 if additional ITE calculations are required
            // Also returns 0 if it was a root node
            continue;
        }
        // if it is the root ITE*, no processing is needed
        // if it requires additional ITE, it is added to leaves2

        // if a ITE* result is calculated, we need to continue...

        BDD q = node->result;

        bddcache_t parent;
        while (llvector_pop(&node->parents, &parent)) {
            BDD parent_c = GETCACHEBDD(parent);

            // Verify that it is a ITE* node
            assert(parent->a & bddinternal);

            if (BDD_STRIPMARK(parent->cache_low) == node_c) {
                parent->low = BDD_TRANSFERMARK(parent->cache_low, q);
                parent->cache_low = 0;
            }

            if (BDD_STRIPMARK(parent->cache_high) == node_c) {
                parent->high = BDD_TRANSFERMARK(parent->cache_high, q);
                parent->cache_high = 0;
            }

            __sync_synchronize();

            if (parent->low == sylvan_invalid || parent->high == sylvan_invalid) {
                // One of the children still needs calculation
                continue;
            }

            //sylvan_calculate_result(thread, parent, parent_c);

            if (!__sync_bool_compare_and_swap(&parent->result, sylvan_invalid, bddhandled)) {
                // We are not the thread that has acquired this parent
                continue;
            }

            // Add to queue
            llsched_push(_bdd.sched, thread, &parent_c);
            /*
			if (sylvan_process_ite_ex(thread, parent, 0)) {
				// if a ITE* result is calculated...
				llsched_push(_bdd.sched, thread, &parent_c);
			}*/
        }

        // free parents data and delete node
        llvector_deinit(&node->parents);
        llset_delete(_bdd.cache, node_c);
    }
}

BDD sylvan_restructure(BDD a, BDD b, BDD c, BDD* pairs, size_t n)
{
    _bdd.replaceBy = pairs;
    _bdd.replaceLast = n - 1;

    // wait until all threads are ready
    sylvan_wait_for_threads();

    template_apply_node[0]->a = a | bddinternal;
    template_apply_node[0]->b = b;
    template_apply_node[0]->c = c;

    uint32_t idx;
    bddcache_t ptr = llset_get_or_create(_bdd.cache, template_apply_node[0], NULL, &idx);

    // Send "ite-down" command to all threads
    for (int i=1; i<_bdd.threadCount; i++) {
        atomic8_write(&_bdd.flags[i], bddcommand_ite_down);
    }

    llsched_setupwait(_bdd.sched);
    llsched_push(_bdd.sched, 0, &idx);

    sylvan_execute_ite_down(0);
    sylvan_wait_for_threads();

    printf("After ITE*-down:\n");
    sylvan_print_cache(idx);

    //struct llvector v;
    //llvector_init(&v, sizeof(BDD));

    //while (llvector_count(&_bdd.leaves2)>0) {
        //llvector_move(&_bdd.leaves2, &v);

        for (int i=1; i<_bdd.threadCount; i++) {
            atomic8_write(&_bdd.flags[i], bddcommand_ite);
        }

        llsched_setupwait(_bdd.sched);
/*
        while (llvector_count(&v)>0) {
            BDD node_c;
            llvector_pop(&v, &node_c);
            llsched_push(_bdd.sched, 0, &node_c);
        }*/

        while (llvector_count(&_bdd.leaves2)>0) {
            BDD node_c;
            llvector_pop(&_bdd.leaves2, &node_c);
            llsched_push(_bdd.sched, 0, &node_c);
        }

        //llvector_deinit(&v);

        sylvan_execute_ite(0);
        sylvan_wait_for_threads();
    //}

    BDD result = ptr->result;

    if (result == sylvan_invalid) sylvan_print_cache(GETCACHEBDD(ptr));

    assert(result != sylvan_invalid);

    llset_delete(_bdd.cache, idx);

    _bdd.replaceBy = 0;
    _bdd.replaceLast = -1;

    return result;
}


BDD sylvan_ite_ex(BDD a, BDD b, BDD c, const BDD* restrict pairs, size_t n) {
    assert(a != sylvan_invalid);
    assert(b != sylvan_invalid);
    assert(c != sylvan_invalid);

    // Prepare struct
    BDD last = 0;
    for (BDD i=0;i<n;i++) if (pairs[2*i] > last) last = pairs[2*i];

    BDD *newPairs = _bdd.replaceBy = malloc(sizeof(BDD)*(last+1));
    for (BDD i=0;i<=last;i++) newPairs[i] = sylvan_ithvar(i);
    for (BDD i=0;i<n;i++) {
        BDD p = pairs[2*i+1];
        if (p & 0x40000000) { // forall, exists, unique
            newPairs[pairs[2*i]] = p;
        } else {
            if (p & bddmark) newPairs[pairs[2*i]] = sylvan_nithvar(p & ~bddmark);
            else newPairs[pairs[2*i]] = sylvan_ithvar(p);
        }
    }

    BDD result = sylvan_restructure(a, b, c, newPairs, last + 1);

    free(newPairs);
    _bdd.replaceBy = 0;

    return result;
}

BDD sylvan_replace(BDD a, const BDD* restrict pairs, size_t n) {
    return sylvan_ite_ex(a, sylvan_true, sylvan_false, pairs, n);
}

BDD sylvan_quantify(BDD a, const BDD* restrict pairs, size_t n) {
    return sylvan_ite_ex(a, sylvan_true, sylvan_false, pairs, n); // same as replace...
}

void sylvan_wait_for_threads()
{
    for (int i=1;i<_bdd.threadCount; i++) {
        while (atomic8_read(&_bdd.flags[i]) != 0) {}
    }
}

static void *sylvan_thread(void *data) {
    int t = (int)data;

    while (1) {
        /// SPIN-WAIT FOR COMMAND
        while (atomic8_read(&_bdd.flags[t]) == 0) {}

        int value = _bdd.flags[t];

        //if (value == bddcommand_apply) sylvan_execute_apply(t);
        if (value == bddcommand_ite_down) sylvan_execute_ite_down(t);
        if (value == bddcommand_ite) sylvan_execute_ite(t);

        if (value == bddcommand_quit) break;

        atomic8_write(&_bdd.flags[t], 0);
    }

    return NULL;
}


double sylvan_satcount_do(BDD bdd, const BDDLEVEL *variables, size_t n, int index)
{
	if (bdd == sylvan_false) return 0;
	if (bdd == sylvan_true) {
		if (index < n) return pow(2.0, (n-index));
		else return 1;
	}

	BDDLEVEL level = sylvan_var(bdd);
	if (level == variables[index]) {
		// take high, take low
		double high = sylvan_satcount_do(sylvan_high(bdd), variables, n, index+1);
		double low = sylvan_satcount_do(sylvan_low(bdd), variables, n, index+1);
		return high+low;
	} else {
		return 2 * sylvan_satcount_do(bdd, variables, n, index+1);
	}
}

double sylvan_satcount(BDD bdd, const BDDLEVEL *variables, size_t n)
{
	return sylvan_satcount_do(bdd, variables, n, 0);
}




static inline void sylvan_printbdd(const char *s, BDD bdd) {
    if (bdd==sylvan_invalid || bdd==bddhandled) {
        printf("-1");
        return;
    }
    printf("%c", BDD_HASMARK(bdd)?'~':' ');
    printf(s, BDD_STRIPMARK(bdd));
}

void sylvan_print(BDD bdd) {
    if (bdd == sylvan_invalid) return;

    printf("Dump of ");
    sylvan_printbdd("%u", bdd);
    printf("\n");

    bdd = BDD_STRIPMARK(bdd);

    if (bdd < 2) return;

    llvector_t v = llvector_create(sizeof(BDD));
    llset_t s = llset_create(sizeof(BDD), 12, NULL, NULL);
    int created;

    llvector_push(v, &bdd);
    llset_get_or_create(s, &bdd, &created, NULL);

    while (llvector_count(v) > 0) {
        llvector_pop(v, &bdd);

        sylvan_printbdd("% 10d", bdd);
        printf(": %u low=", sylvan_var(bdd));
        sylvan_printbdd("%u", sylvan_low(bdd));
        printf(" high=");
        sylvan_printbdd("%u", sylvan_high(bdd));
        printf("\n");

        BDD low = BDD_STRIPMARK(sylvan_low(bdd));
        BDD high = BDD_STRIPMARK(sylvan_high(bdd));
        if (low >= 2) {
            llset_get_or_create(s, &low, &created, NULL);
            if (created) llvector_push(v, &low);
        }
        if (high >= 2) {
            llset_get_or_create(s, &high, &created, NULL);
            if (created) llvector_push(v, &high);
        }
    }

    llvector_free(v);
    llset_free(s);
}

void sylvan_print_cache_node(bddcache_t node) {
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
    for (uint32_t i=0;i<llvector_count(&node->parents);i++) {
        if (i>0) printf(",");
        bddcache_t n;
        llvector_get(&node->parents, i, &n);
        printf("%u", (uint32_t)GETCACHEBDD(n));
    }
    printf("}, r=%x\n", node->result);
}

void sylvan_print_cache(BDD root) {
    llvector_t v = llvector_create(sizeof(BDD));
    llset_t s = llset_create(sizeof(BDD), 13, NULL, NULL);
    int created;

    printf("Dump of cache ");
    sylvan_printbdd("%u\n", root);

    llvector_push(v, &root);
    llset_get_or_create(s, &root, &created, NULL);

    while (llvector_count(v) > 0) {
        llvector_pop(v, &root);

        sylvan_print_cache_node(GETCACHE(root));

        BDD low = GETCACHE(root)->cache_low;
        BDD high = GETCACHE(root)->cache_high;
        if (low) {
            llset_get_or_create(s, &low, &created, NULL);
            if (created) llvector_push(v, &low);
        }
        if (high) {
            llset_get_or_create(s, &high, &created, NULL);
            if (created) llvector_push(v, &high);
        }
    }

    llvector_free(v);
    llset_free(s);
}
