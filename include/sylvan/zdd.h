/*
 * Copyright 2011-2016 Tom van Dijk
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
 * This is a multi-core implementation of Zero-suppressed Binary Decision Diagrams.
 * 
 * Unlike BDDs, the interpretation of a ZDD depends on the "domain" of variables.
 * Variables not encountered in the ZDD are *false*.
 * The representation of the universe set is NOT the leaf "true".
 * Also, no complement edges. They do not work here.
 * Thus, computing "not" is not a trivial constant operation.
 * 
 * To represent "domain" and "set of variables" we use the same cubes
 * as for BDDs, i.e., var1 \and var2 \and var3... 
 * 
 * All operations with multiple input ZDDs interpret the ZDDs in the same domain.
 * For some operations, this domain must be supplied.
 */

/* Do not include this file directly. Instead, include sylvan.h */

#ifndef SYLVAN_ZDD_H
#define SYLVAN_ZDD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * A ZDD is a 64-bit value. The low 40 bits are an index into the unique table.
 * Indices 0 and 1 are reserved for the empty family and the family containing
 * only the empty set.
 */
#define zdd_false       ((uint64_t)0)
#define zdd_invalid     ((uint64_t)0xffffffffffffffff)

/**
 * Initialize ZDD functionality.
 * This initializes internal and external referencing datastructures
 * and registers them in the garbage collection framework.
 */
void zdd_init(void);

/**
 * Create an internal ZDD node of Boolean variable <var>, with low edge <low> and high edge <high>.
 * <var> is a 24-bit integer.
 * This function does NOT check/enforce variable ordering!
 */
ZDD _zdd_make_node(uint32_t var, ZDD low, ZDD high);

static inline ZDD
zdd_make_node(uint32_t var, ZDD low, ZDD high)
{
    if (high == zdd_false) return low;
    else return _zdd_make_node(var, low, high);
}

/**
 * Returns 1 is the ZDD is a leaf, or 0 otherwise.
 */
int zdd_is_leaf(ZDD dd);

/**
 * Returns 1 if the ZDD is an interlan node, or 0 otherwise.
 */
#define zdd_is_node(dd) (zdd_is_leaf(dd) ? 0 : 1)

/**
 * Given internal node <dd>, obtain the variable.
 */
uint32_t zdd_top_var(ZDD dd);

/**
 * Given internal node <dd>, follow the low edge.
 */
ZDD zdd_node_low(ZDD dd);

/**
 * Given internal node <dd>, follow the high edge.
 */
ZDD zdd_node_high(ZDD dd);

/**
 * TODO: zdd_gettype, zdd_getvalue etc for leaves
 */

/**
 * Convert a Boolean BDD to the equivalent ZDD over <domain>. The caller must
 * protect <result>. Returns SYLVAN_OK on success or a negative status on
 * failure, leaving <result> unchanged.
 */
TASK(int, zdd_from_bdd, ZDD*, result, BDD, dd, BDDSET, domain)

/**
 * Convert a Boolean ZDD over <domain> to the equivalent BDD. The caller must
 * protect <result>. Returns SYLVAN_OK on success or a negative status on
 * failure, leaving <result> unchanged.
 */
TASK(int, bdd_from_zdd, BDD*, result, ZDD, dd, BDDSET, domain)

/**
 * Substitute the literals fixed by <cube> in <dd> and remove those variables
 * from the result domain. The cube must be one conjunction of literals and
 * its support must be contained in <domain>. The caller must protect <result>.
 * Returns SYLVAN_OK on success or a negative status on failure, leaving
 * <result> unchanged.
 */
TASK(int, zdd_cofactor, ZDD*, result, ZDD, dd, BDD, cube, BDDSET, domain)

/** Return Boolean true for the given ZDD domain. */
static inline int zdd_true(ZDD *result, BDDSET domain)
{
    return zdd_from_bdd(result, bdd_true, domain);
}

/**
 * Create a cube of literals of the given domain with the values given in <arr>.
 * Uses the given leaf as leaf.
 * For values, 0 (negative literal), 1 (positive), 2 (both values).
 * The resulting ZDD is defined on the domain <variables>. The caller must
 * protect <result>. Returns SYLVAN_OK on success or a negative status on
 * failure, leaving <result> unchanged.
 */
