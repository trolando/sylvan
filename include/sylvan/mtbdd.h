/*
 * Copyright 2011-2016 Tom van Dijk, University of Twente
 * Copyright 2016-2018 Tom van Dijk, Johannes Kepler University Linz
 * Copyright 2019-2026 Tom van Dijk, University of Twente
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

#pragma once

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
typedef MTBDD BDD;
typedef MTBDD BDDMAP;
typedef MTBDD BDDSET;
typedef uint32_t BDDVAR;

/**
 * mtbdd_true and mtbdd_false are the Boolean leaves representing True and False.
 * False is also used in Integer/Real/Fraction MTBDDs for partially defined functions.
 */
static const MTBDD mtbdd_complement = 0x8000000000000000ULL;
static const MTBDD mtbdd_false      = 0;
static const MTBDD mtbdd_true       = 0x8000000000000000ULL;
static const MTBDD mtbdd_invalid    = 0xffffffffffffffffULL;

// TODO: there is one ambiguity here: "false" can mean "undefined" or it can actually mean the false terminal...


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
 * Create an internal MTBDD node of Boolean variable <var>,
 * with low edge <low> and high edge <high>.
 * <var> is a 24-bit integer.
 * Please note that this does NOT check variable ordering!
 * TODO: move this to internals! Users should use bdd_ite or mtbdd_ite.
 */
MTBDD _mtbdd_makenode(uint32_t var, MTBDD low, MTBDD high);

/**
 * Wrapper for _mtbdd_makenode (only if low <> high)
 */
static inline MTBDD mtbdd_makenode(uint32_t var, MTBDD low, MTBDD high);

/**
 * Return 1 if the MTBDD is a terminal, or 0 otherwise.
 */
int mtbdd_isleaf(MTBDD mtbdd);

/**
 * Return 1 if the MTBDD is an internal node, or 0 otherwise.
 */
static inline int mtbdd_isnode(MTBDD mtbdd);

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
 * Return 1 if the MTBDD has the complement bit set, 0 otherwise.
 * TODO: move to internal?
 */
static inline int mtbdd_hascomp(MTBDD dd);

/**
 * Return the MTBDD with complement bit toggled.
 * TODO: move to internal?
 */
static inline MTBDD mtbdd_comp(MTBDD dd);

/**
 * Return the negation of the MTBDD (toggle complement bit).
 * Only valid for Boolean MTBDDs or custom implementations that support it.
 */
static inline MTBDD mtbdd_not(MTBDD dd);

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
static inline int32_t mtbdd_getnumer(MTBDD terminal);

/**
 * Obtain the denominator of a Fraction leaf.
 */
static inline uint32_t mtbdd_getdenom(MTBDD terminal);

/**
 * Create the Boolean MTBDD representing "if <var> then True else False"
 */
MTBDD mtbdd_ithvar(uint32_t var);

/*
 * ========================
 * Variable set operations
 * ========================
 *
 * A set of variables is represented by a cube/conjunction of (positive) variables.
 */

 /**
  * Return the empty set of variables (equivalent to mtbdd_true).
  */
static inline MTBDD mtbdd_set_empty(void);

/**
 * Return 1 if the given set is empty, 0 otherwise.
 */
static inline int mtbdd_set_isempty(MTBDD set);

/**
 * Return the first (lowest) variable in the given set.
 */
static inline uint32_t mtbdd_set_first(MTBDD set);

/**
 * Return the set with the first variable removed.
 */
static inline MTBDD mtbdd_set_next(MTBDD set);

/**
 * Create a set of variables from an array of variable indices.
 */
MTBDD mtbdd_set_from_array(uint32_t* arr, size_t length);

/**
 * Write all variables in a variable set to the given array.
 * The array must be sufficiently large.
 */
void mtbdd_set_to_array(MTBDD set, uint32_t* arr);

/**
 * Compute the number of variables in a given set.
 */
size_t mtbdd_set_count(MTBDD set);

/**
 * Compute the union of two variable sets.
 */
static inline MTBDD mtbdd_set_union(MTBDD set1, MTBDD set2);

/**
 * Remove variables in <set2> from <set1>.
 */
static inline MTBDD mtbdd_set_minus(MTBDD set1, MTBDD set2);

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

