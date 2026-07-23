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

#ifndef SYLVAN_LISTDD_H
#define SYLVAN_LISTDD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

static const LISTDD listdd_empty = 0;
static const LISTDD listdd_empty_list = 1;
static const LISTDD listdd_invalid = UINT64_MAX;

typedef struct listdd_projection listdd_projection;
typedef struct listdd_relation_layout listdd_relation_layout;

/** How a tuple position is treated by a projection. */
typedef enum {
    LISTDD_PROJECT_POSITION = 0,
    LISTDD_KEEP_POSITION = 1
} listdd_projection_kind;

/** How a state-vector position occurs in a relation. */
typedef enum {
    LISTDD_RELATION_UNUSED = 0,
    LISTDD_RELATION_READ = 1,
    LISTDD_RELATION_WRITE = 2,
    LISTDD_RELATION_READ_WRITE = 3
} listdd_relation_access;

/* Initialize ListDD functionality. */
void listdd_init(void);

/**
 * Create immutable ListDD metadata.
 *
 * The returned object owns and protects its canonical ListDD representation
 * until it is destroyed. The input arrays are copied.
 */
int listdd_projection_create(
    listdd_projection **result,
    const listdd_projection_kind *positions,
    size_t count);
void listdd_projection_destroy(listdd_projection *projection);

int listdd_relation_layout_create(
    listdd_relation_layout **result,
    const listdd_relation_access *positions,
    size_t count,
    int has_action_label);
void listdd_relation_layout_destroy(listdd_relation_layout *layout);

/* Primitives */
LISTDD listdd_make_node(uint32_t value, LISTDD ifeq, LISTDD ifneq);
LISTDD listdd_extend_node(LISTDD mdd, uint32_t value, LISTDD ifeq);
uint32_t listdd_node_value(LISTDD mdd);
LISTDD listdd_node_down(LISTDD mdd);
LISTDD listdd_node_right(LISTDD mdd);
LISTDD listdd_follow(LISTDD mdd, uint32_t value);

/**
 * Copy nodes in relations.
 * A copy node represents 'read x, then write x' for every x.
 * In a read-write relation, use copy nodes twice, once on read level, once on write level.
 * Copy nodes are only supported by relprod, relprev and union.
 */

/* Primitive for special 'copy node' (for relprod/relprev) */
LISTDD listdd_make_copy_node(LISTDD ifeq, LISTDD ifneq);
int listdd_is_copy_node(LISTDD mdd);
LISTDD listdd_follow_copy(LISTDD mdd);

/**
 * Infrastructure for external references using a hash table.
 * Two hash tables store external references: a pointers table and a values table.
 * The pointers table stores pointers to LISTDD variables, manipulated with protect and unprotect.
 * The values table stores LISTDD, manipulated with ref and deref.
 * We strongly recommend using the pointers table whenever possible.
 */

/**
 * Store the pointer <ptr> in the pointers table.
 */
void listdd_protect(LISTDD* ptr);

/**
 * Delete the pointer <ptr> from the pointers table.
 */
void listdd_unprotect(LISTDD* ptr);

/**
 * Compute the number of pointers in the pointers table.
 */
size_t listdd_protected_count(void);

/**
 * Store the LISTDD <dd> in the values table.
 */
LISTDD listdd_ref(LISTDD dd);

/**
 * Delete the LISTDD <dd> from the values table.
 */
void listdd_deref(LISTDD dd);

/**
 * Compute the number of values in the values table.
 */
size_t listdd_ref_count(void);

/**
 * Call mtbdd_gc_mark for every mtbdd you want to keep in your custom mark functions.
 */

static inline void listdd_gc_mark(LISTDD dd);

/* Sanity check - returns depth of LISTDD including 'true' terminal or 0 for empty set */
#ifndef NDEBUG
size_t listdd_is_valid(LISTDD mdd);
#endif

/**
 * ListDD set operations write to caller-protected destinations and return
 * SYLVAN_OK on success or a negative status code on failure. Destinations are
 * left unchanged on failure.
 */

/* Compute the union of <a> and <b>. */
static inline int listdd_union(LISTDD *result, LISTDD a, LISTDD b);

/* Compute the elements in <a> that are not in <b>. */
static inline int listdd_diff(LISTDD *result, LISTDD a, LISTDD b);

/* Compute <a> union <b> and, simultaneously, the elements in <b> not in <a>.
 * The two destinations must be distinct. */
static inline int listdd_union_diff(LISTDD *result, LISTDD *difference, LISTDD a, LISTDD b);

/* Compute the intersection of <a> and <b>. */
static inline int listdd_intersection(LISTDD *result, LISTDD a, LISTDD b);

/* Keep vectors from <a> that match a vector in <b> at selected positions. */
static inline int listdd_match(
    LISTDD *result,
    LISTDD a,
    LISTDD b,
    const listdd_projection *projection);

