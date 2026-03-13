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


static inline MTBDD mtbdd_makenode(uint32_t var, MTBDD low, MTBDD high)
{
    return low == high ? low : _mtbdd_makenode(var, low, high);
}

static inline int mtbdd_isnode(MTBDD mtbdd) { return mtbdd_isleaf(mtbdd) ? 0 : 1; }

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


static inline int32_t
mtbdd_getnumer(MTBDD terminal)
{
    return (int32_t)(mtbdd_getvalue(terminal)>>32);
}

static inline uint32_t
mtbdd_getdenom(MTBDD terminal)
{
    return (uint32_t)(mtbdd_getvalue(terminal)&0xffffffff);
}

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

static inline MTBDD mtbdd_set_union(MTBDD set1, MTBDD set2)
{
    return bdd_and(set1, set2);
}


TASK_2(MTBDD, mtbdd_set_minus, MTBDD, set1, MTBDD, set2)

TASK_4(BDD, mtbdd_union_cube, MTBDD, mtbdd, MTBDD, variables, uint8_t*, cube, MTBDD, terminal)

TASK_2(double, mtbdd_satcount, MTBDD, dd, size_t, nvars);

static inline size_t mtbdd_leafcount(MTBDD dd)
{
    return mtbdd_leafcount_more(&dd, 1);
}

static inline size_t mtbdd_nodecount(const MTBDD dd) {
    return mtbdd_nodecount_more(&dd, 1);
}

TASK_3(MTBDD, mtbdd_apply, MTBDD, a, MTBDD, b, mtbdd_apply_op, op);

TASK_5(MTBDD, mtbdd_applyp, MTBDD, a, MTBDD, b, size_t, p, mtbdd_applyp_op, op, uint64_t, opid);

TASK_3(MTBDD, mtbdd_uapply, MTBDD, dd, mtbdd_uapply_op, op, size_t, param);

TASK_3(MTBDD, mtbdd_abstract, MTBDD, a, MTBDD, v, mtbdd_abstract_op, op);

TASK_2(MTBDD, mtbdd_op_negate, MTBDD, a, size_t, param);

TASK_2(MTBDD, mtbdd_op_cmpl, MTBDD, a, size_t, param);

TASK_2(MTBDD, mtbdd_op_plus, MTBDD*, a, MTBDD*, b);
TASK_3(MTBDD, mtbdd_abstract_op_plus, MTBDD, a, MTBDD, b, int, c);

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

VOID_TASK_3(mtbdd_enum_par, MTBDD, dd, mtbdd_enum_cb, cb, void*, context);

TASK_1(int, mtbdd_test_isvalid, MTBDD, dd);

VOID_TASK_4(mtbdd_visit_seq, MTBDD, dd, mtbdd_visit_pre_cb, precb, mtbdd_visit_post_cb, postcb, void*, context);

VOID_TASK_4(mtbdd_visit_par, MTBDD, dd, mtbdd_visit_pre_cb, precb, mtbdd_visit_post_cb, postcb, void*, context);

VOID_TASK_3(mtbdd_writer_tobinary, FILE *, file, MTBDD *, dds, int, count);

VOID_TASK_3(mtbdd_writer_totext, FILE *, file, MTBDD *, dds, int, count);

VOID_TASK_2(mtbdd_writer_add, sylvan_skiplist_t, sl, MTBDD, dd);

TASK_3(int, mtbdd_reader_frombinary, FILE*, file, MTBDD*, dds, int, count);

TASK_1(uint64_t*, mtbdd_reader_readbinary, FILE*, file);

TASK_3(MTBDD, mtbdd_eval_compose, MTBDD, dd, MTBDD, vars, mtbdd_eval_compose_cb, cb);


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

VOID_TASK_1(mtbdd_gc_mark_rec, MTBDD, dd);

#ifdef __cplusplus
}
#endif /* __cplusplus */

