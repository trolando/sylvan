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
 * NONE OF THIS IS PUBLIC API. It is the internal machinery sylvan_sift() --
 * the sifting driver in sift.c -- is built from, and it may change or
 * disappear without notice. It is declared in a header so that the driver,
 * and the tests that drive single swaps directly, can reach it.
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
 * <delta> is the exact size change of the *reachable forest*, which is what a
 * sifting driver scores positions on, and it needs no marking pass because
 * both terms are counted as they happen. That it measures the forest and not
 * merely the table is a consequence of the collection sylvan_reorder_begin()
 * performs: the session opens on a table whose content is exactly the
 * reachable set and never collects again, so every node <created> is a node
 * the forest gained and every node that <died> is one it lost. Without that
 * enforced collection the same two counters would still be exact counts of
 * what the swap did, but they would be measured against a baseline that
 * included whatever garbage happened to be lying around.
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
 * A FULL GARBAGE COLLECTION IS THE FIRST THING THIS DOES, once the refusals
 * below have been decided. It is a precondition of the session rather than a
 * convention its callers are trusted to follow, because everything the
 * session measures is measured against the table it opens on: see <delta>
 * above. The collection also discards the operation cache, which the session
 * requires anyway before it may reuse a retired slot, and it may resize the
 * nodes table, which is why the session reads the table size afterwards.
 *
 * THE ONE PRECONDITION, which cannot be checked cheaply and which the caller
 * owns: no decision diagram operation is in flight.
 *
 * It is about suspended computations, not about roots. An operation that has
 * read a node's fields and not yet combined its sub-results would resume after
 * the session and build its answer out of parts computed under a rank order
 * that no longer holds. Its handles would be fine -- nodes are rewritten in
 * place and pinned roots are never retired -- and its reasoning would not.
 * Nothing the session does can repair that, so the caller must not be in the
 * middle of an operation when it opens one.
 *
 * TEMPORARY REFERENCES ARE NO LONGER A PRECONDITION. reorder_pin_roots pins
 * every root a collection would mark, and every worker's
 * mtbdd_refs_push/pushptr stack is among them, so a handle that lives only on
 * such a stack survives the session at the same handle denoting the same
 * function. What a caller may not do is hold one across a *suspended
 * operation*, which the precondition above already forbids.
 *
 * Refused with SYLVAN_ERR_INVALID when:
 * - a session is already open;
 * - MTBDD support is not initialised;
 * - the nodes table implements no reorder primitives (llht);
 * - the ZDD or ListDD subsystem is initialised. Both order their nodes by
 *   identity rather than by rank and share the node arena, so any order other
 *   than the identity order is meaningless for them.
 */
TASK(int, sylvan_reorder_begin)

/**
 * Close a reorder session and free its state. Returns SYLVAN_OK;
 * SYLVAN_ERR_INVALID when no session is open, or when reference count
 * validation is enabled and the maintained counts disagree with a
 * recomputation; or SYLVAN_ERR_OOM when validation could not allocate its
 * recount. In every case but the first the session is closed afterwards.
 */
TASK(int, sylvan_reorder_end)

/**
 * Swap the variables at ranks <rank> and <rank>+1. Requires an open session.
 *
 * Returns SYLVAN_OK, SYLVAN_ERR_INVALID for a rank that is not followed by
 * another registered rank, or SYLVAN_ERR_OOM when some resource the swap
 * needs cannot be promised. <result> may be NULL.
 *
 * ALL-OR-NOTHING. A swap reserves everything it will need while it is still
 * only reading -- node slots, the backend's index structures, the session's
 * node lists, retirement queue, retired-slot array and overflow map -- and
 * once that classification phase reports success the swap performs no
 * allocation that can fail. So SYLVAN_ERR_OOM always means NOTHING has been
 * mutated: not the forest, not the index, not the rank metadata. There is no
 * partial swap and nothing to roll back.
 *
 * The other side of that contract is that a failure after classification is a
 * bug rather than a condition, and is reported by aborting. A swap that gave
 * up halfway would leave a forest in which some nodes have been rewritten and
 * others have not, which no caller could use and no collection could repair.
 */
TASK(int, sylvan_varswap, uint32_t, rank, sylvan_varswap_result*, result)

/**
 * How many internal (non-leaf, non-terminal) nodes the open session's forest
 * holds. Zero when no session is open.
 *
 * This is the number a sifting driver scores positions on. It is seeded by the
 * pass that seeds the reference counts and moved by every swap's <delta>, so
 * reading it is free; recomputing it between swaps would be a scan of the whole
 * table per swap and would cost more than the swaps themselves.
 */
uint64_t sylvan_reorder_forest_size(void);

/**
 * How many nodes of variable identity <identity> the open session's forest
 * holds, decision nodes and map nodes together. Zero when no session is open.
 *
 * A sifting driver asks this once per variable, before it starts, to decide
 * which variables are worth moving and in what order. The answer comes from
 * the session's own per-identity lists, or from native enumeration where the
 * backend has it, so it costs the population and not the table. Asking also
 * compacts the list, which is why it is not const.
 */
uint64_t sylvan_reorder_population(uint32_t identity);

