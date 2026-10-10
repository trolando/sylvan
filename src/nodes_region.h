/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2018 Tom van Dijk, Johannes Kepler University Linz
 * Copyright 2019-2026 Formal Methods and Tools, University of Twente
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
 * The node arena of the list, trie, llcht and llchst nodes tables: the slot
 * bitmaps and the region allocator that hands their slots out. llht has an
 * allocator of its own and does not include this file.
 *
 * The arena is an array of table_size node slots cut into regions of 512
 * slots. A worker allocates from the one region it owns until the region is
 * used up, gives it back, and claims the next unowned region that still has a
 * free slot. Three bitmaps describe it:
 *
 *     bitmap1  one bit per region: owned by some worker
 *     bitmap2  one bit per slot: holds a node
 *     bitmapc  one bit per slot: the node uses the custom callbacks
 *
 * NOT A SELF-CONTAINED HEADER. Exactly one translation unit per build includes
 * it -- the nodes_*.c that CMake selected -- right after its definition of
 * struct nodes_table, against which everything below compiles. The contract:
 *
 *   - The struct has these members, under these names (the rest is the
 *     backend's own):
 *
 *         size_t             table_size;     current number of slots
 *         _Atomic(uint64_t)* bitmap1;        region_bitmap_size(max) bytes
 *         _Atomic(uint64_t)* bitmap2;        node_bitmap_size(max) bytes
 *         _Atomic(uint64_t)* bitmapc;        node_bitmap_size(max) bytes
 *         _Atomic(uint64_t)* region_cursor;  two words on a cache line of
 *                                            their own: the cursor (see
 *                                            claim_region) and the high-water
 *                                            mark (see region_used_bound)
 *
 *     where max is the largest table_size the table can grow to.
 *
 *   - The includer defines NODES_REGION_BACKEND, its name as a string literal
 *     ("list", ...), before the include; the exhaustion report quotes it.
 *
 *   - The backend allocates and frees the bitmaps and the cursor line
 *     itself, zeroed, keeps slots 0 and 1 marked in bitmap2 so that they are
 *     never handed out, and runs nodes_reset_region_TOGETHER() in
 *     nodes_create. Its nodes_clear clears bitmap1 and bitmap2, zeroes
 *     region_cursor[0] -- the cursor, and not the high-water mark beside it --
 *     runs nodes_reset_region_TOGETHER() again and clears
 *     arena_exhausted_reported.
 *
 *   - nodes_reserve stays in the backend, which does its own checks and then
 *     returns region_reserve() for the slot half; nodes_used_bound returns
 *     region_used_bound().
 *
 * The functions are plain static, not static inline: the inline keyword
 * would raise GCC's inlining limits for them and so change which calls get
 * inlined where. With SYLVAN_REGION_STRIPED_START (a diagnostic CMake option)
 * the shared cursor below gives way to per-worker stripes.
 */

#ifndef SYLVAN_NODES_REGION_H
#define SYLVAN_NODES_REGION_H

#ifndef NODES_REGION_BACKEND
#error "define NODES_REGION_BACKEND before including nodes_region.h"
#endif

#include <sylvan/internal.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static size_t
region_bitmap_size(size_t table_size)
{
    const size_t regions = table_size / 512u;
    return ((regions + 63u) / 64u) * sizeof(uint64_t);
}

static size_t
node_bitmap_size(size_t table_size)
{
    return ((table_size + 63u) / 64u) * sizeof(uint64_t);
}

/* The region the calling worker allocates from, or UINT64_MAX for none. One
   word per worker, not per table: every backend that uses this file assumes
   there is a single nodes table. */
static SYLVAN_TLS uint64_t my_region = UINT64_MAX;

TASK(void, nodes_reset_region)

void
nodes_reset_region_CALL(lace_worker* lace)
{
    (void)lace;
    /* Lace is only the means to run this on every worker. */
    my_region = UINT64_MAX;
}

/** Whether a region still holds a slot the worker that claims it could use. */
static int
region_has_free_slot(const nodes_table* dbs, uint64_t region)
{
    const _Atomic(uint64_t)* words = dbs->bitmap2 + (region * 8u);
    for (unsigned int i = 0; i < 8; i++) {
        if (atomic_load_explicit(words + i, memory_order_relaxed)
            != UINT64_MAX) {
            return 1;
        }
    }
    return 0;
}

/**
 * The high-water mark of the arena, in the second word of the cursor's line:
 * one more than the highest region any worker has ever claimed, 0 before the
 * first. Raised on every claim that goes past it -- with the shared cursor
 * that is most claims while the table fills, and none once it has stopped
 * growing -- and never lowered, not even by nodes_clear, because a
 * collection keeps the nodes that survive at their indices. Every node was
 * placed in a region that was claimed at the time, so no marked slot lies
 * past it but the reserved slots 0 and 1, which region_used_bound covers.
 */
static void
region_note_claim(const nodes_table* dbs, uint64_t region)
{
    _Atomic(uint64_t)* high = dbs->region_cursor + 1;
    uint64_t seen = atomic_load_explicit(high, memory_order_relaxed);
    while (seen <= region &&
           !atomic_compare_exchange_weak_explicit(
               high, &seen, region + 1u,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

/**
 * The slot half of nodes_used_bound: the end of the highest region ever
 * claimed, and at least the end of region 0, which holds the two reserved
 * slots. Read with the world stopped, where every claim that raised it is
 * ordered before the read.
 */
static size_t
region_used_bound(const nodes_table* dbs)
{
    const uint64_t regions = atomic_load_explicit(
        dbs->region_cursor + 1, memory_order_relaxed);
    const uint64_t slots = (regions == 0 ? 1u : regions) * 512u;
    return slots < dbs->table_size ? (size_t)slots : dbs->table_size;
}

/**
 * Claim a region that is unowned and has something to give. The search
 * starts at start_region and proceeds round-robin; the result is the claimed
 * region, or UINT64_MAX if there is none.
 *
 * Skipping the full ones is what makes this terminate: a worker gives an
 * exhausted region back on its next allocation attempt (see below), so
 * without the check a table whose free slots have all been used would hand
 * the same full regions round and round forever. The give-back is lazy, not
 * immediate -- between exhausting a region and its next allocation the worker
 * still owns that full region -- but it is still that worker's one current
 * region, so there are never more owned regions than workers, which is all
 * the reserve margin needs.
 *
 * The claim acquires and giving a region back releases, pairing on the
 * ownership CAS: a worker that claims a region observes every write the
 * previous owner made to it, which is what lets the allocation path write
 * the slot bitmap with plain loads and stores. The free-slot precheck runs
 * before that CAS and is not ordered by it; that is safe because ordinary
 * allocation only ever turns bits from free to occupied, clearing happens
 * only at a synchronised safepoint, and the post-claim allocation loop scans
 * the region again anyway.
 */
static uint64_t
claim_next_region(const nodes_table* dbs, uint64_t start_region)
{
    const uint64_t regions = dbs->table_size / 512u;
    if (regions == 0) return UINT64_MAX;

    start_region %= regions;

    for (uint64_t offset = 0; offset < regions; offset++) {
        const uint64_t region = (start_region + offset) % regions;
        _Atomic(uint64_t)* ptr = dbs->bitmap1 + (region / 64u);
        const uint64_t mask =
            UINT64_C(0x8000000000000000) >> (region & 63u);

        uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
        if (v & mask) continue;
        if (!region_has_free_slot(dbs, region)) continue;

        for (;;) {
            if (v & mask) break;

            if (atomic_compare_exchange_weak_explicit(
                    ptr, &v, v | mask,
                    memory_order_acquire, memory_order_relaxed)) {
                region_note_claim(dbs, region);
                return region;
            }
        }
    }

    return UINT64_MAX;
}

/**
 * Claim the next region in table order.
 *
 * Every worker draws its regions from one shared cursor, so that region
 * order -- and with it the order of node indices -- follows the order in
 * which regions were needed, across all workers. A serial run has that
 * property for free. The anchored backends, list and trie, need it for their
 * layout: a node's family is anchored at its larger child index, so a child
 * that predates its parents' other children anchors only the few parents
 * whose other child is older still. Starting each worker in its own stripe of
 * the table breaks that: everything a high-stripe worker creates out-indexes
 * everything the low-stripe workers made, and a heavily shared node in a high
 * stripe then anchors its whole fan-in. With several workers the largest
 * families of such a table are many times longer than those of the same
 * forest built serially, and every family walk -- lookups, insertions,
 * removals -- pays for it. The chained backends, llcht and llchst, put a node
 * in the chain its hash selects, whatever its index; they have no families
 * for the cursor to keep short and draw from it only because they share this
 * allocator.
 *
 * The cursor is a hint, not a lock: it is advanced to just past whatever was
 * claimed, and two workers that read the same position simply race for the
 * region and the loser walks on to the next free one. Being behind by a few
 * regions costs a few ownership-bit tests. It lives on a line of its own
 * because the table struct itself is read on every lookup by every worker.
 */
#if !SYLVAN_REGION_STRIPED_START
static uint64_t
claim_region(const nodes_table* dbs)
{
    const uint64_t regions = dbs->table_size / 512u;
    if (regions == 0) return UINT64_MAX;
    const uint64_t start = atomic_load_explicit(
        dbs->region_cursor, memory_order_relaxed) % regions;
    const uint64_t region = claim_next_region(dbs, start);
    if (region != UINT64_MAX) {
        atomic_store_explicit(
            dbs->region_cursor, region + 1u, memory_order_relaxed);
    }
    return region;
}
#else
/**
 * Per-worker stripes, the diagnostic alternative to the shared cursor,
 * compiled in only to measure what the cursor is worth (configure with
 * -DSYLVAN_REGION_STRIPED_START=ON). A worker's first search after the table
 * is created or collected starts in its own stripe of the table, and every
 * later one goes on past the region the worker has just used up, so that each
 * worker fills a stripe of its own; the cursor is not consulted at all. Only
 * where nodes are placed changes, and no production build should have it.
 */
static uint64_t
claim_striped_region(const nodes_table* dbs)
{
    const uint64_t regions = dbs->table_size / 512u;
    uint64_t start_region = 0;
    lace_worker* worker = lace_get_worker();
    if (regions != 0 && worker != NULL) {
        start_region = ((uint64_t)worker->worker * regions) /
                       (uint64_t)lace_worker_count();
    }
    return claim_next_region(dbs, start_region);
}
#endif

/**
 * Give back a region this worker has used up.
 *
 * A region is owned for exactly as long as it is some worker's current one,
 * which bounds the number of owned regions by the number of workers. That
 * bound is what nodes_reserve counts against, and giving regions back is what
 * keeps the slots a reorder session releases reachable; see nodes_reserve in
 * <sylvan/nodes.h>.
 */
static void
release_region(const nodes_table* dbs, uint64_t region)
{
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (region & 63u);
    atomic_fetch_and_explicit(
        dbs->bitmap1 + region / 64u, ~mask, memory_order_release);
}

/**
 * Take a free slot for the calling worker: the first one in its current
 * region, or in the region it claims once that one is used up. UINT64_MAX
 * when no unowned region has a free slot left; the caller reports that with
 * report_arena_exhausted.
 */
static uint64_t
claim_data_bucket(const nodes_table* dbs)
{
    for (;;) {
        if (my_region != UINT64_MAX) {
            _Atomic(uint64_t)* ptr = dbs->bitmap2 + (my_region * 8u);

            for (int i = 0; i < 8; i++) {
                uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
                if (v != UINT64_MAX) {
                    const unsigned int j = clz_uint64(~v);
                    const uint64_t mask =
                        UINT64_C(0x8000000000000000) >> j;
                    atomic_store_explicit(ptr, v | mask, memory_order_relaxed);
                    return (8u * my_region + (uint64_t)i) * 64u +
                           (uint64_t)j;
                }
                ptr++;
            }

            const uint64_t exhausted = my_region;
            my_region = UINT64_MAX;
            release_region(dbs, exhausted);
#if SYLVAN_REGION_STRIPED_START
            const uint64_t claimed = claim_next_region(dbs, exhausted + 1u);
#else
            const uint64_t claimed = claim_region(dbs);
#endif
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        } else {
#if SYLVAN_REGION_STRIPED_START
            const uint64_t claimed = claim_striped_region(dbs);
#else
            const uint64_t claimed = claim_region(dbs);
#endif
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        }
    }
}

/**
 * Give back a slot claim_data_bucket has just handed out, when nodes_lookup2
 * does not publish a node in it after all. A plain store, like the claim: the
 * slot lies in the region the calling worker owns. nodes_release, which frees
 * slots in regions it does not own, clears the bit with a read-modify-write
 * of its own instead.
 */
static void
release_data_bucket(const nodes_table* dbs, uint64_t index)
{
    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index / 64);
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63);
    const uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
    atomic_store_explicit(ptr, v & ~mask, memory_order_relaxed);
}

/*
 * The insertion and cleanup paths do not need read-modify-write on bitmapc:
 * during insertion each worker owns a whole data region via bitmap1, so no two
 * workers write custom bits in the same region concurrently, and
 * cleanup/rebuild is stop-the-world relative to lookup. The words are atomic
 * only so that the reorder primitives, which do not own a region, can clear a
 * single bit without racing with those relaxed accesses.
 */
static void
set_custom_bucket(const nodes_table* dbs, uint64_t index, int on)
{
    _Atomic(uint64_t)* ptr = dbs->bitmapc + (index / 64);
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63);
    const uint64_t value = atomic_load_explicit(ptr, memory_order_relaxed);
    atomic_store_explicit(
        ptr, on ? (value | mask) : (value & ~mask), memory_order_relaxed);
}

static int
is_custom_bucket(const nodes_table* dbs, uint64_t index)
{
    const _Atomic(uint64_t)* ptr = dbs->bitmapc + (index / 64);
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63);
    return (atomic_load_explicit(ptr, memory_order_relaxed) & mask) ? 1 : 0;
}

