#include <stdint.h>
#include <stdio.h> // for FILE

#ifndef SYLVAN_H
#define SYLVAN_H

#ifndef CACHE
#define CACHE 1
#endif

#ifndef STATS
#define STATS 1
#endif

#ifndef COLORSTATS
#define COLORSTATS 1
#endif

typedef uint32_t BDD;
typedef uint16_t BDDVAR;
typedef uint32_t BDDOP; // bdd operation

extern const BDD sylvan_true;
extern const BDD sylvan_false;

// use "BDD something = bddinvalid;" instead of "BDD something = 0;"
extern const BDD sylvan_invalid;

// Quantifiers
extern const BDD quant_exists;
extern const BDD quant_forall;

// Would like to use #define statemens, but that is bad for reference counting
extern BDD sylvan_and(BDD a, BDD b);
extern BDD sylvan_xor(BDD a, BDD b);
extern BDD sylvan_or(BDD a, BDD b);
extern BDD sylvan_nand(BDD a, BDD b);
extern BDD sylvan_nor(BDD a, BDD b);
extern BDD sylvan_imp(BDD a, BDD b);
extern BDD sylvan_biimp(BDD a, BDD b);
extern BDD sylvan_diff(BDD a, BDD b);
extern BDD sylvan_less(BDD a, BDD b);
extern BDD sylvan_invimp(BDD a, BDD b);

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize package (program level)
 */
extern void sylvan_package_init();

extern void sylvan_package_exit();

extern void sylvan_report_stats();

/**
 * Initialize BDD subsystem of package
 * datasize in number of nodes will be 1<<datasize
 * cachesize in number of nodes will be 1<<cachesize
 */
extern void sylvan_init(size_t datasize, size_t cachesize, size_t data_gc_size, size_t cache_gc_size);

/**
 * Stop threads and free data
 */
extern void sylvan_quit();

/**
 * Create a BDD representing <level>
 */
extern BDD sylvan_ithvar(BDDVAR level);

/**
 * Create a BDD representing ~<level>
 */
extern BDD sylvan_nithvar(BDDVAR level);

/**
 * Get the <level> of the root node of <bdd>
 */
extern BDDVAR sylvan_var(BDD bdd);

/**
 * Get the <low> child of <bdd>
 */
extern BDD sylvan_low(BDD bdd);

/**
 * Get the <high> child of <bdd>
 */
extern BDD sylvan_high(BDD bdd);

/**
 * Get ~bdd
 */
extern BDD sylvan_not(BDD bdd);

/**
 * Calculate simple if <a> then <b> else <c> (calls sylvan_restructure)
 */
extern BDD sylvan_ite(BDD a, BDD b, BDD c);

extern BDD sylvan_relprods_partial(BDD a, BDD b, BDD excluded_variables);
extern BDD sylvan_relprods_reversed_partial(BDD a, BDD b, BDD excluded_variables);

extern BDD sylvan_relprods(BDD a, BDD b);
extern BDD sylvan_relprods_reversed(BDD a, BDD b);
extern BDD sylvan_exists(BDD a, BDD variables);
extern BDD sylvan_forall(BDD a, BDD variables);

extern BDD sylvan_ref(BDD a);
extern void sylvan_deref(BDD a);
extern void sylvan_gc();

/**
 * Dangerous creation primitive.
 * May cause bad ordering.
 * Do not use except for debugging purposes!
 */
extern BDD sylvan_makenode(BDDVAR level, BDD low, BDD high);

/**
 * Send a dump of the BDD to stdout
 */
extern void sylvan_print(BDD bdd);
extern void sylvan_fprint(FILE *out, BDD bdd);

extern long long sylvan_count_refs();

/**
 * Calculate number of satisfying variable assignments.
 * <bdd> must only have variables in <variables>
 * <variables> is a BDD with every variable 'high' set to 'true'
 */
extern long double sylvan_satcount(BDD bdd, BDD variables);

extern uint32_t sylvan_nodecount(BDD a);

#ifdef __cplusplus
}
#endif


#endif