int zdd_cube(ZDD *result, BDDSET variables, uint8_t *values);

/**
 * Same as zdd_cube, but adds the cube to an existing set of the same domain.
 * Elements already in the set are updated with the given leaf. The caller must
 * protect <result>. Returns SYLVAN_OK on success or a negative status on
 * failure, leaving <result> unchanged.
 */
TASK(int, zdd_or_cube, ZDD*, result, ZDD, set, BDDSET, variables, uint8_t*, values)

/**
 * Compute the irredundant sum of products given lower and upper bounds as BDDs.
 * Writes a ZDD cover between the two bounds to <result>. If <bdd_result> is not
 * NULL, also writes the represented BDD there. The caller must protect both
 * destinations, which must be distinct. Returns SYLVAN_OK on success or a
 * negative status on failure, leaving both destinations unchanged.
 */
TASK(int, zdd_isop, ZDD*, result, MTBDD*, bdd_result, MTBDD, L, MTBDD, U)

/**
 * Compute the BDD representation of a given ZDD cover. The caller must protect
 * <result>. Returns SYLVAN_OK on success or a negative status on failure,
 * leaving <result> unchanged.
 */
TASK(int, bdd_from_zdd_cover, MTBDD*, result, ZDD, dd)

/**
 * Enumerate the cubes of a ZDD cover
 * <arr> must be a sufficiently large pre-allocated array, i.e., 1 + number_of_bdd_variables
 * Returns zdd_base on success or zdd_false if no more cubes in the cover.
 * The array will be filled with even (positive) and odd (negative) cover variables, ending with -1.
 */
ZDD zdd_cover_first_cube(ZDD dd, int32_t *arr);
ZDD zdd_cover_next_cube(ZDD dd, int32_t *arr);

/**
 * Extend the domain of a ZDD, such that all new variables take the given value.
 * The given value can be 0 (always negative), 1 (always positive), 2 (always
 * don't care). The caller must protect <result>. Returns SYLVAN_OK on success
 * or a negative status on failure, leaving <result> unchanged.
 */
TASK(int, zdd_extend_domain, ZDD*, result, ZDD, dd, BDDSET, newvars, int, value)

/**
 * Interpret <dd> over the larger <new_domain>, allowing every value for the
 * added variables. The caller must protect <result>. Returns SYLVAN_OK on
 * success or a negative status on failure, leaving <result> unchanged. The
 * operation fails unless <old_domain> is a subset of <new_domain> and contains
 * the support of <dd>.
 */
TASK(int, zdd_lift, ZDD*, result, ZDD, dd, BDDSET, old_domain, BDDSET, new_domain)

/**
 * Calculate the support of a ZDD, i.e. the cube of all variables that appear
 * in the ZDD nodes. The caller must protect <result>. Returns SYLVAN_OK on
 * success or a negative status on failure, leaving <result> unchanged.
 */
TASK(int, zdd_support, BDDSET*, result, ZDD, dd)

/**
 * Count the number of satisfying assignments (minterms) leading to a non-False leaf.
 * We do not need to give the domain, as skipped variables do not increase the number of minterms.
 * Fun fact: this is the same as zdd_path_count!
 */
/**
 * Count the number of distinct paths leading to a non-False leaf.
 */
TASK(double, zdd_path_count, ZDD, dd)

/**
 * Count the number of nodes (internal nodes plus leaves) in ZDDs.
 * Not thread-safe.
 */
size_t zdd_shared_node_count(const ZDD *dds, size_t count);

static inline size_t
zdd_node_count(const ZDD dd)
{
    return zdd_shared_node_count(&dd, 1);
}

/**
 * Compute IF <f> THEN <g> ELSE <h>.
 * Assuming f, g, h are all Boolean and on the same domain <domain>. The caller
 * must protect <result>. Returns SYLVAN_OK on success or a negative status on
 * failure, leaving <result> unchanged.
 */
TASK(int, zdd_ite, ZDD*, result, ZDD, f, ZDD, g, ZDD, h, BDDSET, domain)

/**
 * Compute the negation of a ZDD with respect to the given domain. The caller
 * must protect <result>. Returns SYLVAN_OK on success or a negative status on
 * failure, leaving <result> unchanged.
 */
