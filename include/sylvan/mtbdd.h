/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
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
 * For non-Boolean MTBDDs, mtbdd_undefined is used for partial functions, i.e. mtbdd_undefined
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
 *
 * A MTBDD node has 24 bits for the variable.
 * A set of MTBDD variables is represented by the MTBDD of the conjunction of these variables.
 * A MTBDDMAP uses special "MAP" nodes in the MTBDD nodes table.
 */
/**
 * The zero handle represents an undefined value in a partial MTBDD function.
 */
static const MTBDD mtbdd_undefined = 0;
static const MTBDD mtbdd_invalid = UINT64_MAX;

/**
 * Initialize MTBDD functionality.
 * This initializes internal and external referencing datastructures,
 * and registers them in the garbage collection framework.
 */
void mtbdd_init(void);

/**
 * Create a MTBDD terminal of type <type> and value <value>.
 * For custom types, the value could be a pointer to some external struct.
 */
MTBDD mtbdd_leaf(uint32_t type, uint64_t value);

/**
 * Create an internal MTBDD node of Boolean variable <var>, with low edge <low> and high edge <high>.
 * <var> is a 24-bit integer.
 * Please note that this does NOT check variable ordering!
 */
static inline MTBDD mtbdd_make_node(uint32_t var, MTBDD low, MTBDD high);

/**
 * Return 1 if the MTBDD is a terminal, or 0 otherwise.
 */
int mtbdd_is_leaf(MTBDD mtbdd);

/**
 * Return 1 if the MTBDD is an internal node, or 0 otherwise.
 */

/**
 * Return the <type> field of the given leaf.
 */
uint32_t mtbdd_leaf_type(MTBDD leaf);

/**
 * Return the <value> field of the given leaf.
 */
uint64_t mtbdd_leaf_value(MTBDD leaf);

/**
 * Return the variable field of the given internal node.
 */
uint32_t mtbdd_node_variable(MTBDD node);

/**
 * Follow the low/false edge of the given internal node.
 * Also takes complement edges into account.
 */
MTBDD mtbdd_node_low(MTBDD node);

/**
 * Follow the high/true edge of the given internal node.
 * Also takes complement edges into account.
 */
MTBDD mtbdd_node_high(MTBDD node);

/**
 * Obtain the complement of the MTBDD.
 * This is only valid for Boolean MTBDDs or custom implementations that support it.
 */

static inline int bdd_is_complemented(MTBDD dd);


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
 *
 * The fraction is reduced before it is stored. The reduced numerator must fit
 * in the range [-INT32_MAX, INT32_MAX], the reduced denominator must fit in a
 * uint32_t, and the denominator must not be zero. Returns mtbdd_invalid when
 * these requirements are not met.
 */
MTBDD mtbdd_fraction(int64_t numer, uint64_t denom);

/**
 * Obtain the value of an Integer leaf.
 */
int64_t mtbdd_leaf_int64(MTBDD terminal);

/**
 * Obtain the value of a Real leaf.
 */
double mtbdd_leaf_double(MTBDD terminal);

/**
 * Obtain both components of a Fraction leaf.
 * Returns 0 on success and -1 if <leaf> is not a Fraction leaf.
 */
int mtbdd_leaf_fraction(MTBDD leaf, int32_t *numerator, uint32_t *denominator);

/**
 * Obtain the complement-aware false and true root cofactors. For a leaf, both
 * outputs receive the leaf itself.
 */
void mtbdd_cofactors(MTBDD dd, MTBDD *if_false, MTBDD *if_true);

/**
 * Create a MTBDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * Use cube[idx]==3 for "s=s'" in interleaved variables (matches with next variable)
 * <variables> is the cube of variables (var1 \and var2 \and ... \and varn).
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
int mtbdd_cube(MTBDD *result, BDDSET variables, const uint8_t *cube, MTBDD terminal);

/**
 * Same as mtbdd_cube, but extends <mtbdd> with the assignment <cube> \to <terminal>.
 * If <mtbdd> already assigns a value to the cube, the new value <terminal> is taken.
 * Does not support cube[idx]==3. Returns SYLVAN_OK on success. On failure,
 * <result> is unchanged.
 */
