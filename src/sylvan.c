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
const BDD ite_ex_second = -4;       // 0xfffffffc

/**
 * Internal BDD constants
 */
#define bddmark 0x80000000
//const BDD bddmark = 0x80000000;
const BDD bddhandled = 0xffffffff; // being handled by operation
const BDD bddinternal = 0x40000000; // ITE* node marker (on "a")

const uint8_t bddcommand_quit = 1;
const uint8_t bddcommand_ite_ex = 2;
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
#if 0
#define REPORT_RESULT(a,b,c) printf("Set result of " #a " %d to %s%d\n", b, c&bddmark?"~":"", c&~bddmark);
#else
#define REPORT_RESULT(a,b,c)
#endif

struct bddnode
{
  BDD low; 
  BDD high;
  BDDLEVEL level; 
  uint32_t filler;
};

typedef struct bddnode* bddnode_t;

static struct {
  /// Main UNIQUE table
  llset_t data;

  /// ITE
  llset_t cache;
  struct llvector leaves;

  /// ITE*
  BDD *replaceBy;
  BDD replaceLast;

  /// THREAD information
  llsched_t sched;
  int threadCount;
  pthread_t *threads;
  uint8_t *flags;

  /// DEBUG information
  BDD root;
} _bdd;

struct bddcache {
  // Following variables are the KEY for the unique table
  BDD a; // if
  BDD b; // then (set to sylvan_invalid for replace nodes)
  BDD c; // else (set to sylvan_invalid for replace nodes)
  // Results
  BDD root; // new if
  BDD high; // new then
  BDD low; // new else
  BDD result; // sylvan_invalid after creation, bddhandled when added to the job queue
  // Children
  BDD cache_low; // index to bddcache else
  BDD cache_high; // index to bddcache then
  // Parents Linked-List
  BDD first_parent; // index to first bddcache parent
  BDD next_low_parent; // index to next bddcache parent of low child
  BDD next_high_parent; // index to next bddcache parent of high child
  uint32_t q1; // 52
  uint32_t q2; // 56
  uint32_t q3; // 60
  uint32_t q4; // 64
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
#define sylvan_makeite(t,a,b,c,x,y) sylvan_makeite_ex(t,a,b,c,x,y,0)
static BDD sylvan_makeite_ex(int thread, BDD a, BDD b, BDD c, int *created, int *cached, int is_ex);
static void sylvan_execute_ite(const int thread);
static void sylvan_execute_ite_ex(const int thread);
static inline void sylvan_handle_ite_parents(const int thread, bddcache_t node, BDD node_c);

static void *sylvan_thread(void *data);
void sylvan_wait_for_threads();

void sylvan_print_cache(BDD root);
static inline void sylvan_printbdd(const char *s, BDD bdd);

/**
 * Hash() and Equals() for the BDD cache
 */
uint32_t sylvan_bdd_hash(const void *data_, int len __attribute__((unused)), uint32_t hash) {
  return SuperFastHash(data_, 12, hash);
}

int sylvan_bdd_equals(const void *a, const void *b, size_t length __attribute__((unused))) {
  return memcmp(a, b, 12) == 0;
}


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
    rt_report_and_exit(1, "BDD_init error: datasize must be < 30!");
  }

  if (cachesize >= 30) {
    rt_report_and_exit(1, "BDD_init error: cachesize must be < 30!");
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

  //_bdd.data = llset_create(sizeof(struct bddnode), datasize, NULL, NULL);
  _bdd.data = llset_create(sizeof(struct bddnode), datasize, sylvan_bdd_hash, sylvan_bdd_equals);
  _bdd.cache = llset_create(sizeof(struct bddcache), cachesize, sylvan_cache_hash, sylvan_cache_equals);

  _bdd.sched = llsched_create(threads, sizeof(BDD));
  _bdd.threadCount = threads;
  _bdd.threads = rt_align(CACHE_LINE_SIZE, sizeof(pthread_t) * threads);
  _bdd.flags = rt_align(CACHE_LINE_SIZE, sizeof(uint8_t) * threads);

  memset(_bdd.flags, 0, sizeof(uint8_t) * threads);

  llvector_init(&_bdd.leaves, sizeof(bddcache_t));

  _bdd.replaceBy = 0;
  _bdd.replaceLast = 0;

  template_apply_node = malloc(sizeof(bddcache_t) * threads);
  for (int i=0;i<threads;i++) {
    template_apply_node[i] = malloc(sizeof(struct bddcache));
    memset(template_apply_node[i], 0, sizeof(struct bddcache));
    template_apply_node[i]->result = sylvan_invalid;
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
  /*
  printf("MakeNode(%d, %s%d, %s%d)", level, 
    low&bddmark?"~":"", low&~bddmark, high&bddmark?"~":"", high&~bddmark);
  */

  BDD result;

  struct bddnode n;
  n.level = level;

  // Normalization
  // low will have no mark and will not be "true"

  if (low == sylvan_true) {
    // ITE(a,b,true) == not ITE(a,not b,false)
    n.low = sylvan_false;
    n.high = BDD_TOGGLEMARK(high);
    if (llset_get_or_create(_bdd.data, &n, NULL, &result) == 0) {
      rt_report_and_exit(1, "BDD Unique table full!");
    }
//    printf(" = ~(%d, %s%d, %s%d)\n", level, n.low&bddmark?"~":"", n.low&~bddmark, n.high&bddmark?"~":"", n.high&~bddmark);
    return result | bddmark;
  } else if (BDD_HASMARK(low)) {
    // ITE(a,b,not c) == not ITE(a,not b, c)
    n.low = BDD_STRIPMARK(low);
    n.high = BDD_TOGGLEMARK(high);
    if (llset_get_or_create(_bdd.data, &n, NULL, &result) == 0) {
      rt_report_and_exit(1, "BDD Unique table full!");
    }
//    printf(" = ~(%d, %s%d, %s%d)\n", level, n.low&bddmark?"~":"", n.low&~bddmark, n.high&bddmark?"~":"", n.high&~bddmark);
    return result | bddmark;
  } else {
    n.low = low;
    n.high = high;
    if(llset_get_or_create(_bdd.data, &n, NULL, &result) == 0) {
      rt_report_and_exit(1, "BDD Unique table full!");
    }
//    printf(" = (%d, %s%d, %s%d)\n", level, n.low&bddmark?"~":"", n.low&~bddmark, n.high&bddmark?"~":"", n.high&~bddmark);
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

/**
 * Add a parent link on the low child.
 * Thread-safe.
 */
static inline void sylvan_parent_add_low(bddcache_t child, bddcache_t parent, BDD parent_c)
{
  while (1) {
    BDD fp = child->first_parent;
    parent->next_low_parent = fp;
    if (__sync_bool_compare_and_swap(&child->first_parent, fp, parent_c)) return;
  }
}

/**
 * Add a parent link on the high child.
 * Thread-safe.
 */
static inline void sylvan_parent_add_high(bddcache_t child, bddcache_t parent, BDD parent_c)
{
  while (1) {
    BDD fp = child->first_parent;
    parent->next_high_parent = fp;
    if (__sync_bool_compare_and_swap(&child->first_parent, fp, parent_c)) return;
  }
}

/**
 * Pop the first parent.
 * Thread-safe.
 */
static inline BDD sylvan_parent_pop(bddcache_t child, BDD child_c)
{
  while (1) {
    BDD fp = child->first_parent;
    if (fp == 0) return 0;

    bddcache_t p = GETCACHE(fp);
    BDD next;
    if (BDD_STRIPMARK(p->cache_low) == child_c) {
      next = p->next_low_parent;
    } else {
      next = p->next_high_parent;
    }
    if (__sync_bool_compare_and_swap(&child->first_parent, fp, next)) return fp;
  }
}

/**
 * Calculate standard triples. Find trivial cases.
 * Version for ITE*. Rules: no marks on result...?
 */
static BDD sylvan_preprocess_ex(BDD *_a, BDD *_b, BDD *_c) 
{
  BDD a=*_a, b=*_b, c=*_c;

  // TERMINAL CASE (attempt 1)

  // ITE(T,B,C) = B
  // ITE(F,B,C) = C
  if (a < 2) return (a == sylvan_true ? b : c);

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
      return b;
   } else {
      // Trivial case: they are constants
      if (b<2) {
        // Then a is a constant too!
        assert(a<2);
        return (a == sylvan_true?b:c);
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
  if (a < 2) return (a == sylvan_true ? b : c);

  // ITE(A,B,C) = ITE(.. , .., ~C)
  /* (A and B) or (~A and C)
                  ~(~A or ~C)
           

   A -> B/C === C -> ~A / A and B
    DeMorgan: ITE(A,B,C) = ~ITE(A,~B,~C) is one way to get rid of both
    But how do we make it so B and C both have mark or both have no mark??
    e.g. A -> B | ~C ==> ? ?A -> B | C

    So l
    
    
   */

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
  /* 
  if (BDD_HASMARK(b) || b == sylvan_false) {
    b = BDD_TOGGLEMARK(b);
    c = BDD_TOGGLEMARK(c);

    *_a=a; *_b=b; *_c=c;
    return sylvan_invalid | bddmark;
  }
  */
  *_a=a; *_b=b; *_c=c;
  return sylvan_invalid;
}

/**
 * Calculate standard triples. Find trivial cases.
 */
static BDD sylvan_prepare(BDD *_a, BDD *_b, BDD *_c) 
{
  BDD a=*_a, b=*_b, c=*_c;

  // TERMINAL CASE (attempt 1)

  // ITE(T,B,C) = B
  // ITE(F,B,C) = C
  if (a < 2) return (a == sylvan_true ? b : c);

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

  if (b == c) return b;

  if (b < 2 && c < 2) {
    if (b == sylvan_true) return a;
    else return BDD_TOGGLEMARK(a);
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

  if (a < 2)  {
    assert(0); 
    return (a == sylvan_true ? b : c);
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

  // ITE(T,B,C) = B
  // ITE(F,B,C) = C
  if (a < 2) { 
    assert(0);
    return (a == sylvan_true ? b : c);
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
  if (BDD_HASMARK(b) || b == sylvan_false) {
    b = BDD_TOGGLEMARK(b);
    c = BDD_TOGGLEMARK(c);

    *_a=a; *_b=b; *_c=c;
    return sylvan_invalid | bddmark;
  }

  *_a=a; *_b=b; *_c=c;
  return sylvan_invalid;
}

/**
 * This creates an ITE node.
 * Uses standard triples.
 * Thread-safe.
 * If *cached == true, then the result is a BDD node and *created is false .
 * If *cached == false, then the result is an ITE node and *created may be true.
 * There is a guarantee that only one thread will get *created as true.
 */
static BDD sylvan_makeite_ex(int thread, BDD a, BDD b, BDD c, int *created, int *cached, int is_ex)
{
  /*printf("Preprocessing%s(%s%d, %s%d, %s%d)", is_ex?"*":"",
    a&bddmark?"~":"", a&~bddmark, 
    b&bddmark?"~":"", b&~bddmark, 
    c&bddmark?"~":"", c&~bddmark);
*/
  //BDD result = is_ex?sylvan_preprocess_ex(&a,&b,&c):sylvan_prepare(&a, &b, &c);
  BDD result = sylvan_prepare(&a, &b, &c);

/*
  if (BDD_STRIPMARK(result) == sylvan_invalid) {
    printf(" = %s(%s%d, %s%d, %s%d)%s\n", result&bddmark?"~":"", 
      a&bddmark?"~":"", a&~bddmark, 
      b&bddmark?"~":"", b&~bddmark, 
      c&bddmark?"~":"", c&~bddmark,
      is_ex?"*":"");
  } else {
    printf(" = %s%d%s\n", result&bddmark?"~":"", BDD_STRIPMARK(result), is_ex?"*":"");
  }
*/
  if (BDD_STRIPMARK(result) != sylvan_invalid) {
    if (is_ex == 0 || result < 2) {
      if (created!=0) *created = 0;
      if (cached!=0) *cached = 1;
      return result;
    } else {
      a = result;
      b = sylvan_true;
      c = sylvan_false;
      result = sylvan_invalid;
    }
  }

  if (is_ex && result & bddmark) {
    // Remove mark.
    result &= ~bddmark;
    b = BDD_TOGGLEMARK(b);
    c = BDD_TOGGLEMARK(c);
  }
 
  BDD mark = result & bddmark;

  template_apply_node[thread]->a = a;
  if (is_ex) template_apply_node[thread]->a |= bddinternal;
  template_apply_node[thread]->b = b;
  template_apply_node[thread]->c = c;
/*
  printf("MakeITE%s(%s%d, %s%d, %s%d)", is_ex?"*":"",
    a&bddmark?"~":"", a&~bddmark, 
    b&bddmark?"~":"", b&~bddmark, 
    c&bddmark?"~":"", c&~bddmark);
*/
  /**
   * Now, we know the following:
   * "if" is not true or false and is not complemented
   * "then" is not complemented and is not false
   * "else" may be complemented or true or false
   */

  int _created;
  bddcache_t ptr = llset_get_or_create(_bdd.cache, template_apply_node[thread], &_created, &result);
  if (ptr == 0) {
    rt_report_and_exit(1, "ITE cache full!");
  }

  // Check if it is an existing and cached ITE node...
  if (!_created && BDD_STRIPMARK(ptr->result) != sylvan_invalid) {
    if (created!=0) *created = 0;
    if (cached!=0) *cached = 1;
    BDD res = BDD_TRANSFERMARK(mark, ptr->result);
//    printf(" = %s%d\n", res&bddmark?"~":"", res&~bddmark);
    return res;
  }

  if (created!=0) *created = _created;
  if (cached!=0) *cached = 0;
  BDD res = BDD_TRANSFERMARK(mark, result);
  //printf(" = %s%d\n", res&bddmark?"~":"", res&~bddmark);
  return res;
}

/**
 * This function is called when all subcomputations of a node have been done.
 * It will first acquire the node using CAS.
 * - Computed ITE nodes are added to the queue
 * - ITE* nodes will be replaced by ITE nodes, added to the queue.
 */
static inline void sylvan_calculate_result(int thread, bddcache_t node, BDD node_c)
{
  // Step 1. Acquire the node
  if (node->a & bddinternal && node->root == ite_ex_second) {
    // ITE* node touched for second time
    if (!__sync_bool_compare_and_swap(&node->result, bddhandled, sylvan_invalid)) {
      return;
    }
  } else { 
    if (!__sync_bool_compare_and_swap(&node->result, sylvan_invalid, bddhandled)) {
      return;
    }
  }

  // Step 2a. Handle ITE nodes
  if ((node->a & bddinternal) == 0) {
    // Normal ITE node
    if (node->low == node->high) {
      // low == high (skip)
      node->result = node->low;
    } else {
      node->result = sylvan_makenode(node->root, node->low, node->high);
    }

    REPORT_RESULT(ITE, node_c, node->result);

    // We calculated the result, add it to the queue
    llsched_push(_bdd.sched, thread, &node_c);
    return;
  } 

  // Step 2b. Handle ITE* nodes

  // First time we calculate ITE* is after low&high is calculated.
  // Second time we calculate ITE* is after ITE is calculated.

  if (node->root == ite_ex_second) {
    node->result = node->low; // or node->high
    if (node->first_parent != 0) {
      // Not the root node, so handle the parents and delete it.
      sylvan_handle_ite_parents(thread, node, node_c);
      llset_delete(_bdd.cache, node_c);      
    }
    return;
  }

  // First time we calculated it, so create the ITE node
  int created, cached;
  BDD result;

  if (node->root == quant_forall) {
    // ForAll: ITE(low, high, F)
    result = sylvan_makeite(thread, node->low, node->high, sylvan_false, &created, &cached);
  } else if (node->root == quant_exists) {
    // Exists: ITE(low, T, high)
    /*printf("\n=== %d exists(%s%d,%s%d) ===\n\n", node_c,
      node->low&bddmark?"~":"", node->low&~bddmark,
      node->high&bddmark?"~":"", node->high&~bddmark);*/
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

  if (cached) {
    // ITE is cached! Add to queue for second touch...
    node->result = result;
    if (node->first_parent != 0) {
      sylvan_handle_ite_parents(thread, node, node_c);
      llset_delete(_bdd.cache, node_c);      
    }
    return;
  }

  // it is not cached
  node->root = ite_ex_second;
  node->cache_low = node->cache_high = result;
  bddcache_t result_node = GETCACHE(result);
  sylvan_parent_add_low(result_node, node, node_c);

  if (created) {
    // Add ITE node to queue if we created it
    BDD unmarked = BDD_STRIPMARK(result);
    llsched_push(_bdd.sched, thread, &unmarked);
  } else {
    // We didn't create ITE node, so it may have been calculated already
    
    // Memory barrier
    __sync_synchronize();

    if (atomicsylvan_read(&result_node->result) != sylvan_invalid) {
      // Wait for computation to complete...
      while (atomicsylvan_read(&result_node->result) == bddhandled) {}

      // Help other thread with parent handling. This is necessary, because
      // the thread may or may not still have been handling parents when we 
      // added our node to the list
      sylvan_handle_ite_parents(thread, result_node, BDD_STRIPMARK(result));
    }
  }
}

/**
 * This function is called when ITE and ITE* nodes have been computed. The
 * results of the computation are forwarded to their parents.
 * When all subcomputations of a parent have been done, the parent will be
 * resolved.
 * ITE* nodes will be deleted from the memoization cache after this.
 */
static inline void sylvan_handle_ite_parents(const int thread, bddcache_t node, BDD node_c)
{
  // Multiple threads may be working here at once!
  BDD q = node->result;

  BDD parent_c;
  while ((parent_c = sylvan_parent_pop(node, node_c)) != 0) {
    bddcache_t parent = GETCACHE(parent_c);

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
    sylvan_parent_add_low(low_node, node, node_c);

    if (created) {
      BDD unmarked = BDD_STRIPMARK(low);
      llsched_push(_bdd.sched, thread, &unmarked);
    }

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
    if (BDD_STRIPMARK(node->cache_high) != BDD_STRIPMARK(node->cache_low)) {
      sylvan_parent_add_high(high_node, node, node_c);
    }

    if (created) { 
      BDD unmarked = BDD_STRIPMARK(high);
      llsched_push(_bdd.sched, thread, &unmarked);
    } else {
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

  _bdd.root = ptr;

  // TEMP
  // llset_clear(_bdd.cache);

  // Start other threads
  for (int i=1; i<_bdd.threadCount; i++) {
    atomic8_write(&_bdd.flags[i], bddcommand_ite);
  }

  // Push root node to scheduler
  BDD unmarked = BDD_STRIPMARK(ptr);
  llsched_push(_bdd.sched, 0, &unmarked);

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

static void sylvan_execute_ite_ex(const int thread) {
  // Phase 1: create calculation tree (ITE* nodes, ITE leafs)
  BDD node_c;
  while (llsched_pop(_bdd.sched, thread, &node_c) == 1) {
    assert((node_c & bddmark) == 0);

    // Assumption: no mark on node_c
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
    // This determines what to do when we have the results
    // from the subcalculations
    node->root = level <= _bdd.replaceLast?_bdd.replaceBy[level]:level;

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

    // Determine if the subcalculations are ITE* or ITE
    if (level < _bdd.replaceLast) {
      // Subcalculations also ITE*
      int created, cached;
      BDD result = sylvan_makeite_ex(thread, aLow, bLow, cLow, &created, &cached, 1);
      if (cached) {
        // result is a BDD value
        node->low = result;
        node->cache_low = 0;
      } else {
        // result is a BDDCACHE value
        bddcache_t low_node = GETCACHE(result);
        node->low = sylvan_invalid;
        node->cache_low = result;
        sylvan_parent_add_low(low_node, node, node_c);
        if (created) {
          // Add new ITE* subcalculation to queue
          BDD unmarked = BDD_STRIPMARK(result);
          llsched_push(_bdd.sched, thread, &unmarked);
        }
      }

      result = sylvan_makeite_ex(thread, aHigh, bHigh, cHigh, &created, &cached, 1);
      if (cached) {
        // result is a BDD value
        node->high = result;
        node->cache_high = 0;
      } else {
        // result is a BDDCACHE value
        bddcache_t high_node = GETCACHE(result);
        node->high = sylvan_invalid;
        node->cache_high = result;
        if (BDD_STRIPMARK(node->cache_high) != BDD_STRIPMARK(node->cache_low)) {
          sylvan_parent_add_high(high_node, node, node_c);
        }         
        if (created) {
          // Add new ITE* subcalculation to queue
          BDD unmarked = BDD_STRIPMARK(result);
          llsched_push(_bdd.sched, thread, &unmarked);
        }
      }

      if (node->high != sylvan_invalid && node->low != sylvan_invalid) {
        assert (node->cache_high == 0 && node->cache_low == 0);
        // Leaf for next phase
        llvector_push(&_bdd.leaves, &node_c);
      }
    } else {
      // we need ITE subcalculations
      int n_cached = 0;

      int created, cached;
      BDD result;

      result = sylvan_makeite(thread, aLow, bLow, cLow, &created, &cached);
      if (cached) {
        node->cache_low = 0;
        node->low = result;
        ++n_cached;
      } else {
        if (created) {
          // Leaf for next phase
          BDD unmarked = result & ~bddmark;
          llvector_push(&_bdd.leaves, &unmarked);
        }
        node->cache_low = result;
        node->low = sylvan_invalid;
        sylvan_parent_add_low(GETCACHE(result), node, node_c);
      }

      result = sylvan_makeite(thread, aHigh, bHigh, cHigh, &created, &cached);
      if (cached) {
        node->cache_high = 0;
        node->high = result;
        ++n_cached;
      } else {
        if (created) {
          // Leaf for next phase
          BDD unmarked = result & ~bddmark;
          llvector_push(&_bdd.leaves, &unmarked);
        }
        node->cache_high = result;
        node->high = sylvan_invalid;
        if (BDD_STRIPMARK(node->cache_low) != BDD_STRIPMARK(node->cache_high)) {
          sylvan_parent_add_high(GETCACHE(result), node, node_c);
        }
      }

      if (n_cached==2) {
        // ITE* leaf: two ITE nodes that are cached
        llvector_push(&_bdd.leaves, &node_c);
      }
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
    // Assumption: node_c is unmarked!
    assert((node_c & bddmark)==0);

    // We will see:
    // - resolved ITE* nodes (phase 1, result == invalid)
    // - resolved ITE* nodes (phase 2, result == handled)
    // - new ITE nodes (result == invalid)
    // - computed ITE nodes (result != invalid)
    // Resolved means: subcalculations have been computed

    bddcache_t node = GETCACHE(node_c);

    // Check the type of the node...
    if (node->a & bddinternal) {
      // ITE* node (node->result can be INVALID and HANDLED)
      assert(BDD_STRIPMARK(node->result) == sylvan_invalid);
        //sylvan_handle_ite_parents(thread, node, node_c);
      sylvan_calculate_result(thread, node, node_c);
    } else {
      // ITE node
      if (node->result != sylvan_invalid) {
        sylvan_handle_ite_parents(thread, node, node_c);
      } else {
        sylvan_prepare_ite(thread, node, node_c);
      }
    }
  }
}

BDD sylvan_restructure(BDD a, BDD b, BDD c, BDD* pairs, size_t n)
{
  _bdd.replaceBy = pairs;
  _bdd.replaceLast = n - 1;

  // wait until all threads are ready
  sylvan_wait_for_threads();

  // TEMPORARY
  // llset_clear(_bdd.cache);

  int cached;
  BDD ptr = sylvan_makeite_ex(0, a, b, c, NULL, &cached, 1);
  
  if (cached) return ptr;

  _bdd.root = ptr;

  // Start other threads to do "ite*-down"
  for (int i=1; i<_bdd.threadCount; i++) {
    atomic8_write(&_bdd.flags[i], bddcommand_ite_ex);
  }

  llsched_setupwait(_bdd.sched); 

  // Push root node to scheduler
  BDD unmarked = BDD_STRIPMARK(ptr);
  llsched_push(_bdd.sched, 0, &unmarked);

  sylvan_execute_ite_ex(0);
  sylvan_wait_for_threads();

  BDD result = GETCACHE(ptr)->result;
  if (result == sylvan_invalid) {
    sylvan_print_cache(ptr);
  }

  assert(result != sylvan_invalid);
  assert(result != bddhandled);

  llset_delete(_bdd.cache, ptr);

  _bdd.replaceBy = 0;
  _bdd.replaceLast = -1;

  return BDD_TRANSFERMARK(ptr, result);
}


BDD sylvan_ite_ex(BDD a, BDD b, BDD c, const BDD* restrict pairs, size_t n) {
  //printf("begin\n");

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

    if (value == bddcommand_ite_ex) sylvan_execute_ite_ex(t);
    if (value == bddcommand_ite) sylvan_execute_ite(t);

    if (value == bddcommand_quit) break;

    atomic8_write(&_bdd.flags[t], 0);
  }

  return NULL;
}


double sylvan_satcount_do(BDD bdd, const BDDLEVEL *variables, size_t n, int index)
{
  if (bdd == sylvan_false) return 0.0;
  if (bdd == sylvan_true) {
    if (index < n) return pow(2.0, (n-index));
    else return 1.0;
  }

  if (index >= n) {
    printf("ERROR REACHED: index=%d n=%d", index, n);
    sylvan_print(bdd);
    return 0.0;
  }

  BDDLEVEL level = sylvan_var(bdd);
  if (level == variables[index]) {
    // take high, take low
    double high = sylvan_satcount_do(sylvan_high(bdd), variables, n, index+1);
    double low = sylvan_satcount_do(sylvan_low(bdd), variables, n, index+1);
    return high+low;
  } else {
    return 2.0 * sylvan_satcount_do(bdd, variables, n, index+1);
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
  printf("%s%s%u", bdd&bddinternal?"!":"", BDD_HASMARK(bdd)?"~":"",
    bdd&~(bddinternal|bddmark));
}

void sylvan_print(BDD bdd) {
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
