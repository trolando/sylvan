/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * This is an implementation of Multi-Terminal Binary Decision Diagrams.
 * They encode functions on Boolean variables to any domain.
 *
 * Three domains are supported by default: Boolean, Integer and Real.
 * Boolean MTBDDs are identical to BDDs (as supported by the bdd subpackage).
 * Integer MTBDDs are encoded using "int64_t" terminals.
 * Real MTBDDs are encoded using "double" terminals.
 *
 * Labels of Boolean variables of MTBDD nodes are 24-bit integers.
 *
 * Custom terminals are supported.
 *
 * Terminal type "0" is the Integer type, type "1" is the Real type.
 * Type "2" is the Fraction type, consisting of two 32-bit integers (numerator and denominator)
 * For non-Boolean MTBDDs, mtbdd_false is used for partial functions, i.e. mtbdd_false
 * indicates that the function is not defined for a certain input.
 */

/* Do not include this file directly. Instead, include sylvan.h */

#ifndef SYLVAN_MTBDD_H
#define SYLVAN_MTBDD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * An MTBDD is a 64-bit value. The low 40 bits are an index into the unique table.
 * The highest 1 bit is the complement edge, indicating negation.
 *
 * Currently, negation using complement edges is only implemented for Boolean MTBDDs.
 * For Integer/Real MTBDDs, negation is not well-defined, as "-0" = "0".
 *
 * A MTBDD node has 24 bits for the variable.
 * A set of MTBDD variables is represented by the MTBDD of the conjunction of these variables.
 * A MTBDDMAP uses special "MAP" nodes in the MTBDD nodes table.
 */
typedef uint64_t MTBDD;
typedef MTBDD MTBDDMAP;

/**
 * mtbdd_true and mtbdd_false are the Boolean leaves representing True and False.
 * False is also used in Integer/Real/Fraction MTBDDs for partially defined functions.
 */
static const MTBDD mtbdd_complement = 0x8000000000000000LL;
static const MTBDD mtbdd_false      = 0;
static const MTBDD mtbdd_true       = 0x8000000000000000LL;
static const MTBDD mtbdd_invalid    = 0xffffffffffffffffLL;

/**
 * Definitions for backward compatibility...
 * We now consider BDDs to be a special case of MTBDDs.
 */
typedef MTBDD BDD;
typedef MTBDDMAP BDDMAP;
typedef MTBDD BDDSET;
typedef uint32_t BDDVAR;
static const MTBDD sylvan_complement = 0x8000000000000000LL;
static const MTBDD sylvan_false      = 0;
static const MTBDD sylvan_true       = 0x8000000000000000LL;
static const MTBDD sylvan_invalid    = 0xffffffffffffffffLL;
#define sylvan_init_bdd         sylvan_init_mtbdd
#define sylvan_ref              mtbdd_ref
#define sylvan_deref            mtbdd_deref
#define sylvan_count_refs       mtbdd_count_refs
#define sylvan_protect          mtbdd_protect
#define sylvan_unprotect        mtbdd_unprotect
#define sylvan_count_protected  mtbdd_count_protected
#define sylvan_gc_mark_rec      mtbdd_gc_mark_rec
#define sylvan_ithvar           mtbdd_ithvar
#define bdd_refs_pushptr        mtbdd_refs_pushptr
#define bdd_refs_popptr         mtbdd_refs_popptr
#define bdd_refs_push           mtbdd_refs_push
#define bdd_refs_pop            mtbdd_refs_pop
#define bdd_refs_spawn          mtbdd_refs_spawn
#define bdd_refs_sync           mtbdd_refs_sync
#define sylvan_map_empty        mtbdd_map_empty
#define sylvan_map_isempty      mtbdd_map_isempty
#define sylvan_map_key          mtbdd_map_key
#define sylvan_map_value        mtbdd_map_value
#define sylvan_map_next         mtbdd_map_next
#define sylvan_map_contains     mtbdd_map_contains
#define sylvan_map_count        mtbdd_map_count
#define sylvan_map_add          mtbdd_map_add
#define sylvan_map_addall       mtbdd_map_addall
#define sylvan_map_remove       mtbdd_map_remove
#define sylvan_map_removeall    mtbdd_map_removeall
#define sylvan_set_empty        mtbdd_set_empty
#define sylvan_set_isempty      mtbdd_set_isempty
#define sylvan_set_add          mtbdd_set_add
#define sylvan_set_addall       mtbdd_set_addall
#define sylvan_set_remove       mtbdd_set_remove
#define sylvan_set_removeall    mtbdd_set_removeall
#define sylvan_set_first        mtbdd_set_first
#define sylvan_set_next         mtbdd_set_next
#define sylvan_set_fromarray    mtbdd_set_fromarray
#define sylvan_set_toarray      mtbdd_set_toarray
#define sylvan_set_in           mtbdd_set_in
#define sylvan_set_count        mtbdd_set_count
#define sylvan_test_isset       mtbdd_test_isset
#define sylvan_var              mtbdd_getvar
#define sylvan_low              mtbdd_getlow
#define sylvan_high             mtbdd_gethigh
#define sylvan_makenode         mtbdd_makenode
#define sylvan_makemapnode      mtbdd_makemapnode
#define sylvan_support          mtbdd_support
#define sylvan_test_isbdd       mtbdd_test_isvalid
#define sylvan_nodecount        mtbdd_nodecount
#define sylvan_printdot         mtbdd_printdot
#define sylvan_fprintdot        mtbdd_fprintdot
#define sylvan_printsha         mtbdd_printsha
#define sylvan_fprintsha        mtbdd_fprintsha
#define sylvan_getsha           mtbdd_getsha