/**
 * The arena has no slot to give. Say so, once.
 *
 * nodes_lookup2 answers 0 here and every caller turns that into "no room",
 * but not every caller says which room ran out. The report that normally
 * announces a full table is gc_note_progress in common.c ("BDD Unique table
 * full"), and it cannot speak for this path: it fires from inside a
 * collection, and only after several consecutive collections of a table that
 * is already at its maximum size and still nearly full. A lookup that fails
 * where no collection may run -- inside a reorder session -- or that fails
 * while the table as a whole is below that threshold, because the free slots
 * sit inside regions other workers own, never reaches that guard at all.
 * Without this report the operation that asked for the node would fail with
 * nothing in its output about the table.
 *
 * Once per exhaustion episode, not once per failing lookup: the flag is
 * cleared in nodes_clear, which is where a collection gives the table its
 * room back. The hot path is untouched -- only the branch that has already
 * failed to claim a slot arrives here. The flag is one word beside the
 * allocator rather than a field of the table because the allocator it speaks
 * for is itself global (see my_region above).
 */
static _Atomic(int) arena_exhausted_reported;

static void SYLVAN_NOINLINE
report_arena_exhausted(const nodes_table* dbs)
{
    if (atomic_exchange_explicit(&arena_exhausted_reported, 1,
                                 memory_order_relaxed) != 0) {
        return;
    }
    fprintf(stderr,
            "sylvan: " NODES_REGION_BACKEND " nodes table out of node slots, "
            "%zu buckets, none free to claim; nodes_lookup returns 0 and the "
            "operation that asked for a node fails. Raise the memory cap "
            "passed to sylvan_set_limits.\n",
            dbs->table_size);
}