static inline int mtbdd_set_cube(MTBDD *result, MTBDD mtbdd, BDDSET variables, const uint8_t *cube, MTBDD terminal);

/**
 * Count the number of satisfying assignments (minterms) leading to a non-false leaf
 */
static inline double mtbdd_sat_count(MTBDD dd, size_t nvars);

/**
 * Count the number of MTBDD leaves (excluding mtbdd_undefined and bdd_true) in the given <count> MTBDDs
 */
size_t mtbdd_shared_leaf_count(const MTBDD *mtbdds, size_t count);

static inline size_t mtbdd_leaf_count(MTBDD dd);

/**
 * Count the number of MTBDD nodes and terminals (excluding mtbdd_undefined and bdd_true) in the given <count> MTBDDs
 */
size_t mtbdd_shared_node_count(const MTBDD *mtbdds, size_t count);

static inline size_t mtbdd_node_count(const MTBDD dd);

/**
 * Callback function types for binary ("dyadic") and unary ("monadic") operations.
 * The callback writes its result to <result> and returns SYLVAN_OK when it
 * handles the operands, SYLVAN_APPLY_RECURSE when generic apply must descend,
 * or a negative status on failure. On recurse or failure, <result> is unchanged.
 * Sylvan protects the callback's <result> destination while the callback runs.
 * The binary function may swap the two parameters (if commutative) to improve caching.
 * The unary function is allowed an extra parameter (be careful of caching)
 */
typedef int (*mtbdd_apply_cb)(lace_worker* lace, MTBDD *result, MTBDD*, MTBDD*);
typedef int (*mtbdd_apply_param_cb)(lace_worker* lace, MTBDD *result, MTBDD*, MTBDD*, size_t);
typedef int (*mtbdd_apply_unary_cb)(lace_worker* lace, MTBDD *result, MTBDD, size_t);

/**
 * Apply a binary operation <op> to <a> and <b>.
 * Callback <op> is consulted before the cache, thus the application to terminals is not cached.
 * The caller must protect <result>. Returns SYLVAN_OK on success. On failure,
 * <result> is unchanged.
 */
static inline int mtbdd_apply(MTBDD *result, MTBDD a, MTBDD b, mtbdd_apply_cb op);

/**
 * Apply a binary operation <op> with id <opid> to <a> and <b> with parameter <p>
 * Callback <op> is consulted before the cache, thus the application to terminals is not cached.
 * The caller must protect <result>. Returns SYLVAN_OK on success. On failure,
 * <result> is unchanged.
 */
static inline int mtbdd_apply_param(MTBDD *result, MTBDD a, MTBDD b, size_t p, mtbdd_apply_param_cb op, uint64_t opid);

/**
 * Apply a unary operation <op> to <dd>.
 * Callback <op> is consulted after the cache, thus the application to a terminal is cached.
 * The caller must protect <result>. Returns SYLVAN_OK on success. On failure,
 * <result> is unchanged.
 */
static inline int mtbdd_apply_unary(MTBDD *result, MTBDD dd, mtbdd_apply_unary_cb op, size_t param);

/**
 * Callback function types for abstraction.
 * The function is either called with k==0 (apply to two arguments) or k>0 (k skipped BDD variables)
 * k == 0  =>  res := apply op to a and b
 * k  > 0  =>  res := apply op to op(a, a, k-1) and op(a, a, k-1)
 * The number of skipped variables must fit in a non-negative int. Built-in
 * abstraction operations process large values of k in size_t-width chunks.
 * The callback writes to Sylvan's protected <result> destination and returns
 * SYLVAN_OK on success or a negative status on failure. On failure, <result>
 * is unchanged.
 */
typedef int (*mtbdd_abstract_cb)(lace_worker*, MTBDD *result, MTBDD, MTBDD, int);

