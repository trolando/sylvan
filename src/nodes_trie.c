/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2018 Tom van Dijk, Johannes Kepler University Linz
 * Copyright 2019-2025 Formal Methods and Tools, University of Twente
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

#include <sylvan/internal.h>
#include <sylvan/platform.h>

#include <assert.h>
#include <errno.h>  // for errno
#include <stdlib.h> // realloc, free, abort
#include <string.h> // memset

/**
 * Manage nodes in an array, using a trie from the maximum child to ensure that
 * nodes are unique.
 *
 * Node indices are 40-bit numbers.
 *
 * The set has support for stop-the-world garbage collection, implemented by
 * nodes_clear, nodes_mark_rec and nodes_rebuild (which reinserts every marked
 * node with nodes_reinsert_bucket). nodes_reinsert is a reorder primitive;
 * see below.
 */

typedef struct nodes_table
{
    uint8_t*           data;         // array with the nodes
    _Atomic(uint64_t)* first;        // array with the root of the trie
    _Atomic(uint64_t)* next;         // array with the 'next' entries of the trie
    _Atomic(uint64_t)* bitmap1;      // ownership bitmap (per 512 buckets)
    _Atomic(uint64_t)* bitmap2;      // bitmap for "contains data"
    _Atomic(uint64_t)* bitmapc;      // bitmap for "use custom functions"
    _Atomic(uint64_t)* region_cursor; // after the last claimed region; own line
    size_t             max_size;     // maximum size of the nodes table (for resizing)
    size_t             table_size;   // current size of the nodes table
    nodes_hash_cb      hash_cb;      // custom hash function
    nodes_equals_cb    equals_cb;    // custom equals function
    nodes_create_cb    create_cb;    // custom create function
    nodes_destroy_cb   destroy_cb;   // custom destroy function
    _Atomic(uint32_t)  reorder_lock; // serialises the reorder primitives
    uint64_t*          region;       // scratch for the removal region rebuild
    size_t             region_size;  // entries in <region>
    uint64_t*          codes;        // a batch's victim slots, tail of <region>
    _Atomic(uint64_t)* batch;        // batch cursor and error flag; own line
} nodes_table;

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

void* nodes_get_pointer(const nodes_table* dbs, size_t index)
{
    return dbs->data + index * 16;
}

size_t nodes_get_max_size(const nodes_table* dbs)
{
    return dbs->max_size;
}

size_t nodes_get_size(const nodes_table* dbs)
{
    return dbs->table_size;
}

size_t
nodes_get_capacity_bytes(size_t capacity)
{
    return capacity * (3 * sizeof(uint64_t) + 2 * sizeof(uint64_t)) +
           region_bitmap_size(capacity) + 2 * node_bitmap_size(capacity);
}

void nodes_set_size(nodes_table* dbs, size_t size)
{
    /* check bounds (don't be ridiculous) */
    if (size > 128 && size >= dbs->table_size && size <= dbs->max_size) {
        dbs->table_size = size;
    }
}

static SYLVAN_TLS uint64_t my_region = UINT64_MAX;

TASK(void, nodes_reset_region)

void nodes_reset_region_CALL(lace_worker* lace)
{
    (void)lace;
    // we don't actually need Lace, but it's a Lace task to run for initialisation
    my_region = UINT64_MAX; // no region
}

