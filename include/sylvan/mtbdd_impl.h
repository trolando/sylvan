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
 * For non-Boolean MTBDDs, mtbdd_undefined is used for partial functions, i.e. mtbdd_undefined
 * indicates that the function is not defined for a certain input.
 */

/* Do not include this file directly. Instead, include sylvan.h */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */
MTBDD _mtbdd_make_node(uint32_t var, MTBDD low, MTBDD high);
static inline MTBDD mtbdd_make_node(uint32_t var, MTBDD low, MTBDD high)
{
    return low == high ? low : _mtbdd_make_node(var, low, high);
}

static inline int
bdd_is_complemented(MTBDD dd)
{
    return (dd & bdd_complement) ? 1 : 0;
}

TASK(int, mtbdd_set_cube, MTBDD*, result, MTBDD, mtbdd, BDDSET, variables, const uint8_t*, cube, MTBDD, terminal)

TASK(double, mtbdd_sat_count, MTBDD, dd, size_t, nvars);

static inline size_t mtbdd_leaf_count(MTBDD dd)
{
    return mtbdd_shared_leaf_count(&dd, 1);
}

static inline size_t mtbdd_node_count(const MTBDD dd) {
    return mtbdd_shared_node_count(&dd, 1);
}

TASK(int, mtbdd_apply, MTBDD*, result, MTBDD, a, MTBDD, b, mtbdd_apply_cb, op);

TASK(int, mtbdd_apply_param, MTBDD*, result, MTBDD, a, MTBDD, b, size_t, p, mtbdd_apply_param_cb, op, uint64_t, opid);

TASK(int, mtbdd_apply_unary, MTBDD*, result, MTBDD, dd, mtbdd_apply_unary_cb, op, size_t, param);

TASK(int, mtbdd_abstract, MTBDD*, result, MTBDD, a, MTBDD, v, mtbdd_abstract_cb, op);

TASK(int, mtbdd_op_negate, MTBDD*, result, MTBDD, a, size_t, param);

TASK(int, mtbdd_op_cmpl, MTBDD*, result, MTBDD, a, size_t, param);

TASK(int, mtbdd_op_plus, MTBDD*, result, MTBDD*, a, MTBDD*, b);
TASK(int, mtbdd_abstract_op_plus, MTBDD*, result, MTBDD, a, MTBDD, b, int, c);

TASK(int, mtbdd_op_minus, MTBDD*, result, MTBDD*, a, MTBDD*, b);

/**
 * Binary operation Times (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is mtbdd_undefined (i.e. not defined).
 */
TASK(int, mtbdd_op_times, MTBDD*, result, MTBDD*, a, MTBDD*, b);
TASK(int, mtbdd_abstract_op_times, MTBDD*, result, MTBDD, a, MTBDD, b, int, c);

/**
 * Binary operation Minimum (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is the other operand.
 */
TASK(int, mtbdd_op_min, MTBDD*, result, MTBDD*, a, MTBDD*, b);
TASK(int, mtbdd_abstract_op_min, MTBDD*, result, MTBDD, a, MTBDD, b, int, c);

/**
 * Binary operation Maximum (for MTBDDs of same type)
 * Only for MTBDDs where either all leaves are Boolean, or Integer, or Double.
 * For Integer/Double MTBDD, if either operand is mtbdd_undefined (not defined),
 * then the result is the other operand.
 */
TASK(int, mtbdd_op_max, MTBDD*, result, MTBDD*, a, MTBDD*, b);
TASK(int, mtbdd_abstract_op_max, MTBDD*, result, MTBDD, a, MTBDD, b, int, c);

/**
 * Compute -a
 * (negation, where 0 stays 0, and x into -x)
 */
static inline int mtbdd_neg(MTBDD *result, MTBDD a)
{
    return mtbdd_apply_unary(result, a, mtbdd_op_negate_CALL, 0);
}

/**
 * Compute ~a for partial MTBDDs.
 * Does not negate Boolean True/False.
 * (complement, where 0 is turned into 1, and non-0 into 0)
 */
static inline int mtbdd_zero_indicator(MTBDD *result, MTBDD dd)
{
    return mtbdd_apply_unary(result, dd, mtbdd_op_cmpl_CALL, 0);
}

/**
 * Compute a + b
 */
static inline int mtbdd_add(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, mtbdd_op_plus_CALL);
}

/**
 * Compute a - b
 */
static inline int mtbdd_sub(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, mtbdd_op_minus_CALL);
}

/**
 * Compute a * b
 */
static inline int mtbdd_mul(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, mtbdd_op_times_CALL);
}

/**
 * Compute min(a, b)
 */
static inline int mtbdd_min(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, mtbdd_op_min_CALL);
}

/**
 * Compute max(a, b)
 */
static inline int mtbdd_max(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, mtbdd_op_max_CALL);
}

/**
 * Abstract the variables in <v> from <a> by taking the sum of all values
 */
static inline int mtbdd_abstract_add(MTBDD *result, MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(result, dd, vars, mtbdd_abstract_op_plus_CALL);
}

/**
 * Abstract the variables in <v> from <a> by taking the product of all values
 */
static inline int mtbdd_abstract_mul(MTBDD *result, MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(result, dd, vars, mtbdd_abstract_op_times_CALL);
}

/**
 * Abstract the variables in <v> from <a> by taking the minimum of all values
 */
static inline int mtbdd_abstract_min(MTBDD *result, MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(result, dd, vars, mtbdd_abstract_op_min_CALL);
}

/**
 * Abstract the variables in <v> from <a> by taking the maximum of all values
 */