/**
 * Abstract the variables in <v> from <a> using the binary operation <op>.
 * The caller must protect <result>. Returns SYLVAN_OK on success or a negative
 * status on failure, leaving <result> unchanged.
 */
static inline int mtbdd_abstract(MTBDD *result, MTBDD a, MTBDD v, mtbdd_abstract_cb op);

/**
 * Unary operation Negate.
 * Supported domains: Integer, Real, Fraction
 */
static inline int mtbdd_op_negate(MTBDD *result, MTBDD a, size_t param);

/**
 * Unary opeation Complement.
 * Supported domains: Integer, Real, Fraction
 */
static inline int mtbdd_op_cmpl(MTBDD *result, MTBDD a, size_t param);

/**
 * Binary operation Plus (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDDs, mtbdd_undefined is interpreted as "0" or "0.0".
 */
static inline int mtbdd_op_plus(MTBDD *result, MTBDD *a, MTBDD *b);
static inline int mtbdd_abstract_op_plus(MTBDD *result, MTBDD a, MTBDD b, int c);

/**
 * Binary operation Minus (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDDs, mtbdd_undefined is interpreted as "0" or "0.0".
 */
static inline int mtbdd_op_minus(MTBDD *result, MTBDD *a, MTBDD *b);

/**
 * Binary operation Times (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is mtbdd_undefined (i.e. not defined).
 */
static inline int mtbdd_op_times(MTBDD *result, MTBDD *a, MTBDD *b);
static inline int mtbdd_abstract_op_times(MTBDD *result, MTBDD a, MTBDD b, int c);

/**
 * Binary operation Minimum (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is the other operand.
 */
static inline int mtbdd_op_min(MTBDD *result, MTBDD *a, MTBDD *b);
static inline int mtbdd_abstract_op_min(MTBDD *result, MTBDD a, MTBDD b, int c);

/**
 * Binary operation Maximum (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is the other operand.
 */
static inline int mtbdd_op_max(MTBDD *result, MTBDD *a, MTBDD *b);
static inline int mtbdd_abstract_op_max(MTBDD *result, MTBDD a, MTBDD b, int c);

/**
 * Compute -a
 * (negation, where 0 stays 0, and x into -x)
 */
static inline int mtbdd_neg(MTBDD *result, MTBDD a);

/**
 * Compute ~a for partial MTBDDs.
 * Does not negate Boolean True/False.
 * (complement, where 0 is turned into 1, and non-0 into 0)
 */
static inline int mtbdd_zero_indicator(MTBDD *result, MTBDD dd);

/**
 * Compute a + b
 */
static inline int mtbdd_add(MTBDD *result, MTBDD a, MTBDD b);

/**
 * Compute a - b
 */
static inline int mtbdd_sub(MTBDD *result, MTBDD a, MTBDD b);

/**
 * Compute a * b
 */
static inline int mtbdd_mul(MTBDD *result, MTBDD a, MTBDD b);

/**
 * Compute min(a, b)
 */
static inline int mtbdd_min(MTBDD *result, MTBDD a, MTBDD b);

/**
 * Compute max(a, b)
 */
static inline int mtbdd_max(MTBDD *result, MTBDD a, MTBDD b);

/**
 * Abstract the variables in <v> from <a> by taking the sum of all values
 */
static inline int mtbdd_abstract_add(MTBDD *result, MTBDD dd, MTBDD vars);

/**
 * Abstract the variables in <v> from <a> by taking the product of all values
 */
static inline int mtbdd_abstract_mul(MTBDD *result, MTBDD dd, MTBDD vars);

/**
 * Abstract the variables in <v> from <a> by taking the minimum of all values
 */
static inline int mtbdd_abstract_min(MTBDD *result, MTBDD dd, MTBDD vars);

/**
 * Abstract the variables in <v> from <a> by taking the maximum of all values
 */
