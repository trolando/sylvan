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

static const BDD bdd_complement = UINT64_C(0x8000000000000000);
static const BDD bdd_false = 0;
static const BDD bdd_true = UINT64_C(0x8000000000000000);

/**
 * Return the variable at the given level through <result>, creating its
 * canonical node when necessary. Levels are limited to 24 bits by the current
 * node encoding. On failure, <result> is unchanged.
 */
int bdd_var_at_level(BDD *result, uint32_t level);

/**
 * Create the next variable after all levels requested so far. On failure,
 * <result> is unchanged.
 */
int bdd_new_var(BDD *result);

/**
 * A BDDSET is a set of variables, represented by their positive conjunction.
 * It is not a set of values or assignments represented by a general BDD.
 * DD-producing set operations return a status and leave <result> unchanged on
 * failure.
 */
static inline BDDSET bdd_set_empty(void);
static inline int bdd_set_is_empty(BDDSET set);
static inline uint32_t bdd_set_first(BDDSET set);
static inline BDDSET bdd_set_next(BDDSET set);

/** Create a variable set from an array of levels. */
int bdd_set_from_array(BDDSET *result, const uint32_t *levels, size_t count);

/** Write the levels in a variable set to a sufficiently large array. */
void bdd_set_to_array(BDDSET set, uint32_t *levels);

/** Return the number of variables in a variable set. */
size_t bdd_set_count(BDDSET set);

/** Return the union of two variable sets through result. */
static inline int bdd_set_union(BDDSET *result, BDDSET set1, BDDSET set2);

/** Remove the variables in set2 from set1, writing the result through result. */
static inline int bdd_set_difference(BDDSET *result, BDDSET set1, BDDSET set2);

/** Return whether set contains the variable at level. */
int bdd_set_contains(BDDSET set, uint32_t level);

/** Add the variable at level to set. */
int bdd_set_add(BDDSET *result, BDDSET set, uint32_t level);

/** Remove the variable at level from set. */
int bdd_set_remove(BDDSET *result, BDDSET set, uint32_t level);

/**
 * Check if the BDD represents constants true or false.
 * For strictly non-MT BDDs (does not test if terminal)
 */
static inline int bdd_is_leaf(MTBDD bdd);

/**
 * Returns the negation of the BDD (using complement edge)
 * Assumes the BDD only has Boolean true/false.
 */
static inline BDD bdd_not(BDD dd);

/**
 * Boolean operations returning a status write to a caller-protected
 * destination. They return SYLVAN_OK on success or a negative status code on
 * failure, leaving the destination unchanged on failure.
 */

/**
 * Compute a then b else c. Returns SYLVAN_OK on success or a negative status
 * code on failure. The caller must protect <result> before calling this
 * operation. The destination is written before the Lace task completes and
 * remains unchanged on failure.
 */
static inline int bdd_ite(BDD *result, BDD a, BDD b, BDD c);

/**
 * Compute the logical AND of two BDDs. Returns SYLVAN_OK on success or a
 * negative status code on failure. The caller must protect <result> before
 * calling this operation. The destination is written before the Lace task
 * completes and remains unchanged on failure.
 */
static inline int bdd_and(BDD *result, BDD a, BDD b);

/**
 * Compute the logical XOR (exclusive or) of two BDDs.
 */
static inline int bdd_xor(BDD *result, BDD a, BDD b);

/**
 * Compute the logical equivalence of two BDDs (same as biimp).
 */
static inline int bdd_xnor(BDD *result, BDD a, BDD b);

/**
 * Compute the logical OR of two BDDs.
 */
static inline int bdd_or(BDD *result, BDD a, BDD b);

/**
 * Compute the logical NAND of two BDDs.
 */
static inline int bdd_nand(BDD *result, BDD a, BDD b);

/**
 * Compute the logical NOR of two BDDs.
 */
static inline int bdd_nor(BDD *result, BDD a, BDD b);