TASK(int, zdd_not, ZDD*, result, ZDD, dd, BDDSET, domain)

/**
 * Compute logical AND of <a> and <b>. The caller must protect <result>.
 * Returns SYLVAN_OK on success or a negative status on failure, leaving
 * <result> unchanged.
 */
TASK(int, zdd_and, ZDD*, result, ZDD, a, ZDD, b)

/**
 * Compute logical OR of <a> and <b>. The caller must protect <result>.
 * Returns SYLVAN_OK on success or a negative status on failure, leaving
 * <result> unchanged.
 */
TASK(int, zdd_or, ZDD*, result, ZDD, a, ZDD, b)

/**
 * Compute logical DIFF of <a> and <b> (set minus). The caller must protect
 * <result>. Returns SYLVAN_OK on success or a negative status on failure,
 * leaving <result> unchanged.
 */
TASK(int, zdd_diff, ZDD*, result, ZDD, a, ZDD, b)

/**
 * Compute logical XOR of <a> and <b>.
 */
// TASK(ZDD, zdd_xor, ZDD, ZDD);
// #define zdd_xor(a, b) RUN(zdd_xor, a, b)

/**
 * Compute logical EQUIV of <a> and <b>.
 * Also called bi-implication. (a <-> b)
 * This operation requires the variable domain <dom>.
 */
// TASK(ZDD, zdd_equiv, ZDD, ZDD, ZDD);
// #define zdd_equiv(a, b, dom) RUN(zdd_equiv, a, b, dom)

/**
 * Compute logical IMP of <a> and <b>. (a -> b)
 * This operation requires the variable domain <dom>.
 */
// TASK(ZDD, zdd_imp, ZDD, ZDD, ZDD);
// #define zdd_imp(a, b, dom) RUN(zdd_imp, a, b, dom)

/**
 * Compute logical INVIMP of <a> and <b>. (b <- a)
 * This operation requires the variable domain <dom>.
 */
// TASK(ZDD, zdd_invimp, ZDD, ZDD, ZDD);
// #define zdd_invimp(a, b, dom) RUN(zdd_invimp, a, b, dom)

// add binary operators
// zdd_diff (no domain) == a and not b
// zdd_less (no domain) == not a and b
// zdd_nand (domain)    == not (a and b)
// zdd_nor  (domain)    == not a and not b

/**
 * Compute \exists <vars>: <dd>.
 * (Stays in same variable domain.) The caller must protect <result>. Returns
 * SYLVAN_OK on success or a negative status on failure, leaving <result>
 * unchanged.
 */
TASK(int, zdd_exists, ZDD*, result, ZDD, dd, BDDSET, vars)

/**
 * Project <dd> onto <domain>, existentially quantifying variables not in the
 * domain. (Changes to the new variable domain.) The caller must protect
 * <result>. Returns SYLVAN_OK on success or a negative status on failure,
 * leaving <result> unchanged.
 */
TASK(int, zdd_project, ZDD*, result, ZDD, dd, BDDSET, domain)

/**
 * Compute \forall <vars>: <dd>.
 */
// TASK(ZDD, zdd_forall, ZDD, ZDD);
// #define zdd_forall(dd, vars) RUN(zdd_forall, dd, vars)

/**
 * Compute \exists <vars>: <a> and <b>.
 * Result is in same domain as <a> and <b>.
 */
// TASK(ZDD, zdd_and_exists, ZDD, ZDD, ZDD);
// #define zdd_and_exists(a, b, vars) RUN(zdd_and_exists, a, b, vars)

/**
 * Compute <a> and <b> and project result on <domain>
 */
// TASK(ZDD, zdd_and_project, ZDD, ZDD, ZDD);
// #define zdd_and_project(a, b, domain) RUN(zdd_and_project, a, b, domain)

/**
 * Function composition, for each node with variable <key> which has a <key,value> pair in <map>,
 * replace the node by the result of zdd_ite(<value>, <low>, <high>).
 * Each <value> in <map> must be a Boolean ZDD.
 */
// TASK(ZDD, zdd_compose, ZDD, ZDDMAP);
// #define zdd_compose(dd, map) RUN(zdd_compose, dd, map)

