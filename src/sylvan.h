#include <stdint.h>
#include <stdio.h> // for FILE
#include "lace.h" // for definitions

#ifndef SYLVAN_H
#define SYLVAN_H

#ifndef COLORSTATS
#define COLORSTATS 1
#endif

typedef uint64_t BDD;
typedef uint32_t BDDVAR;

extern const BDD sylvan_true;
extern const BDD sylvan_false;

// use "BDD something = sylvan_invalid;" instead of "BDD something = 0;"
extern const BDD sylvan_invalid;

// Quantifiers
extern const BDD quant_exists;
extern const BDD quant_forall;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize package (program level)
 */
void sylvan_package_init(int threads, int dq_size);

void sylvan_package_exit();

void sylvan_report_stats();

/**
 * Initialize BDD subsystem of package
 * datasize in number of nodes will be 1<<datasize
 * cachesize in number of nodes will be 1<<cachesize
 */
void sylvan_init(size_t datasize, size_t cachesize, int granularity);

/**
 * Stop threads and free data
 */
void sylvan_quit();

/**
 * Create a BDD representing <level>
 */
BDD sylvan_ithvar(BDDVAR level);

/**
 * Create a BDD representing ~<level>
 */
BDD sylvan_nithvar(BDDVAR level);

/**
 * Get the <level> of the root node of <bdd>
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
 * Get ~bdd
 */
BDD sylvan_not(BDD bdd);

/**
 * Calculate simple if <a> then <b> else <c> (calls sylvan_restructure)
 */
TASK_DECL_4(BDD, sylvan_ite, BDD, BDD, BDD, BDDVAR);
BDD sylvan_ite(BDD a, BDD b, BDD c);
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
 * Calculate \exists variables . a
 */
TASK_DECL_3(BDD, sylvan_exists, BDD, BDD, BDDVAR);
BDD sylvan_exists(BDD a, BDD variables);

/**
 * Calculate \forall variables . a
 */
TASK_DECL_3(BDD, sylvan_forall, BDD, BDD, BDDVAR);
BDD sylvan_forall(BDD a, BDD variables);

BDD sylvan_ref(BDD a);
void sylvan_deref(BDD a);
void sylvan_gc();

/**
 * Dangerous creation primitive.
 * May cause bad ordering.
 * Do not use except for debugging purposes!
 */
BDD sylvan_makenode(BDDVAR level, BDD low, BDD high);

/**
 * Send a dump of the BDD to stdout
 */
void sylvan_print(BDD bdd);
void sylvan_fprint(FILE *out, BDD bdd);

long long sylvan_count_refs();

/**
 * Calculate number of satisfying variable assignments.
 * <bdd> must only have variables in <variables>
 * <variables> is a BDD with every variable 'high' set to 'true'
 */
long double sylvan_satcount(BDD bdd, BDD variables);

TASK_DECL_1(long double, sylvan_pathcount, BDD);
long double sylvan_pathcount(BDD bdd);
uint64_t sylvan_nodecount(BDD a);
void sylvan_nodecount_levels(BDD bdd, uint32_t *variables);

/**
 * To store BDDs, just call sylvan_save_bdd for every BDD, and store
 * the results in a table somewhere.
 * Call sylvan_save_done when all BDDs are stored, this will update 
 * a counter and release all allocated memory.
 */
uint64_t sylvan_save_bdd(FILE* f, BDD bdd);
void sylvan_save_done(FILE *f);

/**
 * To load a set of stored BDDs, first call sylvan_load once, and then
 * call sylvan_load_translate for every value you stored from sylvan_save_done earlier.
 * Finally, call sylvan_load_done to release all allocated memory
 */
void sylvan_load(FILE *f);
BDD sylvan_load_translate(uint64_t bdd);
void sylvan_load_done();

#ifdef __cplusplus
}
#endif


#endif


