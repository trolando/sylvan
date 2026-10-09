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
 * Variable identity and variable rank.
 *
 * A decision node stores an immutable variable *identity* in its 24-bit
 * variable field. It is assigned when the variable is created and never
 * changes for the lifetime of the node.
 *
 * The *rank* of a variable is its position in the current variable order.
 * Rank is metadata, not part of any node, and lives in two dense arrays:
 *
 *     rank_of[identity] -> rank
 *     identity_at[rank] -> identity
 *
 * Both are initialised to the identity permutation, so rank_of[v] == v until
 * something permutes the order.
 *
 * Consequently every operation that decides which of two variables comes
 * first in the order must compare *ranks*, never identities. Use
 * sylvan_var_before() and sylvan_var_min() for that; keep plain equality and
 * arithmetic on identities (interleaved s/t pairs, map keys, subtable routes)
 * as they are.
 *
 * Identities that were never registered (through bdd_new_var() or
 * bdd_var_at_level()) have rank == identity by definition. That keeps the
 * 0xffffffff "no variable" sentinels used throughout the operation code
 * working unchanged: their rank is 0xffffffff, which orders last.
 *
 * ZDD and ListDD are NOT part of this model. ZDD nodes are ordered by
 * identity and ListDD has no variable identities at all; both must keep
 * comparing raw fields. A variable order other than the identity order is
 * therefore only valid for a forest without ZDD nodes (see the guard
 * documented with sylvan_ranks_permute_unsafe).
 */

#ifndef SYLVAN_RANKS_INT_H
#define SYLVAN_RANKS_INT_H

/** One past the largest representable variable identity. */
#define SYLVAN_RANKS_LIMIT UINT32_C(0x01000000)

/**
 * Published rank state.
 *
 * <sylvan_ranks_count> is the number of registered identities. It is
 * published with release ordering *after* the array pointers, and readers
 * load it *before* the array pointers with acquire ordering. A reader that
 * observes a new count therefore also observes the matching (or a newer)
 * array; a reader that observes an old count indexes an array that is at
 * least that large. Superseded arrays are retained until sylvan_ranks_free(),
 * so a stale pointer is never dangling.
 */
extern _Atomic(uint32_t) sylvan_ranks_count;
extern _Atomic(uint32_t*) sylvan_ranks_rank_of;
extern _Atomic(uint32_t*) sylvan_ranks_identity_at;

/** Return the rank of variable identity <var>. One array load on the hot path. */
static inline uint32_t SYLVAN_UNUSED
sylvan_rank_of(uint32_t var)
{
    const uint32_t count =
        atomic_load_explicit(&sylvan_ranks_count, memory_order_acquire);
    if (var >= count) return var;
    const uint32_t *rank_of =
        atomic_load_explicit(&sylvan_ranks_rank_of, memory_order_acquire);
    return rank_of[var];
}

/** Return 1 if variable <a> comes before variable <b> in the order. */
static inline int SYLVAN_UNUSED
sylvan_var_before(uint32_t a, uint32_t b)
{
    return sylvan_rank_of(a) < sylvan_rank_of(b);
}

/** Return whichever of <a> and <b> comes first in the order. */
static inline uint32_t SYLVAN_UNUSED
sylvan_var_min(uint32_t a, uint32_t b)
{
    return sylvan_rank_of(a) < sylvan_rank_of(b) ? a : b;
}

/** Return the identity of the variable at order position <rank>. */
static inline uint32_t SYLVAN_UNUSED
sylvan_identity_at(uint32_t rank)
{
    const uint32_t count =
        atomic_load_explicit(&sylvan_ranks_count, memory_order_acquire);
    if (rank >= count) return rank;
    const uint32_t *identity_at =
        atomic_load_explicit(&sylvan_ranks_identity_at, memory_order_acquire);
    return identity_at[rank];
}

/**
 * Register identities 0 .. <count>-1, giving every newly registered identity
 * a rank equal to itself. Registering fewer identities than are already
 * registered does nothing. Returns SYLVAN_OK, SYLVAN_ERR_INVALID when <count>
 * exceeds SYLVAN_RANKS_LIMIT, or SYLVAN_ERR_OOM.
 */
int sylvan_ranks_reserve(uint32_t count);

/** Return the number of registered identities. */
uint32_t sylvan_ranks_registered(void);

/**
 * Exchange the variables at ranks <rank> and <rank>+1.
 *
 * This is the metadata half of an adjacent variable swap: it is the only
 * thing a swap has to do when the two variables do not interact, and the last
 * thing it does when they do. Unlike sylvan_ranks_permute_unsafe() this is
 * not a test hook -- it is how the order actually changes -- but it is just
 * as unsynchronised, so it may only be called with the world stopped and with
 * the nodes of both variables already brought into agreement with the new
 * order by sylvan_varswap().
 *
 * Returns SYLVAN_OK, or SYLVAN_ERR_INVALID when <rank>+1 is not a registered
 * rank.
 */
int sylvan_ranks_swap_adjacent(uint32_t rank);

/**
 * Install a new variable order. <identity_at> lists the registered identities
 * in rank order and must be a permutation of 0 .. <count>-1, with <count>
 * equal to the number of registered identities.
 *
 * UNSAFE, AND FOR TESTS ONLY. It rewrites the order without touching a single
 * node, so it is only valid while the forest contains no node over any
 * variable whose rank changes -- in practice, before any node is built. It
 * exists so that tests can verify rank-driven behaviour without a variable
 * swap. It also concurrently rewrites the arrays that sylvan_rank_of() reads,
 * so no operation may be in flight.
 *
 * A non-identity order additionally requires that the ZDD subsystem is unused
 * (ZDD orders nodes and its BDDSET domains by identity) and that no
 * serialized diagram is loaded that was written under a different order.
 */
int sylvan_ranks_permute_unsafe(const uint32_t *identity_at, uint32_t count);

/**
 * Return 1 if every registered identity is ranked at its own position, or 0
 * otherwise.
 *
 * This is the question ZDD asks before it initialises, because a non-identity
 * order is exactly what breaks the agreement between a BDDSET domain and the
 * ZDD nodes that read it (see the note at the top of this file).
 *
 * It has to be answered from the current order rather than from history: a
 * swap and its inverse leave the identity order behind, so a session that
 * ends where it started must not lock ZDD out for the rest of the process. So
 * the module only records whether anything *might* have permuted the order.
 * While nothing has, this is one atomic load. Once something has, the answer
 * is recomputed by scanning the order -- O(registered identities) -- and a
 * scan that finds the identity clears the record again. Both callers are
 * cold, so the scan is never on a path where its cost is visible.
 */
int sylvan_ranks_is_identity(void);

/** Release all rank state and forget every registration. */
void sylvan_ranks_free(void);

#endif
