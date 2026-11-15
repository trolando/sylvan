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

/* Do not include this file directly. Instead, include sylvan.h */

#ifndef SYLVAN_BDD_H
#define SYLVAN_BDD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* For strictly non-MT BDDs */
static inline int
sylvan_isconst(MTBDD bdd)
{
    return bdd == mtbdd_true || bdd == mtbdd_false ? 1 : 0;
}

static inline int
sylvan_isnode(MTBDD bdd)
{
    return bdd != mtbdd_true && bdd != mtbdd_false ? 1 : 0;
}

/**
 * Granularity (BDD only) determines usage of operation cache.
 * The smallest value is 1: use the operation cache always.
 * Higher values mean that the cache is used less often. Variables are grouped
 * such that the cache is used when going to the next group, i.e., with
 * granularity=3, variables [0,1,2] are in the first group, [3,4,5] in the next, etc.
 * Then no caching occur between 0->1, 1->2, 0->2. Caching occurs on 0->3, 1->4, 2->3, etc.
 *
 * The appropriate value depends on the number of variables and the structure of
 * the decision diagrams. When in doubt, choose a low value (1-5). The performance
 * gain can be around 0-10%, so it is not extremely important.
 */
void sylvan_set_granularity(int granularity);
int sylvan_get_granularity(void);

/*
 * Unary, binary and if-then-else operations.
 * These operations are all implemented by NOT, AND and XOR.
 */
static inline BDD
sylvan_not(BDD a)
{
    return a ^ sylvan_complement;
}

TASK_DECL_4(BDD, sylvan_ite, BDD, BDD, BDD, BDDVAR);
#define sylvan_ite(a,b,c) (RUN(sylvan_ite,a,b,c,0))
TASK_DECL_3(BDD, sylvan_and, BDD, BDD, BDDVAR);
#define sylvan_and(a,b) (RUN(sylvan_and,a,b,0))
TASK_DECL_3(BDD, sylvan_xor, BDD, BDD, BDDVAR);
#define sylvan_xor(a,b) (RUN(sylvan_xor,a,b,0))
#define sylvan_equiv(a,b) sylvan_not(sylvan_xor(a,b))
#define sylvan_or(a,b) sylvan_not(sylvan_and(sylvan_not(a),sylvan_not(b)))
#define sylvan_nand(a,b) sylvan_not(sylvan_and(a,b))
#define sylvan_nor(a,b) sylvan_not(sylvan_or(a,b))
#define sylvan_imp(a,b) sylvan_not(sylvan_and(a,sylvan_not(b)))
#define sylvan_invimp(a,b) sylvan_not(sylvan_and(sylvan_not(a),b))
#define sylvan_biimp sylvan_equiv
#define sylvan_diff(a,b) sylvan_and(a,sylvan_not(b))
#define sylvan_less(a,b) sylvan_and(sylvan_not(a),b)

TASK_DECL_3(char, sylvan_disjoint, BDD, BDD, BDDVAR);
#define sylvan_disjoint(a,b) (RUN(sylvan_disjoint,a,b,0))
#define sylvan_subset(a,b) (RUN(sylvan_disjoint,a,sylvan_not(b),0))

/**
 * Return a non-false BDD contained in the intersection of <a> and <b>, or
 * sylvan_false if they are disjoint. The result need not be the complete
 * intersection or a single cube. The search prefers low branches and stops
 * searching other branches once a witness is found.
 * Both inputs must be Boolean BDDs. Returns sylvan_invalid for a
 * sylvan_invalid argument. Protect inputs and results as for sylvan_and.
 */
TASK_DECL_2(BDD, sylvan_intersection_witness, BDD, BDD)
#define sylvan_intersection_witness(a, b) RUN(sylvan_intersection_witness, a, b)

/* Create a BDD representing just <var> or the negation of <var> */
static inline BDD
sylvan_nithvar(uint32_t var)
{
    return sylvan_not(sylvan_ithvar(var));
}

/**
 * Existential and universal quantification.
 */
TASK_DECL_3(BDD, sylvan_exists, BDD, BDD, BDDVAR);
#define sylvan_exists(a, vars) (RUN(sylvan_exists, a, vars, 0))
#define sylvan_forall(a, vars) (sylvan_not(RUN(sylvan_exists, sylvan_not(a), vars, 0)))

