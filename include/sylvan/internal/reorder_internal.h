/*
 * Copyright 2026 Tom van Dijk, Formal Methods and Tools, University of Twente
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

/* Do not include this file directly. Instead, include <sylvan/internal.h>. */

/**
 * Dynamic variable reordering: the session and the adjacent variable swap.
 *
 * NONE OF THIS IS PUBLIC API. It is the internal machinery a future
 * sylvan_sift() is built from, and it may change or disappear without notice.
 * The only reason it is declared in a header at all is that the tests drive it
 * directly; there is no sifting driver yet.
 *
 * THE ORDER MODEL. A node stores an immutable variable *identity*; the
 * position of a variable in the order is its *rank*, which lives in the rank
 * metadata (see ranks_internal.h). Swapping two adjacent ranks therefore
 * leaves almost every node alone: the nodes of the variable that moved down,
 * the nodes of the variable that moved up, and everything above and below all
 * keep their records and their handles. Only the nodes of the upper variable
 * that actually *depend* on the lower one ("case B") are rewritten, in place,
 * at the same handle, so no parent edge anywhere in the forest changes.
 *
 * THE SESSION. Every swap runs inside a session, and a session runs with the
 * world stopped (a Lace NEWFRAME, exactly like garbage collection). The
 * session owns the state a swap needs but a swap cannot afford to recompute:
 * exact reference counts for every node, the set of external roots, and the
 * per-identity node lists for backends that cannot enumerate a variable
 * natively. See sylvan_reorder_begin() for the preconditions.
 */

#ifndef SYLVAN_REORDER_INT_H
#define SYLVAN_REORDER_INT_H

/**
 * What one call to sylvan_varswap() did.
 *
 * <delta> is the exact size change of the forest, which is what a sifting
 * driver scores positions on; it needs no marking pass because both terms are
 * counted as they happen.
 */
typedef struct sylvan_varswap_result {
    uint64_t swaps;          /* number of swaps these numbers cover        */
    uint64_t rewritten;      /* case-B decision nodes rewritten in place   */
    uint64_t map_rewritten;  /* case-B map nodes rewritten in place        */
    uint64_t created;        /* node slots newly allocated                 */
    uint64_t died;           /* nodes retired                              */
    uint64_t deep_deaths;    /* retirements past the depth-1 death theorem */
    int64_t  delta;          /* created - died                             */
} sylvan_varswap_result;

/**
 * Open a reorder session. Must run with the world stopped.
 *
 * Preconditions, none of which can be checked cheaply and all of which the
 * caller owns:
 * - no decision diagram operation is in flight;
 * - no worker holds temporary references (mtbdd_refs_push/pushptr). External
 *   roots are read from the mtbdd_refs and mtbdd_protected tables only; a
 *   handle that lives solely on a worker's temporary stack is invisible to
 *   the session and its node may be retired underneath it.
 *
 * Refused with SYLVAN_ERR_INVALID when:
 * - a session is already open;
 * - MTBDD support is not initialised;
 * - the nodes table implements no reorder primitives (llht);
 * - the ZDD or ListDD subsystem is initialised. Both order their nodes by
 *   identity rather than by rank and share the node arena, so any order other
 *   than the identity order is meaningless for them;
 * - a garbage collection mark hook other than Sylvan's own three is
 *   registered. Such a hook owns external roots the session cannot enumerate,
 *   because the hook interface marks roots instead of visiting them. Teaching
 *   it to visit is follow-up work; until then the session refuses rather than
 *   silently retire a root that only that hook knows about.
 */
TASK(int, sylvan_reorder_begin)

/**
 * Close a reorder session and free its state. Returns SYLVAN_OK, or
 * SYLVAN_ERR_INVALID when reference count validation is enabled and the
 * maintained counts disagree with a recomputation.
 */
TASK(int, sylvan_reorder_end)

/**
 * Swap the variables at ranks <rank> and <rank>+1. Requires an open session.
 *
 * Returns SYLVAN_OK, SYLVAN_ERR_INVALID for a rank that is not followed by
 * another registered rank, or SYLVAN_ERR_OOM when the nodes table cannot
 * promise room for the new nodes the swap needs -- in which case NOTHING has
 * been mutated: the classification phase is read-only and the reservation
 * happens before it. <result> may be NULL.
 */
TASK(int, sylvan_varswap, uint32_t, rank, sylvan_varswap_result*, result)

/**
 * Physical slot reuse policy (design experiment E6). Nonzero (the default)
 * releases retired slots at the end of each swap, which bounds memory; zero
 * defers every release to the end of the session, which costs nothing per
 * swap but lets garbage accumulate. Retirement itself is unconditional either
 * way, so scoring is exact in both.
 */
void sylvan_reorder_set_release_at_swap_end(int enable);

/**
 * Recompute every reference count from scratch when the session closes and
 * compare it with the maintained counts. Off by default; the tests turn it on.
 */
void sylvan_reorder_set_validate(int enable);

/**
 * The reference count at which a node moves from the 16-bit array into the
 * sparse overflow map. Defaults to 65535 and exists so that a test can drive
 * the overflow path without building a node with 65535 parents. Must be at
 * least 1 and at most 65535, and may only be changed while no session is open.
 */
void sylvan_reorder_set_overflow_at(uint32_t count);

/** How many nodes the current or last session promoted to the overflow map. */
uint64_t sylvan_reorder_overflow_promotions(void);

/**
 * Test entry points. They open a session, run the swaps, and close it again,
 * all inside a fresh Lace frame, the way sylvan_gc() enters sylvan_gc_go.
 * Call from inside a Lace worker. There is no sifting driver yet; these exist
 * so that the swap can be tested at all.
 */
int sylvan_test_varswap(uint32_t rank, sylvan_varswap_result *result);
int sylvan_test_varswap_sequence(const uint32_t *ranks, size_t count,
                                 sylvan_varswap_result *result);

#endif
