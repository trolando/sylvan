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
 * Calculate the exact number of satisfying assignments over <variables>.
 * The set must contain the support of <dd>. On failure, <result> is unchanged.
 */
int bdd_sat_count_gmp(mpz_t result, BDD dd, BDDSET variables);

/**
 * Calculate the exact number of assignments over <variables> that lead to a
 * nonzero built-in MTBDD leaf. Custom leaves other than mtbdd_undefined are
 * counted. The set must contain the support of <dd>. On failure, <result> is
 * unchanged.
 */
int mtbdd_sat_count_gmp(mpz_t result, MTBDD dd, BDDSET variables);

/**
 * Count the assignments represented by <dd> exactly.
 * A custom non-False leaf counts as one accepting assignment.
 * On failure, <result> is unchanged.
 */
int zdd_count_gmp(mpz_t result, ZDD dd);

/**
 * Count the tuples represented by <dd> exactly.
 * On failure, <result> is unchanged.
 */
int listdd_count_gmp(mpz_t result, LISTDD dd);

/**
 * Create MPQ leaf
 */
MTBDD mtbdd_gmp(mpq_t val);

/**
 * Operation "plus" for two mpq MTBDDs
 */
TASK(int, gmp_op_plus, MTBDD*, result, MTBDD*, a, MTBDD*, b)
TASK(int, gmp_abstract_op_plus, MTBDD*, result, MTBDD, a, MTBDD, b, int, k)

/**
 * Operation "minus" for two mpq MTBDDs
 */
TASK(int, gmp_op_minus, MTBDD*, result, MTBDD*, a, MTBDD*, b)

/**
 * Operation "times" for two mpq MTBDDs
 */
TASK(int, gmp_op_times, MTBDD*, result, MTBDD*, a, MTBDD*, b)
TASK(int, gmp_abstract_op_times, MTBDD*, result, MTBDD, a, MTBDD, c, int, k)

/**
 * Operation "divide" for two mpq MTBDDs
 */
TASK(int, gmp_op_divide, MTBDD*, result, MTBDD*, a, MTBDD*, b)

/**
 * Operation "min" for two mpq MTBDDs
 */
TASK(int, gmp_op_min, MTBDD*, result, MTBDD*, a, MTBDD*, b)
TASK(int, gmp_abstract_op_min, MTBDD*, result, MTBDD, a, MTBDD, b, int, k)

/**
 * Operation "max" for two mpq MTBDDs
 */
TASK(int, gmp_op_max, MTBDD*, result, MTBDD*, a, MTBDD*, b)
TASK(int, gmp_abstract_op_max, MTBDD*, result, MTBDD, a, MTBDD, b, int, k)

/**
 * Operation "negate" for one mpq MTBDD
 */
TASK(int, gmp_op_neg, MTBDD*, result, MTBDD, dd, size_t, p)

/**
 * Operation "abs" for one mpq MTBDD
 */
TASK(int, gmp_op_abs, MTBDD*, result, MTBDD, dd, size_t, p)

/**
 * Compute a + b
 */
static inline int gmp_plus(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, gmp_op_plus_CALL);
}

/**
 * Compute a + b
 */
static inline int gmp_minus(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, gmp_op_minus_CALL);
}

/**
 * Compute a * b
 */
static inline int gmp_times(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, gmp_op_times_CALL);
}

/**
 * Compute a * b
 */
static inline int gmp_divide(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, gmp_op_divide_CALL);
}

/**
 * Compute min(a, b)
 */
static inline int gmp_min(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, gmp_op_min_CALL);
}

/**
 * Compute max(a, b)
 */
static inline int gmp_max(MTBDD *result, MTBDD a, MTBDD b)
{
    return mtbdd_apply(result, a, b, gmp_op_max_CALL);
}

/**
 * Compute -a
 */
static inline int gmp_neg(MTBDD *result, MTBDD dd)
{
    return mtbdd_apply_unary(result, dd, gmp_op_neg_CALL, 0);
}

/**
 * Compute abs(a)
 */
static inline int gmp_abs(MTBDD *result, MTBDD dd)
{
    return mtbdd_apply_unary(result, dd, gmp_op_abs_CALL, 0);
}

/**
 * Abstract the variables in <vars> from <dd> by taking the sum of all values
 */
static inline int gmp_abstract_plus(MTBDD *result, MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(result, dd, vars, gmp_abstract_op_plus_CALL);
}

/**
 * Abstract the variables in <vars> from <dd> by taking the product of all values
 */
static inline int gmp_abstract_times(MTBDD *result, MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(result, dd, vars, gmp_abstract_op_times_CALL);
}

/**
 * Abstract the variables in <vars> from <dd> by taking the minimum of all values
 */
static inline int gmp_abstract_min(MTBDD *result, MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(result, dd, vars, gmp_abstract_op_min_CALL);
}

/**
 * Abstract the variables in <vars> from <dd> by taking the maximum of all values
 */
static inline int gmp_abstract_max(MTBDD *result, MTBDD dd, MTBDD vars)
{
    return mtbdd_abstract(result, dd, vars, gmp_abstract_op_max_CALL);
}

/**
 * Multiply <a> and <b>, and abstract variables <vars> using summation.
 * This is similar to the "and_exists" operation in BDDs.
 * The caller must protect <result>. Returns SYLVAN_OK on success or a negative
 * status on failure, leaving <result> unchanged.
 */
TASK(int, gmp_and_abstract_plus, MTBDD*, result, MTBDD, a, MTBDD, b, MTBDD, vars)
#define gmp_and_exists gmp_and_abstract_plus

/**
 * Multiply <a> and <b>, and abstract variables <vars> by taking the maximum.
 * The caller must protect <result>. Returns SYLVAN_OK on success or a negative
 * status on failure, leaving <result> unchanged.
 */
TASK(int, gmp_and_abstract_max, MTBDD*, result, MTBDD, a, MTBDD, b, MTBDD, vars)

/**
 * Convert to a Boolean MTBDD, translate terminals >= value to 1 and to 0 otherwise;
 * Parameter <dd> is the MTBDD to convert; parameter <value> is an GMP mpq leaf
 */
TASK(int, gmp_op_threshold, MTBDD*, result, MTBDD*, dd, MTBDD*, value)

/**
 * Convert to a Boolean MTBDD, translate terminals > value to 1 and to 0 otherwise;
 * Parameter <dd> is the MTBDD to convert; parameter <value> is an GMP mpq leaf
 */
TASK(int, gmp_op_strict_threshold, MTBDD*, result, MTBDD*, dd, MTBDD*, value)

/**
 * Convert to a Boolean MTBDD, translating terminals >= <value> to true and
 * other terminals to undefined. The caller must protect <result>. Returns
 * SYLVAN_OK on success or a negative status on failure, leaving <result>
 * unchanged.
 */
TASK(int, gmp_threshold_d, MTBDD*, result, MTBDD, dd, double, value)

/**
 * As gmp_threshold_d, using strict greater-than.
 */
TASK(int, gmp_strict_threshold_d, MTBDD*, result, MTBDD, dd, double, value)

#ifdef __cplusplus
}
}
#endif /* __cplusplus */

#endif