/**
 * Unique (parity) quantification: XOR all cofactors of <a> over <variables>.
 * The result is true exactly where an odd number of extensions over the set
 * satisfy <a>, not necessarily exactly one. An empty set returns <a>; a
 * quantified variable absent from <a> makes the result false.
 * <a> must be a Boolean BDD. <variables> must be a positive cube with
 * increasing variables, false low edges and a final true leaf (the empty
 * set). Variables outside the set remain free. Returns sylvan_invalid for a
 * sylvan_invalid argument. Protect inputs and results as for sylvan_exists.
 */
TASK_DECL_2(BDD, sylvan_unique, BDD, BDDSET)
#define sylvan_unique(a, variables) RUN(sylvan_unique, a, variables)

/**
 * Select one satisfying assignment of <variables> for every assignment to
 * the remaining variables that can be extended to satisfy <a>. The result
 * is a subset of <a> with the same existential abstraction over the set.
 * Selection prefers false at the first differing selected variable, in
 * increasing variable order. Selected variables absent from <a> are assigned
 * false. An empty set returns <a>; a false input returns false.
 * <a> must be a Boolean BDD. <variables> must be a positive cube with
 * increasing variables, false low edges and a final true leaf (the empty
 * set). Returns sylvan_invalid for a sylvan_invalid argument. Protect inputs
 * and results as for sylvan_exists.
 */
TASK_DECL_2(BDD, sylvan_pick_representatives, BDD, BDDSET)
#define sylvan_pick_representatives(a, variables) RUN(sylvan_pick_representatives, a, variables)

/**
 * Projection. (Same as existential quantification, but <vars> contains variables to keep.
 */
TASK_DECL_2(BDD, sylvan_project, BDD, BDD);
#define sylvan_project(a, vars) RUN(sylvan_project, a, vars)

/**
 * Compute \exists <vars>: <a> \and <b>
 */
TASK_DECL_4(BDD, sylvan_and_exists, BDD, BDD, BDDSET, BDDVAR);
#define sylvan_and_exists(a,b,vars) RUN(sylvan_and_exists,a,b,vars,0)

/**
 * Compute and_exists, but as a projection (only keep given variables)
 */
TASK_DECL_3(BDD, sylvan_and_project, BDD, BDD, BDDSET);
#define sylvan_and_project(a,b,vars) RUN(sylvan_and_project,a,b,vars)

/**
 * Compute R(s,t) = \exists x: A(s,x) \and B(x,t)
 *      or R(s)   = \exists x: A(s,x) \and B(x)
 * Assumes s,t are interleaved with s even and t odd (s+1).
 * Parameter vars is the cube of all s and/or t variables.
 * Other variables in A are "ignored" (existential quantification)
 * Other variables in B are kept
 * Alternatively, vars=false means all variables are in vars
 *
 * Use this function to concatenate two relations   --> -->
 * or to take the 'previous' of a set               -->  S
 */
TASK_DECL_4(BDD, sylvan_relprev, BDD, BDD, BDDSET, BDDVAR);
#define sylvan_relprev(a,b,vars) RUN(sylvan_relprev,a,b,vars,0)

/**
 * Compute R(s) = \exists x: A(x) \and B(x,s)
 * with support(result) = s, support(A) = s, support(B) = s+t
 * Assumes s,t are interleaved with s even and t odd (s+1).
 * Parameter vars is the cube of all s and/or t variables.
 * Other variables in A are kept
 * Other variables in B are "ignored" (existential quantification)
 * Alternatively, vars=false means all variables are in vars
 *
 * Use this function to take the 'next' of a set     S  -->
 */
TASK_DECL_4(BDD, sylvan_relnext, BDD, BDD, BDDSET, BDDVAR);
#define sylvan_relnext(a,b,vars) RUN(sylvan_relnext,a,b,vars,0)

/**
 * Computes the transitive closure (paths of one or more steps) recursively.
 * The result is reflexive only where the relation has a cycle.
 * See Y. Matsunaga, P. C. McGeer, R. K. Brayton
 *     On Computing the Transitive Closure of a State Transition Relation
 *     30th ACM Design Automation Conference, 1993.
 *
 * The input BDD must be a transition relation that only has levels of s,t
 * with s,t interleaved with s even and t odd, i.e.
 * s level 0,2,4 matches with t level 1,3,5 and so forth.
 */
TASK_DECL_2(BDD, sylvan_closure, BDD, BDDVAR);
#define sylvan_closure(a) RUN(sylvan_closure,a,0);