/**
 * Compute logical implication a → b.
 */
static inline int bdd_imp(BDD *result, BDD a, BDD b);

/**
 * Compute a ∧ ¬b (set difference when BDDs encode sets).
 */
static inline int bdd_diff(BDD *result, BDD a, BDD b);

/**
 * Return 1 if a and b have no satisfying assignment in common, 0 otherwise.
 */
static inline char bdd_disjoint(BDD a, BDD b);

/**
 * Return 1 if a implies b (every assignment satisfying a also satisfies b).
 */
static inline char bdd_subseteq(BDD a, BDD b);

/**
 * Existential quantification: compute ∃ <vars> : <dd>.
 */
static inline int bdd_exists(BDD *result, BDD dd, BDDSET vars);

/**
 * Unique (parity) quantification.
 *
 * For one variable x, this computes dd[x=0] XOR dd[x=1]. For several
 * variables, it computes the XOR of all cofactors, and is therefore true
 * exactly where an odd number of extensions over <vars> satisfy <dd>.
 * This is not an "exactly one assignment" test except for one variable.
 *
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int bdd_unique(BDD *result, BDD dd, BDDSET vars);

/**
 * Universal quantification: compute ∀ <vars> : <dd>.
 */
static inline int bdd_forall(BDD *result, BDD dd, BDDSET vars);

/**
 * Projection. Same as existential quantification, but <vars> contains
 * the variables to keep rather than eliminate.
 */
static inline int bdd_project(BDD *result, BDD dd, BDDSET vars);

/**
 * Compute ∃ <vars> : <a> ∧ <b>.
 */
static inline int bdd_and_exists(BDD *result, BDD a, BDD b, BDDSET vars);

/**
 * Compute and_exists, but as a projection (only keep given variables).
 */
static inline int bdd_and_project(BDD *result, BDD a, BDD b, BDDSET vars);

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
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int bdd_rel_prev(BDD *result, BDD a, BDD b, BDDSET vars);

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
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int bdd_rel_next(BDD *result, BDD a, BDD b, BDDSET vars);

/**
 * Computes the transitive closure by traversing the BDD recursively.
 * See Y. Matsunaga, P. C. McGeer, R. K. Brayton
 *     On Computing the Transitive Closure of a State Transition Relation
 *     30th ACM Design Automation Conference, 1993.
 *
 * The input BDD must be a transition relation that only has levels of s,t
 * with s,t interleaved with s even and t odd, i.e.
 * s level 0,2,4 matches with t level 1,3,5 and so forth.
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int bdd_transitive_closure(BDD *result, BDD dd);

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
static inline int bdd_constrain(BDD *result, BDD f, BDD c);

/**
 * Substitute the literals fixed by <cube> in <f> and remove those variables
 * from the result. The cube must be one conjunction of positive or negative
 * literals. Returns SYLVAN_ERR_INVALID and leaves <result> unchanged if
 * <cube> is not such a conjunction.
 */
int bdd_cofactor(BDD *result, BDD f, BDD cube);

/**
 * Simplify <f> with respect to the care function <c> using the Coudert-Madre
 * algorithm. The result agrees with <f> wherever <c> is true, does not
 * introduce variables that occur only in <c>, and is never larger than <f>.
 */
static inline int bdd_simplify(BDD *result, BDD f, BDD c);

/**
 * Function composition.
 * For each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of bdd_ite(<value>, <low>, <high>).
 */
static inline int bdd_compose(BDD *result, BDD f, MTBDDMAP map);

/**
 * Evaluate <f> under a complete assignment.
 *
 * <values> is packed in the order of <variables>, contains only 0 and 1, and
 * <count> must equal bdd_set_count(<variables>). The variables must contain
 * the complete support of <f>; additional variables are ignored. The result
 * is bdd_false or bdd_true.
 *
 * This is a sequential, non-allocating operation. Returns SYLVAN_OK on success
 * or SYLVAN_ERR_INVALID for invalid arguments or an encountered unassigned
 * variable. On failure, <result> is unchanged.
 */