/**
 * Initialize MTBDD functionality.
 * This initializes internal and external referencing datastructures,
 * and registers them in the garbage collection framework.
 */
void sylvan_init_mtbdd(void);

/**
 * Create a MTBDD terminal of type <type> and value <value>.
 * For custom types, the value could be a pointer to some external struct.
 */
MTBDD mtbdd_makeleaf(uint32_t type, uint64_t value);

/**
 * Create an internal MTBDD node of Boolean variable <var>, with low edge <low> and high edge <high>.
 * <var> is a 24-bit integer.
 * Please note that this does NOT check variable ordering!
 */
MTBDD _mtbdd_makenode(uint32_t var, MTBDD low, MTBDD high);
static inline MTBDD mtbdd_makenode(uint32_t var, MTBDD low, MTBDD high)
{
    return low == high ? low : _mtbdd_makenode(var, low, high);
}

/**
 * Return 1 if the MTBDD is a terminal, or 0 otherwise.
 */
int mtbdd_isleaf(MTBDD mtbdd);

/**
 * Return 1 if the MTBDD is an internal node, or 0 otherwise.
 */
static inline int mtbdd_isnode(MTBDD mtbdd) { return mtbdd_isleaf(mtbdd) ? 0 : 1; }

/**
 * Return the <type> field of the given leaf.
 */
uint32_t mtbdd_gettype(MTBDD leaf);

/**
 * Return the <value> field of the given leaf.
 */
uint64_t mtbdd_getvalue(MTBDD leaf);

/**
 * Return the variable field of the given internal node.
 */
uint32_t mtbdd_getvar(MTBDD node);

/**
 * Follow the low/false edge of the given internal node.
 * Also takes complement edges into account.
 */
MTBDD mtbdd_getlow(MTBDD node);

/**
 * Follow the high/true edge of the given internal node.
 * Also takes complement edges into account.
 */
MTBDD mtbdd_gethigh(MTBDD node);

/**
 * Obtain the complement of the MTBDD.
 * This is only valid for Boolean MTBDDs or custom implementations that support it.
 */

static inline int
mtbdd_hascomp(MTBDD dd)
{
    return (dd & mtbdd_complement) ? 1 : 0;
}

static inline MTBDD
mtbdd_comp(MTBDD dd)
{
    return dd ^ mtbdd_complement;
}

static inline MTBDD
mtbdd_not(MTBDD dd)
{
    return dd ^ mtbdd_complement;
}

/**
 * Create an Integer leaf with the given value.
 */
MTBDD mtbdd_int64(int64_t value);

/**
 * Create a Real leaf with the given value.
 */
MTBDD mtbdd_double(double value);

/**
 * Create a Fraction leaf with the given numerator and denominator.
 */
MTBDD mtbdd_fraction(int64_t numer, uint64_t denom);

/**
 * Obtain the value of an Integer leaf.
 */
int64_t mtbdd_getint64(MTBDD terminal);

/**
 * Obtain the value of a Real leaf.
 */
double mtbdd_getdouble(MTBDD terminal);

/**
 * Obtain the numerator of a Fraction leaf.
 */
static inline int32_t
mtbdd_getnumer(MTBDD terminal)
{
    return (int32_t)(mtbdd_getvalue(terminal)>>32);
}

/**
 * Obtain the denominator of a Fraction leaf.
 */
static inline uint32_t
mtbdd_getdenom(MTBDD terminal)
{
    return (uint32_t)(mtbdd_getvalue(terminal)&0xffffffff);
}

/**
 * Create the Boolean MTBDD representing "if <var> then True else False"
 */
MTBDD mtbdd_ithvar(uint32_t var);

/**
 * Functions to manipulate sets of MTBDD variables.
 *
 * A set of variables is represented by a cube/conjunction of (positive) variables.
 */
static inline MTBDD
mtbdd_set_empty()
{
    return mtbdd_true;
}

static inline int
mtbdd_set_isempty(MTBDD set)
{
    return (set == mtbdd_true) ? 1 : 0;
}

static inline uint32_t
mtbdd_set_first(MTBDD set)
{
    return mtbdd_getvar(set);
}

static inline MTBDD
mtbdd_set_next(MTBDD set)
{
    return mtbdd_gethigh(set);
}

