#include <stdint.h>
#include <stdio.h> // for FILE
#include <lace.h> // for definitions

#ifndef SYLVAN_H
#define SYLVAN_H

/**
 * Sylvan: parallel BDD package.
 *
 * This is a multi-core implementation of BDDs with complement edges.
 *
 * This package requires parallel work-stealing framework Lace.
 * Lace must be initialized before calling any Sylvan operations.
 *
 * This package uses explicit referencing.
 * Use sylvan_ref and sylvan_deref to manage external references.
 *
 * Garbage collection requires all workers to cooperate. Garbage collection is either initiated
 * by the user (calling sylvan_gc) or when the BDD node table is full. All Sylvan operations
 * check whether they need to cooperate on garbage collection. Garbage collection cannot occur
 * otherwise. This means that it is perfectly fine to do this:
 *              BDD a = sylvan_ref(sylvan_and(b, c));
 * since it is not possible that garbage collection occurs between the two calls.
 *
 * To temporarily disable garbage collection, use sylvan_gc_disable() and sylvan_gc_enable().
 */

// For now, only support 64-bit systems
typedef char __sylvan_check_size_t_is_8_bytes[(sizeof(uint64_t) == sizeof(size_t))?1:-1];

typedef uint64_t BDD;       // low 40 bits used for index, highest bit for complement, rest 0
// BDDSET uses the BDD node hash table. A BDDSET is an ordered BDD.
typedef uint64_t BDDSET;    // encodes a set of variables (e.g. for exists etc.)
typedef uint32_t BDDVAR;    // low 24 bits only

#define sylvan_complement   ((uint64_t)0x8000000000000000)
#define sylvan_false        ((BDD)0x0000000000000000)
#define sylvan_true         (sylvan_false|sylvan_complement)
#define sylvan_true_nc      ((BDD)0x000000ffffffffff)  // sylvan_true without complement edges
#define sylvan_invalid      ((BDD)0x7fffffffffffffff)

#define sylvan_isconst(a)   ( ((a&(~sylvan_complement)) == sylvan_false) || (a == sylvan_true_nc) )
#define sylvan_isnode(a)    ( ((a&(~sylvan_complement)) != sylvan_false) && ((a&(~sylvan_complement)) < sylvan_true_nc) )

/**
 * Initialize the Sylvan parallel BDD package.
 *
 * Allocates a BDD node table of size "2^datasize" and a operation cache of size "2^cachesize".
 * Every BDD node requires 32 bytes memory. (16 data + 16 bytes overhead)
 * Every operation cache entry requires 36 bytes memory. (32 bytes data + 4 bytes overhead)
 *
 * Granularity determines usage of operation cache. Smallest value is 1: use the operation cache always.
 * Higher values mean that the cache is used less often. Variables are grouped such that
 * the cache is used when going to the next group, i.e., with granularity=3, variables [0,1,2] are in the
 * first group, [3,4,5] in the next, etc. Then no caching occur between 0->1, 1->2, 0->2. Caching occurs
 * on 0->3, 1->4, 2->3, etc.
 * 
 * Reasonable defaults: datasize of 26 (2048 MB), cachesize of 24 (576 MB), granularity of 4-16
 */
void sylvan_init(size_t datasize, size_t cachesize, int granularity);

/**
 * Frees all Sylvan data.
 */
void sylvan_quit();

/* Create a BDD representing just <var> */
BDD sylvan_ithvar(BDDVAR var);
/* Create a BDD representing the negation of <var> */
static inline BDD sylvan_nithvar(BDD var) { return sylvan_ithvar(var) ^ sylvan_complement; }

/* Retrieve the <var> of the BDD node <bdd> */
BDDVAR sylvan_var(BDD bdd);

/* Follow <low> and <high> edges */
BDD sylvan_low(BDD bdd);
BDD sylvan_high(BDD bdd);

/* Add or remove external reference to BDD */
BDD sylvan_ref(BDD a); 
void sylvan_deref(BDD a);

/* Return the number of external references */
size_t sylvan_count_refs();

/* Perform garbage collection */
void sylvan_gc();

/* Enable or disable garbage collection. It is enabled by default. */
void sylvan_gc_enable();
void sylvan_gc_disable();

/* Unary, binary and if-then-else operations */
#define sylvan_not(a) (((BDD)a)^sylvan_complement)
TASK_DECL_4(BDD, sylvan_ite, BDD, BDD, BDD, BDDVAR);
static inline BDD sylvan_ite(BDD a, BDD b, BDD c) { return CALL(sylvan_ite, a, b, c, 0); }
static inline BDD sylvan_xor(BDD a, BDD b) { return sylvan_ite(a, sylvan_not(b), b); }
static inline BDD sylvan_equiv(BDD a, BDD b) { return sylvan_ite(a, b, sylvan_not(b)); }
#define sylvan_or(a,b) sylvan_ite(a, sylvan_true, b)
#define sylvan_and(a,b) sylvan_ite(a,b,sylvan_false)
#define sylvan_nand(a,b) sylvan_not(sylvan_and(a,b))
#define sylvan_nor(a,b) sylvan_not(sylvan_or(a,b))
#define sylvan_imp(a,b) sylvan_not(sylvan_and(a,sylvan_not(b)))
#define sylvan_invimp(a,b) sylvan_not(sylvan_and(sylvan_not(a),b))
#define sylvan_biimp sylvan_equiv
#define sylvan_diff(a,b) sylvan_and(a,sylvan_not(b))
#define sylvan_less(a,b) sylvan_and(sylvan_not(a),b)

