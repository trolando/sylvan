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

#ifndef SYLVAN_LDD_H
#define SYLVAN_LDD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef uint64_t MDD;       // Note: low 40 bits only

static const MDD lddmc_false = 0;
static const MDD lddmc_true = 1;

/* Initialize LDD functionality */
void sylvan_init_ldd(void);

/* Primitives */
MDD lddmc_makenode(uint32_t value, MDD ifeq, MDD ifneq);
MDD lddmc_extendnode(MDD mdd, uint32_t value, MDD ifeq);
uint32_t lddmc_getvalue(MDD mdd);
MDD lddmc_getdown(MDD mdd);
MDD lddmc_getright(MDD mdd);
MDD lddmc_follow(MDD mdd, uint32_t value);

/**
 * Copy nodes in relations.
 * A copy node represents 'read x, then write x' for every x.
 * In a read-write relation, use copy nodes twice, once on read level, once on write level.
 * Copy nodes are only supported by relprod, relprev and union.
 */

/* Primitive for special 'copy node' (for relprod/relprev) */
MDD lddmc_make_copynode(MDD ifeq, MDD ifneq);
int lddmc_iscopy(MDD mdd);
MDD lddmc_followcopy(MDD mdd);

/**
 * Infrastructure for external references using a hash table.
 * Two hash tables store external references: a pointers table and a values table.
 * The pointers table stores pointers to MDD variables, manipulated with protect and unprotect.
 * The values table stores MDD, manipulated with ref and deref.
 * We strongly recommend using the pointers table whenever possible.
 */

/**
 * Store the pointer <ptr> in the pointers table.
 */
void lddmc_protect(MDD* ptr);

/**
 * Delete the pointer <ptr> from the pointers table.
 */
void lddmc_unprotect(MDD* ptr);

/**
 * Compute the number of pointers in the pointers table.
 */
size_t lddmc_count_protected(void);

/**
 * Store the MDD <dd> in the values table.
 */
MDD lddmc_ref(MDD dd);

/**
 * Delete the MDD <dd> from the values table.
 */
void lddmc_deref(MDD dd);

/**
 * Compute the number of values in the values table.
 */
size_t lddmc_count_refs(void);

/**
 * Call mtbdd_gc_mark_rec for every mtbdd you want to keep in your custom mark functions.
 */

static inline void lddmc_gc_mark_rec(MDD dd);

/* Sanity check - returns depth of MDD including 'true' terminal or 0 for empty set */
#ifndef NDEBUG
size_t lddmc_test_ismdd(MDD mdd);
#endif

/* Operations for model checking */
static inline MDD lddmc_union(MDD a, MDD b);

static inline MDD lddmc_minus(MDD a, MDD b);

static inline MDD lddmc_zip(MDD a, MDD b, MDD* res);

static inline MDD lddmc_intersect(MDD a, MDD b);

static inline MDD lddmc_match(MDD a, MDD b, MDD proj);

MDD lddmc_union_cube(MDD a, uint32_t* values, size_t count);
int lddmc_member_cube(MDD a, uint32_t* values, size_t count);
MDD lddmc_cube(uint32_t* values, size_t count);

MDD lddmc_union_cube_copy(MDD a, uint32_t* values, int* copy, size_t count);
int lddmc_member_cube_copy(MDD a, uint32_t* values, int* copy, size_t count);
MDD lddmc_cube_copy(uint32_t* values, int* copy, size_t count);

TASK_3(MDD, lddmc_relprod, MDD, a, MDD, b, MDD, proj)

TASK_4(MDD, lddmc_relprod_union, MDD, a, MDD, b, MDD, meta, MDD, un)

/**
 * Calculate all predecessors to a in uni according to rel[proj]
 * <proj> follows the same semantics as relprod
 * i.e. 0 (not in rel), 1 (read+write), 2 (read), 3 (write), -1 (end; rest=0)
 */
TASK_4(MDD, lddmc_relprev, MDD, dd, MDD, rel, MDD, proj, MDD, uni);

// so: proj: -2 (end; quantify rest), -1 (end; keep rest), 0 (quantify), 1 (keep)
TASK_2(MDD, lddmc_project, MDD, dd, MDD, proj);

TASK_3(MDD, lddmc_project_minus, MDD, dd, MDD, proj, MDD, avoid);

TASK_4(MDD, lddmc_join, MDD, a, MDD, b, MDD, a_proj, MDD, b_proj);

/* Write a DOT representation */
void lddmc_printdot(MDD mdd);
void lddmc_fprintdot(FILE *out, MDD mdd);

void lddmc_fprint(FILE *out, MDD mdd);
void lddmc_print(MDD mdd);

void lddmc_printsha(MDD mdd);
void lddmc_fprintsha(FILE *out, MDD mdd);
void lddmc_getsha(MDD mdd, char *target); // at least 65 bytes...

/**
 * Calculate number of satisfying variable assignments.
 * The set of variables must be >= the support of the MDD.
 * (i.e. all variables in the MDD must be in variables)
 *
 * The cached version uses the operation cache, but is limited to 64-bit floating point numbers.
 */

typedef double lddmc_satcount_double_t;
// if this line below gives an error, modify the above typedef until fixed ;)
typedef char __lddmc_check_float_is_8_bytes[(sizeof(lddmc_satcount_double_t) == sizeof(uint64_t))?1:-1];

TASK_1(lddmc_satcount_double_t, lddmc_satcount_cached, MDD, dd);

TASK_1(long double, lddmc_satcount, MDD, dd);

/**
 * A callback for enumerating functions like sat_all_par, collect and match
 * Example:
 * TASK_3(void*, my_function, uint32_t*, values, size_t, count, void*, context) ...
 * For collect, use:
 * TASK_3(MDD, ...)
 */
