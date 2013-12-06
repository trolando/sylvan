#include <stdint.h>
#include <stdio.h> // for FILE
#include "lace.h" // for definitions

#ifndef SYLVAN_H
#define SYLVAN_H

/**
 * Sylvan: BDD package.
 *
 * Explicit referencing, so use sylvan_ref and sylvan_deref manually.
 * To temporarily disable garbage collection, use sylvan_gc_disable() and sylvan_gc_enable().
 */

typedef uint64_t BDD;
typedef uint64_t BDDSET;
typedef uint32_t BDDVAR;

extern const BDD sylvan_true;
extern const BDD sylvan_false;

// use "BDD something = sylvan_invalid;" instead of "BDD something = 0;"
extern const BDD sylvan_invalid;

/**
 * Initialize Sylvan BDD package
 * datasize in number of nodes will be 1<<datasize
 * cachesize in number of nodes will be 1<<cachesize
 * granularity determines usage of memoization cache, default=1 (memoize at every level)
 *   [memoization occurs at every next 'level % granularity' recursion, e.g.
 *    with granularity=3, from lvl 0->1, 1->2 no memoization, but memoize 0->3, 1->3, 1->4, etc.]
 * reasonable default values: datasize=24, cachesize=20, granularity=2
 */
void sylvan_init(size_t datasize, size_t cachesize, int granularity);

/**
 * Frees all Sylvan data
 */
void sylvan_quit();

/**
 * Create a BDD representing <var>
 */
BDD sylvan_ithvar(BDDVAR var);

/**
 * Create a BDD representing not(<var>)
 */
BDD sylvan_nithvar(BDDVAR var);

/**
 * Create a BDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * For example, sylvan_cube({2,4,6,8},4,{0,1,2,1}) returns BDD of Boolean formula "not(x_2) & x_4 & x_8"
 */
BDD sylvan_cube(BDDVAR *variables, size_t count, char* cube);

/**
 * Get the <var> of the root node of <bdd>
 * This is also the first variable in the support of <bdd> according to the ordering.
 */
BDDVAR sylvan_var(BDD bdd);

/**
 * Get the <low> child of <bdd>
 */
BDD sylvan_low(BDD bdd);

/**
 * Get the <high> child of <bdd>
 */
BDD sylvan_high(BDD bdd);

/**
 * Get not(bdd)
 */
BDD sylvan_not(BDD bdd);

/**
 * Calculate simple if <a> then <b> else <c>
 */
TASK_DECL_4(BDD, sylvan_ite, BDD, BDD, BDD, BDDVAR);
BDD sylvan_ite(BDD a, BDD b, BDD c);

/**
 * Binary BDD operations (implemented using ite)
 */
BDD sylvan_and(BDD a, BDD b);
BDD sylvan_xor(BDD a, BDD b);
BDD sylvan_or(BDD a, BDD b);
BDD sylvan_nand(BDD a, BDD b);
BDD sylvan_nor(BDD a, BDD b);
BDD sylvan_imp(BDD a, BDD b);
BDD sylvan_biimp(BDD a, BDD b);
BDD sylvan_diff(BDD a, BDD b);
BDD sylvan_less(BDD a, BDD b);
BDD sylvan_invimp(BDD a, BDD b);

/**
 * Specialized RelProdS using paired variables (X even, X' odd)
 */
TASK_DECL_4(BDD, sylvan_relprods, BDD, BDD, BDD, BDDVAR);
BDD sylvan_relprods(BDD a, BDD b, BDD vars);

typedef void (*void_cb)();
int sylvan_relprods_analyse(BDD a, BDD b, void_cb cb_in, void_cb cb_out);

/**
 * Reversed RelProdS using paired variables (X even, X' odd)
 */
TASK_DECL_4(BDD, sylvan_relprods_reversed, BDD, BDD, BDD, BDDVAR);
BDD sylvan_relprods_reversed(BDD a, BDD b, BDD vars);

/**
 * Calculate RelProd: \exists vars (a \and b)
 */
TASK_DECL_4(BDD, sylvan_relprod, BDD, BDD, BDD, BDDVAR);
BDD sylvan_relprod(BDD a, BDD b, BDD vars);

/**
 * Calculate substitution from X to X' using paired variables (X even, X' odd)
 */
TASK_DECL_3(BDD, sylvan_substitute, BDD, BDD, BDDVAR);
BDD sylvan_substitute(BDD a, BDD vars);

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
BDD sylvan_constrain(BDD a, BDD b);

/**
 * Calculate \exists variables . a
 * Calculate \forall variables . a
 */
TASK_DECL_3(BDD, sylvan_exists, BDD, BDD, BDDVAR);
BDD sylvan_exists(BDD a, BDD variables);
BDD sylvan_forall(BDD a, BDD variables);

/**
 * Calculate the support of <bdd>.
 * This is the set of used variables in the BDD nodes.
 * A variable v is in the support of BDD F iff not F[v<-0] = F[v<-1].
 */
TASK_DECL_1(BDD, sylvan_support, BDD);
BDD sylvan_support(BDD bdd);

BDD sylvan_ref(BDD a);
void sylvan_deref(BDD a);
size_t sylvan_count_refs();
void sylvan_gc();

void sylvan_gc_enable();
void sylvan_gc_disable();

/**
 * Reset all counters (for statistics)
 */
void sylvan_reset_counters();

/**
 * Write statistic report to stdout
 */
void sylvan_report_stats();



/**
 * Set using BDD operations
 */
int sylvan_set_isempty(BDDSET set);
BDDVAR sylvan_set_var(BDDSET set);
BDDSET sylvan_set_empty();
BDDSET sylvan_set_add(BDDSET set, BDDVAR level);
BDDSET sylvan_set_addall(BDDSET set, BDD toadd);
BDDSET sylvan_set_remove(BDDSET set, BDDVAR level);
BDDSET sylvan_set_removeall(BDDSET set, BDDSET toremove);
int sylvan_set_in(BDDSET set, BDDVAR level);
BDDSET sylvan_set_next(BDDSET set);
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

void sylvan_print(BDD bdd);
void sylvan_fprint(FILE *f, BDD bdd);

/**
 * Calculate number of satisfying variable assignments.
 * <bdd> must only have variables in <variables>
 * <variables> is a BDD with every variable 'high' set to 'true'
 */
long double sylvan_satcount(BDD bdd, BDD variables);

/**
 * Pick one satisfying variable assignment randomly from the given <bdd>.
 * sizeof(str) must be >= sylvan_set_count(<variables>)
 * str[index] where index is the index in the <variables> set is set to
 *   0 when 0, 1 when 1, or 2 when either 0 or 1
 * Returns 1 when succesful, or 0 when no assignment is found.
 */
int sylvan_sat_one(BDD bdd, BDDVAR *variables, size_t count, char* str);
#define sylvan_pick_cube sylvan_sat_one

TASK_DECL_1(long double, sylvan_pathcount, BDD);
long double sylvan_pathcount(BDD bdd);

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