/**
 * For debugging.
 * Tests if all nodes in the ZDD are correctly ``marked'' in the nodes table.
 * Tests if variables in the internal nodes appear in-order.
 * In Debug mode, this will cause assertion failures instead of returning 0.
 * Returns 1 if all is fine, or 0 otherwise.
 */
// TASK(int, zdd_test_isvalid, ZDD);
// #define zdd_test_isvalid(zdd) RUN(zdd_test_isvalid, zdd)

/**
 * Write a DOT representation of a ZDD
 * The callback function is required for custom terminals.
 */
void zdd_fprint_dot(FILE *out, ZDD zdd);
#define zdd_print_dot(zdd) zdd_fprint_dot(stdout, zdd)

/**
 * ZDDMAP, maps uint32_t variables to ZDDs.
 * A ZDDMAP node has variable level, low edge going to the next ZDDMAP, high edge to the mapped ZDD
 */
#define zdd_map_empty() zdd_false
#define zdd_map_is_empty(map) (map == zdd_false ? 1 : 0)
#define zdd_map_key(map) zdd_top_var(map)
#define zdd_map_value(map) zdd_node_high(map)
#define zdd_map_next(map) zdd_node_low(map)

/**
 * Return 1 if the map contains the key, 0 otherwise.
 */
int zdd_map_contains(ZDDMAP map, uint32_t key);

/**
 * Retrieve the number of keys in the map.
 */
size_t zdd_map_count(ZDDMAP map);

/**
 * Add the pair <key,value> to the map, overwrites if key already in map.
 */
ZDDMAP zdd_map_set(ZDDMAP map, uint32_t key, ZDD value);

/**
 * Add all values from map2 to map1, overwrites if key already in map1.
 */
ZDDMAP zdd_map_update(ZDDMAP map1, ZDDMAP map2);

/**
 * Remove the key <key> from the map and return the result
 */
ZDDMAP zdd_map_remove(ZDDMAP map, uint32_t key);

/**
 * Remove all keys in the cube <variables> from the map and return the result
 */
ZDDMAP zdd_map_remove_all(ZDDMAP map, ZDD variables);

/**
 * Enumerate all minterms (non-False assignments)
 *
 * Given a ZDD <dd> and a variable domain <dom>, zdd_first_minterm and zdd_next_minterm enumerate
 * all assignments to the variables in <dom> that lead to a non-False leaf.
 * 
 * The function returns the leaf (or zdd_false if no new path is found) and encodes the path
 * in the supplied array <arr>: 0 for a low edge, 1 for a high edge.
 *
 * Usage:
 * ZDD leaf = zdd_first_minterm(dd, variables, arr, NULL);
 * while (leaf != zdd_false) {
 *     .... // do something with arr/leaf
 *     leaf = zdd_next_minterm(dd, variables, arr, NULL);
 * }
 *
 * The callback is an optional function that returns 0 when the given leaf should be skipped, or 1 otherwise.
 */
typedef int (*zdd_enum_filter_cb)(ZDD);
ZDD zdd_first_minterm(ZDD dd, BDDSET variables, uint8_t *arr, zdd_enum_filter_cb filter_cb);
ZDD zdd_next_minterm(ZDD dd, BDDSET variables, uint8_t *arr, zdd_enum_filter_cb filter_cb);

/**
 * Enumerate minterms of the ZDD <dd>, interpreted along the domain <dom>.
 * Obtain the first minterm in arr, setting arr values to 0/1 in the order of the variable domain.
 */
// ZDD zdd_first_minterm(ZDD dd, ZDD dom, uint8_t *arr);

/**
 * Enumerate minterms of the ZDD <dd>, interpreted along the domain <dom>.
 * Obtain the next minterm in arr, setting arr values to 0/1 in the order of the variable domain.
 */
// ZDD zdd_next_minterm(ZDD dd, ZDD dom, uint8_t *arr);

