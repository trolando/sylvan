#include <stdint.h>

#ifndef SYLVAN_H
#define SYLVAN_H

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

#define sylvan_and(a, b)    sylvan_ite((a), (b), sylvan_false)
#define sylvan_xor(a, b)    sylvan_ite((a), sylvan_not(b), (b))
#define sylvan_or(a, b)     sylvan_ite((a), sylvan_true, (b))
#define sylvan_nand(a, b)   sylvan_ite((a), sylvan_not(b), sylvan_true)
#define sylvan_nor(a, b)    sylvan_ite((a), sylvan_false, sylvan_not(b))
#define sylvan_imp(a, b)    sylvan_ite((a), (b), sylvan_true)
#define sylvan_biimp(a, b)  sylvan_ite((a), (b), sylvan_not(b))
#define sylvan_diff(a, b)   sylvan_ite((a), sylvan_not(b), sylvan_false)
#define sylvan_less(a, b)   sylvan_ite((a), sylvan_false, (b))
#define sylvan_invimp(a, b) sylvan_ite((a), sylvan_true, sylvan_not(b))

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
//extern BDD sylvan_restructure(BDD a, BDD b, BDD c, BDD* replace, size_t n);
//extern BDD sylvan_restructure_st(BDD a, BDD b, BDD c, BDD* replace, size_t n);

/**
 * Calculate simple if <a> then <b> else <c> (calls sylvan_restructure)
 */
extern BDD sylvan_ite(BDD a, BDD b, BDD c);
//extern BDD sylvan_ite_st(BDD a, BDD b, BDD c);

extern BDD sylvan_relprods_partial(BDD a, BDD b, BDD variables);

extern BDD sylvan_relprods(BDD a, BDD b);
extern BDD sylvan_relprods_reversed(BDD a, BDD b);
extern BDD sylvan_exists(BDD a, BDD variables);
extern BDD sylvan_forall(BDD a, BDDVAR* variables, int size);

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
//extern BDD sylvan_ite_ex(BDD a, BDD b, BDD c, const BDDVAR* pairs, size_t n);

//extern BDD sylvan_ite_ex_st(BDD a, BDD b, BDD c, const BDDVAR* pairs, size_t n);

/**
 * Replace variables in bdd
 * Calls sylvan_ite_ex(a, bddtrue, bddfalse, pairs, n)
 */
//extern BDD sylvan_replace(BDD a, const BDDVAR *pairs, size_t n);
//extern BDD sylvan_replace_st(BDD a, const BDDVAR *pairs, size_t n);

/**
 * Quantify variables in bdd
 * Calls sylvan_ite_ex(a, bddtrue, bddfalse, pairs, n)
 */
//extern BDD sylvan_quantify(BDD a, const BDDVAR *pairs, size_t n);
//extern BDD sylvan_quantify_st(BDD a, const BDDVAR *pairs, size_t n);

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