/*
 * ========================
 * Cube operations
 * ========================
 */

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
static inline BDD mtbdd_union_cube(MTBDD mtbdd, MTBDD variables, uint8_t* cube, MTBDD terminal);

/*
 * ========================
 * Counting operations
 * ========================
 */

/**
 * Count the number of satisfying assignments (minterms) leading to a non-false leaf
 */
static inline double mtbdd_satcount(MTBDD dd, size_t nvars);

/**
 * Count the number of MTBDD leaves (excluding mtbdd_false and mtbdd_true)
 * in the given <count> MTBDDs.
 */
size_t mtbdd_leafcount_more(const MTBDD* mtbdds, size_t count);

/**
 * Count the number of leaves (excluding mtbdd_false and mtbdd_true) in a single MTBDD.
 */
static inline size_t mtbdd_leafcount(MTBDD dd);

/**
 * Count the number of MTBDD nodes and terminals (excluding mtbdd_false and mtbdd_true)
 * in the given <count> MTBDDs.
 */
size_t mtbdd_nodecount_more(const MTBDD* mtbdds, size_t count);

/**
 * Count the number of nodes and terminals (excluding mtbdd_false and mtbdd_true)
 * in a single MTBDD.
 */
static inline size_t mtbdd_nodecount(const MTBDD dd);

/*
 * ========================
 * Generic apply operations
 * ========================
 */

/**
 *Callback function type for binary("dyadic") operations.
 * Returns the MTBDD result, or mtbdd_invalid if the operation cannot be applied.
 * May swap parameters(if commutative) to improve caching.
 */
typedef MTBDD(*mtbdd_apply_op)(lace_worker* lace, MTBDD*, MTBDD*);

/**
 * Callback function type for binary operations with an extra parameter.
 * Returns the MTBDD result, or mtbdd_invalid if the operation cannot be applied.
 */
typedef MTBDD(*mtbdd_applyp_op)(lace_worker* lace, MTBDD*, MTBDD*, size_t);

/**
 * Callback function type for unary ("monadic") operations.
 * Allowed an extra parameter (be careful of caching).
 */
typedef MTBDD(*mtbdd_uapply_op)(lace_worker* lace, MTBDD, size_t);

/**
 * Callback function type for abstraction operations.
 *   k == 0: apply the operation to <a> and <b>
 *   k  > 0: apply the operation to op(a, a, k-1) and op(a, a, k-1), skipping k variables
 */
typedef MTBDD(*mtbdd_abstract_op)(lace_worker*, MTBDD, MTBDD, int);

/**
 * Apply a binary operation <op> to <a> and <b>.
 * Callback <op> is consulted before the cache, thus the application to terminals is not cached.
 */
static inline MTBDD mtbdd_apply(MTBDD a, MTBDD b, mtbdd_apply_op op);

/**
 * Apply a binary operation <op> to <a> and <b> with extra parameter <p>.
 * The <opid> is used as cache key to distinguish different operations.
 * Callback <op> is consulted before the cache, thus the application to terminals is not cached.
 */
static inline MTBDD mtbdd_applyp(MTBDD a, MTBDD b, size_t p, mtbdd_applyp_op op, uint64_t opid);

/**
 * Apply a unary operation <op> to <dd>.
 * Callback <op> is consulted after the cache, thus the application to a terminal is cached.
 */
static inline MTBDD mtbdd_uapply(MTBDD dd, mtbdd_uapply_op op, size_t param);

/**
 * Abstract the variables in <v> from <a> using the binary operation <op>.
 */
static inline MTBDD mtbdd_abstract(MTBDD a, MTBDD v, mtbdd_abstract_op op);

/*
 * ========================
 * Built-in leaf operations
 * ========================
 */

/**
  * Unary operation: negate the leaf value.
  * Supported domains: Integer, Real, Fraction.
  */
static inline MTBDD mtbdd_op_negate(MTBDD a, size_t param);

/**
 * Unary operation: complement a partial MTBDD.
 * Turns 0 into 1 and non-0 into 0. Does not negate Boolean True/False.
 * Supported domains: Integer, Real, Fraction.
 */
static inline MTBDD mtbdd_op_cmpl(MTBDD a, size_t param);

/**
 * Binary operation: addition.
 * Only for MTBDDs where all leaves are Boolean, Integer, or Double.
 * For Integer/Double MTBDDs, mtbdd_false is interpreted as "0" or "0.0".
 */
