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

/* Do not include this file directly. Instead, include sylvan.h */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif 

/**
 * Check if the BDD represents constants true or false.
 * For strictly non-MT BDDs (does not test if terminal)
 */
static inline int bdd_isconst(MTBDD bdd);

/**
 * Return 1 if the given BDD is a node instead of a leaf/terminal
 */
static inline int bdd_isnode(MTBDD bdd);

/**
 * Returns the negation of the BDD (using complement edge)
 * Assumes the BDD only has Boolean true/false.
 */
static inline BDD bdd_not(BDD dd);

/**
 * Computes a then b else c. Assumes all parameters are Boolean BDDs.
 */
static inline BDD bdd_ite(BDD a, BDD b, BDD c);

/**
 * Compute the logical AND of two BDDs.
 */
static inline BDD bdd_and(BDD a, BDD b);

/**
 * Compute the logical XOR (exclusive or) of two BDDs.
 */
static inline BDD bdd_xor(BDD a, BDD b);

/**
 * Compute the logical equivalence of two BDDs (same as biimp).
 */
static inline BDD bdd_equiv(BDD a, BDD b);

/**
 * Compute the logical OR of two BDDs.
 */
static inline BDD bdd_or(BDD a, BDD b);

/**
 * Compute the logical NAND of two BDDs.
 */
static inline BDD bdd_nand(BDD a, BDD b);

/**
 * Compute the logical NOR of two BDDs.
 */
static inline BDD bdd_nor(BDD a, BDD b);

/**
 * Compute logical implication a → b.
 */
static inline BDD bdd_imp(BDD a, BDD b);

/**
 * Compute reverse implication b → a.
 */
static inline BDD bdd_invimp(BDD a, BDD b);

/**
 * Compute bi-implication (logical equivalence) of two BDDs.
 */
static inline BDD bdd_biimp(BDD a, BDD b);

/**
 * Compute a ∧ ¬b (set difference when BDDs encode sets).
 */
static inline BDD bdd_diff(BDD a, BDD b);

/**
 * Compute ¬a ∧ b (reverse difference).
 */
static inline BDD bdd_less(BDD a, BDD b);

/**
 * Return 1 if a and b have no satisfying assignment in common, 0 otherwise.
 */
static inline char bdd_disjoint(BDD a, BDD b);

/**
 * Return 1 if a implies b (every assignment satisfying a also satisfies b).
 */
static inline char bdd_subset(BDD a, BDD b);

/**
 * Create a BDD representing just the negation of <var>.
 */
static inline BDD bdd_nithvar(uint32_t var);

/**
 * Existential quantification: compute ∃ <vars> : <dd>.
 */
static inline BDD bdd_exists(BDD dd, BDDSET vars);

/**
 * Universal quantification: compute ∀ <vars> : <dd>.
 */
static inline BDD bdd_forall(BDD dd, BDDSET vars);

/**
 * Projection. Same as existential quantification, but <vars> contains
 * the variables to keep rather than eliminate.
 */
static inline BDD bdd_project(BDD dd, BDDSET vars);

/**
 * Compute ∃ <vars> : <a> ∧ <b>.
 */
static inline BDD bdd_and_exists(BDD a, BDD b, BDDSET vars);

/**
 * Compute and_exists, but as a projection (only keep given variables).
 */
static inline BDD bdd_and_project(BDD a, BDD b, BDDSET vars);

/**
 * Compute R(s,t) = ∃ x: A(s,x) ∧ B(x,t)
 *      or R(s)   = ∃ x: A(s,x) ∧ B(x)
 * Assumes s,t are interleaved with s even and t odd (s+1).
 * Parameter vars is the cube of all s and/or t variables.
 * Other variables in A are "ignored" (existential quantification)
 * Other variables in B are kept.
 * Alternatively, vars=false means all variables are in vars.
 *
 * Use this function to concatenate two relations   --> -->
 * or to take the 'previous' of a set               -->  S
 */
static inline BDD bdd_relprev(BDD a, BDD b, BDDSET vars);

/**
 * Compute R(s) = ∃ x: A(x) ∧ B(x,s)
 * with support(result) = s, support(A) = s, support(B) = s+t
 * Assumes s,t are interleaved with s even and t odd (s+1).
 * Parameter vars is the cube of all s and/or t variables.
 * Other variables in A are kept.
 * Other variables in B are "ignored" (existential quantification)
 * Alternatively, vars=false means all variables are in vars.
 *
 * Use this function to take the 'next' of a set     S  -->
 */
static inline BDD bdd_relnext(BDD a, BDD b, BDDSET vars);

/**
 * Computes the transitive closure by traversing the BDD recursively.
 * See Y. Matsunaga, P. C. McGeer, R. K. Brayton
 *     On Computing the Transitive Closure of a State Transition Relation
 *     30th ACM Design Automation Conference, 1993.
 *
 * The input BDD must be a transition relation that only has levels of s,t
 * with s,t interleaved with s even and t odd, i.e.
 * s level 0,2,4 matches with t level 1,3,5 and so forth.
 */