/**
 * Create a set of variables, represented as the conjunction of (positive) variables.
 */
MTBDD mtbdd_set_from_array(uint32_t* arr, size_t length);

/**
 * Write all variables in a variable set to the given array.
 * The array must be sufficiently large.
 */
void mtbdd_set_to_array(MTBDD set, uint32_t *arr);

/**
 * Compute the number of variables in a given set of variables.
 */
size_t mtbdd_set_count(MTBDD set);

/**
 * Compute the union of <set1> and <set2>
 */
//FIXME
static inline BDD sylvan_and(BDD, BDD, BDDVAR);

static inline MTBDD mtbdd_set_union(MTBDD set1, MTBDD set2)
{
    return sylvan_and(set1, set2, 0);
}

/**
 * Remove variables in <set2> from <set1>
 */
TASK_2(MTBDD, mtbdd_set_minus, MTBDD, set1, MTBDD, set2)

/**
 * Return 1 if <set> contains <var>, 0 otherwise.
 */
int mtbdd_set_contains(MTBDD set, uint32_t var);

/**
 * Add the variable <var> to <set>.
 */
MTBDD mtbdd_set_add(MTBDD set, uint32_t var);

/**
 * Remove the variable <var> from <set>.
 */
MTBDD mtbdd_set_remove(MTBDD set, uint32_t var);

/**
 * Sanity check if the given MTBDD is a conjunction of positive variables,
 * and if all nodes are marked in the nodes table (detects violations after garbage collection).
 */
void mtbdd_test_isset(MTBDD set);

/**
 * Definitions for backwards compatibility
 */
// FIXME throw awaaaaay
#define mtbdd_fromarray mtbdd_set_from_array
#define mtbdd_set_fromarray mtbdd_set_from_array
#define mtbdd_set_toarray mtbdd_set_to_array
#define mtbdd_set_addall mtbdd_set_union
#define mtbdd_set_removeall mtbdd_set_minus
#define mtbdd_set_in mtbdd_set_contains

/**
 * Create a MTBDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * Use cube[idx]==3 for "s=s'" in interleaved variables (matches with next variable)
 * <variables> is the cube of variables (var1 \and var2 \and ... \and varn)
 */
MTBDD mtbdd_cube(MTBDD variables, uint8_t *cube, MTBDD terminal);

/**
 * Same as mtbdd_cube, but extends <mtbdd> with the assignment <cube> \to <terminal>.
 * If <mtbdd> already assigns a value to the cube, the new value <terminal> is taken.
 * Does not support cube[idx]==3.
 */
TASK_4(BDD, mtbdd_union_cube, MTBDD, mtbdd, MTBDD, variables, uint8_t*, cube, MTBDD, terminal)

/**
 * Count the number of satisfying assignments (minterms) leading to a non-false leaf
 */
TASK_2(double, mtbdd_satcount, MTBDD, dd, size_t, nvars);

/**
 * Count the number of MTBDD leaves (excluding mtbdd_false and mtbdd_true) in the given <count> MTBDDs
 */
size_t mtbdd_leafcount_more(const MTBDD *mtbdds, size_t count);

static inline size_t mtbdd_leafcount(MTBDD dd)
{
    return mtbdd_leafcount_more(&dd, 1);
}

/**
 * Count the number of MTBDD nodes and terminals (excluding mtbdd_false and mtbdd_true) in the given <count> MTBDDs
 */
size_t mtbdd_nodecount_more(const MTBDD *mtbdds, size_t count);

static inline size_t
mtbdd_nodecount(const MTBDD dd) {
    return mtbdd_nodecount_more(&dd, 1);
}

/**
 * Callback function types for binary ("dyadic") and unary ("monadic") operations.
 * The callback function returns either the MTBDD that is the result of applying op to the MTBDDs,
 * or mtbdd_invalid if op cannot be applied.
 * The binary function may swap the two parameters (if commutative) to improve caching.
 * The unary function is allowed an extra parameter (be careful of caching)
 */
typedef MTBDD (*mtbdd_apply_op)(lace_worker* lace, MTBDD*, MTBDD*);
typedef MTBDD (*mtbdd_applyp_op)(lace_worker* lace, MTBDD*, MTBDD*, size_t);
typedef MTBDD (*mtbdd_uapply_op)(lace_worker* lace, MTBDD, size_t);

/**
 * Apply a binary operation <op> to <a> and <b>.
 * Callback <op> is consulted before the cache, thus the application to terminals is not cached.
 */
TASK_3(MTBDD, mtbdd_apply, MTBDD, a, MTBDD, b, mtbdd_apply_op, op);

/**
 * Apply a binary operation <op> with id <opid> to <a> and <b> with parameter <p>
 * Callback <op> is consulted before the cache, thus the application to terminals is not cached.
 */
TASK_5(MTBDD, mtbdd_applyp, MTBDD, a, MTBDD, b, size_t, p, mtbdd_applyp_op, op, uint64_t, opid);