static inline MTBDD mtbdd_op_plus(MTBDD* a, MTBDD* b);

/**
 * Abstraction operator for summation. For use with mtbdd_abstract.
 */
static inline MTBDD mtbdd_abstract_op_plus(MTBDD a, MTBDD b, int c);

/**
 * Binary operation: subtraction.
 * Only for MTBDDs where all leaves are Boolean, Integer, or Double.
 * For Integer/Double MTBDDs, mtbdd_false is interpreted as "0" or "0.0".
 */
static inline MTBDD mtbdd_op_minus(MTBDD* a, MTBDD* b);

/**
 * Binary operation: multiplication.
 * Only for MTBDDs where all leaves are Boolean, Integer, or Double.
 * If either operand is mtbdd_false (not defined), the result is mtbdd_false.
 */
static inline MTBDD mtbdd_op_times(MTBDD* a, MTBDD* b);

/**
 * Abstraction operator for multiplication. For use with mtbdd_abstract.
 */
static inline MTBDD mtbdd_abstract_op_times(MTBDD a, MTBDD b, int c);

/**
 * Binary operation: minimum.
 * Only for MTBDDs where all leaves are Boolean, Integer, or Double.
 * If either operand is mtbdd_false (not defined), the result is the other operand.
 */
static inline MTBDD mtbdd_op_min(MTBDD* a, MTBDD* b);

/**
 * Abstraction operator for minimum. For use with mtbdd_abstract.
 */
static inline MTBDD mtbdd_abstract_op_min(MTBDD a, MTBDD b, int c);

/**
 * Binary operation: maximum.
 * Only for MTBDDs where all leaves are Boolean, Integer, or Double.
 * If either operand is mtbdd_false (not defined), the result is the other operand.
 */
static inline MTBDD mtbdd_op_max(MTBDD* a, MTBDD* b);

/**
 * Abstraction operator for maximum. For use with mtbdd_abstract.
 */
static inline MTBDD mtbdd_abstract_op_max(MTBDD a, MTBDD b, int c);

/*
 * ========================
 * Convenience wrappers
 * ========================
 */

/**
 * Compute -a (negate all leaf values; 0 stays 0).
 */
static inline MTBDD mtbdd_negate(MTBDD a);

/**
 * Compute complement of a partial MTBDD.
 * Turns 0 into 1 and non-0 into 0. Does not negate Boolean True/False.
 */
static inline MTBDD mtbdd_cmpl(MTBDD dd);

/**
 * Compute a + b.
 */
static inline MTBDD mtbdd_plus(MTBDD a, MTBDD b);

/**
 * Compute a - b.
 */
static inline MTBDD mtbdd_minus(MTBDD a, MTBDD b);

/**
 * Compute a * b.
 */
static inline MTBDD mtbdd_times(MTBDD a, MTBDD b);

/**
 * Compute min(a, b).
 */
static inline MTBDD mtbdd_min(MTBDD a, MTBDD b);

/**
 * Compute max(a, b).
 */
static inline MTBDD mtbdd_max(MTBDD a, MTBDD b);

/**
 * Abstract the variables in <vars> from <dd> by taking the sum of all values.
 */
static inline MTBDD mtbdd_abstract_plus(MTBDD dd, MTBDD vars);

/**
 * Abstract the variables in <vars> from <dd> by taking the product of all values.
 */
static inline MTBDD mtbdd_abstract_times(MTBDD dd, MTBDD vars);

/**
 * Abstract the variables in <vars> from <dd> by taking the minimum of all values.
 */
static inline MTBDD mtbdd_abstract_min(MTBDD dd, MTBDD vars);

/**
 * Abstract the variables in <vars> from <dd> by taking the maximum of all values.
 */
static inline MTBDD mtbdd_abstract_max(MTBDD dd, MTBDD vars);

/*
 * ========================
 * MTBDD operations
 * ========================
 */

/**
 * Compute IF <f> THEN <g> ELSE <h>.
 * <f> must be a Boolean MTBDD (or standard BDD).
 */
static inline MTBDD mtbdd_ite(MTBDD f, MTBDD g, MTBDD h);

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 */
static inline MTBDD mtbdd_and_abstract_plus(MTBDD a, MTBDD b, MTBDD vars);

