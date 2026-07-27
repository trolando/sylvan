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

/* Fallible node construction for protected-destination operations. */
int _mtbdd_try_make_node(MTBDD *destination, uint32_t var, MTBDD low, MTBDD high);
int _zdd_try_make_node(ZDD *destination, uint32_t var, ZDD low, ZDD high);
int _listdd_try_make_node(LISTDD *destination, uint32_t value, LISTDD down, LISTDD right);
int _listdd_try_make_copy_node(LISTDD *destination, LISTDD down, LISTDD right);

/* Shared implementation for BDD and MTBDD evaluation. */
int _mtbdd_eval(MTBDD *destination, MTBDD dd, BDDSET variables, const uint8_t *values, size_t count);

/** Assert that <set> is a referenced conjunction of positive variables. */
void bdd_set_assert_valid(BDDSET set);

/** The ZDD terminal representing the family containing only the empty set. */
static const ZDD zdd_base = UINT64_C(1);

/** Advanced multi-terminal ZDD conversion primitives. */
TASK(int, zdd_from_mtbdd, ZDD*, result, MTBDD, dd, BDDSET, domain)
TASK(int, zdd_to_mtbdd, MTBDD*, result, ZDD, dd, BDDSET, domain)

/**
 * Experimental generic Boolean apply/abstract engine.
 *
 * This remains in the advanced API until representative benchmarks justify
 * exposing it as part of the normal operation surface.
 */
typedef enum {
    BDD_APPLY_AND,
    BDD_APPLY_XOR,
    BDD_APPLY_OR,
    BDD_APPLY_XNOR,
    BDD_APPLY_NAND,
    BDD_APPLY_NOR,
    BDD_APPLY_IMP,
    BDD_APPLY_DIFF
} bdd_apply_operator;

typedef enum {
    BDD_ABSTRACT_EXISTS,
    BDD_ABSTRACT_FORALL,
    BDD_ABSTRACT_UNIQUE
} bdd_abstract_operator;

TASK(int, bdd_apply_abstract, BDD*, result, BDD, a, BDD, b,
     BDDSET, variables, bdd_apply_operator, apply,
     bdd_abstract_operator, abstract)

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

#include <sylvan/internal/mtbdd_internal.h>
#include <sylvan/internal/listdd_internal.h>
#include <sylvan/internal/zdd_internal.h>

#ifdef __cplusplus
} /* namespace */
#endif

#endif