/**
 * Apply a unary operation <op> to <dd>.
 * Callback <op> is consulted after the cache, thus the application to a terminal is cached.
 */
TASK_3(MTBDD, mtbdd_uapply, MTBDD, dd, mtbdd_uapply_op, op, size_t, param);

/**
 * Callback function types for abstraction.
 * MTBDD mtbdd_abstract_op(MTBDD a, MTBDD b, int k).
 * The function is either called with k==0 (apply to two arguments) or k>0 (k skipped BDD variables)
 * k == 0  =>  res := apply op to a and b
 * k  > 0  =>  res := apply op to op(a, a, k-1) and op(a, a, k-1)
 */
typedef MTBDD (*mtbdd_abstract_op)(lace_worker*, MTBDD, MTBDD, int);

/**
 * Abstract the variables in <v> from <a> using the binary operation <op>.
 */
TASK_3(MTBDD, mtbdd_abstract, MTBDD, a, MTBDD, v, mtbdd_abstract_op, op);

/**
 * Unary operation Negate.
 * Supported domains: Integer, Real, Fraction
 */
TASK_2(MTBDD, mtbdd_op_negate, MTBDD, a, size_t, param);

/**
 * Unary opeation Complement.
 * Supported domains: Integer, Real, Fraction
 */
TASK_2(MTBDD, mtbdd_op_cmpl, MTBDD, a, size_t, param);

/**
 * Binary operation Plus (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDDs, mtbdd_false is interpreted as "0" or "0.0".
 */
TASK_2(MTBDD, mtbdd_op_plus, MTBDD*, a, MTBDD*, b);
TASK_3(MTBDD, mtbdd_abstract_op_plus, MTBDD, a, MTBDD, b, int, c);

/**
 * Binary operation Minus (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDDs, mtbdd_false is interpreted as "0" or "0.0".
 */
TASK_2(MTBDD, mtbdd_op_minus, MTBDD*, a, MTBDD*, b);

/**
 * Binary operation Times (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_false (not defined),
 * then the result is mtbdd_false (i.e. not defined).
 */
TASK_2(MTBDD, mtbdd_op_times, MTBDD*, a, MTBDD*, b);
TASK_3(MTBDD, mtbdd_abstract_op_times, MTBDD, a, MTBDD, b, int, c);

/**
 * Binary operation Minimum (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_false (not defined),
 * then the result is the other operand.
 */
TASK_2(MTBDD, mtbdd_op_min, MTBDD*, a, MTBDD*, b);
TASK_3(MTBDD, mtbdd_abstract_op_min, MTBDD, a, MTBDD, b, int, c);

/**
 * Binary operation Maximum (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_false (not defined),
 * then the result is the other operand.
 */
TASK_2(MTBDD, mtbdd_op_max, MTBDD*, a, MTBDD*, b);
TASK_3(MTBDD, mtbdd_abstract_op_max, MTBDD, a, MTBDD, b, int, c);

/**
 * Compute -a
 * (negation, where 0 stays 0, and x into -x)
 */
static inline MTBDD mtbdd_negate(MTBDD a)
{
    return mtbdd_uapply(a, mtbdd_op_negate_CALL, 0);
}

/**
 * Compute ~a for partial MTBDDs.
 * Does not negate Boolean True/False.
 * (complement, where 0 is turned into 1, and non-0 into 0)
 */
static inline MTBDD mtbdd_cmpl(MTBDD dd)
{
    return mtbdd_uapply(dd, mtbdd_op_cmpl_CALL, 0);
}

/**
 * Compute a + b
 */
static inline MTBDD mtbdd_plus(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, mtbdd_op_plus_CALL);
}

/**
 * Compute a - b
 */
static inline MTBDD mtbdd_minus(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, mtbdd_op_minus_CALL);
}

/**
 * Compute a * b
 */
static inline MTBDD mtbdd_times(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, mtbdd_op_times_CALL);
}

/**
 * Compute min(a, b)
 */
static inline MTBDD mtbdd_min(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, mtbdd_op_min_CALL);
}

/**
 * Compute max(a, b)
 */
static inline MTBDD mtbdd_max(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, mtbdd_op_max_CALL);
}

/**
 * Abstract the variables in <v> from <a> by taking the sum of all values
 */
static inline MTBDD mtbdd_abstract_plus(MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(dd, vars, mtbdd_abstract_op_plus_CALL);
}

/**
 * Abstract the variables in <v> from <a> by taking the product of all values
 */
static inline MTBDD mtbdd_abstract_times(MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(dd, vars, mtbdd_abstract_op_times_CALL);
}

/**
 * Abstract the variables in <v> from <a> by taking the minimum of all values
 */
static inline MTBDD mtbdd_abstract_min(MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(dd, vars, mtbdd_abstract_op_min_CALL);
}

/**
 * Abstract the variables in <v> from <a> by taking the maximum of all values
 */