/**
 * Multiply <a> and <b>, and abstract variables <vars> by taking the maximum.
 */
static inline MTBDD mtbdd_and_abstract_max(MTBDD a, MTBDD b, MTBDD vars);

/*
 * ========================
 * Threshold operations
 * ========================
 */

/**
 * Unary operator: convert Double MTBDD to Boolean, mapping terminals ≥ value to 1, else 0.
 * For use with mtbdd_uapply.
 */
static inline MTBDD mtbdd_op_threshold_double(MTBDD a, size_t b);

/**
 * Unary operator: convert Double MTBDD to Boolean, mapping terminals > value to 1, else 0.
 * For use with mtbdd_uapply.
 */
static inline MTBDD mtbdd_op_strict_threshold_double(MTBDD a, size_t b);

/**
 * Convert Double MTBDD to Boolean, mapping terminals ≥ value to 1, else 0.
 */
static inline MTBDD mtbdd_threshold_double(MTBDD a, double b);

/**
 * Convert Double MTBDD to Boolean, mapping terminals > value to 1, else 0.
 */
static inline MTBDD mtbdd_strict_threshold_double(MTBDD a, double b);

/*
 * ========================
 * Comparison operations
 * ========================
 */

/**
 * For two Double MTBDDs, test whether they are equal modulo some epsilon.
 * i.e. abs(a-b) < epsilon.
 */
static inline MTBDD mtbdd_equal_norm_d(MTBDD a, MTBDD b, double epsilon);

/**
 * For two Double MTBDDs, test whether they are equal modulo some relative epsilon.
 * i.e. abs((a-b)/a) < epsilon.
 */
static inline MTBDD mtbdd_equal_norm_rel_d(MTBDD a, MTBDD b, double epsilon);

/**
 * For two MTBDDs a, b, return mtbdd_true if for all common assignments a(s) ≤ b(s).
 * For domains not in a or b, assume True.
 */
static inline MTBDD mtbdd_leq(MTBDD a, MTBDD b);

/**
 * For two MTBDDs a, b, return mtbdd_true if for all common assignments a(s) < b(s).
 * For domains not in a or b, assume True.
 */
static inline MTBDD mtbdd_less(MTBDD a, MTBDD b);

/**
 * For two MTBDDs a, b, return mtbdd_true if for all common assignments a(s) ≥ b(s).
 * For domains not in a or b, assume True.
 */
static inline MTBDD mtbdd_geq(MTBDD a, MTBDD b);

/**
 * For two MTBDDs a, b, return mtbdd_true if for all common assignments a(s) > b(s).
 * For domains not in a or b, assume True.
 */
static inline MTBDD mtbdd_greater(MTBDD a, MTBDD b);

/*
 * ========================
 * Support and composition
 * ========================
 */

/**
 * Calculate the support of an MTBDD, i.e. the cube of all variables that appear in the nodes.
 */
static inline MTBDD mtbdd_support(MTBDD dd);

/**
 * Function composition: for each node with variable <key> that has a <key,value> pair in <map>,
 * replace the node by the result of mtbdd_ite(<value>, <low>, <high>).
 * Each <value> in <map> must be a Boolean MTBDD.
 */
static inline MTBDD mtbdd_compose(MTBDD dd, MTBDDMAP map);

/**
 * Compute the minimal leaf in the MTBDD (for Integer, Double, Rational MTBDDs).
 */
static inline MTBDD mtbdd_minimum(MTBDD dd);

/**
 * Compute the maximal leaf in the MTBDD (for Integer, Double, Rational MTBDDs).
 */
static inline MTBDD mtbdd_maximum(MTBDD dd);

/*
 * ========================
 * Enumeration
 * ========================
 */

 /**
  * Optional filter callback for enumeration.
  * Return 0 to skip the given terminal node, non-zero to include it.
  */
typedef int (*mtbdd_enum_filter_cb)(MTBDD);

/**
 * Enumerate unique paths in <dd> leading to a non-False leaf.
 * The path is encoded in <arr>: 0 for low, 1 for high, 2 if the variable is skipped.
 * The array must be large enough for all variables in <variables>.
 * Returns the leaf, or mtbdd_false when no (more) paths are found.
 *
 * Usage:
 *   MTBDD leaf = mtbdd_enum_first(dd, variables, arr, NULL);
 *   while (leaf != mtbdd_false) {
 *       // ... do something with arr/leaf ...
 *       leaf = mtbdd_enum_next(dd, variables, arr, NULL);
 *   }
 */
