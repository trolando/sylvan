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
    fprintf(f, "%s%zu,", mtbdd_complement ? "!" : "", v);
    bdd_serialize_totext(f);
}

static void SYLVAN_UNUSED bdd_print(BDD bdd)
{
    bdd_fprint(stdout, bdd);
}

static inline int bdd_isconst(MTBDD bdd)
{
    return bdd == mtbdd_true || bdd == mtbdd_false ? 1 : 0;
}

static inline int bdd_isnode(MTBDD bdd)
{
    return bdd != mtbdd_true && bdd != mtbdd_false ? 1 : 0;
}

static inline BDD bdd_not(BDD dd)
{
    return dd ^ mtbdd_complement;
}

static inline BDD bdd_equiv(BDD a, BDD b)
{
    return bdd_not(bdd_xor(a, b));
}

static inline BDD bdd_or(BDD a, BDD b) {
    return bdd_not(bdd_and(bdd_not(a), bdd_not(b)));
}

static inline BDD bdd_nand(BDD a, BDD b) {
    return bdd_not(bdd_and(a, b));
}

static inline BDD bdd_nor(BDD a, BDD b) {
    return bdd_and(bdd_not(a), bdd_not(b));
}

static inline BDD bdd_imp(BDD a, BDD b) {
    return bdd_not(bdd_and(a, bdd_not(b)));
}

static inline BDD bdd_invimp(BDD a, BDD b) {
    return bdd_not(bdd_and(bdd_not(a), b));
}

static inline BDD bdd_biimp(BDD a, BDD b) {
    return bdd_equiv(a, b);
}

static inline BDD bdd_diff(BDD a, BDD b) {
    return bdd_and(a, bdd_not(b));
}

static inline BDD bdd_less(BDD a, BDD b) {
    return bdd_and(bdd_not(a), b);
}

static inline char bdd_subset(BDD a, BDD b)
{
    return bdd_disjoint(a, bdd_not(b));
}

static inline BDD bdd_nithvar(uint32_t var)
{
    return bdd_not(mtbdd_ithvar(var));
}

static inline BDD bdd_forall(BDD dd, BDDSET vars)
{
    return bdd_not(bdd_exists(bdd_not(dd), vars));
}

TASK(BDD, bdd_ite, BDD, a, BDD, b, BDD, c)
TASK(BDD, bdd_and, BDD, a, BDD, b)
TASK(BDD, bdd_xor, BDD, a, BDD, b)
TASK(char, bdd_disjoint, BDD, a, BDD, b)
TASK(BDD, bdd_exists, BDD, dd, BDD, vars)
TASK(BDD, bdd_project, BDD, dd, BDD, vars);
TASK(BDD, bdd_and_exists, BDD, a, BDD, b, BDDSET, vars)
TASK(BDD, bdd_and_project, BDD, a, BDD, b, BDDSET, vars);
TASK(BDD, bdd_relprev, BDD, a, BDD, b, BDDSET, vars)
TASK(BDD, bdd_relnext, BDD, a, BDD, b, BDDSET, vars)
TASK(BDD, bdd_closure, BDD, a)
TASK(BDD, bdd_constrain, BDD, f, BDD, c)
TASK(BDD, bdd_restrict, BDD, f, BDD, c)
TASK(BDD, bdd_compose, BDD, f, BDDMAP, m)
TASK(double, bdd_satcount, BDD, dd, BDDSET, vars)
TASK(void, bdd_enum, BDD, dd, BDDSET, vars, bdd_enum_cb, cb, void*, context)
TASK(void, bdd_enum_par, BDD, dd, BDDSET, vars, bdd_enum_cb, cb, void*, context)
TASK(BDD, bdd_collect, BDD, dd, BDDSET, vars, bdd_collect_cb, cb, void*, context)
TASK(double, bdd_pathcount, BDD, dd)
TASK(BDD, bdd_union_cube, BDD, dd, BDDSET, vars, uint8_t*, cube)

#ifdef __cplusplus
}
#endif /* __cplusplus */