static inline MTBDD mtbdd_abstract_max(MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(dd, vars, mtbdd_abstract_op_max_CALL);
}

/**
 * Compute IF <f> THEN <g> ELSE <h>.
 * <f> must be a Boolean MTBDD (or standard BDD).
 */
TASK_3(MTBDD, mtbdd_ite, MTBDD, f, MTBDD, g, MTBDD, h);

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 */
TASK_3(MTBDD, mtbdd_and_abstract_plus, MTBDD, a, MTBDD, b, MTBDD, c);

/**
 * Multiply <a> and <b>, and abstract variables <vars> by taking the maximum.
 */
TASK_3(MTBDD, mtbdd_and_abstract_max, MTBDD, a, MTBDD, b, MTBDD, c);

/**
 * Monad that converts double to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 */
TASK_2(MTBDD, mtbdd_op_threshold_double, MTBDD, a, size_t, b)

/**
 * Monad that converts double to a Boolean MTBDD, translate terminals > value to 1 and to 0 otherwise;
 */
TASK_2(MTBDD, mtbdd_op_strict_threshold_double, MTBDD, a, size_t, b)

/**
 * Convert double to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 */
TASK_2(MTBDD, mtbdd_threshold_double, MTBDD, a, double, b);

/**
 * Convert double to a Boolean MTBDD, translate terminals > value to 1 and to 0 otherwise;
 */
TASK_2(MTBDD, mtbdd_strict_threshold_double, MTBDD, a, double, b);

/**
 * For two Double MTBDDs, calculate whether they are equal module some value epsilon
 * i.e. abs(a-b) < e
 */
TASK_3(MTBDD, mtbdd_equal_norm_d, MTBDD, a, MTBDD, b, double, c);

/**
 * For two Double MTBDDs, calculate whether they are equal modulo some value epsilon
 * This version computes the relative difference vs the value in a.
 * i.e. abs((a-b)/a) < e
 */
TASK_3(MTBDD, mtbdd_equal_norm_rel_d, MTBDD, a, MTBDD, b, double, c);

/**
 * For two MTBDDs a, b, return mtbdd_true if all common assignments a(s) <= b(s), mtbdd_false otherwise.
 * For domains not in a / b, assume True.
 */
TASK_2(MTBDD, mtbdd_leq, MTBDD, a, MTBDD, b);

/**
 * For two MTBDDs a, b, return mtbdd_true if all common assignments a(s) < b(s), mtbdd_false otherwise.
 * For domains not in a / b, assume True.
 */
TASK_2(MTBDD, mtbdd_less, MTBDD, a, MTBDD, b);

/**
 * For two MTBDDs a, b, return mtbdd_true if all common assignments a(s) >= b(s), mtbdd_false otherwise.
 * For domains not in a / b, assume True.
 */
TASK_2(MTBDD, mtbdd_geq, MTBDD, a, MTBDD, b);

/**
 * For two MTBDDs a, b, return mtbdd_true if all common assignments a(s) > b(s), mtbdd_false otherwise.
 * For domains not in a / b, assume True.
 */
TASK_2(MTBDD, mtbdd_greater, MTBDD, a, MTBDD, b);

/**
 * Calculate the support of a MTBDD, i.e. the cube of all variables that appear in the MTBDD nodes.
 */
TASK_1(MTBDD, mtbdd_support, MTBDD, dd);

/**
 * Function composition, for each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of mtbdd_ite(<value>, <low>, <high>).
 * Each <value> in <map> must be a Boolean MTBDD.
 */
TASK_2(MTBDD, mtbdd_compose, MTBDD, dd, MTBDDMAP, map);

/**
 * Compute minimal leaf in the MTBDD (for Integer, Double, Rational MTBDDs)
 */
TASK_1(MTBDD, mtbdd_minimum, MTBDD, dd);

/**
 * Compute maximal leaf in the MTBDD (for Integer, Double, Rational MTBDDs)
 */
TASK_1(MTBDD, mtbdd_maximum, MTBDD, dd);

/**
 * Given a MTBDD <dd> and a cube of variables <variables> expected in <dd>,
 * mtbdd_enum_first and mtbdd_enum_next enumerates the unique paths in <dd> that lead to a non-False leaf.
 * 
 * The function returns the leaf (or mtbdd_false if no new path is found) and encodes the path
 * in the supplied array <arr>: 0 for a low edge, 1 for a high edge, and 2 if the variable is skipped.
 *
 * The supplied array <arr> must be large enough for all variables in <variables>.
 *
 * Usage:
 * MTBDD leaf = mtbdd_enum_first(dd, variables, arr, NULL);
 * while (leaf != mtbdd_false) {
 *     .... // do something with arr/leaf
 *     leaf = mtbdd_enum_next(dd, variables, arr, NULL);
 * }
 *
 * The callback is an optional function that returns 0 when the given terminal node should be skipped.
 */
typedef int (*mtbdd_enum_filter_cb)(MTBDD);
MTBDD mtbdd_enum_first(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb);
MTBDD mtbdd_enum_next(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb);