/* Add one <count>-element state vector to <a>. */
int listdd_add(LISTDD *result, LISTDD a, const uint32_t *values, size_t count);
int listdd_contains(LISTDD a, const uint32_t *values, size_t count);
/* Construct the singleton set containing one <count>-element state vector. */
int listdd_singleton(LISTDD *result, const uint32_t *values, size_t count);

/**
 * Add or inspect a relation tuple described by <layout>.
 *
 * The read and write arrays are indexed by state-vector position. <count>
 * must equal the number of positions in <layout>. If <retain_source>[i] is
 * nonzero, position i must be read/write and its two relation levels are COPY
 * nodes. <action_label> must be non-NULL exactly when the layout has an action
 * label.
 */
int listdd_relation_add(
    LISTDD *result,
    LISTDD relation,
    const listdd_relation_layout *layout,
    const uint32_t *read_values,
    const uint32_t *write_values,
    const uint8_t *retain_source,
    size_t count,
    const uint32_t *action_label);
int listdd_relation_contains(
    int *result,
    LISTDD relation,
    const listdd_relation_layout *layout,
    const uint32_t *read_values,
    const uint32_t *write_values,
    const uint8_t *retain_source,
    size_t count,
    const uint32_t *action_label);
int listdd_relation_singleton(
    LISTDD *result,
    const listdd_relation_layout *layout,
    const uint32_t *read_values,
    const uint32_t *write_values,
    const uint8_t *retain_source,
    size_t count,
    const uint32_t *action_label);

/** Compute the successors of <set> under <relation>. */
TASK(int, listdd_rel_next, LISTDD*, result, LISTDD, set, LISTDD, relation, const listdd_relation_layout*, layout)

/** Compute the successors of <set> and unite them with <un>. */
TASK(int, listdd_rel_next_union, LISTDD*, result, LISTDD, set, LISTDD, relation, const listdd_relation_layout*, layout, LISTDD, un)

/** Compute predecessors in <universe> under <relation>. */
TASK(int, listdd_rel_prev, LISTDD*, result, LISTDD, dd, LISTDD, relation, const listdd_relation_layout*, layout, LISTDD, universe)

TASK(int, listdd_project, LISTDD*, result, LISTDD, dd, const listdd_projection*, projection)

TASK(int, listdd_project_diff, LISTDD*, result, LISTDD, dd, const listdd_projection*, projection, LISTDD, avoid)

TASK(int, listdd_join, LISTDD*, result, LISTDD, a, LISTDD, b, const listdd_projection*, a_projection, const listdd_projection*, b_projection)

/* Write a DOT representation */
void listdd_print_dot(LISTDD mdd);
void listdd_fprint_dot(FILE *out, LISTDD mdd);

void listdd_fprint(FILE *out, LISTDD mdd);
void listdd_print(LISTDD mdd);

void listdd_print_sha256(LISTDD mdd);
void listdd_fprint_sha256(FILE *out, LISTDD mdd);
void listdd_sha256(LISTDD mdd, char *target); // at least 65 bytes...

/** Return the exact tuple count, or SYLVAN_ERR_OVERFLOW. */
TASK(int, listdd_count_u64, uint64_t*, result, LISTDD, dd)

/** Return an approximate tuple count. Invalid input produces NaN. */
TASK(double, listdd_count_double, LISTDD, dd)

/** Legacy extended-precision tuple count. */
TASK(long double, listdd_count, LISTDD, dd)

/**
 * Create a pull iterator over complete concrete tuples in lexicographic order.
 * ListDDs containing relation COPY nodes are not iterable as concrete tuples.
 */
int listdd_iterator_create(sylvan_iterator **result, LISTDD dd);

/**
 * Advance a ListDD tuple iterator.
 *
 * <count> must equal the tuple width determined at creation. On success,
 * <has_item> is set to one and <values> receives the tuple, or <has_item> is
 * set to zero after exhaustion.
 */
int listdd_iterator_next(
    sylvan_iterator *iterator,
    uint32_t *values,
    size_t count,
    int *has_item);

/**
 * A callback for enumerating functions like sat_all_par, collect and match
 * Example:
 * TASK(void*, my_function, uint32_t*, values, size_t, count, void*, context) ...
 * Map/reduce callbacks write their result to a caller-protected destination
 * and return SYLVAN_OK or a negative status code. A positive status or success
 * without a result is reported as SYLVAN_ERR_CALLBACK.
 */
typedef void (*listdd_enum_cb)(uint32_t*, size_t, void*);
typedef int (*listdd_map_reduce_union_cb)(LISTDD*, uint32_t*, size_t, void*);

TASK(void, listdd_enumerate_parallel, LISTDD, dd, listdd_enum_cb, cb, void*, context, uint32_t*, arr, size_t, len)

TASK(void, listdd_enumerate, LISTDD, dd, listdd_enum_cb, cb, void*, context)