/**
 * nodes_reinsert found a different node with the same key already in the
 * index. Shared by the four backends' nodes_reinsert; the unique table
 * invariant is broken and there is nothing to recover.
 */
static void
reorder_duplicate_abort(uint64_t index, uint64_t other)
{
    assert(!"nodes_reinsert: an equal node is already in the index");
    fprintf(stderr,
            "sylvan: nodes_reinsert: node %llu is equal to node %llu, which "
            "is already in the index; the unique table invariant is broken. "
            "Aborting.\n",
            (unsigned long long)index, (unsigned long long)other);
    abort();
}

/**
 * The free slots of the regions no worker owns, counted until they reach
 * <needed>: the scan stops there, so a small reservation on a table with room
 * costs a handful of popcounts. Free slots inside an owned region are not
 * counted, because only its owner can reach them; region_reserve adds the
 * margin that covers them. See nodes_reserve in <sylvan/nodes.h> for why that
 * margin makes the count a guarantee.
 */
static uint64_t
reserve_free_slots(const nodes_table* dbs, uint64_t needed)
{
    const uint64_t regions = dbs->table_size / 512u;
    uint64_t free_slots = 0;

    for (uint64_t region = 0; region < regions; region++) {
        const uint64_t owned = atomic_load_explicit(
            dbs->bitmap1 + region / 64u, memory_order_relaxed);
        const uint64_t owner_mask =
            UINT64_C(0x8000000000000000) >> (region & 63u);
        if (owned & owner_mask) continue;

        const _Atomic(uint64_t)* words = dbs->bitmap2 + region * 8u;
        for (unsigned int i = 0; i < 8; i++) {
            free_slots += 64u - popcnt_uint64(atomic_load_explicit(
                words + i, memory_order_relaxed));
        }
        if (free_slots >= needed) return free_slots;
    }
    return free_slots;
}

/**
 * The slot half of nodes_reserve, which each backend returns after its own
 * checks: SYLVAN_OK when the unowned regions hold <count> free slots plus the
 * margin, SYLVAN_ERR_OOM otherwise.
 */
static int
region_reserve(const nodes_table* dbs, uint64_t count)
{
    /* One whole region per worker on top of what the caller asked for; see
       nodes_reserve in <sylvan/nodes.h> for why the bound needs it. */
    const uint64_t margin = (uint64_t)lace_worker_count() * 512u;
    if (count > UINT64_MAX - margin) return SYLVAN_ERR_OOM;
    const uint64_t wanted = count + margin;
    return reserve_free_slots(dbs, wanted) >= wanted
        ? SYLVAN_OK : SYLVAN_ERR_OOM;
}

#endif