/* Existential and Universal quantifiers */
TASK_DECL_3(BDD, sylvan_exists, BDD, BDD, BDDVAR);
static inline BDD sylvan_exists(BDD a, BDD variables) { return CALL(sylvan_exists, a, variables, 0); }
static inline BDD sylvan_forall(BDD a, BDD variables) { return sylvan_not(CALL(sylvan_exists, sylvan_not(a), variables, 0)); }

/**
 * Specialized RelProdS using paired variables (X even, X' odd)
 * For example, variable x_1 is paired with x_0, with x_1 being the X' equivalent of x_0.
 * When using relprods to generate the 'next' states, you will want to
 * use <var> 0,2,4,6,8 etc for the 'state' booleans, and 1,3,5,7,9 etc for the 'next state' booleans
 */
TASK_DECL_4(BDD, sylvan_relprods, BDD, BDD, BDD, BDDVAR);
static inline BDD sylvan_relprods(BDD a, BDD b, BDD vars) { return CALL(sylvan_relprods, a, b, vars, 0); }

typedef void (*void_cb)();
int sylvan_relprods_analyse(BDD a, BDD b, void_cb cb_in, void_cb cb_out);

/**
 * Reversed RelProdS using paired variables (X even, X' odd)
 */
TASK_DECL_4(BDD, sylvan_relprods_reversed, BDD, BDD, BDD, BDDVAR);
static inline BDD sylvan_relprods_reversed(BDD a, BDD b, BDD vars) { return CALL(sylvan_relprods_reversed, a, b, vars, 0); }

/**
 * RelProd: \exists vars (a \and b)
 */
TASK_DECL_4(BDD, sylvan_relprod, BDD, BDD, BDD, BDDVAR);
static inline BDD sylvan_relprod(BDD a, BDD b, BDD vars) { return CALL(sylvan_relprod, a, b, vars, 0); }

/**
 * Calculate substitution from X to X' using paired variables (X even, X' odd)
 */
TASK_DECL_3(BDD, sylvan_substitute, BDD, BDD, BDDVAR);
static inline BDD sylvan_substitute(BDD a, BDD vars) { return CALL(sylvan_substitute, a, vars, 0); }

/**
 * Calculate a@b (a constrain b), such that (b -> a@b) = (b -> a)
 * Special cases:
 *   - a@0 = 0
 *   - a@1 = f
 *   - 0@b = 0
 *   - 1@b = 1
 *   - a@a = 1
 *   - a@not(a) = 0
 */
TASK_DECL_3(BDD, sylvan_constrain, BDD, BDD, BDDVAR);
static inline BDD sylvan_constrain(BDD a, BDD b) { return CALL(sylvan_constrain, a, b, 0); }

/**
 * Calculate the support of a BDD.
 * A variable v is in the support of a Boolean function f iff f[v<-0] != f[v<-1]
 * It is also the set of all variables in the BDD nodes of the BDD.
 */
TASK_DECL_1(BDD, sylvan_support, BDD);
static inline BDD sylvan_support(BDD bdd) { return CALL(sylvan_support, bdd); }

/**
 * Reset all counters (for statistics)
 */
void sylvan_reset_counters();

/**
 * Write statistic report to stdout
 */
void sylvan_report_stats();


/**
 * A BDDSET, used by BDD functions.
 * Basically this is a union of all variables in the set in their positive form.
 * Note that you need to do external referencing manually.
 * If using this during your initialization, you could disable GC temporarily.
 */
// empty bddset
static inline BDDSET sylvan_set_empty() { return sylvan_false; }
static inline int sylvan_set_isempty(BDDSET set) { return set == sylvan_false ? 1 : 0; }
// add variables to the bddset
static inline BDDSET sylvan_set_add(BDDSET set, BDDVAR var) { return sylvan_or(set, sylvan_ithvar(var)); }
static inline BDDSET sylvan_set_addall(BDDSET set, BDDSET toadd) { return sylvan_or(set, toadd); }
// remove variables from the bddset
static inline BDDSET sylvan_set_remove(BDDSET set, BDDVAR var) { return sylvan_constrain(set, sylvan_nithvar(var)); }
static inline BDDSET sylvan_set_removeall(BDDSET set, BDDSET toremove) { return sylvan_constrain(set, sylvan_not(toremove)); }
// iterate through all variables
static inline BDDVAR sylvan_set_var(BDDSET set) { return sylvan_var(set); }
static inline BDDSET sylvan_set_next(BDDSET set) { return sylvan_low(set); }
int sylvan_set_in(BDDSET set, BDDVAR var);
size_t sylvan_set_count(BDDSET set);
void sylvan_set_toarray(BDDSET set, BDDVAR *arr);
BDDSET sylvan_set_fromarray(BDDVAR *arr, size_t length);