static inline int mtbdd_abstract_max(MTBDD *result, MTBDD dd, MTBDD vars);

/**
 * Compute IF <f> THEN <g> ELSE <h>.
 * <f> must be a Boolean MTBDD (or standard BDD).
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int mtbdd_ite(MTBDD *result, BDD condition, MTBDD if_true, MTBDD if_false);

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 * The caller must protect <result>. Returns SYLVAN_OK on success or a negative
 * status on failure, leaving <result> unchanged.
 */
static inline int mtbdd_mul_abstract_add(MTBDD *result, MTBDD a, MTBDD b, MTBDD vars);

/**
 * Multiply <a> and <b>, and abstract variables <vars> by taking the maximum.
 * The caller must protect <result>. Returns SYLVAN_OK on success or a negative
 * status on failure, leaving <result> unchanged.
 */
static inline int mtbdd_mul_abstract_max(MTBDD *result, MTBDD a, MTBDD b, MTBDD vars);

/**
 * Monad that converts double to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 */
static inline int mtbdd_op_threshold_double(MTBDD *result, MTBDD a, size_t b);

/**
 * Monad that converts double to a Boolean MTBDD, translate terminals > value to 1 and to 0 otherwise;
 */
static inline int mtbdd_op_strict_threshold_double(MTBDD *result, MTBDD a, size_t b);

/**
 * Convert double to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 */
static inline MTBDD mtbdd_threshold_double(MTBDD a, double b);

/**
 * Convert double to a Boolean MTBDD, translate terminals > value to 1 and to 0 otherwise;
 */
static inline MTBDD mtbdd_strict_threshold_double(MTBDD a, double b);

/**
 * For two Double MTBDDs, calculate whether they are equal module some value epsilon
 * i.e. abs(a-b) < e
 */
static inline MTBDD mtbdd_equal_abs_double(MTBDD a, MTBDD b, double c);

/**
 * For two Double MTBDDs, calculate whether they are equal modulo some value epsilon
 * This version computes the relative difference vs the value in a.
 * i.e. abs((a-b)/a) < e
 */
static inline MTBDD mtbdd_equal_rel_double(MTBDD a, MTBDD b, double c);

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) <= b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
static inline MTBDD mtbdd_leq(MTBDD a, MTBDD b);

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) < b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
static inline MTBDD mtbdd_lt(MTBDD a, MTBDD b);

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) >= b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
static inline MTBDD mtbdd_geq(MTBDD a, MTBDD b);

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) > b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
static inline MTBDD mtbdd_gt(MTBDD a, MTBDD b);

/**
 * Calculate the support of a MTBDD, i.e. the cube of all variables that appear in the MTBDD nodes.
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int mtbdd_support(BDDSET *result, MTBDD dd);

/**
 * Function composition, for each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of mtbdd_ite(<value>, <high>, <low>).
 * Each <value> in <map> must be a Boolean MTBDD.
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int mtbdd_compose(MTBDD *result, MTBDD dd, MTBDDMAP map);

/**
 * Compute minimal leaf in the MTBDD. All leaves must have the same supported
 * numeric type: Integer, Double, or Rational.
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int mtbdd_find_min(MTBDD *result, MTBDD dd);

/**
 * Compute maximal leaf in the MTBDD. All leaves must have the same supported
 * numeric type: Integer, Double, or Rational.
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int mtbdd_find_max(MTBDD *result, MTBDD dd);

/**
 * Given a MTBDD <dd> and a cube of variables <variables> expected in <dd>,
 * mtbdd_first_cube and mtbdd_next_cube enumerates the unique paths in <dd> that lead to a non-False leaf.
 * 
 * The function returns the leaf (or mtbdd_undefined if no new path is found) and encodes the path
 * in the supplied array <arr>: 0 for a low edge, 1 for a high edge, and 2 if the variable is skipped.
 *
 * The supplied array <arr> must be large enough for all variables in <variables>.
 *
 * Usage:
 * MTBDD leaf = mtbdd_first_cube(dd, variables, arr, NULL);
 * while (leaf != mtbdd_undefined) {
 *     .... // do something with arr/leaf
 *     leaf = mtbdd_next_cube(dd, variables, arr, NULL);
 * }
 *
 * The callback is an optional function that returns 0 when the given terminal node should be skipped.
 */