/**
 * Compute f@c (f constrain c), such that f and f@c are the same when c is true
 * The BDD c is also called the "care function"
 * Special cases:
 *   - f@0 = 0
 *   - f@1 = f
 *   - 0@c = 0
 *   - 1@c = 1
 *   - f@f = 1
 *   - f@not(f) = 0
 */
TASK_DECL_3(BDD, sylvan_constrain, BDD, BDD, BDDVAR);
#define sylvan_constrain(f,c) (RUN(sylvan_constrain, f, c, 0))

/**
 * Compute restrict f@c, which uses a heuristic to try and minimize a BDD f with respect to a care function c
 * Similar to constrain, but avoids introducing variables from c into f.
 */
TASK_DECL_3(BDD, sylvan_restrict, BDD, BDD, BDDVAR);
#define sylvan_restrict(f,c) (RUN(sylvan_restrict, f, c, 0))

/**
 * Restrict <f> by a cube of positive or negative literals.
 * Returns sylvan_invalid if <cube> is not a cube. The empty cube is sylvan_true;
 * sylvan_false is not a cube. Unlike sylvan_restrict, rejects general care sets.
 */
TASK_DECL_2(BDD, sylvan_cofactor, BDD, BDD)
#define sylvan_cofactor(f, cube) RUN(sylvan_cofactor, f, cube)

/**
 * Evaluate <f> under a complete assignment, without allocating nodes.
 * <values> contains <count> entries, each 0 or 1, in the order of <variables>.
 * <count> must equal sylvan_set_count(variables); the set must contain the support
 * of <f>. Additional variables are ignored. Returns sylvan_false or sylvan_true,
 * or sylvan_invalid for invalid arguments or an encountered unassigned variable.
 * The traversal is sequential and follows only the selected path.
 */
TASK_DECL_4(BDD, sylvan_eval, BDD, BDDSET, const uint8_t*, size_t)
#define sylvan_eval(f, variables, values, count) RUN(sylvan_eval, f, variables, values, count)

/**
 * Function composition.
 * For each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of sylvan_ite(<value>, <low>, <high>).
 */
TASK_DECL_3(BDD, sylvan_compose, BDD, BDDMAP, BDDVAR);
#define sylvan_compose(f,m) (RUN(sylvan_compose, (f), (m), 0))

/**
 * Calculate number of satisfying variable assignments.
 * The set of variables must be >= the support of the BDD.
 */

TASK_DECL_3(double, sylvan_satcount, BDD, BDDSET, BDDVAR);
#define sylvan_satcount(bdd, variables) RUN(sylvan_satcount, bdd, variables, 0)

/**
 * Calculate the exact number of satisfying assignments over <variables>.
 * <variables> must be a positive cube: increasing variables, false low edges,
 * and a final true leaf (sylvan_true is the empty set). It must contain the
 * support of <bdd>. The set is validated once, in O(set size).
 * Returns 1 on success, or 0 on overflow, a NULL <result>, sylvan_invalid,
 * a non-Boolean leaf in <bdd>, an invalid set, or a missing support variable.
 * On failure, <result> is unchanged.
 */
TASK_DECL_3(int, sylvan_satcount_u64, BDD, BDDSET, uint64_t*)
#define sylvan_satcount_u64(bdd, variables, result) RUN(sylvan_satcount_u64, bdd, variables, result)

/**
 * Construct the BDD that is true exactly when between <minimum> and
 * <maximum> variables in <variables> are true, inclusive. Variables outside
 * the set remain free. This covers exact-k, at-most-k and at-least-k tests.
 * <variables> must be a positive cube with increasing variables, false low
 * edges and a final true leaf (the empty set). The set is validated once.
 * Requires minimum <= maximum <= sylvan_set_count(variables); returns
 * sylvan_invalid for an invalid set or bounds. The empty set with bounds
 * [0, 0] returns sylvan_true. Protect the set and result as for sylvan_exists.
 */
TASK_DECL_3(BDD, sylvan_cardinality, BDDSET, size_t, size_t)
#define sylvan_cardinality(variables, minimum, maximum) RUN(sylvan_cardinality, variables, minimum, maximum)

/**
 * Create a BDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * CHANGED 2014/09/19: vars is now a BDDSET (ordered!)
 */
BDD sylvan_cube(BDDSET variables, uint8_t *cube);
TASK_DECL_3(BDD, sylvan_union_cube, BDD, BDDSET, uint8_t*);
#define sylvan_union_cube(bdd, variables, cube) RUN(sylvan_union_cube, bdd, variables, cube)