MTBDD mtbdd_enum_first(MTBDD dd, MTBDD variables, uint8_t* arr, mtbdd_enum_filter_cb filter_cb);

/**
 * Continue enumeration after mtbdd_enum_first. See mtbdd_enum_first for details.
 */
MTBDD mtbdd_enum_next(MTBDD dd, MTBDD variables, uint8_t* arr, mtbdd_enum_filter_cb filter_cb);

/**
 * Enumerate all satisfying assignments in <dd> leading to a non-False leaf.
 * Unlike mtbdd_enum_first/next, every variable is set to 0 or 1 (no "don't care").
 * Returns the leaf, or mtbdd_false when no (more) assignments are found.
 *
 * Usage:
 *   MTBDD leaf = mtbdd_enum_all_first(dd, variables, arr, NULL);
 *   while (leaf != mtbdd_false) {
 *       // ... do something with arr/leaf ...
 *       leaf = mtbdd_enum_all_next(dd, variables, arr, NULL);
 *   }
 */
MTBDD mtbdd_enum_all_first(MTBDD dd, MTBDD variables, uint8_t* arr, mtbdd_enum_filter_cb filter_cb);

/**
 * Continue enumeration after mtbdd_enum_all_first. See mtbdd_enum_all_first for details.
 */
MTBDD mtbdd_enum_all_next(MTBDD dd, MTBDD variables, uint8_t* arr, mtbdd_enum_filter_cb filter_cb);

/**
 * A trace element for parallel enumeration.
 * Forms a linked list recording the path taken through the MTBDD.
 */
typedef struct mtbdd_enum_trace {
    struct mtbdd_enum_trace* prev;
    uint32_t var;
    int val;  // 0 or 1
} mtbdd_enum_trace;

/**
 * Callback type for parallel enumeration.
 */
typedef void (*mtbdd_enum_cb)(mtbdd_enum_trace*, MTBDD, void*);

/**
 * Enumerate all unique paths in <dd> in parallel using Lace tasks.
 * Calls <cb> for every path leading to a non-False leaf.
 */
static inline void mtbdd_enum_par(MTBDD dd, mtbdd_enum_cb cb, void* context);

/*
 * ========================
 * Eval-compose
 * ========================
 */

/**
 * Callback type for eval_compose: given an MTBDD, return the transformed MTBDD.
 */
typedef MTBDD(*mtbdd_eval_compose_cb)(lace_worker* lace, MTBDD);

/**
 * Function composition after partial evaluation.
 *
 * Given a function F(X) = f, compute the composition F'(X) = g(f) for every assignment to X.
 * All variables X in <vars> must appear before all variables in f and g(f).
 *
 * Usage:
 *   TASK_2(MTBDD, g, MTBDD, in) { ... return g of <in> ... }
 *   MTBDD x_vars = ...;  // the cube of variables x
 *   MTBDD result = mtbdd_eval_compose(dd, x_vars, TASK(g));
 */
static inline MTBDD mtbdd_eval_compose(MTBDD dd, MTBDD vars, mtbdd_eval_compose_cb cb);

/*
 * ========================
 * Validation and debugging
 * ========================
 */

/**
 * Tests if all nodes in the MTBDD are correctly marked in the nodes table
 * and if variables in internal nodes appear in-order.
 * In Debug mode, this will cause assertion failures instead of returning 0.
 * Returns 1 if all is fine, or 0 otherwise.
 */
static inline int mtbdd_test_isvalid(MTBDD dd);

/**
 * Write a .dot representation of a given MTBDD to the given file.
 */
void mtbdd_fprintdot(FILE* out, MTBDD mtbdd);

/**
 * Write a .dot representation of a given MTBDD to stdout.
 */
#define mtbdd_printdot(mtbdd) mtbdd_fprintdot(stdout, mtbdd)

 /**
  * Write a .dot representation without complement edges to the given file.
  */
void mtbdd_fprintdot_nc(FILE* out, MTBDD mtbdd);

/**
 * Write a .dot representation without complement edges to stdout.
 */
