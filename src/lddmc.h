#include <stdint.h>
#include <stdio.h> // for FILE
#include <lace.h> // for definitions

#ifndef LDDMC_H
#define LDDMC_H

/**
 * LDDmc: multi-core ListDD package.
 *
 * This package requires parallel work-stealing framework Lace.
 * Lace must be initialized before calling any LDDMC operations.
 *
 * This package uses explicit referencing.
 * Use lddmc_ref and lddmc_deref to manage external references.
 *
 * Garbage collection requires all workers to cooperate. Garbage collection is either initiated
 * by the user (calling lddmc_gc) or when the MDD node table is full. All LDDMC operations
 * check whether they need to cooperate on garbage collection. Garbage collection cannot occur
 * otherwise. This means that it is perfectly fine to do this:
 *              MDD a = lddmc_ref(lddmc_and(b, c));
 * since it is not possible that garbage collection occurs between the two calls.
 */

// We only support 64-bit systems...
typedef char __lddmc_check_64bit_system[(sizeof(uint64_t) == sizeof(size_t))?1:-1];

typedef uint64_t MDD;       // Note: low 42 bits only

#define lddmc_false         ((MDD)0)
#define lddmc_true          ((MDD)1)

/**
 * Initialize LDDMC.
 *
 * Allocates a MDD node table of size "2^datasize" and a operation cache of size "2^cachesize".
 * Every MDD node requires 32 bytes memory. (16 data + 16 bytes overhead)
 * Every operation cache entry requires 36 bytes memory. (32 bytes data + 4 bytes overhead)
 *
 * Granularity determines usage of operation cache. Smallest value is 1: use the operation cache always.
 * Higher values mean that the cache is used less often. Variables are grouped such that
 * the cache is used when going to the next group, i.e., with granularity=3, variables [0,1,2] are in the
 * first group, [3,4,5] in the next, etc. Then no caching occur between 0->1, 1->2, 0->2. Caching occurs
 * on 0->3, 1->4, 2->3, etc.
 *
 * Reasonable defaults: datasize of 26 (2048 MB), cachesize of 24 (576 MB), granularity of 4-16
 */
void lddmc_init(size_t datasize, size_t cachesize);

/**
 * Frees all Sylvan data.
 */
void lddmc_quit();

/* Primitives */
MDD lddmc_makenode(uint32_t value, MDD ifeq, MDD ifneq);
MDD lddmc_extendnode(MDD mdd, uint32_t value, MDD ifeq);
uint32_t lddmc_value(MDD mdd);
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

/* Add or remove external reference to MDD */
MDD lddmc_ref(MDD a);
void lddmc_deref(MDD a);
size_t lddmc_count_refs();

/* Garbage collection */
void lddmc_gc();
void lddmc_gc_enable();
void lddmc_gc_disable();

/* Sanity check - returns depth of MDD including 'true' terminal or 0 for empty set */
size_t lddmc_test_ismdd(MDD mdd);

/* Operations for model checking */
TASK_DECL_2(MDD, lddmc_union, MDD, MDD);
#define lddmc_union(a, b) CALL(lddmc_union, a, b)

TASK_DECL_2(MDD, lddmc_minus, MDD, MDD);
#define lddmc_minus(a, b) CALL(lddmc_minus, a, b)

TASK_DECL_3(MDD, lddmc_zip, MDD, MDD, MDD*);
#define lddmc_zip(a, b, res) CALL(lddmc_zip, a, b, res)

TASK_DECL_2(MDD, lddmc_intersect, MDD, MDD);
#define lddmc_intersect(a, b) CALL(lddmc_intersect, a, b)

TASK_DECL_3(MDD, lddmc_match, MDD, MDD, MDD);
#define lddmc_match(a, b, proj) CALL(lddmc_match, a, b, proj)

MDD lddmc_union_cube(MDD a, uint32_t* values, size_t count);
int lddmc_member_cube(MDD a, uint32_t* values, size_t count);
MDD lddmc_cube(uint32_t* values, size_t count);

MDD lddmc_union_cube_copy(MDD a, uint32_t* values, int* copy, size_t count);
int lddmc_member_cube_copy(MDD a, uint32_t* values, int* copy, size_t count);
MDD lddmc_cube_copy(uint32_t* values, int* copy, size_t count);