static inline BDD bdd_closure(BDD dd);

/**
 * Compute f@c (f constrain c), such that f and f@c are the same when c is true.
 * The BDD c is also called the "care function".
 * Special cases:
 *   - f@0 = 0
 *   - f@1 = f
 *   - 0@c = 0
 *   - 1@c = 1
 *   - f@f = 1
 *   - f@¬f = 0
 */
static inline BDD bdd_constrain(BDD f, BDD c);

/**
 * Compute restrict f@c, which uses a heuristic to try and minimize a BDD f
 * with respect to a care function c.
 * Similar to constrain, but avoids introducing variables from c into f.
 */
static inline BDD bdd_restrict(BDD f, BDD c);

/**
 * Function composition.
 * For each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of bdd_ite(<value>, <low>, <high>).
 */
static inline BDD bdd_compose(BDD f, BDD map);

/**
 * Calculate number of satisfying variable assignments.
 * The set of variables must be >= the support of the BDD.
 */
static inline double bdd_satcount(BDD dd, BDDSET variables);

/**
 * Create a BDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 */
BDD bdd_cube(BDDSET variables, uint8_t *cube);

/**
 * Compute the union of a BDD and a cube (disjunction of the BDD with the given cube).
 */
static inline BDD bdd_union_cube(BDD dd, BDDSET variables, uint8_t* cube);

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
 * Returns 1 when succesful, or 0 when no assignment is found (i.e. bdd==false).
 */
int bdd_sat_one(BDD bdd, BDDSET variables, uint8_t* str);

/**
 * Pick one satisfying variable assignment randomly from the given <bdd>.
 * Functionally equivalent to performing bdd_cube on the result of bdd_sat_one.
 * For the result: bdd_and(res, bdd) = res.
 */
BDD bdd_sat_one_bdd(BDD bdd);

/**
 * Pick one satisfying assignment where every variable in <vars> is set to 0 or 1
 * (no "don't care" values). Returns false if no assignment exists.
 */
BDD bdd_sat_single(BDD bdd, BDDSET vars);

/**
 * Enumerate all satisfying variable assignments from the given <bdd> using variables <vars>.
 * Calls <cb> with four parameters: a user-supplied context, the array of BDD variables in <vars>,
 * the cube (array of values 0 and 1 for each variable in <vars>) and the length of the two arrays.
 */
typedef void (*bdd_enum_cb)(void*, BDDVAR*, uint8_t*, int);

/**
 * Enumerate all satisfying assignments sequentially.
 */
static inline void bdd_enum(BDD dd, BDDSET vars, bdd_enum_cb cb, void* context);

/**
 * Enumerate all satisfying assignments in parallel using Lace tasks.
 */
static inline void bdd_enum_par(BDD dd, BDDSET vars, bdd_enum_cb cb, void* context);

/**
 * Enumerate all satisfying variable assignments of the given <bdd> using variables <vars>.
 * Calls <cb> with two parameters: a user-supplied context and the cube (array of
 * values 0 and 1 for each variable in <vars>).
 * The BDD that <cb> returns is pair-wise merged (using or) and returned.
 */
typedef BDD (*bdd_collect_cb)(void*, uint8_t*);

/**
 * Collect BDDs produced by the callback for each satisfying assignment.
 */
static inline BDD bdd_collect(BDD dd, BDDSET vars, bdd_collect_cb cb, void* context);

/**
 * Compute the number of distinct paths to true in the BDD.
 */
static inline double bdd_pathcount(BDD dd);

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

 /**
  * Add the given BDD to the serialization buffer. Returns a key for later retrieval.
  */
size_t bdd_serialize_add(BDD bdd);

/**
 * Retrieve the serialization key of a previously added BDD.
 */
size_t bdd_serialize_get(BDD bdd);

/**
 * Retrieve a BDD from its serialization key (after loading from file).
 */
BDD bdd_serialize_get_reversed(size_t value);

/**
 * Free all structures allocated by the serialization mechanism.
 */
void bdd_serialize_reset(void);

/**
 * Write a textual representation of all serialized BDDs to the given file.
 */
void bdd_serialize_totext(FILE *out);

/**
 * Write all serialized BDDs in binary format to the given file.
 */
void bdd_serialize_tofile(FILE *out);

/**
 * Read serialized BDDs from the given file. Implies bdd_serialize_reset.
 */
void bdd_serialize_fromfile(FILE *in);

/**
 * Print a textual representation of the BDD to the given file.
 */
static void SYLVAN_UNUSED bdd_fprint(FILE* f, BDD bdd);

/**
 * Print a textual representation of the BDD to stdout.
 */
static void SYLVAN_UNUSED bdd_print(BDD bdd);

#ifdef __cplusplus
}
#endif /* __cplusplus */