TASK(int, listdd_map_reduce_union, LISTDD*, result, LISTDD, dd, listdd_map_reduce_union_cb, cb, void*, context, uint32_t*, arr, size_t, len)

TASK(void, listdd_enumerate_matching_parallel, LISTDD, dd, LISTDD, match, LISTDD, proj, listdd_enum_cb, cb, void*, context)

int listdd_pick_values(LISTDD mdd, uint32_t *values, size_t count);
int listdd_pick(LISTDD *result, LISTDD mdd);

/**
 * Callback functions for visiting nodes.
 * listdd_visit sequentially visits nodes, down first, then right.
 * listdd_visit_parallel visits nodes in parallel (down || right).
 */
typedef int (*listdd_visit_pre_cb)(LISTDD, void*); // int pre(LISTDD, context)
typedef void (*listdd_visit_post_cb)(LISTDD, void*); // void post(LISTDD, context)
typedef void (*listdd_visit_init_context_cb)(void*, void*, int); // void init_context(context, parent, is_down)

typedef struct listdd_visit_node_callbacks {
    listdd_visit_pre_cb listdd_visit_pre;
    listdd_visit_post_cb listdd_visit_post;
    listdd_visit_init_context_cb listdd_visit_init_context;
} listdd_visit_callbacks;

TASK(void, listdd_visit_parallel, LISTDD, dd, listdd_visit_callbacks*, cbs, size_t, ctx_size, void*, context)

TASK(void, listdd_visit, LISTDD, dd, listdd_visit_callbacks*, cbs, size_t, ctx_size, void*, context)

size_t listdd_node_count(LISTDD mdd);
void listdd_node_count_per_level(LISTDD mdd, size_t *variables);

/**
 * Functional composition
 * For every node at depth <depth>, call <cb> and replace the node by the
 * callback result. The callback follows the protected-destination status
 * convention used by ListDD operations. A positive status or success without
 * a result is reported as SYLVAN_ERR_CALLBACK.
 */
typedef int (*listdd_transform_at_level_cb)(LISTDD*, LISTDD, void*);
TASK(int, listdd_transform_at_level, LISTDD*, result, LISTDD, dd, listdd_transform_at_level_cb, cb, void*, context, int, depth)

/**
 * SAVING:
 * use listdd_serialize_add on every LISTDD you want to store
 * use listdd_serialize_get to retrieve the key of every stored LISTDD
 * use listdd_serialize_tofile
 *
 * LOADING:
 * use listdd_serialize_fromfile (implies listdd_serialize_reset)
 * use listdd_serialize_get_reversed for every key
 *
 * MISC:
 * use listdd_serialize_reset to free all allocated structures
 * use listdd_serialize_totext to write a textual list of tuples of all MDDs.
 *         format: [(<key>,<level>,<key_low>,<key_high>,<complement_high>),...]
 *
 * for the old listdd_print functions, use listdd_serialize_totext
 */
size_t listdd_serialize_add(LISTDD mdd);
size_t listdd_serialize_get(LISTDD mdd);
LISTDD listdd_serialize_get_reversed(size_t value);
void listdd_serialize_reset(void);
void listdd_serialize_totext(FILE *out);
void listdd_serialize_tofile(FILE *out);
void listdd_serialize_fromfile(FILE *in);
void listdd_serialize_fromfile_old(FILE *in);

/**
 * Infrastructure for internal references.
 * Every thread has its own pointer and value reference stacks.
 * The pointers stack stores pointers to LDD variables, manipulated with pushptr and popptr.
 * The values stack stores LDD, manipulated with push and pop.
 *
 * It is recommended to use the pointers stack for local variables and task destinations.
 */

/**
 * Push a LDD variable to the pointer reference stack.
 * During garbage collection the variable will be inspected and the contents will be marked.
 */
void listdd_refs_pushptr(const LISTDD *ptr);

/**
 * Pop the last <amount> LDD variables from the pointer reference stack.
 */
void listdd_refs_popptr(size_t amount);

/**
 * Push an LDD to the values reference stack.
 * During garbage collection the references LDD will be marked.
 */
LISTDD listdd_refs_push(LISTDD dd);

/**
 * Pop the last <amount> LDD from the values reference stack.
 */
void listdd_refs_pop(long amount);

TASK(void, listdd_gc_mark, LISTDD, dd)
TASK(int, listdd_union, LISTDD*, result, LISTDD, a, LISTDD, b)
TASK(int, listdd_diff, LISTDD*, result, LISTDD, a, LISTDD, b)
TASK(int, listdd_union_diff, LISTDD*, result, LISTDD*, difference, LISTDD, a, LISTDD, b)
TASK(int, listdd_intersection, LISTDD*, result, LISTDD, a, LISTDD, b)
TASK(int, listdd_match, LISTDD*, result, LISTDD, a, LISTDD, b, const listdd_projection*, projection)



#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