#define mtbdd_printdot_nc(mtbdd) mtbdd_fprintdot_nc(stdout, mtbdd)

/**
 * Write a text representation of a leaf to the given file.
 */
void mtbdd_fprint_leaf(FILE* out, MTBDD leaf);

/**
 * Write a text representation of a leaf to stdout.
 */
void mtbdd_print_leaf(MTBDD leaf);

/**
 * Obtain the textual representation of a leaf.
 * Returns <buf> if the result fits, or a newly malloc'd array otherwise.
 */
char* mtbdd_leaf_to_str(MTBDD leaf, char* buf, size_t buflen);

/**
 * Print a SHA2 hash of the MTBDD to stdout.
 * Independent of node placement in hash tables; useful for sanity checks.
 */
void mtbdd_printsha(MTBDD dd);

/**
 * Print a SHA2 hash of the MTBDD to the given file.
 */
void mtbdd_fprintsha(FILE* f, MTBDD dd);

/**
 * Compute a SHA2 hash of the MTBDD. Target array must be at least 65 bytes.
 */
void mtbdd_getsha(MTBDD dd, char* target);

/*
 * ========================
 * Visitor operations
 * ========================
 */

/**
 * Pre-visit callback: given an MTBDD and context,
 * return non-zero to visit children (if not a leaf), 0 to skip.
 */
typedef int (*mtbdd_visit_pre_cb)(MTBDD dd, void* context);

/**
 * Post-visit callback: given an MTBDD and context.
 */
typedef void (*mtbdd_visit_post_cb)(MTBDD dd, void* context);

/**
 * Sequential depth-first visit of all nodes in the MTBDD.
 */
static inline void mtbdd_visit_seq(MTBDD dd, mtbdd_visit_pre_cb precb, mtbdd_visit_post_cb postcb, void* context);

/**
 * Parallel depth-first visit of all nodes in the MTBDD using Lace tasks.
 */
static inline void mtbdd_visit_par(MTBDD dd, mtbdd_visit_pre_cb precb, mtbdd_visit_post_cb postcb, void* context);

/*
 * ========================
 * Binary I/O
 * ========================
 */

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
  * The internal binary format:
  *   uint64_t: nodecount (number of nodes)
  *   <nodecount> × uint128_t: each leaf/node
  *   uint64_t: count (number of stored decision diagrams)
  *   <count> × uint64_t: each stored decision diagram
  */
static inline void mtbdd_writer_tobinary(FILE* file, MTBDD* dds, int count);

/**
 * Write <count> decision diagrams given in <dds> in ASCII form to <file>.
 *
 * The text format:
 *   [
 *     node(id, var, low, high),   -- internal node (no complement on high)
 *     node(id, var, low, ~high),  -- internal node (complement on high)
 *     leaf(id, type, "value"),    -- leaf (with value between "")
 *   ],[dd1, dd2, dd3, ...,]      -- stored decision diagrams
 */
static inline void mtbdd_writer_totext(FILE* file, MTBDD* dds, int count);

/**
 * Skeleton typedef for the skiplist used by the writer.
 */
typedef struct sylvan_skiplist* sylvan_skiplist_t;

/**
 * Allocate a skiplist for writing MTBDDs.
 */
sylvan_skiplist_t mtbdd_writer_start(void);

/**
 * Add the given MTBDD to the skiplist for writing.
 */
static inline void mtbdd_writer_add(sylvan_skiplist_t sl, MTBDD dd);

/**
 * Write all assigned MTBDD nodes in binary format to the file.
 */
void mtbdd_writer_writebinary(FILE* out, sylvan_skiplist_t sl);

/**
 * Retrieve the identifier of the given stored MTBDD in the skiplist.
 */
uint64_t mtbdd_writer_get(sylvan_skiplist_t sl, MTBDD dd);

/**
 * Free the allocated skiplist.
 */
void mtbdd_writer_end(sylvan_skiplist_t sl);

/**
 * Read <count> decision diagrams from <file> in internal binary form into <dds>.
 * Returns 0 if successful, -1 otherwise.
 */
static inline int mtbdd_reader_frombinary(FILE* file, MTBDD* dds, int count);

/**
 * Read nodes from a file written with mtbdd_writer_writebinary.
 * Returns an array mapping stored identifiers to MTBDDs (allocated with malloc).
 * Returns NULL on error. The caller must free the returned array.
 */
