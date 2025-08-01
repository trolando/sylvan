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
 * This is an implementation of GMP mpq custom leaves of MTBDDs
 */

#include <sylvan.h>
#include <gmp.h>

#ifndef SYLVAN_GMP_H
#define SYLVAN_GMP_H

#ifdef __cplusplus
namespace sylvan {
extern "C" {
#endif /* __cplusplus */

/**
 * Initialize GMP custom leaves
 */
void gmp_init(void);

/**
 * Create MPQ leaf
 */
MTBDD mtbdd_gmp(mpq_t val);

/**
 * Operation "plus" for two mpq MTBDDs
 */
TASK_2(MTBDD, gmp_op_plus, MTBDD*, a, MTBDD*, b);
TASK_3(MTBDD, gmp_abstract_op_plus, MTBDD, a, MTBDD, b, int, k);

/**
 * Operation "minus" for two mpq MTBDDs
 */
TASK_2(MTBDD, gmp_op_minus, MTBDD*, a, MTBDD*, b);

/**
 * Operation "times" for two mpq MTBDDs
 */
TASK_2(MTBDD, gmp_op_times, MTBDD*, a, MTBDD*, b);
TASK_3(MTBDD, gmp_abstract_op_times, MTBDD, a, MTBDD, c, int, k);

/**
 * Operation "divide" for two mpq MTBDDs
 */
TASK_2(MTBDD, gmp_op_divide, MTBDD*, a, MTBDD*, b);

/**
 * Operation "min" for two mpq MTBDDs
 */
TASK_2(MTBDD, gmp_op_min, MTBDD*, a, MTBDD*, b);
TASK_3(MTBDD, gmp_abstract_op_min, MTBDD, a, MTBDD, b, int, k);

/**
 * Operation "max" for two mpq MTBDDs
 */
TASK_2(MTBDD, gmp_op_max, MTBDD*, a, MTBDD*, b);
TASK_3(MTBDD, gmp_abstract_op_max, MTBDD, a, MTBDD, b, int, k);

/**
 * Operation "negate" for one mpq MTBDD
 */
TASK_2(MTBDD, gmp_op_neg, MTBDD, dd, size_t, p);

/**
 * Operation "abs" for one mpq MTBDD
 */
TASK_2(MTBDD, gmp_op_abs, MTBDD, dd, size_t, p);

/**
 * Compute a + b
 */
static inline MTBDD gmp_plus(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, gmp_op_plus_CALL);
}

/**
 * Compute a + b
 */
static inline MTBDD gmp_minus(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, gmp_op_minus_CALL);
}

/**
 * Compute a * b
 */
static inline MTBDD gmp_times(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, gmp_op_times_CALL);
}

/**
 * Compute a * b
 */
static inline MTBDD gmp_divide(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, gmp_op_divide_CALL);
}

/**
 * Compute min(a, b)
 */
static inline MTBDD gmp_min(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, gmp_op_min_CALL);
}

/**
 * Compute max(a, b)
 */
static inline MTBDD gmp_max(MTBDD a, MTBDD b)
{
    return mtbdd_apply(a, b, gmp_op_max_CALL);
}

/**
 * Compute -a
 */
static inline MTBDD gmp_neg(MTBDD dd)
{
    return mtbdd_uapply(dd, gmp_op_neg_CALL, 0);
}

/**
 * Compute abs(a)
 */
static inline MTBDD gmp_abs(MTBDD dd)
{
    return mtbdd_uapply(dd, gmp_op_abs_CALL, 0);
}

/**
 * Abstract the variables in <vars> from <dd> by taking the sum of all values
 */
static inline MTBDD gmp_abstract_plus(MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(dd, vars, gmp_abstract_op_plus_CALL);
}

/**
 * Abstract the variables in <vars> from <dd> by taking the product of all values
 */
static inline MTBDD gmp_abstract_times(MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(dd, vars, gmp_abstract_op_times_CALL);
}

/**
 * Abstract the variables in <vars> from <dd> by taking the minimum of all values
 */
static inline MTBDD gmp_abstract_min(MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(dd, vars, gmp_abstract_op_min_CALL);
}

/**
 * Abstract the variables in <vars> from <dd> by taking the maximum of all values
 */
static inline MTBDD gmp_abstract_max(MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(dd, vars, gmp_abstract_op_max_CALL);
}

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 */
TASK_3(MTBDD, gmp_and_abstract_plus, MTBDD, a, MTBDD, b, MTBDD, vars)
#define gmp_and_exists gmp_and_abstract_plus

/**
 * Multiply <a> and <b>, and abstract variables <vars> by taking the maximum.
 */
TASK_3(MTBDD, gmp_and_abstract_max, MTBDD, a, MTBDD, b, MTBDD, vars);

/**
 * Convert to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 * Parameter <dd> is the MTBDD to convert; parameter <value> is an GMP mpq leaf
 */
TASK_2(MTBDD, gmp_op_threshold, MTBDD*, dd, MTBDD*, value);

/**
 * Convert to a Boolean MTBDD, translate terminals > value to 1 and to 0 otherwise;
 * Parameter <dd> is the MTBDD to convert; parameter <value> is an GMP mpq leaf
 */
TASK_2(MTBDD, gmp_op_strict_threshold, MTBDD*, dd, MTBDD*, value);

/**
 * Convert to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 */
TASK_2(MTBDD, gmp_threshold_d, MTBDD, dd, double, value);

/**
 * Convert to a Boolean MTBDD, translate terminals > value to 1 and to 0 otherwise;
 */
TASK_2(MTBDD, gmp_strict_threshold_d, MTBDD, dd, double, value);

#ifdef __cplusplus
}
}
#endif /* __cplusplus */

#endif