/**
 * Given an MTBDD <dd> and a cube of variables <variables> expected in <dd>,
 * mtbdd_enum_all_first and mtbdd_enum_all_next enumerate all satisfying assignments in <dd> that lead
 * to a non-False leaf.
 *
 * The functions return the leaf (or mtbdd_false if no new satisfying assignment is found) and encodes
 * the assignment in the supplied array <arr>, 0 for False and 1 for True.
 *
 * The supplied array <arr> must be large enough for all variables in <variables>.
 *
 * Usage:
 * MTBDD leaf = mtbdd_enum_first(dd, variables, arr, NULL);
 * while (leaf != mtbdd_false) {
 *     .... // do something with arr/leaf
 *     leaf = mtbdd_enum_next(dd, variables, arr, NULL);
 * }
 *
 * The callback is an optional function that returns 0 when the given terminal node should be skipped.
 */
MTBDD mtbdd_enum_all_first(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb);
MTBDD mtbdd_enum_all_next(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb);

/**
 * Given a MTBDD <dd>, call <cb> with context <context> for every unique path in <dd> ending in leaf <leaf>.
 *
 * Usage:
 * VOID_TASK_3(cb, mtbdd_enum_trace_t, trace, MTBDD, leaf, void*, context) { ... do something ... }
 * mtbdd_enum_par(dd, cb, context);
 */
typedef struct mtbdd_enum_trace {
    struct mtbdd_enum_trace *prev;
    uint32_t var;
    int val;  // 0 or 1
} * mtbdd_enum_trace_t;

typedef void (*mtbdd_enum_cb)(mtbdd_enum_trace_t, MTBDD, void*);
VOID_TASK_3(mtbdd_enum_par, MTBDD, dd, mtbdd_enum_cb, cb, void*, context);

/**
 * Function composition after partial evaluation.
 *
 * Given a function F(X) = f, compute the composition F'(X) = g(f) for every assignment to X.
 * All variables X in <vars> must appear before all variables in f and g(f).
 *
 * Usage:
 * TASK_2(MTBDD, g, MTBDD, in) { ... return g of <in> ... }
 * MTBDD x_vars = ...;  // the cube of variables x
 * MTBDD result = mtbdd_eval_compose(dd, x_vars, TASK(g));
 */
typedef MTBDD (*mtbdd_eval_compose_cb)(lace_worker* lace, MTBDD);
TASK_3(MTBDD, mtbdd_eval_compose, MTBDD, dd, MTBDD, vars, mtbdd_eval_compose_cb, cb);

/**
 * For debugging.
 * Tests if all nodes in the MTBDD are correctly ``marked'' in the nodes table.
 * Tests if variables in the internal nodes appear in-order.
 * In Debug mode, this will cause assertion failures instead of returning 0.
 * Returns 1 if all is fine, or 0 otherwise.
 */
TASK_1(int, mtbdd_test_isvalid, MTBDD, dd);

/**
 * Write a .dot representation of a given MTBDD
 * The callback function is required for custom terminals.
 */
void mtbdd_fprintdot(FILE *out, MTBDD mtbdd);
#define mtbdd_printdot(mtbdd, cb) mtbdd_fprintdot(stdout, mtbdd)

/**
 * Write a .dot representation of a given MTBDD, but without complement edges.
 */
void mtbdd_fprintdot_nc(FILE *out, MTBDD mtbdd);
#define mtbdd_printdot_nc(mtbdd, cb) mtbdd_fprintdot_nc(stdout, mtbdd)

/**
 * Write a text representation of a leaf to the given file.
 */
void mtbdd_fprint_leaf(FILE *out, MTBDD leaf);

/**
 * Write a text representation of a leaf to stdout.
 */
void mtbdd_print_leaf(MTBDD leaf);

/**
 * Obtain the textual representation of a leaf.
 * The returned result is either equal to the given <buf> (if the results fits)
 * or to a newly allocated array (with malloc).
 */
char *mtbdd_leaf_to_str(MTBDD leaf, char *buf, size_t buflen);

/**
 * Some debugging functions that generate SHA2 hashes of MTBDDs.
 * They are independent of where nodes are located in hash tables.
 * Note that they are not "perfect", but they can be useful to run easy sanity checks.
 */

/**
 * Print SHA2 hash to stdout.
 */
void mtbdd_printsha(MTBDD dd);

/**
 * Print SHA2 hash to given file.
 */
void mtbdd_fprintsha(FILE *f, MTBDD dd);

/**
 * Obtain SHA2 hash; target array must be at least 65 bytes long.
 */
void mtbdd_getsha(MTBDD dd, char *target);

/**
 * Visitor functionality for MTBDDs.
 * Visits internal nodes and leafs.
 */

/**
 * pre_cb callback: given input MTBDD and context,
 *                  return whether to visit children (if not leaf)
 * post_cb callback: given input MTBDD and context
 */