static inline uint64_t* mtbdd_reader_readbinary(FILE* file);

/**
 * Retrieve the MTBDD for the given stored identifier.
 */
MTBDD mtbdd_reader_get(uint64_t* arr, uint64_t identifier);

/**
 * Free the translation array returned by mtbdd_reader_readbinary.
 */
void mtbdd_reader_end(uint64_t* arr);

/*
 * ========================
 * MTBDD map operations
 * ========================
 *
 * MTBDDMAP maps uint32_t variables to MTBDDs.
 * A MTBDDMAP node has a variable level, low edge going to the next entry, high edge to the mapped MTBDD.
 */

/**
 * Return the empty map.
 */
static inline MTBDD mtbdd_map_empty(void);

/**
 * Return 1 if the map is empty, 0 otherwise.
 */
static inline int mtbdd_map_isempty(MTBDD map);

/**
 * Return the key (variable) of the first entry in the map.
 */
static inline uint32_t mtbdd_map_key(MTBDD map);

/**
 * Return the value (MTBDD) of the first entry in the map.
 */
static inline MTBDD mtbdd_map_value(MTBDD map);

/**
 * Return the rest of the map (all entries after the first).
 */
static inline MTBDD mtbdd_map_next(MTBDD map);

/**
 * Return 1 if the map contains the key, 0 otherwise.
 */
int mtbdd_map_contains(MTBDDMAP map, uint32_t key);

/**
 * Return the number of entries in the map.
 */
size_t mtbdd_map_count(MTBDDMAP map);

/**
 * Add the pair <key,value> to the map. Overwrites if the key already exists.
 */
MTBDDMAP mtbdd_map_add(MTBDDMAP map, uint32_t key, MTBDD value);

/**
 * Add all entries from <map2> to <map1>. Overwrites keys already in <map1>.
 */
MTBDDMAP mtbdd_map_update(MTBDDMAP map1, MTBDDMAP map2);

/**
 * Remove the entry with <key> from the map.
 */
MTBDDMAP mtbdd_map_remove(MTBDDMAP map, uint32_t key);

/**
 * Remove all entries whose keys appear in the variable cube <variables>.
 */
MTBDDMAP mtbdd_map_removeall(MTBDDMAP map, MTBDD variables);

/*
 * ========================
 * Garbage collection
 * ========================
 *
 * Sylvan supplies two default methods to handle references to nodes, but the user
 * is encouraged to implement custom handling. Simply add a handler using sylvan_gc_add_mark
 * and let the handler call mtbdd_gc_mark_rec for every MTBDD that should be saved
 * during garbage collection.
 */

 /**
  * Recursively mark an MTBDD and all its children for garbage collection.
  * Call this for every MTBDD you want to keep in your custom mark functions.
  */
static inline void mtbdd_gc_mark_rec(MTBDD dd);

/*
 * ========================
 * External references
 * ========================
 *
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
 * Return the number of pointers in the pointers table.
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
 * Return the number of values in the values table.
 */
size_t mtbdd_count_refs(void);

/*
 * ========================
 * Internal references
 * ========================
 *
 * Every thread has its own reference stacks. There are three stacks: pointer, values, tasks.
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
void mtbdd_refs_pushptr(const MTBDD* ptr);

/**
 * Pop the last <amount> MTBDD variables from the pointer reference stack.
 */
void mtbdd_refs_popptr(size_t amount);

/**
 * Push an MTBDD to the values reference stack.
 * During garbage collection the referenced MTBDD will be marked.
 */
MTBDD mtbdd_refs_push(MTBDD mtbdd);

/**
 * Pop the last <amount> MTBDDs from the values reference stack.
 */
void mtbdd_refs_pop(long amount);

/**
 * Push a Lace task that returns an MTBDD to the tasks reference stack.
 * Usage: mtbdd_refs_spawn(SPAWN(function, ...));
 */
void mtbdd_refs_spawn(lace_task* t);

/**
 * Pop a task from the task reference stack.
 * Usage: MTBDD result = mtbdd_refs_sync(SYNC(function));
 */
MTBDD mtbdd_refs_sync(MTBDD mtbdd);

#ifdef __cplusplus
}
#endif /* __cplusplus */

