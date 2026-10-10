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

/**
 * Sylvan: parallel MTBDD/ListDD package.
 * Include this file for the advanced extension API.
 */

#ifndef SYLVAN_INTERNAL_H
#define SYLVAN_INTERNAL_H

#include <sylvan/sylvan.h>

#ifdef __cplusplus
namespace sylvan {
#endif

/**
 * Sylvan internal header files inside the namespace
 */

#include <sylvan/cache.h>
#include <sylvan/nodes.h>
#include <sylvan/hash.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Nodes table.
 */
extern nodes_table* nodes;

/**
 * Is <index> a bucket that the nodes table actually has?
 *
 * This is the one predicate behind the root validation the marked traversals
 * perform; see "Root validation at traversal entry points" in mtbdd.h for the
 * contract, including which entry points call it and which deliberately do
 * not. Callers pass the bucket index, not the handle: MTBDD_GETINDEX for an
 * MTBDD, ZDD_GETINDEX for a ZDD, and the handle itself for a LISTDD. Indices
 * 0 and 1 are the terminal buckets and are accepted, because every family
 * encodes at least one terminal as an index in that range and the traversals
 * mark them like any other node.
 *
 * Note that this is a bound check and nothing more. It rejects a handle that
 * cannot be dereferenced without leaving the table, which is what turns the
 * counters and printers from a wild read into a defined answer. It does not
 * establish that the index holds a live node of the expected family, and it
 * is not a substitute for the caller keeping its roots referenced.
 */
static inline int
sylvan_index_is_valid(uint64_t index)
{
    return index < nodes_get_size(nodes);
}

/**
 * Optional decision diagram handle checking.
 *
 * Compile with -DSYLVAN_CHECK_HANDLES=1, or configure with
 * -DSYLVAN_CHECK_HANDLES=ON, to make every node dereference verify that the
 * handle's bucket index is inside the nodes table, and abort with the
 * offending index when it is not. This is the instrument for finding where an
 * invalid handle entered a computation, rather than where it was finally
 * dereferenced; the entry points listed in mtbdd.h under "Root validation at
 * traversal entry points" already reject one without it.
 *
 * Like SYLVAN_GC_DIAGNOSTICS in common.c, this is a diagnostic aid only. It is
 * disabled by default and costs nothing when disabled: the macro expands to
 * ((void)0) and does not even evaluate its argument, so the generated code is
 * unchanged. It is deliberately not spelled assert(), so that enabling it in a
 * Release build works and it does not switch itself off with NDEBUG.
 */
#ifndef SYLVAN_CHECK_HANDLES
#define SYLVAN_CHECK_HANDLES 0
#endif

#if SYLVAN_CHECK_HANDLES
/** Report an out-of-bounds handle index and abort. Never returns. */
void sylvan_handle_check_failed(uint64_t index, const char *file, int line);
#define SYLVAN_CHECK_HANDLE_INDEX(index)                                       \
    ((void)(sylvan_index_is_valid(index)                                       \
                ? (void)0                                                      \
                : sylvan_handle_check_failed((index), __FILE__, __LINE__)))
#else
#define SYLVAN_CHECK_HANDLE_INDEX(index) ((void)0)
#endif

/**
 * ROOTS AND MARKING.
 *
 * The public interface is sylvan_gc_add_roots() in <sylvan/common.h>: a
 * consumer yields its roots as values and Sylvan decides what to do with them.
 * Two things are done with them, and both go through the one walk below --
 * garbage collection marks what they reach, and a reorder session pins them so
 * that no swap retires them. There is deliberately no second path: a root that
 * a collection would keep is a root a session pins, because they read the same
 * registrations in the same order.
 *
 * Sylvan's own MTBDD root sources -- the mtbdd_protect() table and the
 * per-worker temporary reference stacks -- are registered here like any other
 * visitor, so that a session enumerates them too.
 */
void sylvan_gc_visit_roots(sylvan_roots_yield yield, void *arg);

/**
 * Add a marking callback, for a Sylvan subsystem only.
 *
 * A marking callback marks and cannot be enumerated, so it is not public: a
 * consumer that owned roots this way could never be reordered around. The ZDD
 * and ListDD subsystems use it -- neither can be reordered at all, so there is
 * nothing for a session to enumerate -- and so does the MTBDD subsystem, whose
 * one callback is the one that marks through the root visitors above.
 */
void sylvan_gc_add_mark_internal(gc_hook_cb mark_cb);

/* Fallible node construction for protected-destination operations. */
int _mtbdd_try_make_node(MTBDD *destination, uint32_t var, MTBDD low, MTBDD high);
int _zdd_try_make_node(ZDD *destination, uint32_t var, ZDD low, ZDD high);
int _listdd_try_make_node(LISTDD *destination, uint32_t value, LISTDD down, LISTDD right);
int _listdd_try_make_copy_node(LISTDD *destination, LISTDD down, LISTDD right);

/**
 * Advanced structural unary apply.
 *
 * The callback is consulted for every subdiagram and may return
 * SYLVAN_APPLY_RECURSE to descend. Ordinary leaf transformations should use
 * mtbdd_map instead.
 */
typedef int (*mtbdd_apply_unary_cb)(
    lace_worker *lace, MTBDD *result, MTBDD dd, size_t parameter);
TASK(int, mtbdd_apply_unary, MTBDD*, result, MTBDD, dd,
     mtbdd_apply_unary_cb, op, size_t, parameter)

/* Shared implementation for BDD and MTBDD evaluation. */
int _mtbdd_eval(MTBDD *destination, MTBDD dd, BDDSET variables, const uint8_t *values, size_t count);

/** Assert that <set> is a referenced conjunction of positive variables. */
void bdd_set_assert_valid(BDDSET set);

/**
 * Return 1 if bdd_rel_next and bdd_rel_prev called with <vars> (and, for
 * bdd_false, bdd_transitive_closure) take their fused kernels under the
 * current order: every pair (2k, 2k+1) in play occupies adjacent ranks with
 * 2k first. The pairs in play are those of the variables in <vars>, or every
 * registered pair when <vars> is bdd_false. Exposed for the tests. See "THE
 * RELATIONAL PRODUCTS AND THE VARIABLE ORDER" in bdd.c for why.
 */
int bdd_rel_pairs_adjacent(BDDSET vars);

/** The ZDD terminal representing the family containing only the empty set. */
static const ZDD zdd_base = UINT64_C(1);

/** Advanced multi-terminal ZDD conversion primitives. */
TASK(int, zdd_from_mtbdd, ZDD*, result, MTBDD, dd, BDDSET, domain)
TASK(int, zdd_to_mtbdd, MTBDD*, result, ZDD, dd, BDDSET, domain)

/**
 * Experimental fused binary MTBDD combine/reduce engine.
 *
 * The combine callback follows the mtbdd_apply callback contract, with an
 * additional borrowed context: it writes a handled result and returns
 * SYLVAN_OK, returns SYLVAN_APPLY_RECURSE to request structural recursion, or
 * returns a negative status. It may swap its local operand handles to
 * canonicalize a commutative operation. A handled result must not introduce
 * decision variables absent from the operands.
 *
 * Reduction has the same identity, associativity, skipped-variable, context,
 * thread-safety, and cache-identity contract as mtbdd_map_reduce.
 */
typedef int (*mtbdd_combine_reduce_combine_cb)(
    lace_worker *lace, MTBDD *result, MTBDD *a, MTBDD *b, void *context);

typedef struct mtbdd_combine_reduce_op {
    mtbdd_combine_reduce_combine_cb combine;
    mtbdd_map_reduce_reduce_cb reduce;
    MTBDD identity;
    void *context;
    uint64_t cache_id;
} mtbdd_combine_reduce_op;

/**
 * Pointwise combine <a> and <b>, then reduce <variables>, in one recursive
 * traversal without constructing the complete intermediate MTBDD.
 *
 * The caller must protect <result>, <a>, <b>, <variables>, and
 * <operation->identity>. Returns SYLVAN_OK on success or a negative status on
 * failure, leaving <result> unchanged.
 */
TASK(int, mtbdd_combine_reduce, MTBDD*, result, MTBDD, a, MTBDD, b,
     BDDSET, variables, const mtbdd_combine_reduce_op*, operation)

/**
 * Macros for all operation identifiers for the operation cache
 */

// BDD operations
static const uint64_t CACHE_BDD_ITE                 = (0LL<<40);
static const uint64_t CACHE_BDD_AND                 = (1LL<<40);
static const uint64_t CACHE_BDD_XOR                 = (2LL<<40);
static const uint64_t CACHE_BDD_EXISTS              = (3LL<<40);
static const uint64_t CACHE_BDD_PROJECT             = (4LL<<40);
static const uint64_t CACHE_BDD_AND_EXISTS          = (5LL<<40);
static const uint64_t CACHE_BDD_AND_PROJECT         = (6LL<<40);
static const uint64_t CACHE_BDD_RELNEXT             = (7LL<<40);
static const uint64_t CACHE_BDD_RELPREV             = (8LL<<40);
static const uint64_t CACHE_BDD_SAT_COUNT_DOUBLE    = (9LL<<40);
static const uint64_t CACHE_BDD_COMPOSE             = (10LL<<40);
static const uint64_t CACHE_BDD_SIMPLIFY            = (11LL<<40);
static const uint64_t CACHE_BDD_CONSTRAIN           = (12LL<<40);
static const uint64_t CACHE_BDD_CLOSURE             = (13LL<<40);
static const uint64_t CACHE_BDD_ISBDD               = (14LL<<40);
static const uint64_t CACHE_BDD_SUPPORT             = (15LL<<40);
static const uint64_t CACHE_BDD_PATHCOUNT           = (16LL<<40);
static const uint64_t CACHE_BDD_DISJOINT            = (17LL<<40);
static const uint64_t CACHE_BDD_UNIQUE              = (18LL<<40);
static const uint64_t CACHE_BDD_INTERSECTION_WITNESS = (19LL<<40);
static const uint64_t CACHE_BDD_SAT_COUNT_U64       = (31LL<<40);
static const uint64_t CACHE_BDD_APPLY_ABSTRACT      = (32LL<<40);
static const uint64_t CACHE_BDD_PICK_REPRESENTATIVES = (33LL<<40);
static const uint64_t CACHE_BDD_PROBABILITY         = (34LL<<40);
static const uint64_t CACHE_BDD_CARDINALITY         = (35LL<<40);
static const uint64_t CACHE_BDD_REL_COMPOSE_ANY     = (36LL<<40);

// LISTDD operations
static const uint64_t CACHE_MDD_RELPROD             = (20LL<<40);
static const uint64_t CACHE_MDD_MINUS               = (21LL<<40);
static const uint64_t CACHE_MDD_UNION               = (22LL<<40);
static const uint64_t CACHE_MDD_INTERSECT           = (23LL<<40);
static const uint64_t CACHE_MDD_PROJECT             = (24LL<<40);
static const uint64_t CACHE_MDD_JOIN                = (25LL<<40);
static const uint64_t CACHE_MDD_MATCH               = (26LL<<40);
static const uint64_t CACHE_MDD_RELPREV             = (27LL<<40);
static const uint64_t CACHE_MDD_SATCOUNT            = (28LL<<40);
static const uint64_t CACHE_MDD_SATCOUNTL1          = (29LL<<40);
static const uint64_t CACHE_MDD_SATCOUNTL2          = (30LL<<40);

// MTBDD operations
static const uint64_t CACHE_MTBDD_APPLY             = (40LL<<40);
static const uint64_t CACHE_MTBDD_UAPPLY            = (41LL<<40);
static const uint64_t CACHE_MTBDD_ABSTRACT          = (42LL<<40);
static const uint64_t CACHE_MTBDD_ITE               = (43LL<<40);
static const uint64_t CACHE_MTBDD_AND_ABSTRACT_PLUS = (44LL<<40);
static const uint64_t CACHE_MTBDD_AND_ABSTRACT_MAX  = (45LL<<40);
static const uint64_t CACHE_MTBDD_SUPPORT           = (46LL<<40);
static const uint64_t CACHE_MTBDD_COMPOSE           = (47LL<<40);
static const uint64_t CACHE_MTBDD_ALL_EQUAL_ABS     = (48LL<<40);
static const uint64_t CACHE_MTBDD_ALL_EQUAL_REL     = (49LL<<40);
static const uint64_t CACHE_MTBDD_MINIMUM           = (50LL<<40);
static const uint64_t CACHE_MTBDD_MAXIMUM           = (51LL<<40);
static const uint64_t CACHE_MTBDD_ALL_LEQ           = (52LL<<40);
static const uint64_t CACHE_MTBDD_ALL_LT            = (53LL<<40);
static const uint64_t CACHE_MTBDD_ALL_GEQ           = (54LL<<40);
static const uint64_t CACHE_MTBDD_ALL_GT            = (55LL<<40);
static const uint64_t CACHE_MTBDD_EVAL_COMPOSE      = (56LL<<40);
static const uint64_t CACHE_MTBDD_ANY_LEQ            = (57LL<<40);
static const uint64_t CACHE_MTBDD_ANY_LT             = (58LL<<40);
static const uint64_t CACHE_MTBDD_ANY_GEQ            = (59LL<<40);
static const uint64_t CACHE_MTBDD_ANY_GT             = (60LL<<40);
static const uint64_t CACHE_MTBDD_ANY_EQUAL_ABS      = (61LL<<40);
static const uint64_t CACHE_MTBDD_ANY_EQUAL_REL      = (62LL<<40);
static const uint64_t CACHE_MTBDD_COMPARE_LEQ        = (63LL<<40);
static const uint64_t CACHE_MTBDD_COMPARE_LT         = (64LL<<40);
static const uint64_t CACHE_MTBDD_COMPARE_GEQ        = (65LL<<40);
static const uint64_t CACHE_MTBDD_COMPARE_GT         = (66LL<<40);
static const uint64_t CACHE_MTBDD_COMPARE_EQUAL_ABS  = (67LL<<40);
static const uint64_t CACHE_MTBDD_COMPARE_EQUAL_REL  = (68LL<<40);
static const uint64_t CACHE_MTBDD_SAT_COUNT_DOUBLE   = (69LL<<40);
static const uint64_t CACHE_MTBDD_SAT_COUNT_U64      = (70LL<<40);

// ZDD operations
static const uint64_t CACHE_ZDD_FROM_MTBDD          = (80LL<<40);
static const uint64_t CACHE_ZDD_TO_MTBDD            = (81LL<<40);
static const uint64_t CACHE_ZDD_EXTEND_DOMAIN       = (82LL<<40);
static const uint64_t CACHE_ZDD_SUPPORT             = (83LL<<40);
static const uint64_t CACHE_ZDD_PATHCOUNT           = (84LL<<40);
static const uint64_t CACHE_ZDD_AND                 = (85LL<<40);
static const uint64_t CACHE_ZDD_OR                  = (86LL<<40);
static const uint64_t CACHE_ZDD_ITE                 = (87LL<<40);
static const uint64_t CACHE_ZDD_NOT                 = (88LL<<40);
static const uint64_t CACHE_ZDD_DIFF                = (89LL<<40);
static const uint64_t CACHE_ZDD_EXISTS              = (90LL<<40);
static const uint64_t CACHE_ZDD_PROJECT             = (91LL<<40);
static const uint64_t CACHE_ZDD_ISOP                = (92LL<<40);
static const uint64_t CACHE_ZDD_COVER_TO_BDD        = (93LL<<40);
static const uint64_t CACHE_ZDD_COUNT_U64           = (94LL<<40);
static const uint64_t CACHE_ZDD_FORALL              = (95LL<<40);
static const uint64_t CACHE_ZDD_UNIQUE              = (96LL<<40);
static const uint64_t CACHE_ZDD_WITHOUT_SUPERSETS   = (97LL<<40);
static const uint64_t CACHE_ZDD_MINIMAL_SETS        = (98LL<<40);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <sylvan/internal/ranks_internal.h>
#include <sylvan/internal/mtbdd_internal.h>
#include <sylvan/internal/listdd_internal.h>
#include <sylvan/internal/zdd_internal.h>
#include <sylvan/internal/reorder_internal.h>

#ifdef __cplusplus
} /* extern "C" */
} /* namespace */
#endif

#endif