TASK_DECL_3(MDD, lddmc_relprod, MDD, MDD, MDD);
#define lddmc_relprod(a, b, proj) CALL(lddmc_relprod, a, b, proj)

/**
 * Calculate all predecessors to a in uni according to rel[proj]
 * <proj> follows the same semantics as relprod
 * i.e. 0 (not in rel), 1 (read+write), 2 (read), 3 (write), -1 (end; rest=0)
 */
TASK_DECL_4(MDD, lddmc_relprev, MDD, MDD, MDD, MDD);
#define lddmc_relprev(a, rel, proj, uni) CALL(lddmc_relprev, a, rel, proj, uni)

// so: proj: -2 (end; quantify rest), -1 (end; keep rest), 0 (quantify), 1 (keep)
TASK_DECL_2(MDD, lddmc_project, MDD, MDD);
#define lddmc_project(mdd, proj) CALL(lddmc_project, mdd, proj)

TASK_DECL_3(MDD, lddmc_project_minus, MDD, MDD, MDD);
#define lddmc_project_minus(mdd, proj, avoid) CALL(lddmc_project_minus, mdd, proj, avoid)

TASK_DECL_4(MDD, lddmc_join, MDD, MDD, MDD, MDD);
#define lddmc_join(a, b, a_proj, b_proj) CALL(lddmc_join, a, b, a_proj, b_proj)

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

TASK_DECL_1(lddmc_satcount_double_t, lddmc_satcount_cached, MDD);
#define lddmc_satcount_cached(mdd) CALL(lddmc_satcount_cached, mdd)

TASK_DECL_1(long double, lddmc_satcount, MDD);
#define lddmc_satcount(mdd) CALL(lddmc_satcount, mdd)

/**
 * A callback for enumerating functions like sat_all_par, collect and match
 * Example:
 * TASK_3(void*, my_function, uint32_t*, values, size_t, count, void*, context) ...
 * For collect, use:
 * TASK_3(MDD, ...)
 */
LACE_TYPEDEF_CB(lddmc_sat_cb, uint32_t*, size_t, void*);

VOID_TASK_DECL_5(lddmc_sat_all_par, MDD, lddmc_sat_cb, void*, uint32_t*, size_t);
#define lddmc_sat_all_par(mdd, cb, context) CALL(lddmc_sat_all_par, mdd, cb, context, 0, 0)

VOID_TASK_DECL_3(lddmc_sat_all_nopar, MDD, lddmc_sat_cb, void*);
#define lddmc_sat_all_nopar(mdd, cb, context) CALL(lddmc_sat_all_nopar, mdd, cb, context)

TASK_DECL_5(MDD, lddmc_collect, MDD, lddmc_sat_cb, void*, uint32_t*, size_t);
#define lddmc_collect(mdd, cb, context) CALL(lddmc_collect, mdd, cb, context, 0, 0)

VOID_TASK_DECL_5(lddmc_match_sat_par, MDD, MDD, MDD, lddmc_sat_cb, void*);
#define lddmc_match_sat_par(mdd, match, proj, cb, context) CALL(lddmc_match_sat_par, mdd, match, proj, cb, context)

int lddmc_sat_one(MDD mdd, uint32_t *values, size_t count);
MDD lddmc_sat_one_mdd(MDD mdd);
#define lddmc_pick_cube lddmc_sat_one_mdd

size_t lddmc_nodecount(MDD mdd);
void lddmc_nodecount_levels(MDD mdd, size_t *variables);

/**
 * Functional composition
 * For every node at depth <depth>, call function cb (MDD -> MDD).
 * and replace the node by the result of the function
 */
LACE_TYPEDEF_CB(lddmc_compose_cb, MDD, void*);
TASK_DECL_4(MDD, lddmc_compose, MDD, lddmc_compose_cb, void*, int);
#define lddmc_compose(mdd, cb, context, depth) CALL(lddmc_compose, mdd, cb, context, depth)

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
void lddmc_serialize_reset();
void lddmc_serialize_totext(FILE *out);
void lddmc_serialize_tofile(FILE *out);
void lddmc_serialize_fromfile(FILE *in);

#endif


