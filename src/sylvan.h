#include <stdint.h>

#ifndef SYLVAN_H
#define SYLVAN_H

typedef uint32_t BDD;
typedef uint32_t BDDLEVEL;

extern const BDD sylvan_true;
extern const BDD sylvan_false;

// use "BDD something = bddinvalid;" instead of "BDD something = 0;"
extern const BDD sylvan_invalid;

// Quantifiers
extern const BDD quant_exists;
extern const BDD quant_forall;
extern const BDD quant_unique;

enum sylvan_operators {
    operator_and = 0,
    operator_xor,
    operator_or,
    operator_nand,
    operator_nor,
    operator_imp,
    operator_biimp,
    operator_diff,
    operator_less,
    operator_invimp
};

typedef enum sylvan_operators sylvan_operator;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize BDD package
 * This will also create spinlocking threads!
 * datasize in number of nodes will be 1<<datasize
 * cachesize in number of nodes will be 1<<cachesize
 */
extern void sylvan_init(int threads, size_t datasize, size_t cachesize);

/**
 * Stop threads and free data
 */
extern void sylvan_quit();

/**
 * Create a BDD representing <level>
 */
extern BDD sylvan_ithvar(BDDLEVEL level);

/**
 * Create a BDD representing ~<level>
 */
extern BDD sylvan_nithvar(BDDLEVEL level);

/**
 * Get the <level> of the root node of <bdd>
 */
extern BDDLEVEL sylvan_var(BDD bdd);

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
 * Calculate if <a> then <b> else <c> and quantify/replace variables.
 * a,b,c must all be valid BDDs or bddtrue/bddfalse
 * Variables to replace are in pairs:
 * - replace 0 by replace[0]
 * - replace 1 by replace[1]
 * - replace n-1 by replace[n-1]
 * Replacement can be:
 * - a BDD  [use sylvan_ithvar(level) if necessary]
 * - sylvan_exists
 * - sylvan_forall
 * - sylvan_unique
 * - bddtrue/bddfalse
 */
extern BDD sylvan_restructure(BDD a, BDD b, BDD c, BDD* replace, size_t n);

/**
 * Apply operator (calls sylvan_restructure)
 */
extern BDD sylvan_apply(BDD a, BDD b, sylvan_operator op);

/**
 * Calculate simple if <a> then <b> else <c> (calls sylvan_restructure)
 */
extern BDD sylvan_ite(BDD a, BDD b, BDD c);

/**
 * Extended versions of sylvan_apply and sylvan_ite
 *
 * In this case, pairs is an array of n*2 elements
 * - replace pairs[0] by pairs[1]
 * - replace pairs[2] by pairs[3]
 * Replacement can be:
 * - a level
 * - sylvan_exists
 * - sylvan_forall
 */
extern BDD sylvan_apply_ex(BDD a, BDD b, sylvan_operator op, const BDDLEVEL* pairs, size_t n);
extern BDD sylvan_ite_ex(BDD a, BDD b, BDD c, const BDDLEVEL* pairs, size_t n);

/**
 * Replace variables in bdd
 * Calls sylvan_ite_ex(a, bddtrue, bddfalse, pairs, n)
 */
extern BDD sylvan_replace(BDD a, const BDDLEVEL *pairs, size_t n);

/**
 * Quantify variables in bdd
 * Calls sylvan_ite_ex(a, bddtrue, bddfalse, pairs, n)
 */
extern BDD sylvan_quantify(BDD a, const BDDLEVEL *pairs, size_t n);

/**
 * Dangerous creation primitive.
 * May cause bad ordering.
 * Do not use except for debugging purposes!
 */
extern BDD sylvan_makenode(BDDLEVEL level, BDD low, BDD high);

/**
 * Send a dump of the BDD to stdout
 */
extern void sylvan_print(BDD bdd);


#ifdef __cplusplus
}
#endif


#endif


