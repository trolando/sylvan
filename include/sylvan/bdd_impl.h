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

/* Transitional value-return bridges for operations not converted yet. */
static inline BDD bdd_and_value(BDD a, BDD b)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_and(&result, a, b);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static inline BDD bdd_ite_value(BDD a, BDD b, BDD c)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_ite(&result, a, b, c);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static inline BDD bdd_xnor(BDD a, BDD b)
{
    return bdd_not(bdd_xor(a, b));
}

static inline BDD bdd_or(BDD a, BDD b) {
    return bdd_not(bdd_and_value(bdd_not(a), bdd_not(b)));
}

static inline BDD bdd_nand(BDD a, BDD b) {
    return bdd_not(bdd_and_value(a, b));
}

static inline BDD bdd_nor(BDD a, BDD b) {
    return bdd_and_value(bdd_not(a), bdd_not(b));
}

static inline BDD bdd_imp(BDD a, BDD b) {
    return bdd_not(bdd_and_value(a, bdd_not(b)));
}

static inline BDD bdd_diff(BDD a, BDD b) {
    return bdd_and_value(a, bdd_not(b));
}

static inline char bdd_subseteq(BDD a, BDD b)
{
    return bdd_disjoint(a, bdd_not(b));
}

static inline BDD bdd_forall(BDD dd, BDDSET vars)
{
    return bdd_not(bdd_exists(bdd_not(dd), vars));
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

static inline BDDSET bdd_set_union(BDDSET set1, BDDSET set2)
{
    return bdd_and_value(set1, set2);
}

TASK(int, bdd_ite, BDD*, result, BDD, a, BDD, b, BDD, c)
TASK(int, bdd_and, BDD*, result, BDD, a, BDD, b)
TASK(BDD, bdd_xor, BDD, a, BDD, b)
TASK(char, bdd_disjoint, BDD, a, BDD, b)
TASK(BDD, bdd_exists, BDD, dd, BDD, vars)
TASK(BDD, bdd_project, BDD, dd, BDD, vars);
TASK(BDD, bdd_and_exists, BDD, a, BDD, b, BDDSET, vars)
TASK(BDD, bdd_and_project, BDD, a, BDD, b, BDDSET, vars);
TASK(BDD, bdd_rel_prev, BDD, a, BDD, b, BDDSET, vars)
TASK(BDD, bdd_rel_next, BDD, a, BDD, b, BDDSET, vars)
TASK(BDD, bdd_transitive_closure, BDD, a)
TASK(BDD, bdd_constrain, BDD, f, BDD, c)
TASK(BDD, bdd_restrict, BDD, f, BDD, c)
TASK(BDD, bdd_compose, BDD, f, MTBDDMAP, m)
TASK(double, bdd_sat_count, BDD, dd, BDDSET, vars)
TASK(void, bdd_enumerate_minterms, BDD, dd, BDDSET, vars, bdd_enumerate_cb, cb, void*, context)
TASK(void, bdd_enumerate_minterms_parallel, BDD, dd, BDDSET, vars, bdd_enumerate_cb, cb, void*, context)
TASK(BDD, bdd_map_reduce_or, BDD, dd, BDDSET, vars, bdd_map_reduce_or_cb, cb, void*, context)
TASK(double, bdd_path_count, BDD, dd)
TASK(BDD, bdd_or_cube, BDD, dd, BDDSET, vars, uint8_t*, cube)
TASK(BDDSET, bdd_set_difference, BDDSET, set1, BDDSET, set2)

#ifdef __cplusplus
}
#endif /* __cplusplus */