/**
 * Pick one satisfying variable assignment randomly for which <bdd> is true.
 * The <variables> set must include all variables in the support of <bdd>.
 *
 * The function will set the values of str, such that
 * str[index] where index is the index in the <variables> set is set to
 * 0 when the variable is negative, 1 when positive, or 2 when it could be either.
 *
 * This implies that str[i] will be set in the variable ordering as in <variables>.
 *
 * Returns 1 when succesful, or 0 when no assignment is found (i.e. bdd==sylvan_false).
 */
int sylvan_sat_one(BDD bdd, BDDSET variables, uint8_t* str);

/**
 * Pick one satisfying variable assignment randomly from the given <bdd>.
 * Functionally equivalent to performing sylvan_cube on the result of sylvan_sat_one.
 * For the result: sylvan_and(res, bdd) = res.
 */
BDD sylvan_sat_one_bdd(BDD bdd);
#define sylvan_pick_cube sylvan_sat_one_bdd

BDD sylvan_sat_single(BDD bdd, BDDSET vars);
#define sylvan_pick_single_cube sylvan_sat_single

/**
 * Enumerate all satisfying variable assignments from the given <bdd> using variables <vars>.
 * Calls <cb> with four parameters: a user-supplied context, the array of BDD variables in <vars>,
 * the cube (array of values 0 and 1 for each variables in <vars>) and the length of the two arrays.
 */
LACE_TYPEDEF_CB(void, enum_cb, void*, BDDVAR*, uint8_t*, int);
VOID_TASK_DECL_4(sylvan_enum, BDD, BDDSET, enum_cb, void*);
#define sylvan_enum(bdd, vars, cb, context) RUN(sylvan_enum, bdd, vars, cb, context)
VOID_TASK_DECL_4(sylvan_enum_par, BDD, BDDSET, enum_cb, void*);
#define sylvan_enum_par(bdd, vars, cb, context) RUN(sylvan_enum_par, bdd, vars, cb, context)

/**
 * Enumerate all satisfyable variable assignments of the given <bdd> using variables <vars>.
 * Calls <cb> with two parameters: a user-supplied context and the cube (array of
 * values 0 and 1 for each variable in <vars>).
 * The BDD that <cb> returns is pair-wise merged (using or) and returned.
 */
LACE_TYPEDEF_CB(BDD, sylvan_collect_cb, void*, uint8_t*);
TASK_DECL_4(BDD, sylvan_collect, BDD, BDDSET, sylvan_collect_cb, void*);
#define sylvan_collect(bdd, vars, cb, context) RUN(sylvan_collect, bdd, vars, cb, context)

/**
 * Compute the number of distinct paths to sylvan_true in the BDD
 */
TASK_DECL_2(double, sylvan_pathcount, BDD, BDDVAR);
#define sylvan_pathcount(bdd) (RUN(sylvan_pathcount, bdd, 0))

/**
 * SAVING:
 * use sylvan_serialize_add on every BDD you want to store
 * use sylvan_serialize_get to retrieve the key of every stored BDD
 * use sylvan_serialize_tofile
 *
 * LOADING:
 * use sylvan_serialize_fromfile (implies sylvan_serialize_reset)
 * use sylvan_serialize_get_reversed for every key
 *
 * MISC:
 * use sylvan_serialize_reset to free all allocated structures
 * use sylvan_serialize_totext to write a textual list of tuples of all BDDs.
 *         format: [(<key>,<level>,<key_low>,<key_high>,<complement_high>),...]
 */
size_t sylvan_serialize_add(BDD bdd);
size_t sylvan_serialize_get(BDD bdd);
BDD sylvan_serialize_get_reversed(size_t value);
void sylvan_serialize_reset(void);
void sylvan_serialize_totext(FILE *out);
void sylvan_serialize_tofile(FILE *out);
void sylvan_serialize_fromfile(FILE *in);

static void SYLVAN_UNUSED
sylvan_fprint(FILE *f, BDD bdd)
{
    sylvan_serialize_reset();
    size_t v = sylvan_serialize_add(bdd);
    fprintf(f, "%s%zu,", bdd&sylvan_complement?"!":"", v);
    sylvan_serialize_totext(f);
}

static void SYLVAN_UNUSED
sylvan_print(BDD bdd)
{
    sylvan_fprint(stdout, bdd);
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