/**
 * Node creation primitive.
 * Careful: does not check ordering!
 */
BDD sylvan_makenode(BDDVAR level, BDD low, BDD high);

/**
 * Write a DOT representation of a BDD
 */
void sylvan_printdot(BDD bdd);
void sylvan_fprintdot(FILE *out, BDD bdd);

void sylvan_printdot_nocomp(BDD bdd);
void sylvan_fprintdot_nocomp(FILE *out, BDD bdd);

void sylvan_print(BDD bdd);
void sylvan_fprint(FILE *f, BDD bdd);

/**
 * Convert normal BDD to a BDD without complement edges
 * Also replaces sylvan_true by sylvan_true_nc
 * Function only meant for debugging purposes.
 */
BDD sylvan_bdd_to_nocomp(BDD bdd);

/**
 * Calculate number of satisfying variable assignments.
 * The set of variables must be >= the support of the BDD.
 * (i.e. all variables in the BDD must be in variables)
 * 
 * The cached version uses the operation cache, but is limited to 64-bit floating point numbers.
 */

typedef double sylvan_satcount_double_t;
// if this line below gives an error, modify the above typedef until fixed ;)
typedef char __sylvan_check_double_is_8_bytes[(sizeof(sylvan_satcount_double_t) == sizeof(size_t))?1:-1];

TASK_DECL_3(sylvan_satcount_double_t, sylvan_satcount_cached, BDD, BDDSET, BDDVAR);
TASK_DECL_2(long double, sylvan_satcount, BDD, BDDSET);

static inline sylvan_satcount_double_t sylvan_satcount_cached(BDD bdd, BDDSET variables) { return CALL(sylvan_satcount_cached, bdd, variables, 0); }
static inline long double sylvan_satcount(BDD bdd, BDDSET variables) { return CALL(sylvan_satcount, bdd, variables); }

/**
 * Create a BDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * For example, sylvan_cube({2,4,6,8},4,{0,1,2,1}) returns BDD of Boolean formula "not(x_2) & x_4 & x_8"
 */
BDD sylvan_cube(BDDVAR *variables, size_t count, char *cube);

/**
 * Pick one satisfying variable assignment randomly for which <bdd> is true.
 * Note that <variables> must include all variables in the support of <bdd>,
 * and that count must equal the size of both arrays.
 *
 * The function will set the values of str, such that
 * str[index] where index is the index in the <variables> set is set to
 * 0 when the variable is negative, 1 when positive, or 2 when it could be either.
 *
 * Returns 1 when succesful, or 0 when no assignment is found (i.e. bdd==sylvan_false).
 */
int sylvan_sat_one(BDD bdd, BDDVAR *variables, size_t count, char* str);

/**
 * Pick one satisfying variable assignment randomly from the given <bdd>.
 * Functionally equivalent to performing sylvan_cube on the result of sylvan_sat_one.
 * For the result: sylvan_and(res, bdd) = res.
 */
BDD sylvan_sat_one_bdd(BDD bdd);
#define sylvan_pick_cube sylvan_sat_one_bdd

TASK_DECL_1(long double, sylvan_pathcount, BDD);
static inline long double sylvan_pathcount(BDD bdd) { return CALL(sylvan_pathcount, bdd); }

// TASK_DECL_1(size_t, sylvan_nodecount, BDD);
size_t sylvan_nodecount(BDD a);
void sylvan_nodecount_levels(BDD bdd, uint32_t *variables);

/**
 * SAVING:
 * use sylvan_serialize_add on every BDD you want to store
 * use sylvan_serialize_get to retrieve the key of every stored BDD
 * use sylvan_serialize_tofile
 *
 * LOADING:
 * use sylvan_serialize_fromfile (implies sylvan_serialize_reset)
 * use sylvan_serialize_get_reversed for every key
 *
 * MISC:
 * use sylvan_serialize_reset to free all allocated structures
 * use sylvan_serialize_totext to write a textual list of tuples of all BDDs.
 *         format: [(<key>,<level>,<key_low>,<key_high>,<complement_high>),...]
 *
 * for the old sylvan_print functions, use sylvan_serialize_totext
 */
size_t sylvan_serialize_add(BDD bdd);
size_t sylvan_serialize_get(BDD bdd);
BDD sylvan_serialize_get_reversed(size_t value);
void sylvan_serialize_reset();
void sylvan_serialize_totext(FILE *out);
void sylvan_serialize_tofile(FILE *out);
void sylvan_serialize_fromfile(FILE *in);

#endif