typedef int (*mtbdd_visit_pre_cb)(MTBDD dd, void* context);
typedef void (*mtbdd_visit_post_cb)(MTBDD dd, void* context);

/**
 * Sequential visit operation
 */
VOID_TASK_4(mtbdd_visit_seq, MTBDD, dd, mtbdd_visit_pre_cb, precb, mtbdd_visit_post_cb, postcb, void*, context);

/**
 * Parallel visit operation
 */
VOID_TASK_4(mtbdd_visit_par, MTBDD, dd, mtbdd_visit_pre_cb, precb, mtbdd_visit_post_cb, postcb, void*, context);

/**
 * Writing MTBDDs to file.
 *
 * Every node that is to be written is assigned a number, starting from 1,
 * such that reading the result in the future can be done in one pass.
 *
 * We use a skiplist to store the assignment.
 *
 * The functions mtbdd_writer_tobinary and mtbdd_writer_totext can be used to
 * store an array of MTBDDs to binary format or text format.
 *
 * One could also do the procedure manually instead.
 * - call mtbdd_writer_start to allocate the skiplist.
 * - call mtbdd_writer_add to add a given MTBDD to the skiplist
 * - call mtbdd_writer_writebinary to write all added nodes to a file
 * - OR:  mtbdd_writer_writetext to write all added nodes in text format
 * - call mtbdd_writer_get to obtain the MTBDD identifier as stored in the skiplist
 * - call mtbdd_writer_end to free the skiplist
 */

/**
 * Write <count> decision diagrams given in <dds> in internal binary form to <file>.
 *
 * The internal binary format is as follows, to store <count> decision diagrams...
 * uint64_t: nodecount -- number of nodes
 * <nodecount> times uint128_t: each leaf/node
 * uint64_t: count -- number of stored decision diagrams
 * <count> times uint64_t: each stored decision diagram
 */
VOID_TASK_3(mtbdd_writer_tobinary, FILE *, file, MTBDD *, dds, int, count);

/**
 * Write <count> decision diagrams given in <dds> in ASCII form to <file>.
 * Also supports custom leaves using the leaf_to_str callback.
 *
 * The text format writes in the same order as the binary format, except...
 * [
 *   node(id, var, low, high), -- for a normal node (no complement on high)
 *   node(id, var, low, ~high), -- for a normal node (complement on high)
 *   leaf(id, type, "value"), -- for a leaf (with value between "")
 * ],[dd1, dd2, dd3, ...,] -- and each the stored decision diagram.
 */

VOID_TASK_3(mtbdd_writer_totext, FILE *, file, MTBDD *, dds, int, count);

/**
 * Skeleton typedef for the skiplist
 */
typedef struct sylvan_skiplist *sylvan_skiplist_t;

/**
 * Allocate a skiplist for writing an MTBDD.
 */
sylvan_skiplist_t mtbdd_writer_start(void);

/**
 * Add the given MTBDD to the skiplist.
 */
VOID_TASK_2(mtbdd_writer_add, sylvan_skiplist_t, sl, MTBDD, dd);
#define mtbdd_writer_add(sl, dd) RUN(mtbdd_writer_add, sl, dd)

/**
 * Write all assigned MTBDD nodes in binary format to the file.
 */
void mtbdd_writer_writebinary(FILE *out, sylvan_skiplist_t sl);

/**
 * Retrieve the identifier of the given stored MTBDD.
 * This is useful if you want to be able to retrieve the stored MTBDD later.
 */
uint64_t mtbdd_writer_get(sylvan_skiplist_t sl, MTBDD dd);

/**
 * Free the allocated skiplist.
 */
void mtbdd_writer_end(sylvan_skiplist_t sl);

/**
 * Reading MTBDDs from file.
 *
 * The function mtbdd_reader_frombinary is basically the reverse of mtbdd_writer_tobinary.
 *
 * One can also perform the procedure manually.
 * - call mtbdd_reader_readbinary to read the nodes from file
 * - call mtbdd_reader_get to obtain the MTBDD for the given identifier as stored in the file.
 * - call mtbdd_reader_end to free the array returned by mtbdd_reader_readbinary
 *
 * Returns 0 if successful, -1 otherwise.
 */

/*
 * Read <count> decision diagrams to <dds> from <file> in internal binary form.
 */
TASK_3(int, mtbdd_reader_frombinary, FILE*, file, MTBDD*, dds, int, count);

/**
 * Reading a file earlier written with mtbdd_writer_writebinary
 * Returns an array with the conversion from stored identifier to MTBDD
 * This array is allocated with malloc and must be freed afterwards.
 * Returns NULL if there was an error.
 */

TASK_1(uint64_t*, mtbdd_reader_readbinary, FILE*, file);

/**
 * Retrieve the MTBDD of the given stored identifier.
 */
MTBDD mtbdd_reader_get(uint64_t* arr, uint64_t identifier);

/**
 * Free the allocated translation array
 */