typedef int (*mtbdd_enum_filter_cb)(MTBDD);
MTBDD mtbdd_first_cube(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb);
MTBDD mtbdd_next_cube(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb);

/**
 * Given an MTBDD <dd> and a cube of variables <variables> expected in <dd>,
 * mtbdd_first_minterm and mtbdd_next_minterm enumerate all satisfying assignments in <dd> that lead
 * to a non-False leaf.
 *
 * The functions return the leaf (or mtbdd_undefined if no new satisfying assignment is found) and encodes
 * the assignment in the supplied array <arr>, 0 for False and 1 for True.
 *
 * The supplied array <arr> must be large enough for all variables in <variables>.
 *
 * Usage:
 * MTBDD leaf = mtbdd_first_cube(dd, variables, arr, NULL);
 * while (leaf != mtbdd_undefined) {
 *     .... // do something with arr/leaf
 *     leaf = mtbdd_next_cube(dd, variables, arr, NULL);
 * }
 *
 * The callback is an optional function that returns 0 when the given terminal node should be skipped.
 */
MTBDD mtbdd_first_minterm(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb);
MTBDD mtbdd_next_minterm(MTBDD dd, MTBDD variables, uint8_t *arr, mtbdd_enum_filter_cb filter_cb);

/**
 * Given a MTBDD <dd>, call <cb> with context <context> for every unique path in <dd> ending in leaf <leaf>.
 *
 * Usage:
 * TASK(void, cb, mtbdd_enum_trace*, trace, MTBDD, leaf, void*, context) { ... do something ... }
 * mtbdd_enumerate_parallel(dd, cb, context);
 */
typedef struct mtbdd_enum_trace {
    struct mtbdd_enum_trace *prev;
    uint32_t var;
    int val;  // 0 or 1
} mtbdd_enum_trace;

typedef void (*mtbdd_enumerate_cb)(mtbdd_enum_trace*, MTBDD, void*);
static inline void mtbdd_enumerate_parallel(MTBDD dd, mtbdd_enumerate_cb cb, void *context);

/**
 * Function composition after partial evaluation.
 *
 * Given a function F(X) = f, compute the composition F'(X) = g(f) for every assignment to X.
 * All variables X in <vars> must appear before all variables in f and g(f).
 *
 * Usage:
 * TASK(int, g, MTBDD*, result, MTBDD, in) { ... write g of <in> to result ... }
 * MTBDD x_vars = ...;  // the cube of variables x
 * MTBDD result = mtbdd_invalid;
 * int status = mtbdd_eval_compose(&result, dd, x_vars, TASK(g));
 *
 * The callback writes to Sylvan's protected <result> destination and returns
 * SYLVAN_OK on success or a negative status on failure. The caller must protect
 * the operation's <result>. On failure, <result> is unchanged.
 */
typedef int (*mtbdd_eval_compose_cb)(lace_worker* lace, MTBDD *result, MTBDD);
static inline int mtbdd_eval_compose(MTBDD *result, MTBDD dd, MTBDD vars, mtbdd_eval_compose_cb cb);

/**
 * For debugging.
 * Tests if all nodes in the MTBDD are correctly ``marked'' in the nodes table.
 * Tests if variables in the internal nodes appear in-order.
 * In Debug mode, this will cause assertion failures instead of returning 0.
 * Returns 1 if all is fine, or 0 otherwise.
 */
static inline int mtbdd_is_valid(MTBDD dd);

/**
 * Write a .dot representation of a given MTBDD
 * The callback function is required for custom terminals.
 */
void mtbdd_fprint_dot(FILE *out, MTBDD mtbdd);
#define mtbdd_print_dot(mtbdd, cb) mtbdd_fprint_dot(stdout, mtbdd)