int bdd_eval(BDD *result, BDD f, BDDSET variables, const uint8_t *values, size_t count);

/**
 * Calculate number of satisfying variable assignments.
 * The set of variables must be >= the support of the BDD.
 */
static inline double bdd_sat_count(BDD dd, BDDSET variables);

/**
 * Create a BDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * Returns SYLVAN_ERR_INVALID and leaves <result> unchanged for any other value.
 */
static inline int bdd_cube(BDD *result, BDDSET variables, const uint8_t *cube);

/**
 * Compute the union of a BDD and a cube (disjunction of the BDD with the given cube).
 */
static inline int bdd_or_cube(BDD *result, BDD dd, BDDSET variables, const uint8_t *cube);

/**
 * Pick one satisfying assignment projected to <variables>.
 * The low/false branch is preferred whenever it can reach true.
 *
 * The function will set the values of str, such that
 * str[index] where index is the index in the <variables> set is set to
 * 0 when the variable is negative, 1 when positive, or 2 when it could be either.
 *
 * This implies that str[i] will be set in the variable ordering as in <variables>.
 *
 * Returns 1 when succesful, or 0 when no assignment is found (i.e. bdd==false).
 */
int bdd_pick_cube_values(BDD bdd, BDDSET variables, uint8_t* str);

/**
 * Pick one satisfying path from <bdd>, projected to <variables>, as a cube.
 * Variables that do not occur on the chosen path are omitted from the cube.
 * The low/false branch is preferred whenever it can reach true.
 * Functionally equivalent to applying bdd_cube to bdd_pick_cube_values and
 * omitting entries marked as don't-care. If <variables> contains the support
 * of <bdd>, then the conjunction of the resulting cube and <bdd> equals the
 * resulting cube.
 */
static inline int bdd_pick_cube(BDD *result, BDD bdd, BDDSET variables);

/**
 * Pick one compatible assignment where every variable in <vars> is set to 0 or 1
 * (no "don't care" values). Writes false if no assignment exists.
 */
static inline int bdd_pick_minterm(BDD *result, BDD bdd, BDDSET vars);

/**
 * Enumerate all satisfying variable assignments from the given <bdd> using variables <vars>.
 * Calls <cb> with four parameters: a user-supplied context, the array of BDD variables in <vars>,
 * the cube (array of values 0 and 1 for each variable in <vars>) and the length of the two arrays.
 */
typedef void (*bdd_enumerate_cb)(void*, uint32_t*, uint8_t*, int);

/**
 * Enumerate all satisfying assignments sequentially.
 */
static inline void bdd_enumerate_minterms(BDD dd, BDDSET vars, bdd_enumerate_cb cb, void* context);

/**
 * Enumerate all satisfying assignments in parallel using Lace tasks.
 */
static inline void bdd_enumerate_minterms_parallel(BDD dd, BDDSET vars, bdd_enumerate_cb cb, void* context);

/**
 * Enumerate all satisfying variable assignments of the given <bdd> using variables <vars>.
 * Calls <cb> with two parameters: a user-supplied context and the cube (array of
 * values 0 and 1 for each variable in <vars>).
 * The BDD that <cb> returns is pair-wise merged using Boolean or. Returning
 * mtbdd_invalid from <cb> reports SYLVAN_ERR_CALLBACK.
 */
typedef BDD (*bdd_map_reduce_or_cb)(void*, uint8_t*);

/**
 * Collect BDDs produced by the callback for each satisfying assignment.
 * Returns SYLVAN_OK on success. On failure, <result> is unchanged.
 */
static inline int bdd_map_reduce_or(BDD *result, BDD dd, BDDSET vars, bdd_map_reduce_or_cb cb, void* context);

/**
 * Compute the number of distinct paths to true in the BDD.
 */
static inline double bdd_path_count(BDD dd);

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