void mtbdd_reader_end(uint64_t *arr);

/**
 * MTBDDMAP, maps uint32_t variables to MTBDDs.
 * A MTBDDMAP node has variable level, low edge going to the next MTBDDMAP, high edge to the mapped MTBDD.
 */
static inline MTBDD
mtbdd_map_empty()
{
    return mtbdd_false;
}

static inline int
mtbdd_map_isempty(MTBDD map)
{
    return (map == mtbdd_false) ? 1 : 0;
}

static inline uint32_t
mtbdd_map_key(MTBDD map)
{
    return mtbdd_getvar(map);
}

static inline MTBDD
mtbdd_map_value(MTBDD map)
{
    return mtbdd_gethigh(map);
}

static inline MTBDD
mtbdd_map_next(MTBDD map)
{
    return mtbdd_getlow(map);
}

/**
 * Return 1 if the map contains the key, 0 otherwise.
 */
int mtbdd_map_contains(MTBDDMAP map, uint32_t key);

/**
 * Retrieve the number of keys in the map.
 */
size_t mtbdd_map_count(MTBDDMAP map);

/**
 * Add the pair <key,value> to the map, overwrites if key already in map.
 */
MTBDDMAP mtbdd_map_add(MTBDDMAP map, uint32_t key, MTBDD value);

/**
 * Add all values from map2 to map1, overwrites if key already in map1.
 */
MTBDDMAP mtbdd_map_update(MTBDDMAP map1, MTBDDMAP map2);
#define mtbdd_map_addall mtbdd_map_update

/**
 * Remove the key <key> from the map and return the result
 */
MTBDDMAP mtbdd_map_remove(MTBDDMAP map, uint32_t key);

/**
 * Remove all keys in the cube <variables> from the map and return the result
 */
MTBDDMAP mtbdd_map_removeall(MTBDDMAP map, MTBDD variables);

/**
 * Garbage collection
 * Sylvan supplies two default methods to handle references to nodes, but the user
 * is encouraged to implement custom handling. Simply add a handler using sylvan_gc_add_mark
 * and let the handler call mtbdd_gc_mark_rec for every MTBDD that should be saved
 * during garbage collection.
 */

/**
 * Call mtbdd_gc_mark_rec for every mtbdd you want to keep in your custom mark functions.
 */
VOID_TASK_1(mtbdd_gc_mark_rec, MTBDD, dd);

/**
 * Infrastructure for external references using a hash table.
 * Two hash tables store external references: a pointers table and a values table.
 * The pointers table stores pointers to MTBDD variables, manipulated with protect and unprotect.
 * The values table stores MTBDDs, manipulated with ref and deref.
 * We strongly recommend using the pointers table whenever possible.
 */

/**
 * Store the pointer <ptr> in the pointers table.
 */
void mtbdd_protect(MTBDD* ptr);

/**
 * Delete the pointer <ptr> from the pointers table.
 */
void mtbdd_unprotect(MTBDD* ptr);

/**
 * Compute the number of pointers in the pointers table.
 */
size_t mtbdd_count_protected(void);

/**
 * Store the MTBDD <dd> in the values table.
 */
MTBDD mtbdd_ref(MTBDD dd);

/**
 * Delete the MTBDD <dd> from the values table.
 */
void mtbdd_deref(MTBDD dd);

/**
 * Compute the number of values in the values table.
 */
size_t mtbdd_count_refs(void);

/**
 * Infrastructure for internal references.
 * Every thread has its own reference stacks. There are three stacks: pointer, values, tasks stack.
 * The pointers stack stores pointers to MTBDD variables, manipulated with pushptr and popptr.
 * The values stack stores MTBDDs, manipulated with push and pop.
 * The tasks stack stores Lace tasks (that return MTBDDs), manipulated with spawn and sync.
 *
 * It is recommended to use the pointers stack for local variables and the tasks stack for tasks.
 */

/**
 * Push a MTBDD variable to the pointer reference stack.
 * During garbage collection the variable will be inspected and the contents will be marked.
 */
void mtbdd_refs_pushptr(const MTBDD *ptr);

/**
 * Pop the last <amount> MTBDD variables from the pointer reference stack.
 */
void mtbdd_refs_popptr(size_t amount);

/**
 * Push an MTBDD to the values reference stack.
 * During garbage collection the references MTBDD will be marked.
 */
MTBDD mtbdd_refs_push(MTBDD mtbdd);

/**
 * Pop the last <amount> MTBDDs from the values reference stack.
 */
void mtbdd_refs_pop(long amount);

/**
 * Push a Task that returns an MTBDD to the tasks reference stack.
 * Usage: mtbdd_refs_spawn(SPAWN(function, ...));
 */
void mtbdd_refs_spawn(lace_task* t);

/**
 * Pop a Task from the task reference stack.
 * Usage: MTBDD result = mtbdd_refs_sync(SYNC(function));
 */
MTBDD mtbdd_refs_sync(MTBDD mtbdd);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