/**
 * Write a .dot representation of a given MTBDD, but without complement edges.
 */
void mtbdd_fprint_dot_no_complement(FILE *out, MTBDD mtbdd);
#define mtbdd_print_dot_no_complement(mtbdd, cb) mtbdd_fprint_dot_no_complement(stdout, mtbdd)

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
char *mtbdd_leaf_to_string(MTBDD leaf, char *buf, size_t buflen);

/**
 * Some debugging functions that generate SHA2 hashes of MTBDDs.
 * They are independent of where nodes are located in hash tables.
 * Note that they are not "perfect", but they can be useful to run easy sanity checks.
 */

/**
 * Print SHA2 hash to stdout.
 */
void mtbdd_print_sha256(MTBDD dd);

/**
 * Print SHA2 hash to given file.
 */
void mtbdd_fprint_sha256(FILE *f, MTBDD dd);

/**
 * Obtain SHA2 hash; target array must be at least 65 bytes long.
 */
void mtbdd_sha256(MTBDD dd, char *target);

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
static inline void mtbdd_visit(MTBDD dd, mtbdd_visit_pre_cb precb, mtbdd_visit_post_cb postcb, void *context);

/**
 * Parallel visit operation
 */
static inline void mtbdd_visit_parallel(MTBDD dd, mtbdd_visit_pre_cb precb, mtbdd_visit_post_cb postcb, void *context);

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
static inline void mtbdd_writer_tobinary(FILE *file, MTBDD *dds, int count);

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

static inline void mtbdd_writer_totext(FILE *file, MTBDD *dds, int count);

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
static inline void mtbdd_writer_add(sylvan_skiplist_t sl, MTBDD dd);

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
static inline int mtbdd_reader_frombinary(FILE *file, MTBDD *dds, int count);

/**
 * Reading a file earlier written with mtbdd_writer_writebinary
 * Returns an array with the conversion from stored identifier to MTBDD
 * This array is allocated with malloc and must be freed afterwards.
 * Returns NULL if there was an error.
 */

static inline uint64_t *mtbdd_reader_readbinary(FILE *file);

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
static inline MTBDD mtbdd_map_empty(void);

static inline int mtbdd_map_is_empty(MTBDD map);

static inline uint32_t mtbdd_map_key(MTBDD map);

static inline MTBDD mtbdd_map_value(MTBDD map);

static inline MTBDD mtbdd_map_next(MTBDD map);

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
 * Keys are limited to 24 bits. On failure, <result> is unchanged.
 */
int mtbdd_map_set(MTBDDMAP *result, MTBDDMAP map, uint32_t key, MTBDD value);

/**
 * Add all values from map2 to map1, overwrites if key already in map1.
 * On failure, <result> is unchanged.
 */
int mtbdd_map_update(MTBDDMAP *result, MTBDDMAP map1, MTBDDMAP map2);

/**
 * Remove the key <key> from the map. Keys are limited to 24 bits.
 * On failure, <result> is unchanged.
 */
int mtbdd_map_remove(MTBDDMAP *result, MTBDDMAP map, uint32_t key);

/**
 * Remove all keys in the variable set <variables> from the map.
 * On failure, <result> is unchanged.
 */
int mtbdd_map_remove_all(MTBDDMAP *result, MTBDDMAP map, BDDSET variables);

/**
 * Garbage collection
 * Sylvan supplies two default methods to handle references to nodes, but the user
 * is encouraged to implement custom handling. Simply add a handler using sylvan_gc_add_mark
 * and let the handler call mtbdd_gc_mark for every MTBDD that should be saved
 * during garbage collection.
 */

/**
 * Call mtbdd_gc_mark for every mtbdd you want to keep in your custom mark functions.
 */
static inline void mtbdd_gc_mark(MTBDD dd);

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
size_t mtbdd_protected_count(void);

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
size_t mtbdd_ref_count(void);

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