typedef void (*lddmc_enum_cb)(uint32_t*, size_t, void*);
typedef MDD (*lddmc_collect_cb)(uint32_t*, size_t, void*);

VOID_TASK_5(lddmc_sat_all_par, MDD, dd, lddmc_enum_cb, cb, void*, context, uint32_t*, arr, size_t, len);

VOID_TASK_3(lddmc_sat_all_nopar, MDD, dd, lddmc_enum_cb, cb, void*, context);

TASK_5(MDD, lddmc_collect, MDD, dd, lddmc_collect_cb, cb, void*, context, uint32_t*, arr, size_t, len);

VOID_TASK_5(lddmc_match_sat_par, MDD, dd, MDD, match, MDD, proj, lddmc_enum_cb, cb, void*, context);

int lddmc_sat_one(MDD mdd, uint32_t *values, size_t count);
MDD lddmc_sat_one_mdd(MDD mdd);
#define lddmc_pick_cube lddmc_sat_one_mdd

/**
 * Callback functions for visiting nodes.
 * lddmc_visit_seq sequentially visits nodes, down first, then right.
 * lddmc_visit_par visits nodes in parallel (down || right)
 */
typedef int (*lddmc_visit_pre_cb)(MDD, void*); // int pre(MDD, context)
typedef void (*lddmc_visit_post_cb)(MDD, void*); // void post(MDD, context)
typedef void (*lddmc_visit_init_context_cb)(void*, void*, int); // void init_context(context, parent, is_down)

typedef struct lddmc_visit_node_callbacks {
    lddmc_visit_pre_cb lddmc_visit_pre;
    lddmc_visit_post_cb lddmc_visit_post;
    lddmc_visit_init_context_cb lddmc_visit_init_context;
} lddmc_visit_callbacks_t;

VOID_TASK_4(lddmc_visit_par, MDD, dd, lddmc_visit_callbacks_t*, cbs, size_t, ctx_size, void*, context);

VOID_TASK_4(lddmc_visit_seq, MDD, dd, lddmc_visit_callbacks_t*, cbs, size_t, ctx_size, void*, context);

size_t lddmc_nodecount(MDD mdd);
void lddmc_nodecount_levels(MDD mdd, size_t *variables);

/**
 * Functional composition
 * For every node at depth <depth>, call function cb (MDD -> MDD).
 * and replace the node by the result of the function
 */
typedef MDD (*lddmc_compose_cb)(MDD, void*);
TASK_4(MDD, lddmc_compose, MDD, dd, lddmc_compose_cb, cb, void*, context, int, depth);

/**
 * SAVING:
 * use lddmc_serialize_add on every MDD you want to store
 * use lddmc_serialize_get to retrieve the key of every stored MDD
 * use lddmc_serialize_tofile
 *
 * LOADING:
 * use lddmc_serialize_fromfile (implies lddmc_serialize_reset)
 * use lddmc_serialize_get_reversed for every key
 *
 * MISC:
 * use lddmc_serialize_reset to free all allocated structures
 * use lddmc_serialize_totext to write a textual list of tuples of all MDDs.
 *         format: [(<key>,<level>,<key_low>,<key_high>,<complement_high>),...]
 *
 * for the old lddmc_print functions, use lddmc_serialize_totext
 */
size_t lddmc_serialize_add(MDD mdd);
size_t lddmc_serialize_get(MDD mdd);
MDD lddmc_serialize_get_reversed(size_t value);
void lddmc_serialize_reset(void);
void lddmc_serialize_totext(FILE *out);
void lddmc_serialize_tofile(FILE *out);
void lddmc_serialize_fromfile(FILE *in);

/**
 * Infrastructure for internal references.
 * Every thread has its own reference stacks. There are three stacks: pointer, values, tasks stack.
 * The pointers stack stores pointers to LDD variables, manipulated with pushptr and popptr.
 * The values stack stores LDD, manipulated with push and pop.
 * The tasks stack stores Lace tasks (that return LDD), manipulated with spawn and sync.
 *
 * It is recommended to use the pointers stack for local variables and the tasks stack for tasks.
 */

/**
 * Push a LDD variable to the pointer reference stack.
 * During garbage collection the variable will be inspected and the contents will be marked.
 */
void lddmc_refs_pushptr(const MDD *ptr);

/**
 * Pop the last <amount> LDD variables from the pointer reference stack.
 */
void lddmc_refs_popptr(size_t amount);

/**
 * Push an LDD to the values reference stack.
 * During garbage collection the references LDD will be marked.
 */
MDD lddmc_refs_push(MDD dd);

/**
 * Pop the last <amount> LDD from the values reference stack.
 */
void lddmc_refs_pop(long amount);

/**
 * Push a Task that returns an LDD to the tasks reference stack.
 * Usage: lddmc_refs_spawn(SPAWN(function, ...));
 */
void lddmc_refs_spawn(lace_task* t);

/**
 * Pop a Task from the task reference stack.
 * Usage: MDD result = lddmc_refs_sync(SYNC(function));
 */
MDD lddmc_refs_sync(MDD dd);

VOID_TASK_1(lddmc_gc_mark_rec, MDD, dd)
TASK_2(MDD, lddmc_union, MDD, a, MDD, b);
TASK_2(MDD, lddmc_minus, MDD, a, MDD, b);
TASK_3(MDD, lddmc_zip, MDD, a, MDD, b, MDD*, res);
TASK_2(MDD, lddmc_intersect, MDD, a, MDD, b);
TASK_3(MDD, lddmc_match, MDD, a, MDD, b, MDD, proj);



#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