/*
typedef struct zdd_trace {
        struct zdd_trace *prev;
            uint32_t var;
                uint8_t val;
} zdd_trace;

LACE_TYPEDEF_CB(void, zdd_enum_cb, void*, uint8_t*, size_t);
TASK(void, zdd_enum, ZDD, ZDD, zdd_enum_cb, void*)
#define zdd_enum(dd, dom, cb, context) RUN(zdd_enum, dd, dom, cb, context)

TASK(void, zdd_enum_seq, ZDD, ZDD, zdd_enum_cb, void*)
#define zdd_enum_seq(dd, dom, cb, context) RUN(zdd_enum_seq, dd, dom, cb, context)

LACE_TYPEDEF_CB(ZDD, zdd_collect_cb, void*, uint8_t*, size_t);
TASK(ZDD, zdd_collect, ZDD, ZDD, ZDD, zdd_collect_cb, void*)
#define zdd_collect(dd, dom, res_dom, cb, context) RUN(zdd_collect, dd, dom, res_dom, cb, context)
*/

// sat_one / pick_cube, visitor
// relnext, relprev
// compose
// serialization

/**
 * Visitor functionality for ZDDs.
 * Visits internal nodes, not leaves (TODO FIXME)
 */

/**
 * pre_cb callback: given input ZDD and context, return whether to visit children
 * post_cb callback: given input ZDD and context
 */
typedef int (*zdd_visit_pre_cb)(ZDD, void*);
typedef void (*zdd_visit_post_cb)(ZDD, void*);

/**
 * Sequential visit operation
 */
TASK(void, zdd_visit, ZDD, dd, zdd_visit_pre_cb, precb, zdd_visit_post_cb, postcb, void*, context)

/**
 * Parallel visit operation
 */
TASK(void, zdd_visit_parallel, ZDD, dd, zdd_visit_pre_cb, precb, zdd_visit_post_cb, postcb, void*, context)

/**
 * Writing ZDDs to file.
 *
 * Every node that is to be written is assigned a number, starting from 1,
 * such that reading the result in the future can be done in one pass.
 *
 * We use a skiplist to store the assignment.
 *
 * One could use the following two methods to store an array of ZDDs.
 * - call zdd_write_binary to store ZDDs in binary format.
 * - call zdd_write_text to store ZDDs in text format.
 *
 * One could also do the procedure manually instead.
 * - call zdd_writer_start to allocate the skiplist.
 * - call zdd_writer_add to add a given ZDD to the skiplist
 * - call zdd_writer_writebinary to write all added nodes to a file
 * - OR:  zdd_writer_writetext to write all added nodes in text format
 * - call zdd_writer_get to obtain the ZDD identifier as stored in the skiplist
 * - call zdd_writer_end to free the skiplist
 */

/**
 * Write <count> decision diagrams given in <dds> in internal binary form to <file>.
 *
 * The internal binary format is as follows, to store <count> decision diagrams...
 * uint64_t: nodecount -- number of nodes
 * <nodecount> times uint128_t: each leaf/node
 * uint64_t: count -- number of stored decision diagrams
 * <count> times uint64_t: each stored decision diagram
 */
TASK(void, zdd_write_binary, FILE *, file, ZDD *, dds, int, count)

/**
 * Write <count> decision diagrams given in <dds> in ASCII form to <file>.
 *
 * The text format writes in the same order as the binary format, except...
 * [
 *   node(id, var, low, high), -- for a normal node (no complement on high)
 *   node(id, var, low, ~high), -- for a normal node (complement on high)
 * ],[dd1, dd2, dd3, ...,] -- and each the stored decision diagram.
 */

TASK(void, zdd_write_text, FILE *, file, ZDD *, dds, int, count)

/**
 * Skeleton typedef for the skiplist
 */
typedef struct sylvan_skiplist sylvan_skiplist;

/**
 * Allocate a skiplist for writing an BDD.
 */
sylvan_skiplist* zdd_writer_start(void);

/**
 * Add the given ZDD to the skiplist.
 */
TASK(void, zdd_writer_add, sylvan_skiplist*, sl, ZDD, dd)

/**
 * Write all assigned ZDD nodes in binary format to the file.
 */
void zdd_writer_writebinary(FILE *out, sylvan_skiplist* sl);

/**
 * Retrieve the identifier of the given stored ZDD.
 * This is useful if you want to be able to retrieve the stored ZDD later.
 */
uint64_t zdd_writer_get(sylvan_skiplist* sl, ZDD dd);

/**
 * Free the allocated skiplist.
 */
void zdd_writer_end(sylvan_skiplist* sl);