/**
 * Build the variable interaction matrix for the open session, from its external
 * roots. Two identities interact when some function depends on both; this is a
 * property of the functions, so the matrix is fixed for the session and keyed
 * by identity. A sifting driver builds it once and uses it to bound its legs.
 *
 * Returns SYLVAN_OK, SYLVAN_ERR_INVALID if no session is open, or
 * SYLVAN_ERR_OOM. On OOM the matrix is simply absent, which the interaction
 * query reports as "everything interacts", so the driver still runs correctly,
 * only without the bound. Rebuilding replaces any previous matrix.
 */
int sylvan_reorder_interact_build(void);

/**
 * Whether identities <a> and <b> interact. A variable can only exchange nodes
 * with variables it interacts with, which is what lets a driver bound a leg.
 * Without a matrix (not built, or its allocation refused) this returns 1 for
 * every distinct pair, so a driver that consults it stays correct and merely
 * unbounded. A variable never interacts with itself.
 */
int sylvan_reorder_interact(uint32_t a, uint32_t b);

/**
 * Physical slot reuse policy. Nonzero (the default)
 * releases retired slots at the end of each swap, which bounds memory; zero
 * defers every release to the end of the session, which costs nothing per
 * swap but lets garbage accumulate. Retirement itself is unconditional either
 * way, so scoring is exact in both.
 */
void sylvan_reorder_set_release_at_swap_end(int enable);

/**
 * Batched index surgery (design experiment H2). Nonzero (the default) hands a
 * swap's removals and re-indexings to the backend as whole sets wherever it
 * implements NODES_CAP_BATCH and more than one worker runs; zero makes every
 * swap take the per-node path it takes on a backend without the capability,
 * which is the ablation that measures what batching is worth. The swaps, the
 * forest and the scores are the same either way; only the schedule differs.
 */
void sylvan_reorder_set_batched(int enable);

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

#if SYLVAN_SIFT_PHASE_TIMING
/**
 * Diagnostic builds only (-DSYLVAN_SIFT_PHASE_TIMING=ON): where the time of a
 * session went. Wall-clock seconds from the monotonic clock, each field summed
 * over the current or last session and cleared when sylvan_reorder_begin opens
 * the next one. The swap fields are the phases of sylvan_varswap, in the order
 * they run (see PHASES in src/reorder.c); <classify> includes the swaps that
 * were refused there, and <swap_total> covers every swap that got past it, end
 * to end, so what it holds beyond the sum of its phases is the bookkeeping
 * between them. The sifting driver prints all of this, with its own share, as
 * one SIFT-PHASES line on stderr at the end of every sift.
 */
typedef struct sylvan_reorder_timing {
    uint64_t swaps;          /* swaps that got past classification        */
    double gc;               /* begin: the opening collection             */
    double seed;             /* begin: reference counts, populations      */
    double scatter;          /* begin: the per-identity node lists        */
    double pin;              /* begin: pinning the external roots         */
    double begin_total;      /* sylvan_reorder_begin, end to end          */
    double classify;         /* swap: classification and reservation      */
    double intern;           /* swap: the INTERN phase                    */
    double remove_batch;     /* swap: batched removal (near 0 unbatched)  */
    double rewrite;          /* swap: the REWRITE phase                   */
    double reinsert_batch;   /* swap: batched re-indexing (ditto)         */
    double reinsert;         /* swap: the REINSERT phase                  */
    double retire;           /* swap: retirement rounds -- bookkeeping,
                                unhashing and the dropped edges            */
    double release;          /* swap: giving retired slots back           */
    double swap_total;       /* swap, end to end                          */
    double end;              /* sylvan_reorder_end, end to end            */
} sylvan_reorder_timing;

/** Copy out the sums above. */
void sylvan_reorder_phase_timing(sylvan_reorder_timing *timing);
#endif

/**
 * THE SIFT RECOMMENDATION, from the inside.
 *
 * The policy and all of its state live in sift.c, next to the sifting it is a
 * policy about; these are the two lines of it that garbage collection has to
 * reach. See sylvan_sift_recommended and sylvan_gc_set_auto_sift in
 * <sylvan/reorder.h> for what they implement.
 */

/**
 * Fold the result of a collection into the recommendation. <live_nodes> is the
 * number of buckets that survived marking, which the collection counted for
 * its own sizing heuristics. Called once per collection, from either entry.
 */
void sylvan_sift_note_collection(uint64_t live_nodes);

/** Forget the growth baseline. Called by sylvan_init_package(). */
void sylvan_sift_reset_recommendation(void);

/**
 * Run the automatic sift if one is configured and recommended, otherwise do
 * nothing at all. Called by sylvan_gc() and by nothing else -- see
 * sylvan_gc_set_auto_sift for why that restriction is the whole of what makes
 * this sound. Must be called from a Lace worker, outside any frame it would
 * nest in: it opens a frame of its own.
 */
void sylvan_sift_auto(void);

/**
 * Test entry points. They open a session, run the swaps, and close it again,
 * all inside a fresh Lace frame, the way sylvan_gc() enters sylvan_gc_go.
 * Call from inside a Lace worker. They exist so that a swap can be tested on
 * its own, without the sifting driver choosing which swaps to make.
 */
int sylvan_test_varswap(uint32_t rank, sylvan_varswap_result *result);
int sylvan_test_varswap_sequence(const uint32_t *ranks, size_t count,
                                 sylvan_varswap_result *result);

#endif