static inline int mtbdd_abstract_max(MTBDD *result, MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(result, dd, vars, mtbdd_abstract_op_max_CALL);
}

/**
 * Compute IF <f> THEN <g> ELSE <h>.
 * <f> must be a Boolean MTBDD (or standard BDD).
 */
TASK(int, mtbdd_ite, MTBDD*, result, BDD, condition, MTBDD, if_true, MTBDD, if_false);

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 */
TASK(int, mtbdd_mul_abstract_add, MTBDD*, result, MTBDD, a, MTBDD, b, MTBDD, vars);

/**
 * Multiply <a> and <b>, and abstract variables <vars> by taking the maximum.
 */
TASK(int, mtbdd_mul_abstract_max, MTBDD*, result, MTBDD, a, MTBDD, b, MTBDD, vars);

/**
 * Monad that converts double to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 */
TASK(int, mtbdd_op_threshold_double, MTBDD*, result, MTBDD, a, size_t, b)

/**
 * Monad that converts double to a Boolean MTBDD, translate terminals > value to 1 and to 0 otherwise;
 */
TASK(int, mtbdd_op_strict_threshold_double, MTBDD*, result, MTBDD, a, size_t, b)

/**
 * Convert double to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 */
TASK(int, mtbdd_threshold_double, MTBDD*, result, MTBDD, a, double, b);

/**
 * Convert double to a Boolean MTBDD, translate terminals > value to 1 and to 0 otherwise;
 */
TASK(int, mtbdd_strict_threshold_double, MTBDD*, result, MTBDD, a, double, b);

/**
 * For two Double MTBDDs, calculate whether they are equal module some value epsilon
 * i.e. abs(a-b) < e
 */
TASK(int, mtbdd_equal_abs_double, MTBDD*, result, MTBDD, a, MTBDD, b, double, c);

/**
 * For two Double MTBDDs, calculate whether they are equal modulo some value epsilon
 * This version computes the relative difference vs the value in a.
 * i.e. abs((a-b)/a) < e
 */
TASK(int, mtbdd_equal_rel_double, MTBDD*, result, MTBDD, a, MTBDD, b, double, c);

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) <= b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(int, mtbdd_leq, MTBDD*, result, MTBDD, a, MTBDD, b);

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) < b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(int, mtbdd_lt, MTBDD*, result, MTBDD, a, MTBDD, b);

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) >= b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(int, mtbdd_geq, MTBDD*, result, MTBDD, a, MTBDD, b);

/**
 * For two MTBDDs a, b, return bdd_true if all common assignments a(s) > b(s), mtbdd_undefined otherwise.
 * For domains not in a / b, assume True.
 */
TASK(int, mtbdd_gt, MTBDD*, result, MTBDD, a, MTBDD, b);

/**
 * Calculate the support of a MTBDD, i.e. the cube of all variables that appear in the MTBDD nodes.
 */
TASK(int, mtbdd_support, BDDSET*, result, MTBDD, dd);

/**
 * Function composition, for each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of mtbdd_ite(<value>, <high>, <low>).
 * Each <value> in <map> must be a Boolean MTBDD.
 */
TASK(int, mtbdd_compose, MTBDD*, result, MTBDD, dd, MTBDDMAP, map);

/**
 * Compute minimal leaf in the MTBDD (for Integer, Double, Rational MTBDDs)
 */
TASK(int, mtbdd_find_min, MTBDD*, result, MTBDD, dd);

/**
 * Compute maximal leaf in the MTBDD (for Integer, Double, Rational MTBDDs)
 */
TASK(int, mtbdd_find_max, MTBDD*, result, MTBDD, dd);

TASK(void, mtbdd_enumerate_parallel, MTBDD, dd, mtbdd_enumerate_cb, cb, void*, context);

TASK(int, mtbdd_is_valid, MTBDD, dd);

TASK(void, mtbdd_visit, MTBDD, dd, mtbdd_visit_pre_cb, precb, mtbdd_visit_post_cb, postcb, void*, context);

TASK(void, mtbdd_visit_parallel, MTBDD, dd, mtbdd_visit_pre_cb, precb, mtbdd_visit_post_cb, postcb, void*, context);

TASK(void, mtbdd_writer_tobinary, FILE *, file, MTBDD *, dds, int, count);

TASK(void, mtbdd_writer_totext, FILE *, file, MTBDD *, dds, int, count);

TASK(void, mtbdd_writer_add, sylvan_skiplist_t, sl, MTBDD, dd);

TASK(int, mtbdd_reader_frombinary, FILE*, file, MTBDD*, dds, int, count);

TASK(uint64_t*, mtbdd_reader_readbinary, FILE*, file);

TASK(int, mtbdd_eval_compose, MTBDD*, result, MTBDD, dd, MTBDD, vars, mtbdd_eval_compose_cb, cb);


static inline MTBDD
mtbdd_map_empty()
{
    return mtbdd_undefined;
}

static inline int
mtbdd_map_is_empty(MTBDD map)
{
    return (map == mtbdd_undefined) ? 1 : 0;
}

static inline uint32_t
mtbdd_map_key(MTBDD map)
{
    return mtbdd_node_variable(map);
}

static inline MTBDD
mtbdd_map_value(MTBDD map)
{
    return mtbdd_node_high(map);
}

static inline MTBDD
mtbdd_map_next(MTBDD map)
{
    return mtbdd_node_low(map);
}

TASK(void, mtbdd_gc_mark, MTBDD, dd);

#ifdef __cplusplus
}
#endif /* __cplusplus */
