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

#pragma once

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

static void SYLVAN_UNUSED bdd_fprint(FILE* f, BDD bdd)
{
    bdd_serialize_reset();
    size_t v = bdd_serialize_add(bdd);
    fprintf(f, "%s%zu,", bdd_complement ? "!" : "", v);
    bdd_serialize_totext(f);
}

static void SYLVAN_UNUSED bdd_print(BDD bdd)
{
    bdd_fprint(stdout, bdd);
}

static inline int bdd_is_leaf(MTBDD bdd)
{
    return bdd == bdd_true || bdd == bdd_false ? 1 : 0;
}

static inline BDD bdd_not(BDD dd)
{
    return dd ^ bdd_complement;
}

static inline int bdd_xnor(BDD *result, BDD a, BDD b)
{
    if (result == NULL || a == mtbdd_invalid || b == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    int status = bdd_xor(result, a, b);
    if (status == SYLVAN_OK) *result = bdd_not(*result);
    return status;
}

static inline int bdd_or(BDD *result, BDD a, BDD b)
{
    if (result == NULL || a == mtbdd_invalid || b == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    int status = bdd_and(result, bdd_not(a), bdd_not(b));
    if (status == SYLVAN_OK) *result = bdd_not(*result);
    return status;
}

static inline int bdd_nand(BDD *result, BDD a, BDD b)
{
    if (result == NULL || a == mtbdd_invalid || b == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    int status = bdd_and(result, a, b);
    if (status == SYLVAN_OK) *result = bdd_not(*result);
    return status;
}

static inline int bdd_nor(BDD *result, BDD a, BDD b)
{
    if (result == NULL || a == mtbdd_invalid || b == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    return bdd_and(result, bdd_not(a), bdd_not(b));
}

static inline int bdd_imp(BDD *result, BDD a, BDD b)
{
    if (result == NULL || a == mtbdd_invalid || b == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    int status = bdd_and(result, a, bdd_not(b));
    if (status == SYLVAN_OK) *result = bdd_not(*result);
    return status;
}

static inline int bdd_diff(BDD *result, BDD a, BDD b)
{
    if (result == NULL || a == mtbdd_invalid || b == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    return bdd_and(result, a, bdd_not(b));
}

static inline char bdd_subseteq(BDD a, BDD b)
{
    return bdd_disjoint(a, bdd_not(b));
}

static inline int bdd_forall(BDD *result, BDD dd, BDDSET vars)
{
    if (result == NULL || dd == mtbdd_invalid || vars == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    int status = bdd_exists(result, bdd_not(dd), vars);
    if (status == SYLVAN_OK) *result = bdd_not(*result);
    return status;
}

static inline BDDSET bdd_set_empty(void)
{
    return bdd_true;
}

static inline int bdd_set_is_empty(BDDSET set)
{
    return set == bdd_true;
}

static inline uint32_t bdd_set_first(BDDSET set)
{
    return mtbdd_node_variable(set);
}

static inline BDDSET bdd_set_next(BDDSET set)
{
    return mtbdd_node_high(set);
}

static inline int bdd_set_union(BDDSET *result, BDDSET set1, BDDSET set2)
{
    if (result == NULL || set1 == mtbdd_invalid || set2 == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    return bdd_and(result, set1, set2);
}

TASK(int, bdd_ite, BDD*, result, BDD, a, BDD, b, BDD, c)
TASK(int, bdd_and, BDD*, result, BDD, a, BDD, b)
TASK(int, bdd_xor, BDD*, result, BDD, a, BDD, b)
TASK(char, bdd_disjoint, BDD, a, BDD, b)
TASK(int, bdd_exists, BDD*, result, BDD, dd, BDD, vars)
TASK(int, bdd_unique, BDD*, result, BDD, dd, BDDSET, vars)
TASK(int, bdd_project, BDD*, result, BDD, dd, BDD, vars);
TASK(int, bdd_and_exists, BDD*, result, BDD, a, BDD, b, BDDSET, vars)
TASK(int, bdd_and_project, BDD*, result, BDD, a, BDD, b, BDDSET, vars);
TASK(int, bdd_rel_prev, BDD*, result, BDD, a, BDD, b, BDDSET, vars)
TASK(int, bdd_rel_next, BDD*, result, BDD, a, BDD, b, BDDSET, vars)
TASK(int, bdd_transitive_closure, BDD*, result, BDD, a)
TASK(int, bdd_constrain, BDD*, result, BDD, f, BDD, c)
TASK(int, bdd_simplify, BDD*, result, BDD, f, BDD, c)
TASK(int, bdd_compose, BDD*, result, BDD, f, MTBDDMAP, m)
TASK(double, bdd_sat_count, BDD, dd, BDDSET, vars)
TASK(void, bdd_enumerate_minterms, BDD, dd, BDDSET, vars, bdd_enumerate_cb, cb, void*, context)
TASK(void, bdd_enumerate_minterms_parallel, BDD, dd, BDDSET, vars, bdd_enumerate_cb, cb, void*, context)
TASK(int, bdd_map_reduce_or, BDD*, result, BDD, dd, BDDSET, vars, bdd_map_reduce_or_cb, cb, void*, context)
TASK(double, bdd_path_count, BDD, dd)
TASK(int, bdd_cube, BDD*, result, BDDSET, vars, const uint8_t*, cube)
TASK(int, bdd_or_cube, BDD*, result, BDD, dd, BDDSET, vars, const uint8_t*, cube)
TASK(int, bdd_pick_cube, BDD*, result, BDD, dd, BDDSET, vars)
TASK(int, bdd_pick_minterm, BDD*, result, BDD, dd, BDDSET, vars)
TASK(int, bdd_set_difference, BDDSET*, result, BDDSET, set1, BDDSET, set2)

#ifdef __cplusplus
}
#endif /* __cplusplus */