/** Whether a region still holds a slot the worker that claims it could use. */
static int region_has_free_slot(const nodes_table* dbs, uint64_t region)
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
 * Try to claim a free region that still has a slot to give.
 * The search starts at start_region and proceeds round-robin.
 * Returns the claimed region index, or UINT64_MAX if none available.
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
static uint64_t claim_next_region(const nodes_table* dbs, uint64_t start_region)
{
    const uint64_t regions = dbs->table_size / (64u * 8u);
    if (regions == 0) return UINT64_MAX;

    start_region %= regions;

    for (uint64_t offset = 0; offset < regions; offset++) {
        const uint64_t region = (start_region + offset) % regions;
        _Atomic(uint64_t)*ptr = dbs->bitmap1 + (region / 64u);
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
 * property for free, and the anchored backends depend on it more than it
 * looks: a node's family is anchored at its larger child index, so a child
 * that predates its parents' other children anchors only the few parents
 * whose other child is older still. Starting each worker in its own stripe
 * of the table breaks that: everything a high-stripe worker creates
 * out-indexes everything the low-stripe workers made, and a heavily shared
 * node in a high stripe then anchors its whole fan-in. Measured on C880 with
 * eight workers, the stripes made the largest family seven times longer and
 * the summed squared family length twelve times larger than the same forest
 * built serially, and every family walk -- lookups, insertions, removals --
 * paid for it.
 *
 * The cursor is a hint, not a lock: it is advanced to just past whatever was
 * claimed, and two workers that read the same position simply race for the
 * region and the loser walks on to the next free one. Being behind by a few
 * regions costs a few ownership-bit tests. It lives on a line of its own
 * because the table struct itself is read on every lookup by every worker.
 */
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

/**
 * Give back a region this worker has used up.
 *
 * A region is owned for exactly as long as it is some worker's current one,
 * which bounds the number of owned regions by the number of workers. That
 * bound is not cosmetic: it is what nodes_reserve counts against, and it is
 * what makes a slot released into an exhausted region reachable again. A
 * session releases retired slots all over the table, and the region they land
 * in is almost never the region the worker that once filled it is still
 * standing in.
 */
static void release_region(const nodes_table* dbs, uint64_t region)
{
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (region & 63u);
    atomic_fetch_and_explicit(
        dbs->bitmap1 + region / 64u, ~mask, memory_order_release);
}

static uint64_t claim_data_bucket(const nodes_table* dbs)
{
    for (;;) {
        if (my_region != UINT64_MAX) {
            _Atomic(uint64_t)*ptr = dbs->bitmap2 + (my_region * 8u);

            for (int i = 0; i < 8; i++) {
                uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
                if (v != UINT64_MAX) {
                    unsigned int j = clz_uint64(~v);
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
            uint64_t claimed = claim_region(dbs);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        } else {
            uint64_t claimed = claim_region(dbs);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        }
    }
}

static void release_data_bucket(const nodes_table* dbs, uint64_t index)
{
    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
    atomic_store_explicit(ptr, v & ~mask, memory_order_relaxed);
}

/*
 * The insertion and cleanup paths do not need read-modify-write on bitmapc:
 * during insertion each worker owns a whole data region via bitmap1, and
 * cleanup/rebuild is stop-the-world relative to lookup. The words are atomic
 * only so that the reorder primitives, which do not own a region, can clear a
 * single bit without racing with those relaxed accesses.
 */
static void set_custom_bucket(const nodes_table* dbs, uint64_t index, int on)
{
    _Atomic(uint64_t) *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    const uint64_t value = atomic_load_explicit(ptr, memory_order_relaxed);
    atomic_store_explicit(
        ptr, on ? (value | mask) : (value & ~mask), memory_order_relaxed);
}

static int is_custom_bucket(const nodes_table* dbs, uint64_t index)
{
    const _Atomic(uint64_t) *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    return (atomic_load_explicit(ptr, memory_order_relaxed) & mask) ? 1 : 0;
}

static inline uint64_t ror64(uint64_t x, unsigned int k) {
    return (x >> k) | (x << (64 - k));
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
 * sit inside regions other workers own, never reaches that guard at all. In
 * the wild that surfaced as a bare "build failed" with nothing in it about
 * the table.
 *
 * Once per exhaustion episode, not once per failing lookup: the flag is
 * cleared in nodes_clear, which is where a collection gives the table its
 * room back. The hot path is untouched -- only the branch that has already
 * failed to claim a slot arrives here. The flag is one word beside the
 * allocator rather than a field of the table because the allocator it speaks
 * for is itself global (see the note on a single nodes table in
 * nodes_create below).
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
            "sylvan: trie nodes table out of node slots, %zu buckets, none "
            "free to claim; nodes_lookup returns 0 and the operation that "
            "asked for a node fails. Raise the memory cap passed to "
            "sylvan_set_limits.\n",
            dbs->table_size);
}

static inline uint64_t nodes_lookup2(const nodes_table* dbs, uint64_t a, uint64_t b, int* created, const int custom)
{
    sylvan_stats_count(LLMSSET_LOOKUP_CALLS);

    // Determine index in the trie array (first)
    uint64_t trie;
    if (a & 0x4000000000000000) {
        trie = 1; // it's a leaf
    } else {
        uint64_t A = a & 0x000000ffffffffff;
        uint64_t B = b & 0x000000ffffffffff;
        trie = A > B ? A : B;
    }

    // Calculate the hash
    uint64_t hash = 14695981039346656037LLU;
    if (custom) hash = dbs->hash_cb(a, b, hash);
    else hash = sylvan_tabhash16(a, b, hash);
    uint64_t masked_hash = hash & 0xffffff0000000000;
    uint64_t created_idx = 0;

    // First check the root of the trie
    uint64_t value = atomic_load_explicit(&dbs->first[trie], memory_order_acquire);
    if (value == 0) {
        // Empty, so not yet inserted!
        // Claim data bucket and write data
        created_idx = claim_data_bucket(dbs);
        if (created_idx == UINT64_MAX) {
            report_arena_exhausted(dbs); // failed to claim a data bucket!!
            return 0;
        }
        if (custom) {
            const int status = dbs->create_cb(&a, &b);
            if (status != 0) {
                release_data_bucket(dbs, created_idx);
                *created = status;
                return 0;
            }
        }
        uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*created_idx;
        d_ptr[0] = a;
        d_ptr[1] = b;
        // Use compare-exchange to store in the trie
        if (atomic_compare_exchange_strong_explicit(
                &dbs->first[trie], &value, masked_hash | created_idx,
                memory_order_acq_rel, memory_order_acquire)) {
            if (custom) set_custom_bucket(dbs, created_idx, custom);
            *created = 1;
            return created_idx;
        }
    }

    // Root of the trie already has a value. Compare it!
    if (masked_hash == (value&0xffffff0000000000)) {
        uint64_t d_idx = value & 0x000000ffffffffff;
        uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*d_idx;
        if (custom) {
            if (dbs->equals_cb(a, b, d_ptr[0], d_ptr[1])) {
                if (created_idx != 0) {
                    dbs->destroy_cb(a, b);
                    release_data_bucket(dbs, created_idx);
                }
                *created = 0;
                return d_idx;
            }
        } else {
            if (d_ptr[0] == a && d_ptr[1] == b) {
                if (created_idx != 0) {
                    release_data_bucket(dbs, created_idx);
                }
                *created = 0;
                return d_idx;
            }
        }
    }

    // Was a different value, continue trying to find a place.
    for (;;) {
        uint64_t n_idx = trie*2 + (hash&1);
        hash = ror64(hash, 1);
        sylvan_stats_count(LLMSSET_LOOKUP);
        value = atomic_load_explicit(&dbs->next[n_idx], memory_order_acquire);
        if (value == 0) {
            if (created_idx == 0) {
                // Claim data bucket and write data
                created_idx = claim_data_bucket(dbs);
                if (created_idx == UINT64_MAX) {
                    report_arena_exhausted(dbs); // failed to claim a data bucket!!
                    return 0;
                }
                if (custom) {
                    const int status = dbs->create_cb(&a, &b);
                    if (status != 0) {
                        release_data_bucket(dbs, created_idx);
                        *created = status;
                        return 0;
                    }
                }
                uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*created_idx;
                d_ptr[0] = a;
                d_ptr[1] = b;
            }
            if (atomic_compare_exchange_strong_explicit(
                    &dbs->next[n_idx], &value, masked_hash | created_idx,
                    memory_order_acq_rel, memory_order_acquire)) {
                if (custom) set_custom_bucket(dbs, created_idx, custom);
                *created = 1;
                return created_idx;
            }
        }
        // Compare the data
        if (masked_hash == (value&0xffffff0000000000)) {
            uint64_t d_idx = value & 0x000000ffffffffff;
            uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*d_idx;
            if (custom) {
                if (dbs->equals_cb(a, b, d_ptr[0], d_ptr[1])) {
                    if (created_idx != 0) {
                        dbs->destroy_cb(a, b);
                        release_data_bucket(dbs, created_idx);
                    }
                    *created = 0;
                    return d_idx;
                }
            } else {
                if (d_ptr[0] == a && d_ptr[1] == b) {
                    if (created_idx != 0) {
                        release_data_bucket(dbs, created_idx);
                    }
                    *created = 0;
                    return d_idx;
                }
            }
        }
        // Not equal, continue...
        trie = value&0x000000ffffffffff;
    }
}

uint64_t nodes_lookup(const nodes_table* dbs, const uint64_t a, const uint64_t b, int* created)
{
    return nodes_lookup2(dbs, a, b, created, 0);
}

uint64_t nodes_lookupc(const nodes_table* dbs, const uint64_t a, const uint64_t b, int* created)
{
    return nodes_lookup2(dbs, a, b, created, 1);
}

uint64_t nodes_lookup_subtable(const nodes_table* dbs, uint32_t subtable,
                               uint64_t a, uint64_t b, int* created)
{
    (void)subtable;
    return nodes_lookup2(dbs, a, b, created, 0);
}

uint64_t nodes_lookupc_subtable(const nodes_table* dbs, uint32_t subtable,
                                uint64_t a, uint64_t b, int* created)
{
    (void)subtable;
    return nodes_lookup2(dbs, a, b, created, 1);
}

/**
 * Reinsert one marked bucket into the (cleared) trie.
 *
 * This walks the same probe sequence as nodes_lookup2, but skips the equality
 * checks: the marked buckets are the canonical nodes of the previous table, so
 * they are pairwise distinct and no bucket can find itself already present.
 *
 * Concurrency. Several workers may run this at the same time on different
 * buckets. Note that the families do not have disjoint slots: the family
 * rooted at first[X] uses next[X*2+0] and next[X*2+1] as its depth-1 branch
 * pair, and those are the very same words that the family of X's own anchor
 * uses when its probe sequence passes through handle X. So workers cannot be
 * partitioned by anchor; they genuinely share slots. It is nevertheless safe:
 *
 * 1. Slots are write-once. Every store is a CAS against the value 0, and the
 *    value stored is (masked_hash | handle) with handle >= 2, so it is never
 *    0. A slot therefore changes at most once, and any non-zero value read
 *    from it is already its final value.
 *
 * 2. A walk never diverges from the final probe sequence. At each step the
 *    worker either reads a non-zero (hence final) value and descends through
 *    it, or reads 0 and attempts the CAS. If the CAS succeeds the node is
 *    placed and the walk ends; if it fails, the CAS has written the current
 *    (hence final) value into `value` and the walk descends through that. So
 *    the slots a worker visits are always a prefix of the probe sequence that
 *    a later nodes_lookup2 will follow for the same node.
 *
 *    This is what defeats the "lost node" interleaving: two workers reinserting
 *    into a shared slot cannot both believe they own it, and the loser does not
 *    abandon its node - it continues the descent from the winner's entry and
 *    lands deeper on the same, now longer, sequence. Combined with (1), no
 *    later insertion can re-route the prefix that led to a placed node, so a
 *    node that has been placed stays reachable.
 *
 * 3. The walk terminates. Handles are unique and each is stored in exactly one
 *    slot, so a probe sequence that revisited a handle would revisit the slot
 *    holding it, which forces its two predecessors to be equal, and inductively
 *    forces the anchor itself to occur as an interior handle of its own
 *    sequence. That would make the "is stored in the branch pair of" relation
 *    cyclic. It cannot be: publishing a handle is a release CAS that is
 *    ordered after the acquire load which read its predecessor, so the relation
 *    strictly increases in the happens-before order. Sequences are finite and
 *    the walk stops at the first empty slot. (The acquire/release pairs above
 *    are what make this argument hold; plain relaxed accesses would not order
 *    the two slots against each other.)
 *
 * Consequence: every marked node is reinserted exactly once and is found at
 * its original index afterwards, for any interleaving. The *shape* of the trie
 * does depend on the order in which buckets happen to be reinserted, so it is
 * not bit-identical across worker counts; node handles are unaffected.
 */
static int nodes_reinsert_bucket(nodes_table* dbs, uint64_t d_idx)
{
    const uint64_t * const d_ptr = ((uint64_t*)dbs->data) + 2*d_idx;
    const uint64_t a = d_ptr[0];
    const uint64_t b = d_ptr[1];

    // Calculate the hash
    uint64_t hash = 14695981039346656037LLU;
    const int custom = is_custom_bucket(dbs, d_idx) ? 1 : 0;
    if (custom) hash = dbs->hash_cb(a, b, hash);
    else hash = sylvan_tabhash16(a, b, hash);
    uint64_t masked_hash = hash & 0xffffff0000000000;

    uint64_t trie;
    if (a & 0x4000000000000000) {
        trie = 1; // it's a leaf
    } else {
        uint64_t A = a & 0x000000ffffffffff;
        uint64_t B = b & 0x000000ffffffffff;
        trie = A > B ? A : B;
    }

    uint64_t value = atomic_load_explicit(&dbs->first[trie], memory_order_acquire);
    if (value == 0) {
        if (atomic_compare_exchange_strong_explicit(
                &dbs->first[trie], &value, masked_hash | d_idx,
                memory_order_acq_rel, memory_order_acquire)) return 1;
    }

    for (;;) {
        uint64_t n_idx = trie*2 + (hash&1);
        hash = ror64(hash, 1);
        value = atomic_load_explicit(&dbs->next[n_idx], memory_order_acquire);
        if (value == 0) {
            if (atomic_compare_exchange_strong_explicit(
                    &dbs->next[n_idx], &value, masked_hash | d_idx,
                    memory_order_acq_rel, memory_order_acquire)) return 1;
        }
        trie = value&0x000000ffffffffff;
    }
}

/**
 * NOTE: the failures below still exit the process instead of returning NULL.
 * Reporting them means unwinding a partially allocated table in every
 * backend; part of the outstanding exit(1) cleanup. sylvan_init_package()
 * already handles a NULL return, so only this function has to change.
 */
nodes_table* nodes_create(size_t initial_size, size_t max_size)
{
    nodes_table* dbs = sylvan_alloc_aligned(sizeof(struct nodes_table));
    if (dbs == 0) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

    if (initial_size > max_size) {
        fprintf(stderr, "nodes_create: initial_size > max_size!\n");
        exit(1);
    }

    if (max_size > UINT64_C(0x0000010000000000)) {
        fprintf(stderr, "nodes_create: max_size exceeds the 40-bit index!\n");
        exit(1);
    }

    // minimum size is 512 buckets (region size)

    if (initial_size < 512) {
        fprintf(stderr, "nodes_create: initial_size too small!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    nodes_set_size(dbs, initial_size);

    dbs->data = (uint8_t*)sylvan_alloc_aligned(dbs->max_size * 16);
    dbs->first = (_Atomic(uint64_t)*) sylvan_alloc_aligned(dbs->max_size * 8);
    dbs->next = (_Atomic(uint64_t)*) sylvan_alloc_aligned(dbs->max_size * 16);

    /* Allocate bitmaps. Each region is 64*8 = 512 buckets.
       Overhead of bitmap1: 1 bit per 4096 bucket.
       Overhead of bitmap2: 1 bit per bucket.
       Overhead of bitmapc: 1 bit per bucket. */

    dbs->bitmap1 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(region_bitmap_size(dbs->max_size));
    dbs->bitmap2 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(node_bitmap_size(dbs->max_size));
    dbs->bitmapc = (_Atomic(uint64_t)*)sylvan_alloc_aligned(node_bitmap_size(dbs->max_size));
    dbs->region_cursor = (_Atomic(uint64_t)*)sylvan_alloc_aligned(SYLVAN_CACHE_LINE_SIZE);
    dbs->batch = (_Atomic(uint64_t)*)sylvan_alloc_aligned(SYLVAN_CACHE_LINE_SIZE);

    if (dbs->data == 0 || dbs->first == 0 || dbs->next == 0 || dbs->bitmap1 == 0 || dbs->bitmap2 == 0 || dbs->bitmapc == 0 || dbs->region_cursor == 0 || dbs->batch == 0) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);

    dbs->hash_cb = NULL;
    dbs->equals_cb = NULL;
    dbs->create_cb = NULL;
    dbs->destroy_cb = NULL;
    atomic_init(&dbs->reorder_lock, 0u);
    dbs->region = NULL;
    dbs->region_size = 0;
    dbs->codes = NULL;

    // this design assumes there is only one nodes table...

    nodes_reset_region_TOGETHER();

    // initialize hashtab; it fills a static table and cannot fail
    (void)sylvan_init_hash();

    return dbs;
}

void nodes_free(nodes_table* dbs)
{
    sylvan_free_aligned((void*)dbs->data, dbs->max_size * 16);
    sylvan_free_aligned((void*)dbs->first, dbs->max_size * 8);
    sylvan_free_aligned((void*)dbs->next, dbs->max_size * 16);
    sylvan_free_aligned((void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_free_aligned((void*)dbs->bitmap2, node_bitmap_size(dbs->max_size));
    sylvan_free_aligned((void*)dbs->bitmapc, node_bitmap_size(dbs->max_size));
    if (dbs->region != NULL) {
        sylvan_free_aligned(dbs->region, dbs->region_size * sizeof(uint64_t));
    }
    sylvan_free_aligned((void*)dbs->region_cursor, SYLVAN_CACHE_LINE_SIZE);
    sylvan_free_aligned((void*)dbs->batch, SYLVAN_CACHE_LINE_SIZE);
    sylvan_free_aligned(dbs, sizeof(struct nodes_table));
}

void nodes_clear_CALL(lace_worker* lace, nodes_table* dbs)
{
    (void)lace;
    sylvan_clear_aligned((void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_clear_aligned((void*)dbs->bitmap2, node_bitmap_size(dbs->max_size));
    sylvan_clear_aligned((void*)dbs->first, dbs->max_size * 8);
    sylvan_clear_aligned((void*)dbs->next, dbs->max_size * 16);

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);

    atomic_store_explicit(dbs->region_cursor, 0, memory_order_relaxed);
    nodes_reset_region_TOGETHER();

    /* A collection has given the table its room back, so the next
       exhaustion is a new episode and reports itself again. */
    atomic_store_explicit(
        &arena_exhausted_reported, 0, memory_order_relaxed);
}

int nodes_is_marked(const nodes_table* dbs, uint64_t index)
{
    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    return (atomic_load_explicit(ptr, memory_order_relaxed) & mask) ? 1 : 0;
}

void nodes_mark_rec_CALL(lace_worker* lace, const nodes_table* dbs, uint64_t index)
{
    if (index == 0 || index == 1) return; // reserved for true/false

    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
    for (;;) {
        if (v & mask) return;
        if (atomic_compare_exchange_weak_explicit(
                ptr, &v, v|mask,
                memory_order_relaxed, memory_order_relaxed)) {
            const uint64_t * const d_ptr = ((uint64_t*)dbs->data) + 2*index;
            const uint64_t a = d_ptr[0];
            const uint64_t b = d_ptr[1];
            if (a & 0x4000000000000000) return; // leaf node
            nodes_mark_rec_SPAWN(lace, dbs, b & 0x000000ffffffffff);
            nodes_mark_rec_CALL(lace, dbs, a & 0x000000ffffffffff);
            nodes_mark_rec_SYNC(lace);
            return;
        }
    }
}

/**
 * Reinsert every marked bucket in the given range of mark words.
 * Scanning mark words avoids walking every empty bucket.
 */
static int nodes_rebuild_words(nodes_table* dbs, size_t first_word, size_t word_count)
{
    int bad = 0;
    for (size_t word = first_word; word < first_word + word_count; word++) {
        uint64_t marked = atomic_load_explicit(dbs->bitmap2 + word, memory_order_relaxed);
        if (word == 0) marked &= UINT64_C(0x3fffffffffffffff); // skip reserved indices 0 and 1
        while (marked != 0) {
            const unsigned int bit = clz_uint64(marked);
            const size_t index = word * 64 + bit;
            if (index < dbs->table_size && nodes_reinsert_bucket(dbs, index) == 0) bad++;
            marked &= ~(UINT64_C(0x8000000000000000) >> bit);
        }
    }
    return bad;
}

TASK(int, nodes_rebuild_par, nodes_table*, dbs, size_t, first_word, size_t, word_count)

int nodes_rebuild_par_CALL(lace_worker* lace, nodes_table* dbs, size_t first_word, size_t word_count)
{
    if (word_count > 8) { // 8 mark words = 512 buckets
        const size_t split = word_count / 2;
        nodes_rebuild_par_SPAWN(lace, dbs, first_word, split);
        const int bad = nodes_rebuild_par_CALL(lace, dbs, first_word + split, word_count - split);
        return bad + nodes_rebuild_par_SYNC(lace);
    } else {
        return nodes_rebuild_words(dbs, first_word, word_count);
    }
}

/**
 * Reinsert all marked buckets. This runs concurrently; see the comment on
 * nodes_reinsert_bucket for why concurrent reconstruction keeps every marked
 * canonical node reachable at its original index.
 */
int nodes_rebuild_CALL(lace_worker* lace, nodes_table* dbs)
{
    return nodes_rebuild_par_CALL(lace, dbs, 0, (dbs->table_size + 63) / 64);
}

TASK(size_t, nodes_count_nodes_par, nodes_table*, dbs, size_t, first, size_t, count)

size_t nodes_count_nodes_par_CALL(lace_worker* lace, nodes_table* dbs, size_t first, size_t count)
{
    if (count > 512) {
        size_t split = count/2;
        nodes_count_nodes_par_SPAWN(lace, dbs, first, split);
        size_t right = nodes_count_nodes_par_CALL(lace, dbs, first + split, count - split);
        size_t left = nodes_count_nodes_par_SYNC(lace);
        return left + right;
    } else {
        size_t result = 0;
        _Atomic(uint64_t)* ptr = dbs->bitmap2 + (first / 64);
        if (count == 512 && (first & 63) == 0) {
            result += popcnt_uint64(atomic_load_explicit(ptr+0, memory_order_relaxed));
            result += popcnt_uint64(atomic_load_explicit(ptr+1, memory_order_relaxed));
            result += popcnt_uint64(atomic_load_explicit(ptr+2, memory_order_relaxed));
            result += popcnt_uint64(atomic_load_explicit(ptr+3, memory_order_relaxed));
            result += popcnt_uint64(atomic_load_explicit(ptr+4, memory_order_relaxed));
            result += popcnt_uint64(atomic_load_explicit(ptr+5, memory_order_relaxed));
            result += popcnt_uint64(atomic_load_explicit(ptr+6, memory_order_relaxed));
            result += popcnt_uint64(atomic_load_explicit(ptr+7, memory_order_relaxed));
        } else {
            uint64_t mask = UINT64_C(0x8000000000000000) >> (first & 63);
            for (size_t k=0; k<count; k++) {
                if (atomic_load_explicit(ptr, memory_order_relaxed) & mask) result += 1;
                mask >>= 1;
                if (mask == 0) {
                    ptr++;
                    mask = UINT64_C(0x8000000000000000);
                }
            }
        }
        return result;
    }
}

size_t nodes_count_nodes_CALL(lace_worker* lace, nodes_table* dbs)
{
    return nodes_count_nodes_par_CALL(lace, dbs, 0, dbs->table_size);
}

TASK(void, nodes_destroy_par, nodes_table*, dbs, size_t, first_word, size_t, word_count)

void nodes_destroy_par_CALL(lace_worker* lace, nodes_table* dbs, size_t first_word, size_t word_count)
{
    if (word_count > 16) {
        size_t split = word_count/2;
        nodes_destroy_par_SPAWN(lace, dbs, first_word, split);
        nodes_destroy_par_CALL(lace, dbs, first_word + split, word_count - split);
        nodes_destroy_par_SYNC(lace);
    } else {
        for (size_t word=first_word; word<first_word+word_count; word++) {
            const uint64_t marked = atomic_load_explicit(dbs->bitmap2 + word, memory_order_relaxed);
            const uint64_t custom = atomic_load_explicit(
                dbs->bitmapc + word, memory_order_relaxed);
            uint64_t dead = custom & ~marked;

            while (dead != 0) {
                const unsigned int bit = clz_uint64(dead);
                const size_t index = word * 64 + bit;
                const uint64_t mask = UINT64_C(0x8000000000000000) >> bit;
                if (index < dbs->table_size && dbs->destroy_cb != NULL) {
                    uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*index;
                    dbs->destroy_cb(d_ptr[0], d_ptr[1]);
                }
                dead &= ~mask;
            }

            atomic_store_explicit(
                dbs->bitmapc + word, custom & marked, memory_order_relaxed);
        }
    }
}

void nodes_cleanup_custom_CALL(lace_worker* lace, nodes_table* dbs)
{
    nodes_destroy_par_CALL(lace, dbs, 0, (dbs->table_size + 63) / 64);
}

/**
 * This backend never requests maintenance: every insertion updates the index
 * in place and only a collection rebuilds it, so there is nothing to
 * reorganize between collections.
 */
void nodes_maintain_CALL(lace_worker* lace, nodes_table* dbs)
{
    (void)lace;
    (void)dbs;
}

/**
 * Set custom functions
 */
void nodes_set_custom(nodes_table* dbs, nodes_hash_cb hash_cb, nodes_equals_cb equals_cb, nodes_create_cb create_cb, nodes_destroy_cb destroy_cb)
{
    dbs->hash_cb = hash_cb;
    dbs->equals_cb = equals_cb;
    dbs->create_cb = create_cb;
    dbs->destroy_cb = destroy_cb;
}

/**
 * Reorder primitives.
 *
 * SERIAL, ONE NODE AT A TIME. nodes_remove and nodes_reinsert both take a
 * per-table spin lock, so they are serialised against each other across all
 * workers. The contract in <sylvan/nodes.h> only asks that the session's
 * workers may call them at the same time; serialising them is how this backend
 * meets it, and it is not a shortcut for removal alone:
 *
 *  - removal below rewrites several slots that are not related by any single
 *    compare-and-swap, and it breaks the write-once property of the slots,
 *    which is exactly the property the parallel rebuild proof on
 *    nodes_reinsert_bucket rests on. That proof therefore does not extend to
 *    a table that is being removed from;
 *  - reinsertion must be serialised with removal for the same reason. A
 *    descent that publishes into a slot which a concurrent removal is about
 *    to clear, or into a branch pair it is about to detach, loses the node.
 *
 * PARALLEL, A PHASE AT A TIME. What the lock cannot give back is scaling: a
 * swap removes and re-indexes thousands of nodes, and a caller that issues
 * them one at a time from every worker gets one worker's throughput. The way
 * out is not a finer lock but a wider call. nodes_remove_batch and
 * nodes_reinsert_batch take the whole set at once and run the phases of the
 * work in parallel with barriers between them; see the block above
 * nodes_remove_batch for the design and its correctness argument. The
 * single-node entry points keep the lock, so a caller that cannot batch -- a
 * single-worker sift, the table's unit tests -- gets them exactly as above.
 *
 * WHY NOT LEAF PROMOTION. The obvious removal -- pull a leaf out of the
 * subtree below the removed node and give it the removed node's branch pair
 * -- does not work for this trie, for two structural reasons that the slot
 * layout makes unavoidable:
 *
 *  1. The depth-1 branch pair of the family anchored at A is next[A*2+0..1],
 *     not the pair of whoever occupies first[A] (nodes_lookup2 only assigns
 *     the occupant to <trie> after the root comparison). So the occupant of
 *     first[A] owns no branch pair in its own family, and there is no pair to
 *     hand over; clearing first[A] instead would stop every descent into the
 *     family at its root.
 *  2. A node X's pair next[X*2+0..1] is shared between the family anchored at
 *     X and the family that routes through X at depth one or deeper -- they
 *     are literally the same two words. Moving that pair onto a promoted leaf
 *     repairs the second family and silently detaches the first, whose
 *     descent goes first[X] -> next[X*2+b] and would now find those slots
 *     empty and insert duplicates.
 *
 * WHAT IT DOES INSTEAD. Removal clears the node's slot and locally rebuilds
 * whatever depended on it:
 *
 *  - Determine the region whose descents run through the vanishing slot. For
 *    a node at depth one or deeper, that is the subtree hanging off its own
 *    branch pair. For a node occupying first[A], it is everything below the
 *    anchor's pair, because an empty family root truncates the whole family.
 *  - Collect that region (breadth first over occupants' pairs), then clear
 *    its slots and the node's own slot, then reinsert every collected node
 *    with the ordinary descent, which puts each of them back on its own probe
 *    sequence.
 *
 * That this is exactly the right region is the invariant to hold on to: a
 * node whose slot is not in the region cannot have the region on its own
 * probe sequence, because a sequence that entered the region and continued to
 * that node's slot would have made the slot reachable from the region roots,
 * hence part of the region. Nodes outside the region are therefore untouched
 * and stay reachable, and nodes inside it are all reinserted.
 *
 * Cost is the size of that region, not of the table. It is O(1) for a node
 * with an empty branch pair, and largest when the node occupies the root of a
 * big family -- the leaf family, anchored at handle 1, being the extreme
 * case. Retirement in a reorder session removes decision and map nodes, never
 * leaves.
 */

#define MASK_INDEX UINT64_C(0x000000ffffffffff)
#define MASK_HASH  UINT64_C(0xffffff0000000000)

uint32_t
nodes_reorder_caps(const nodes_table* dbs)
{
    (void)dbs;
    return NODES_CAP_REMOVE | NODES_CAP_RELEASE | NODES_CAP_BATCH;
}

static void
reorder_lock(nodes_table* dbs)
{
    for (;;) {
        uint32_t expected = 0;
        if (atomic_compare_exchange_weak_explicit(
                &dbs->reorder_lock, &expected, 1u,
                memory_order_acquire, memory_order_relaxed)) {
            return;
        }
        while (atomic_load_explicit(
                   &dbs->reorder_lock, memory_order_relaxed) != 0u) {
        }
    }
}

static void
reorder_unlock(nodes_table* dbs)
{
    atomic_store_explicit(&dbs->reorder_lock, 0u, memory_order_release);
}

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

/** The key of a node, as the index derives it from the record. */
struct reorder_key {
    uint64_t a;
    uint64_t b;
    uint64_t hash;
    uint64_t anchor;
    int custom;
};

static struct reorder_key
reorder_key_of(const nodes_table* dbs, uint64_t index)
{
    const uint64_t* d_ptr = ((const uint64_t*)dbs->data) + 2 * index;
    struct reorder_key key;
    key.a = d_ptr[0];
    key.b = d_ptr[1];
    key.custom = is_custom_bucket(dbs, index);
    key.hash = 14695981039346656037LLU;
    key.hash = key.custom ? dbs->hash_cb(key.a, key.b, key.hash)
                          : sylvan_tabhash16(key.a, key.b, key.hash);
    if (key.a & UINT64_C(0x4000000000000000)) {
        key.anchor = 1;
    } else {
        const uint64_t A = key.a & MASK_INDEX;
        const uint64_t B = key.b & MASK_INDEX;
        key.anchor = A > B ? A : B;
    }
    return key;
}

/**
 * Find the slot that holds <index>, following its own probe sequence.
 * Sets *owner to the node whose branch pair roots the region that depends on
 * that slot: the anchor when the node sits at the root of its family (whose
 * depth-1 pair belongs to the anchor), the node itself otherwise.
 */
static _Atomic(uint64_t)*
reorder_find_slot(nodes_table* dbs, uint64_t index,
                  const struct reorder_key* key, uint64_t* owner)
{
    uint64_t trie = key->anchor;
    uint64_t hash = key->hash;
    sylvan_stats_count(LLMSSET_REMOVE_CALLS);

    _Atomic(uint64_t)* slot = dbs->first + trie;
    uint64_t value = atomic_load_explicit(slot, memory_order_relaxed);
    if (value == 0) return NULL;
    if ((value & MASK_INDEX) == index) {
        *owner = key->anchor;
        return slot;
    }

    for (;;) {
        const uint64_t branch = trie * 2 + (hash & 1);
        hash = ror64(hash, 1);
        slot = dbs->next + branch;
        sylvan_stats_count(LLMSSET_REMOVE);
        value = atomic_load_explicit(slot, memory_order_relaxed);
        if (value == 0) return NULL;
        if ((value & MASK_INDEX) == index) {
            *owner = index;
            return slot;
        }
        trie = value & MASK_INDEX;
    }
}

/**
 * The buffer a removal collects its region into.
 *
 * Normally the table's own scratch, which nodes_prepare_reorder sized so that
 * it cannot run out; <owned> then stays zero and nothing is allocated or
 * freed. A caller that removes without preparing -- the table's unit tests --
 * gets a private buffer instead, which grows on demand, counts as a late
 * allocation, and may report out of memory; that is still safe, because
 * collecting a region is read-only and nothing has changed when it fails.
 */
struct reorder_region {
    uint64_t* nodes;
    size_t count;
    size_t capacity;
    int owned;
};

static int
region_push(struct reorder_region* region, uint64_t index)
{
    if (region->count == region->capacity) {
        nodes_note_late_allocation();
        const size_t capacity =
            region->capacity == 0 ? 64u : region->capacity * 2u;
        uint64_t* nodes = (uint64_t*)malloc(capacity * sizeof(uint64_t));
        if (nodes == NULL) return -1;
        if (region->count != 0) {
            memcpy(nodes, region->nodes, region->count * sizeof(uint64_t));
        }
        if (region->owned) free(region->nodes);
        region->nodes = nodes;
        region->capacity = capacity;
        region->owned = 1;
    }
    region->nodes[region->count++] = index;
    return 0;
}

/**
 * Collect every node reachable from <owner>'s branch pair, breadth first.
 * Read-only: on failure the table is untouched, which is what lets removal
 * report out of memory without having changed anything.
 */
static int
reorder_collect_region(const nodes_table* dbs, uint64_t owner,
                       struct reorder_region* region)
{
    for (unsigned int b = 0; b < 2; b++) {
        const uint64_t value = atomic_load_explicit(
            dbs->next + owner * 2 + b, memory_order_relaxed);
        if (value != 0 && region_push(region, value & MASK_INDEX)) return -1;
    }
    for (size_t i = 0; i < region->count; i++) {
        const uint64_t node = region->nodes[i];
        for (unsigned int b = 0; b < 2; b++) {
            const uint64_t value = atomic_load_explicit(
                dbs->next + node * 2 + b, memory_order_relaxed);
            if (value != 0 && region_push(region, value & MASK_INDEX)) {
                return -1;
            }
        }
        assert(region->count <= dbs->table_size);
    }
    return 0;
}

int
nodes_remove(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    const struct reorder_key key = reorder_key_of(dbs, index);
    struct reorder_region region = { NULL, 0, 0, 0 };
    int status = SYLVAN_OK;

    reorder_lock(dbs);

    /* Under the lock, so that one shared scratch serves every remover. */
    region.nodes = dbs->region;
    region.capacity = dbs->region_size;

    uint64_t owner = 0;
    _Atomic(uint64_t)* slot = reorder_find_slot(dbs, index, &key, &owner);
    if (slot == NULL) {
        assert(!"nodes_remove: node is not in its own family");
        status = SYLVAN_ERR_INVALID;
        goto done;
    }
    /* A node at the root of its family owns no branch pair there, so the
       region is rooted at the anchor's pair; otherwise at its own. */
    assert(owner == index || owner == key.anchor);

    if (reorder_collect_region(dbs, owner, &region) != 0) {
        status = SYLVAN_ERR_OOM;
        goto done;
    }

    for (unsigned int b = 0; b < 2; b++) {
        atomic_store_explicit(
            dbs->next + owner * 2 + b, 0, memory_order_relaxed);
    }
    for (size_t i = 0; i < region.count; i++) {
        for (unsigned int b = 0; b < 2; b++) {
            atomic_store_explicit(
                dbs->next + region.nodes[i] * 2 + b, 0,
                memory_order_relaxed);
        }
    }
    atomic_store_explicit(slot, 0, memory_order_relaxed);

    for (size_t i = 0; i < region.count; i++) {
        assert(region.nodes[i] != index);
        nodes_reinsert_bucket(dbs, region.nodes[i]);
    }

done:
    reorder_unlock(dbs);
    if (region.owned) free(region.nodes);
    return status;
}

/**
 * Whether the node already in <value> stops the reinsertion of <index>: it is
 * <index> itself, which is a caller error, or a different node with the same
 * key, which is a broken unique table and does not return.
 */
static int
trie_reinsert_conflict(const nodes_table* dbs, const struct reorder_key* key,
                       uint64_t index, uint64_t value)
{
    const uint64_t occupant = value & MASK_INDEX;
    if (occupant == index) {
        assert(!"nodes_reinsert: the node is already in the index");
        return 1;
    }
    if ((value & MASK_HASH) != (key->hash & MASK_HASH)) return 0;

    const uint64_t* d_ptr = ((const uint64_t*)dbs->data) + 2 * occupant;
    const int equal = key->custom
        ? dbs->equals_cb(key->a, key->b, d_ptr[0], d_ptr[1])
        : (d_ptr[0] == key->a && d_ptr[1] == key->b);
    if (equal) reorder_duplicate_abort(index, occupant);
    return 0;
}

/**
 * Publish <index> on its own probe sequence, aborting if an equal node is
 * already there.
 *
 * This is nodes_reinsert_bucket with the equality test of nodes_lookup2 added,
 * and it is written the same way -- every store a compare-and-swap against
 * zero, every load an acquire, and the family root handled before the loop --
 * so that the write-once argument on nodes_reinsert_bucket covers it verbatim.
 * Under the single-node lock that costs one uncontended compare-and-swap over
 * the plain store it replaces; in a batch it is what lets every worker descend
 * at once.
 *
 * Keeping the root outside the loop is not cosmetic. The branch pair consumed
 * after the family root belongs to the anchor and not to the root's occupant,
 * exactly as in nodes_lookup2, and <trie> may only start following occupants
 * from the second slot on. Inside the loop the value that addresses the next
 * branch is always the one just read, which after a lost compare-and-swap is
 * the winner's link and not the zero that was there before it.
 */
static int
trie_reinsert(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    const struct reorder_key key = reorder_key_of(dbs, index);
    const uint64_t link = (key.hash & MASK_HASH) | index;
    uint64_t hash = key.hash;
    uint64_t trie = key.anchor;
    sylvan_stats_count(LLMSSET_REINSERT_CALLS);

    _Atomic(uint64_t)* slot = dbs->first + trie;
    uint64_t value = atomic_load_explicit(slot, memory_order_acquire);
    if (value == 0) {
        if (atomic_compare_exchange_strong_explicit(
                slot, &value, link,
                memory_order_acq_rel, memory_order_acquire)) {
            return SYLVAN_OK;
        }
    }
    if (trie_reinsert_conflict(dbs, &key, index, value)) {
        return SYLVAN_ERR_INVALID;
    }

    for (;;) {
        const uint64_t branch = trie * 2 + (hash & 1);
        hash = ror64(hash, 1);
        slot = dbs->next + branch;
        sylvan_stats_count(LLMSSET_REINSERT);
        value = atomic_load_explicit(slot, memory_order_acquire);
        if (value == 0) {
            if (atomic_compare_exchange_strong_explicit(
                    slot, &value, link,
                    memory_order_acq_rel, memory_order_acquire)) {
                return SYLVAN_OK;
            }
        }
        if (trie_reinsert_conflict(dbs, &key, index, value)) {
            return SYLVAN_ERR_INVALID;
        }
        trie = value & MASK_INDEX;
    }
}

int
nodes_reinsert(nodes_table* dbs, uint64_t index)
{
    reorder_lock(dbs);
    const int status = trie_reinsert(dbs, index);
    reorder_unlock(dbs);
    return status;
}

/**
 * ---------------------------------------------------------------------------
 * BATCHED REMOVAL AND REINSERTION
 *
 * A removal cannot be made to scale one node at a time, and the reason is in
 * the write set, not in the lock. Removing a node clears its slot and rebuilds
 * the region that hung below it, and the rebuild is an ordinary descent from
 * first[anchor] that publishes into the first empty slot it finds -- which may
 * be anywhere in the table. So a removal's write set is not confined to any
 * region, family or shard, and there is no lock domain to shard it into --
 * which is exactly the point on which sharded and optimistic locking both
 * fail.
 *
 * What does work is to widen the call instead of narrowing the lock. Every
 * caller of this backend already knows its whole removal set up front -- a
 * swap's case-B array, a retirement queue -- and once the set is known the work
 * splits into phases that are each internally parallel, with a barrier where
 * the lock used to be:
 *
 *   1. FIND      read-only. Walk each victim's own probe sequence to the slot
 *                that holds it, and record that slot. Also records which node's
 *                branch pair roots the region that depends on the slot: the
 *                anchor if the victim occupies the family root, the victim
 *                itself otherwise -- the same distinction nodes_remove makes.
 *   2. UNLINK    store zero into every victim's slot.
 *   3. COLLECT   breadth first from the roots' branch pairs: read a branch,
 *                atomically exchange it for zero, and append the node it named
 *                to the region array. One round per level, all rounds
 *                internally parallel.
 *   4. REBUILD   reinsert every collected node with the ordinary write-once
 *                descent, all of them at once.
 *
 * Phase 3 both clears and collects, which is what makes the dedup free. A
 * node's link lives in exactly one slot, so exactly one exchange can take it
 * out non-zero, and only that worker appends it: overlapping regions -- one
 * victim inside another victim's region -- cost nothing and need no set. It is
 * also why victims are unlinked in phase 2 first: with their links already
 * gone, the collection cannot pick a victim up as a member and try to put it
 * back.
 *
 * WHY THE SHARED BRANCH PAIR IS NOT A HAZARD HERE. next[X*2+0..1] serves two
 * families at once -- the one anchored at X and the one that routes through X
 * at depth one or deeper -- so a removal's region crosses family boundaries.
 * That is exactly why the collection follows branch pairs rather than families:
 * the set it clears is CLOSED DOWNWARD ALONG PROBE PATHS. If a slot on some
 * node's probe path is cleared, so is every slot below it on that path, because
 * the node the cleared slot named was appended, and appending it is what makes
 * the collection walk its pair next. A node the batch did not collect therefore
 * has no hole above it and stays reachable where it is, whichever family the
 * hole would have belonged to; a node it did collect is reinserted. Taking the
 * union of several victims' regions preserves this, since a union of
 * downward-closed sets is downward closed.
 *
 * WHY THE PARALLEL REBUILD IS STILL SOUND. Phase 4 runs with no clearing left
 * to do, so every store is again a compare-and-swap against zero and the
 * write-once argument on nodes_reinsert_bucket applies unchanged -- points 1
 * and 2 of it verbatim. Its point 3, that the walk terminates, needs one
 * addition, because unlike the rebuild after a collection this one starts from
 * a table that is not empty: the links the batch did not clear are already
 * there. Call a node MOVING if the batch cleared its link and is putting it
 * back, and STATIC otherwise. Then:
 *
 *  a. A cycle of the "is stored in the branch pair of" relation cannot contain
 *     a static node. A moving node's pair was cleared (that is how it was
 *     collected), so every link now in its pair was published by this batch and
 *     names a moving node; walking a cycle forward from any moving node
 *     therefore only ever meets moving nodes, and a cycle of static nodes alone
 *     would be a cycle that already existed.
 *  b. A cycle of moving nodes cannot form either. Following the argument on
 *     nodes_reinsert_bucket, the last edge of such a cycle would have to be
 *     published by a descent that reached a node whose link was still absent,
 *     or else start from the anchor -- and the anchor case closes it: a moving
 *     node M is published below its own pair only if it descended into it, and
 *     the only way into next[M*2+..] while M's own link is absent is to have
 *     started at first[M], that is, to be a node of family M. So M's anchor
 *     would be a node whose anchor is M: each the other's larger child, which
 *     the forest cannot contain.
 *
 * FAILURE. Only phase 1 can fail, and it is read-only, so a batch that names a
 * node which is not in the index changes nothing at all and reports it -- in a
 * release build; a debug build asserts there first, as nodes_remove does.
 * After phase 1 there is nothing left that can go wrong: the region array is
 * sized to the table by nodes_prepare_reorder, and the nodes it holds are
 * distinct node indices, so it cannot overflow.
 *
 * The reinsertion batch is the same idea with only phase 4: nothing is cleared,
 * so every worker may descend at once, and the batch exists to spare the caller
 * a lock round trip per node rather than to make anything possible that was
 * not.
 * ---------------------------------------------------------------------------
 */

/** Granularity of the parallel phases of a batch; see REORDER_SWAP_BLOCK. */
#define TRIE_BATCH_BLOCK 64

/** Where a victim sits: the root of family <A>, or an index into next[]. */
#define TRIE_SLOT_ROOT    UINT64_C(0x8000000000000000)
#define TRIE_SLOT_MISSING UINT64_MAX

/** The two words of the batch line. */
#define TRIE_BATCH_CURSOR 0
#define TRIE_BATCH_ERROR  1

/**
 * FIND is not there for the contract. This backend has to know where every
 * victim sits before it can unlink anything, so it locates them all first and
 * discovers an absent one on the way. That makes its refusal stronger than the
 * contract asks for -- an in-range index that is not in the index is a caller
 * error the contract lets a backend abort on, and this one refuses cleanly
 * instead in a release build. Nothing may depend on that; see
 * nodes_remove_batch in nodes.h.
 */
enum trie_batch_phase {
    TRIE_BATCH_FIND = 0,
    TRIE_BATCH_UNLINK = 1,
    TRIE_BATCH_COLLECT = 2,
    TRIE_BATCH_REBUILD = 3,
    TRIE_BATCH_REINSERT = 4
};

/**
 * Take both branches of <node>'s pair out of the index and append what they
 * named. The exchange is what makes the append happen exactly once however
 * many workers arrive at the same pair; the load in front of it keeps the
 * common case -- an empty pair -- from turning into a read-modify-write.
 */
static void
trie_batch_collect(nodes_table* dbs, uint64_t node)
{
    for (unsigned int b = 0; b < 2; b++) {
        _Atomic(uint64_t)* slot = dbs->next + node * 2 + b;
        if (atomic_load_explicit(slot, memory_order_relaxed) == 0) continue;
        const uint64_t value =
            atomic_exchange_explicit(slot, 0, memory_order_relaxed);
        if (value == 0) continue;
        const uint64_t at = atomic_fetch_add_explicit(
            dbs->batch + TRIE_BATCH_CURSOR, 1, memory_order_relaxed);
        /* Distinct node indices, and the victims are not among them, so the
           region cannot reach the codes at the far end of the scratch. */
        assert(at < (uint64_t)(dbs->codes - dbs->region));
        dbs->region[at] = value & MASK_INDEX;
    }
}

/** The node whose pair roots the region a victim's slot code stands for. */
static inline uint64_t
trie_batch_owner(const nodes_table* dbs, const uint64_t* indices, size_t at)
{
    const uint64_t code = dbs->codes[at];
    return (code & TRIE_SLOT_ROOT) ? (code & MASK_INDEX) : indices[at];
}

TASK(void, trie_batch_victims, nodes_table*, dbs, int, phase,
     const uint64_t*, indices, size_t, first, size_t, count)

void
trie_batch_victims_CALL(lace_worker* lace, nodes_table* dbs, int phase,
                        const uint64_t* indices, size_t first, size_t count)
{
    if (count > TRIE_BATCH_BLOCK) {
        const size_t split = count / 2;
        trie_batch_victims_SPAWN(lace, dbs, phase, indices, first, split);
        trie_batch_victims_CALL(
            lace, dbs, phase, indices, first + split, count - split);
        trie_batch_victims_SYNC(lace);
        return;
    }

    const size_t end = first + count;
    for (size_t at = first; at < end; at++) {
        const uint64_t index = indices[at];

        switch (phase) {
        case TRIE_BATCH_FIND: {
            dbs->codes[at] = TRIE_SLOT_MISSING;
            if (index < 2 || index >= dbs->table_size) {
                atomic_store_explicit(
                    dbs->batch + TRIE_BATCH_ERROR, 1, memory_order_relaxed);
                break;
            }
            const struct reorder_key key = reorder_key_of(dbs, index);
            uint64_t owner = 0;
            _Atomic(uint64_t)* slot =
                reorder_find_slot(dbs, index, &key, &owner);
            if (slot == NULL) {
                assert(!"nodes_remove_batch: node is not in its own family");
                atomic_store_explicit(
                    dbs->batch + TRIE_BATCH_ERROR, 1, memory_order_relaxed);
                break;
            }
            /* A node is never its own anchor -- the anchor is its larger
               child -- so the two cases the code has to tell apart really are
               told apart by whether the owner is the node. */
            assert(key.anchor != index);
            dbs->codes[at] = owner == index
                ? (uint64_t)(slot - dbs->next)
                : (TRIE_SLOT_ROOT | owner);
            break;
        }

        case TRIE_BATCH_UNLINK: {
            const uint64_t code = dbs->codes[at];
            _Atomic(uint64_t)* slot = (code & TRIE_SLOT_ROOT)
                ? dbs->first + (code & MASK_INDEX)
                : dbs->next + code;
            atomic_store_explicit(slot, 0, memory_order_relaxed);
            break;
        }

        case TRIE_BATCH_COLLECT:
        default:
            trie_batch_collect(dbs, trie_batch_owner(dbs, indices, at));
            break;
        }
    }
}

TASK(void, trie_batch_members, nodes_table*, dbs, int, phase,
     size_t, first, size_t, count)

void
trie_batch_members_CALL(lace_worker* lace, nodes_table* dbs, int phase,
                        size_t first, size_t count)
{
    if (count > TRIE_BATCH_BLOCK) {
        const size_t split = count / 2;
        trie_batch_members_SPAWN(lace, dbs, phase, first, split);
        trie_batch_members_CALL(lace, dbs, phase, first + split, count - split);
        trie_batch_members_SYNC(lace);
        return;
    }

    const size_t end = first + count;
    for (size_t at = first; at < end; at++) {
        if (phase == TRIE_BATCH_COLLECT) {
            trie_batch_collect(dbs, dbs->region[at]);
        } else {
            nodes_reinsert_bucket(dbs, dbs->region[at]);
        }
    }
}

TASK(void, trie_batch_reinsert, nodes_table*, dbs, const uint64_t*, indices,
     size_t, first, size_t, count)

void
trie_batch_reinsert_CALL(lace_worker* lace, nodes_table* dbs,
                         const uint64_t* indices, size_t first, size_t count)
{
    if (count > TRIE_BATCH_BLOCK) {
        const size_t split = count / 2;
        trie_batch_reinsert_SPAWN(lace, dbs, indices, first, split);
        trie_batch_reinsert_CALL(lace, dbs, indices, first + split,
                                 count - split);
        trie_batch_reinsert_SYNC(lace);
        return;
    }

    const size_t end = first + count;
    for (size_t at = first; at < end; at++) {
        if (trie_reinsert(dbs, indices[at]) != SYLVAN_OK) {
            atomic_store_explicit(
                dbs->batch + TRIE_BATCH_ERROR, 1, memory_order_relaxed);
        }
    }
}

/**
 * Lay the batch out over the scratch: the victims' slot codes at the tail, the
 * collected region growing from the front. They cannot meet, because a victim
 * is never collected and both are node indices of the same table.
 */
static int
trie_batch_begin(nodes_table* dbs, size_t count)
{
    if (nodes_reorder_caps(dbs) == 0) return SYLVAN_ERR_INVALID;
    if (count > dbs->table_size) return SYLVAN_ERR_INVALID;
    if (dbs->region_size < dbs->table_size) {
        /* Only a caller that never prepared gets here: a session prepares
           before any swap that can remove a node, and nothing inside a session
           resizes the table. The note is how a test checks that. */
        nodes_note_late_allocation();
        const int status = nodes_prepare_reorder(dbs, NULL, 0);
        if (status != SYLVAN_OK) return status;
    }
    dbs->codes = dbs->region + dbs->region_size - count;
    atomic_store_explicit(
        dbs->batch + TRIE_BATCH_CURSOR, 0, memory_order_relaxed);
    atomic_store_explicit(
        dbs->batch + TRIE_BATCH_ERROR, 0, memory_order_relaxed);
    return SYLVAN_OK;
}

int
nodes_remove_batch_CALL(lace_worker* lace, nodes_table* dbs,
                        const uint64_t* indices, size_t count)
{
    if (count == 0) return SYLVAN_OK;

    reorder_lock(dbs);

    const int begun = trie_batch_begin(dbs, count);
    if (begun != SYLVAN_OK) {
        reorder_unlock(dbs);
        return begun;
    }

    trie_batch_victims_CALL(lace, dbs, TRIE_BATCH_FIND, indices, 0, count);
    if (atomic_load_explicit(
            dbs->batch + TRIE_BATCH_ERROR, memory_order_relaxed) != 0) {
        /* Phase one only read, so the index is exactly as it was. */
        reorder_unlock(dbs);
        return SYLVAN_ERR_INVALID;
    }

    trie_batch_victims_CALL(lace, dbs, TRIE_BATCH_UNLINK, indices, 0, count);
    trie_batch_victims_CALL(lace, dbs, TRIE_BATCH_COLLECT, indices, 0, count);

    /* One round per level of the collected regions; the rounds stop when a
       level adds nothing, and every round is itself parallel. */
    size_t done = 0;
    for (;;) {
        const size_t reached = (size_t)atomic_load_explicit(
            dbs->batch + TRIE_BATCH_CURSOR, memory_order_relaxed);
        if (done == reached) break;
        trie_batch_members_CALL(
            lace, dbs, TRIE_BATCH_COLLECT, done, reached - done);
        done = reached;
    }

    trie_batch_members_CALL(lace, dbs, TRIE_BATCH_REBUILD, 0, done);

    reorder_unlock(dbs);
    return SYLVAN_OK;
}

int
nodes_reinsert_batch_CALL(lace_worker* lace, nodes_table* dbs,
                          const uint64_t* indices, size_t count)
{
    if (count == 0) return SYLVAN_OK;
    if (nodes_reorder_caps(dbs) == 0) return SYLVAN_ERR_INVALID;

    reorder_lock(dbs);
    atomic_store_explicit(
        dbs->batch + TRIE_BATCH_ERROR, 0, memory_order_relaxed);
    trie_batch_reinsert_CALL(lace, dbs, indices, 0, count);
    const int failed = atomic_load_explicit(
        dbs->batch + TRIE_BATCH_ERROR, memory_order_relaxed) != 0;
    reorder_unlock(dbs);

    return failed ? SYLVAN_ERR_INVALID : SYLVAN_OK;
}

/**
 * Return a removed slot to the allocator.
 *
 * The bitmap updates are read-modify-writes and not the plain stores the
 * allocation path uses: a worker that allocates owns the whole region its
 * slot lies in, but a worker that releases a retired node does not, so two
 * releases can land in the same 64-slot word.
 *
 * The custom bit has to go before the slot becomes claimable. A reused slot
 * that still carries it would be hashed and compared through the custom
 * callbacks, and the next nodes_cleanup_custom would destroy its payload a
 * second time.
 */
int
nodes_release(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    /* There is no per-node mark in this backend, so check the real thing:
       walk the node's own probe sequence and require that it is not there.
       The lock keeps that answer stable against a concurrent removal. */
    const struct reorder_key key = reorder_key_of(dbs, index);
    uint64_t owner = 0;
    reorder_lock(dbs);
    const int indexed = reorder_find_slot(dbs, index, &key, &owner) != NULL;
    reorder_unlock(dbs);
    if (indexed) {
        return SYLVAN_ERR_INVALID;
    }

    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63u);
    if (is_custom_bucket(dbs, index)) {
        if (dbs->destroy_cb != NULL) {
            const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
            dbs->destroy_cb(data[0], data[1]);
        }
        atomic_fetch_and_explicit(
            dbs->bitmapc + index / 64u, ~mask, memory_order_relaxed);
    }
    atomic_fetch_and_explicit(
        dbs->bitmap2 + index / 64u, ~mask, memory_order_release);
    return SYLVAN_OK;
}

int
nodes_enumerate_var(nodes_table* dbs, uint32_t variable,
                    nodes_visit_cb cb, void* context)
{
    (void)dbs; (void)variable; (void)cb; (void)context;
    return SYLVAN_ERR_INVALID;
}

/**
 * Preflight <count> further allocations. See <sylvan/nodes.h> for the exact
 * invariant; the short version is that this counts the free slots of the
 * regions no worker owns yet, and asks for one region per worker more than
 * the caller wants.
 *
 * A worker exhausts the region it owns before giving it back and claiming
 * another, and it can only claim a region nobody owns, so an owned region is
 * exactly one worker's current region and there are never more than there are
 * workers. Free slots inside those are not counted: they are reachable by one
 * worker only, and only until that worker moves on. The margin is what covers
 * them. Every counted region that gets claimed during the window is either
 * used up -- every one of its free slots became an allocation -- or is some
 * worker's current region when the window ends, and at most <workers> regions
 * can be in that second state. Reserving <count> plus a whole region per
 * worker therefore guarantees <count> allocations however the workers divide
 * them, which reserving <count> alone does not.
 *
 * The scan stops as soon as the target is reached, so a small reservation on
 * a table with room costs a handful of popcounts.
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

int
nodes_reserve(nodes_table* dbs, uint64_t count)
{
    if (count == 0) return SYLVAN_OK;
    if (nodes_reorder_caps(dbs) == 0) return SYLVAN_ERR_INVALID;
    /* One whole region per worker on top of what the caller asked for; see
       above for why the bound needs it. */
    const uint64_t margin = (uint64_t)lace_worker_count() * 512u;
    if (count > UINT64_MAX - margin) return SYLVAN_ERR_OOM;
    const uint64_t wanted = count + margin;
    return reserve_free_slots(dbs, wanted) >= wanted
        ? SYLVAN_OK : SYLVAN_ERR_OOM;
}

/**
 * Make the removal scratch big enough that no removal can outgrow it.
 *
 * There are no subtables here, so the routes are ignored; what this preflights
 * is the buffer nodes_remove collects a region into.
 *
 * THE BOUND IS STRUCTURAL, NOT MEASURED, and that is deliberate. Walking the
 * regions of the removals a caller is about to perform and taking the largest
 * would be read-only and cheap, and it would also be wrong: the answer does
 * not survive the removals themselves. Removing one node clears its region and
 * reinserts every node in it on its own probe sequence, and those sequences run
 * through whatever occupies the slots along them, so a removal reshapes the
 * regions of the nodes that are still to be removed. Insertions between the
 * preflight and the removals -- and a variable swap does re-index every node it
 * rewrote before it retires anything -- move them again. A measured bound would
 * hold at the moment it was taken and at no later one.
 *
 * What does hold at every moment is that a region is a set of distinct node
 * indices, so it can never hold more entries than the table has slots. That is
 * the same invariant reorder_collect_region already asserts. Sizing the scratch
 * to the table therefore needs no walk at all, and costs one comparison per
 * call after the first.
 *
 * It is not free: eight bytes per slot on top of the forty this backend
 * already keeps, held from the first preflight until the table is freed. It is
 * untouched memory, though, so what it actually costs is the pages the largest
 * region ever reached -- which is what a measured bound would have had to
 * allocate anyway.
 */
int
nodes_prepare_reorder(nodes_table* dbs, const uint32_t* routes, size_t count)
{
    (void)routes;
    (void)count;
    if (nodes_reorder_caps(dbs) == 0) return SYLVAN_ERR_INVALID;
    if (dbs->region_size >= dbs->table_size) return SYLVAN_OK;

    uint64_t* region = (uint64_t*)sylvan_alloc_aligned(
        dbs->table_size * sizeof(uint64_t));
    if (region == NULL) return SYLVAN_ERR_OOM;

    /* The scratch holds nothing between removals, so there is nothing to
       carry over from the smaller one. */
    if (dbs->region != NULL) {
        sylvan_free_aligned(dbs->region, dbs->region_size * sizeof(uint64_t));
    }
    dbs->region = region;
    dbs->region_size = dbs->table_size;
    return SYLVAN_OK;
}