/**
 * Reading ZDDs from file (binary format).
 *
 * The function zdd_read_binary is basically the reverse of zdd_write_binary.
 *
 * One can also perform the procedure manually.
 * - call zdd_reader_readbinary to read the nodes from file
 * - call zdd_reader_get to obtain the ZDD for the given identifier as stored in the file.
 * - call zdd_reader_end to free the array returned by zdd_reader_readbinary
 *
 * Returns 0 if successful, -1 otherwise.
 */

/*
 * Read <count> decision diagrams to <dds> from <file> in internal binary form.
 */
TASK(int, zdd_read_binary, FILE*, file, ZDD*, dds, int, count)

/**
 * Reading a file earlier written with zdd_writer_writebinary
 * Returns an array with the conversion from stored identifier to ZDD
 * This array is allocated with malloc and must be freed afterwards.
 * Returns NULL if there was an error.
 */

TASK(uint64_t*, zdd_reader_readbinary, FILE*, file)

/**
 * Retrieve the ZDD of the given stored identifier.
 */
ZDD zdd_reader_get(uint64_t* arr, uint64_t identifier);

/**
 * Free the allocated translation array
 */
void zdd_reader_end(uint64_t *arr);

/**
 * Garbage collection
 */

/**
 * Call zdd_gc_mark for every zdd you want to keep in your custom mark functions.
 */
TASK(void, zdd_gc_mark, ZDD, dd)

/**
 * Default external pointer referencing. During garbage collection, the pointers are followed and the ZDD
 * that they refer to are kept in the forest.
 */
void zdd_protect(ZDD* ptr);
void zdd_unprotect(ZDD* ptr);
size_t zdd_protected_count(void);

/**
 * If mtbdd_set_ondead is set to a callback, then this function marks ZDDs (terminals).
 * When they are dead after the mark phase in garbage collection, the callback is called for marked ZDDs.
 * The ondead callback can either perform cleanup or resurrect dead terminals.
 */
#define zdd_notify_ondead(dd) llmsset_notify_ondead(nodes, dd)

/**
 * Infrastructure for internal references.
 * Every thread has its own pointer and value reference stacks.
 * The pointers stack stores pointers to ZDD variables, manipulated with pushptr and popptr.
 * The values stack stores ZDDs, manipulated with push and pop.
 *
 * New code should use the pointer stack for local protected destinations.
 */

/**
 * Push a ZDD variable to the pointer reference stack.
 * During garbage collection the variable will be inspected and the contents will be marked.
 */
void zdd_refs_pushptr(ZDD *ptr);

/**
 * Pop the last <amount> ZDD variables from the pointer reference stack.
 */
void zdd_refs_popptr(size_t amount);

/**
 * Push an ZDD to the values reference stack.
 * During garbage collection the references ZDD will be marked.
 */
ZDD zdd_refs_push(ZDD zdd);

/**
 * Pop the last <amount> ZDDs from the values reference stack.
 */
void zdd_refs_pop(long amount);

#ifdef __cplusplus
}
#endif /* __cplusplus */

/* 
 * A lot of functionality is still missing from this initial implementation...
 *
 * Visitor (enumerate using callback)
 * Reader/Writer
 * Enum SAT Par/Seq
 * Enum PATH Par/Seq?
 * Eval_compose (for par learning)
 * printDot
 * compose functions
 *
 * Functions supported by CuDD
 * - zddOne (compute representation of universe given domain)
 * - const functions, specifically diffConst
 * - product and quotient of "unate covers" (zddUnateProduct, zddDivide)
 * - product and quotien of "binate covers" (zddProduct, zddWeakDiv)
 * - complement of a cover (zddComplement)
 * - reorder functions
 * - "realignment of variables"
 * - print a SOP representation of a ZDD
 * Functions that can be done with zdd_compose
 * - zddChange (subst variable by its complement)
 * - cofactor
 * Functions supported by EXTRA
 * - quite a lot.
 * Generic functions for other leaf types
 * - apply_op
 * - uapply_op
 * - applyp_op
 * - abstract_op
 * Specific functions for other leaf types
 * - negate, plus, minus, times, min, max
 * - abstract_plus, abstract_times, abstract_min, abstract_max
 * - non-boolean ite
 * - threshold, strict_threshold
 * - equal_norm_d, equal_norm_rel_d
 * - leq, less, geq, greater
 * - minimum, maximum (gets lowest/highest leaf)
 */

#endif
