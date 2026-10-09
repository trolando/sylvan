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

#include <assert.h>
#include <errno.h>  // for errno
#include <stdlib.h> // abort
#include <string.h> // memset

/**
 * Manage nodes in an array, using a list from the maximum child to ensure that
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
    _Atomic(uint64_t)* first;        // array with the root of the linked list
    _Atomic(uint64_t)* next;         // array with the 'next' entries of the linked list
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
    /* Set by a worker of the current batch when its index is out of range.
       The batch primitives are collective and one runs at a time, so one word
       for the whole set is enough. */
    _Atomic(int)       batch_error;
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
    return capacity * (2 * sizeof(uint64_t) + 2 * sizeof(uint64_t)) +
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
            "sylvan: list nodes table out of node slots, %zu buckets, none "
            "free to claim; nodes_lookup returns 0 and the operation that "
            "asked for a node fails. Raise the memory cap passed to "
            "sylvan_set_limits.\n",
            dbs->table_size);
}

static inline uint64_t nodes_lookup2(const nodes_table* dbs, uint64_t a, uint64_t b, int* created, const int custom)
{
    // Determine first bucket of the linked list
    uint64_t ll_idx;
    if (a & 0x4000000000000000) {
        ll_idx = 1; // it's a leaf
    } else {
        uint64_t A = a & 0x000000ffffffffff;
        uint64_t B = b & 0x000000ffffffffff;
        ll_idx = A > B ? A : B;
    }

    // Calculate the hash
    uint64_t hash = 14695981039346656037LLU;
    if (custom) hash = dbs->hash_cb(a, b, hash);
    else hash = sylvan_tabhash16(a, b, hash);
    uint64_t masked_hash = hash & 0xffffff0000000000;

    uint64_t created_idx = 0;

    // First see if it's in the first bucket
    // Use a LoadLoad barrier to avoid reordering reading d_ptr[0] and d_ptr[1]
    uint64_t value = atomic_load_explicit(&dbs->first[ll_idx], memory_order_acquire);
    if (value == 0) {
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
        if (atomic_compare_exchange_strong_explicit(
                &dbs->first[ll_idx], &value, masked_hash | created_idx,
                memory_order_acq_rel, memory_order_acquire)) {
            if (custom) set_custom_bucket(dbs, created_idx, custom);
            *created = 1;
            return created_idx;
        }
    }

    // The first bucket is already taken. Compare!
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

    // Th first bucket was not equal, so we continue down the list...
    for (;;) {
        ll_idx = value & 0x000000ffffffffff;
        value = atomic_load_explicit(&dbs->next[ll_idx], memory_order_acquire);
        if (value == 0) {
            // no parent yet, so doesn't exist.
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
                    &dbs->next[ll_idx], &value, masked_hash | created_idx,
                    memory_order_acq_rel, memory_order_acquire)) {
                if (custom) set_custom_bucket(dbs, created_idx, custom);
                *created = 1;
                return created_idx;
            }
        }
        // Already claimed, so compare...
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

int nodes_reinsert_bucket(nodes_table* dbs, uint64_t d_idx)
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

    uint64_t ll_idx;
    if (a & 0x4000000000000000) {
        ll_idx = 1; // it's a leaf
    } else {
        uint64_t A = a & 0x000000ffffffffff;
        uint64_t B = b & 0x000000ffffffffff;
        ll_idx = A > B ? A : B;
    }

    uint64_t value = atomic_load_explicit(&dbs->first[ll_idx], memory_order_relaxed); // can be relaxed
    if (value == 0) {
        if (atomic_compare_exchange_strong_explicit(
                &dbs->first[ll_idx], &value, masked_hash | d_idx,
                memory_order_relaxed, memory_order_relaxed)) return 1;
    }

    for (;;) {
        ll_idx = value & 0x000000ffffffffff;
        value = atomic_load_explicit(&dbs->next[ll_idx], memory_order_relaxed); // can be relaxed
        if (value == 0) {
            if (atomic_compare_exchange_strong_explicit(
                    &dbs->next[ll_idx], &value, masked_hash | d_idx,
                    memory_order_relaxed, memory_order_relaxed)) return 1;
        }
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
    dbs->next = (_Atomic(uint64_t)*) sylvan_alloc_aligned(dbs->max_size * 8);

    /* Allocate bitmaps. Each region is 64*8 = 512 buckets.
       Overhead of bitmap1: 1 bit per 4096 bucket.
       Overhead of bitmap2: 1 bit per bucket.
       Overhead of bitmapc: 1 bit per bucket. */

    dbs->bitmap1 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(region_bitmap_size(dbs->max_size));
    dbs->bitmap2 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(node_bitmap_size(dbs->max_size));
    dbs->bitmapc = (_Atomic(uint64_t)*)sylvan_alloc_aligned(node_bitmap_size(dbs->max_size));
    dbs->region_cursor = (_Atomic(uint64_t)*)sylvan_alloc_aligned(SYLVAN_CACHE_LINE_SIZE);

    if (dbs->data == 0 || dbs->first == 0 || dbs->next == 0 || dbs->bitmap1 == 0 || dbs->bitmap2 == 0 || dbs->bitmapc == 0 || dbs->region_cursor == 0) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);

    dbs->hash_cb = NULL;
    dbs->equals_cb = NULL;
    dbs->create_cb = NULL;
    dbs->destroy_cb = NULL;

    // this design assumes there is only one nodes table...
    nodes_reset_region_TOGETHER();

    // initialize hashtab; it fills a static table and cannot fail
    (void)sylvan_init_hash();

    return dbs;
}

void nodes_free(nodes_table* dbs)
{
    sylvan_free_aligned(dbs->data, dbs->max_size * 16);
    sylvan_free_aligned((void*)dbs->first, dbs->max_size * 8);
    sylvan_free_aligned((void*)dbs->next, dbs->max_size * 8);
    sylvan_free_aligned((void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_free_aligned((void*)dbs->bitmap2, node_bitmap_size(dbs->max_size));
    sylvan_free_aligned((void*)dbs->bitmapc, node_bitmap_size(dbs->max_size));
    sylvan_free_aligned((void*)dbs->region_cursor, SYLVAN_CACHE_LINE_SIZE);
    sylvan_free_aligned(dbs, sizeof(struct nodes_table));
}

void nodes_clear_CALL(lace_worker* lace, nodes_table* dbs)
{
    sylvan_clear_aligned((void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_clear_aligned((void*)dbs->bitmap2, node_bitmap_size(dbs->max_size));
    sylvan_clear_aligned((void*)dbs->first, dbs->max_size * 8);
    sylvan_clear_aligned((void*)dbs->next, dbs->max_size * 8);

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

TASK(int, nodes_rebuild_par, nodes_table*, dbs, size_t, first, size_t, count)
int nodes_rebuild_par_CALL(lace_worker* lace, nodes_table* dbs, size_t first, size_t count)
{
    if (count > 512) {
        nodes_rebuild_par_SPAWN(lace, dbs, first, count/2);
        int bad = nodes_rebuild_par_CALL(lace, dbs, first + count/2, count - count/2);
        return bad + nodes_rebuild_par_SYNC(lace);
    } else {
        int bad = 0;
        _Atomic(uint64_t)* ptr = dbs->bitmap2 + (first / 64);
        uint64_t mask = UINT64_C(0x8000000000000000) >> (first & 63);
        for (size_t k=0; k<count; k++) {
            if (first+k > 1 && atomic_load_explicit(ptr, memory_order_relaxed) & mask) {
                if (nodes_reinsert_bucket(dbs, first+k) == 0) bad++;
            }
            mask >>= 1;
            if (mask == 0) {
                ptr++;
                mask = UINT64_C(0x8000000000000000);
            }
        }
        return bad;
    }
}

int nodes_rebuild_CALL(lace_worker* lace, nodes_table* dbs)
{
    return nodes_rebuild_par_CALL(lace, dbs, 0, dbs->table_size);
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
 * The chain surgery is the same as in nodes_llcht: nodes_remove splices the
 * node out of the family anchored at its maximum child with compare-and-swap
 * behind a delete-in-progress link, and nodes_reinsert publishes the node's
 * own link before making the node reachable. See the comment on nodes_remove
 * in nodes_llcht.c for why the delete-in-progress link is needed, why the
 * walk cannot step past its own node or splice the wrong link, why the
 * retries terminate, and why a refusal gives the claimed link back.
 *
 * One deliberate difference from the lookup path: an ordinary insertion
 * appends at the tail of the family, but nodes_reinsert PREPENDS. Chain order
 * is irrelevant to a lookup, which scans the whole family, but it is what
 * makes concurrent removal sound. A remover's walk can be standing on a node
 * that another worker removes and reinserts underneath it; it then reads that
 * node's new link. Prepending means the new link is the old head, so the walk
 * jumps back to the front of the family and re-traverses it, and can never be
 * carried past the node it is looking for. Appending would put the reinserted
 * node at the tail with a zero link, and the walk would mistake that for the
 * end of the family and report the node it is removing as absent.
 */

#define MASK_INDEX UINT64_C(0x000000ffffffffff)
#define MASK_HASH  UINT64_C(0xffffff0000000000)

/* A link with an all-ones fingerprint and a zero index. No published link can
   look like this: a link names a node index >= 2, and the end of a family is
   exactly zero. */
#define LINK_REMOVING MASK_HASH

uint32_t
nodes_reorder_caps(const nodes_table* dbs)
{
    (void)dbs;
    return NODES_CAP_REMOVE | NODES_CAP_RELEASE | NODES_CAP_BATCH;
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

/** The family root of a node, plus its fingerprint, both from the record. */
static _Atomic(uint64_t)*
reorder_anchor(const nodes_table* dbs, uint64_t index, uint64_t* masked_hash)
{
    const uint64_t* d_ptr = ((const uint64_t*)dbs->data) + 2 * index;
    const uint64_t a = d_ptr[0];
    const uint64_t b = d_ptr[1];

    uint64_t hash = 14695981039346656037LLU;
    if (is_custom_bucket(dbs, index)) hash = dbs->hash_cb(a, b, hash);
    else hash = sylvan_tabhash16(a, b, hash);
    *masked_hash = hash & MASK_HASH;

    if (a & UINT64_C(0x4000000000000000)) return dbs->first + 1;
    const uint64_t A = a & MASK_INDEX;
    const uint64_t B = b & MASK_INDEX;
    return dbs->first + (A > B ? A : B);
}

int
nodes_remove(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    /* Claim the node and take its successor with it. The compare-and-swap
       retries because next[index] is not private: when the node behind this
       one is removed, its remover splices by writing this very link. */
    uint64_t successor;
    for (;;) {
        successor = atomic_load_explicit(
            dbs->next + index, memory_order_acquire);
        if (successor == LINK_REMOVING) return SYLVAN_ERR_INVALID;
        if (atomic_compare_exchange_weak_explicit(
                dbs->next + index, &successor, LINK_REMOVING,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }

    uint64_t masked_hash;
    _Atomic(uint64_t)* first = reorder_anchor(dbs, index, &masked_hash);

    for (;;) {
        uint64_t link = atomic_load_explicit(first, memory_order_acquire);
        if ((link & MASK_INDEX) == index) {
            if (atomic_compare_exchange_strong_explicit(
                    first, &link, successor,
                    memory_order_acq_rel, memory_order_acquire)) {
                return SYLVAN_OK;
            }
            continue;
        }

        for (;;) {
            const uint64_t previous = link & MASK_INDEX;
            if (previous == 0) {
                /* Not in its own family. Give the claimed link back before
                   refusing, as nodes_llcht.c does. Here it matters even for a
                   slot that never held a node: an insertion never writes the
                   link of the node it appends, so whatever claims the slot
                   next would inherit LINK_REMOVING (see nodes_release). */
                uint64_t claimed = LINK_REMOVING;
                (void)atomic_compare_exchange_strong_explicit(
                    dbs->next + index, &claimed, successor,
                    memory_order_acq_rel, memory_order_acquire);
                assert(!"nodes_remove: node is not in its own family");
                return SYLVAN_ERR_INVALID;
            }

            uint64_t value = atomic_load_explicit(
                dbs->next + previous, memory_order_acquire);
            if (value == LINK_REMOVING) break;
            if ((value & MASK_INDEX) == index) {
                if (atomic_compare_exchange_strong_explicit(
                        dbs->next + previous, &value, successor,
                        memory_order_acq_rel, memory_order_acquire)) {
                    return SYLVAN_OK;
                }
                break;
            }
            link = value;
        }
    }
}

/**
 * Walk a family looking for a node equal to the given key.
 * Returns the index of an equal node, 0 if there is none, or UINT64_MAX if
 * the walk met a removal in progress and has to be repeated.
 */
static uint64_t
reorder_find_equal(const nodes_table* dbs, uint64_t link,
                   uint64_t masked_hash, uint64_t a, uint64_t b, int custom)
{
    while ((link & MASK_INDEX) != 0) {
        const uint64_t index = link & MASK_INDEX;
        if ((link & MASK_HASH) == masked_hash) {
            const uint64_t* d_ptr = ((const uint64_t*)dbs->data) + 2 * index;
            if (custom) {
                if (dbs->equals_cb(a, b, d_ptr[0], d_ptr[1])) return index;
            } else if (d_ptr[0] == a && d_ptr[1] == b) {
                return index;
            }
        }
        link = atomic_load_explicit(dbs->next + index, memory_order_acquire);
        if (link == LINK_REMOVING) return UINT64_MAX;
    }
    return 0;
}

int
nodes_reinsert(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;
    if (atomic_load_explicit(dbs->next + index, memory_order_relaxed) !=
        LINK_REMOVING) {
        assert(!"nodes_reinsert: the node was not removed first");
        return SYLVAN_ERR_INVALID;
    }

    const uint64_t* d_ptr = ((const uint64_t*)dbs->data) + 2 * index;
    const uint64_t a = d_ptr[0];
    const uint64_t b = d_ptr[1];
    const int custom = is_custom_bucket(dbs, index);

    uint64_t masked_hash;
    _Atomic(uint64_t)* first = reorder_anchor(dbs, index, &masked_hash);
    const uint64_t link = masked_hash | index;

    uint64_t observed = atomic_load_explicit(first, memory_order_acquire);
    for (;;) {
        const uint64_t equal = reorder_find_equal(
            dbs, observed, masked_hash, a, b, custom);
        if (equal == UINT64_MAX) {
            observed = atomic_load_explicit(first, memory_order_acquire);
            continue;
        }
        if (equal != 0) reorder_duplicate_abort(index, equal);

        /* Publish this node's own link before the node itself: the release
           compare-and-swap on the family root orders the store in front of
           it, so a walker can never reach the node with a stale successor. */
        atomic_store_explicit(
            dbs->next + index, observed, memory_order_relaxed);
        uint64_t expected = observed;
        if (atomic_compare_exchange_strong_explicit(
                first, &expected, link,
                memory_order_acq_rel, memory_order_acquire)) {
            return SYLVAN_OK;
        }
        observed = expected;
    }
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
 *
 * So does the link. This backend appends at the tail of a family and never
 * writes the link of the node it is inserting, because a freshly claimed slot
 * has always had a zero link: nodes_create zeroes the array and a collection
 * rebuilds every family from scratch. Releasing a slot has to restore that,
 * or the next node to claim it would inherit the delete-in-progress link and
 * a family walk arriving at it would follow that into node zero. A refused
 * nodes_remove gives back the link it claimed for the same reason.
 */
int
nodes_release(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    /* The delete-in-progress link that nodes_remove leaves behind is exactly
       the evidence that this node is out of the index. */
    if (atomic_load_explicit(dbs->next + index, memory_order_acquire) !=
        LINK_REMOVING) {
        return SYLVAN_ERR_INVALID;
    }
    atomic_store_explicit(dbs->next + index, 0, memory_order_relaxed);

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
 * Nothing to preflight. The index is one family root and one link word per
 * slot, both allocated with the table, and removal, reinsertion and release
 * only ever write words that already exist. There are no subtables either, so
 * the routes say nothing this backend could act on.
 */
int
nodes_prepare_reorder(nodes_table* dbs, const uint32_t* routes, size_t count)
{
    (void)routes;
    (void)count;
    if (nodes_reorder_caps(dbs) == 0) return SYLVAN_ERR_INVALID;
    return SYLVAN_OK;
}

/**
 * Batched removal and reinsertion.
 *
 * There is no phase structure to win here: a removal is a compare-and-swap
 * splice out of one family and touches nothing else, and the single-node calls
 * are already safe from every worker at once -- that is what the chain surgery
 * above was built for. What the batch buys is the WORKERS at the call sites
 * that hand over a whole set, above all the retirement round, which otherwise
 * unhashes its nodes one at a time on one worker. So the loop is the design.
 *
 * The only thing the batch adds is the contract's refusal, and only the half
 * of it that is cheap: every index is range-checked before anything is
 * claimed, so a batch naming an impossible index changes nothing. An index
 * that is in range but not in the index is a caller error and aborts -- see
 * nodes_remove_batch in nodes.h for why the distinction is drawn there rather
 * than paid for with a read-only walk of every victim's family.
 */

#define LIST_BATCH_BLOCK 64

static void
reorder_batch_abort(uint64_t index)
{
    assert(!"nodes_remove_batch: a node in the batch was not in the index");
    fprintf(stderr,
            "sylvan: nodes_remove_batch: node %llu is in range but the index "
            "does not hold it; the caller's bookkeeping is wrong. Aborting.\n",
            (unsigned long long)index);
    abort();
}

/* The three passes share one shape: split until a block is small enough, then
   run the block. LIST_BATCH_BLOCK is the same granularity the trie uses. */

TASK(void, list_batch_range, nodes_table*, dbs, const uint64_t*, indices,
     size_t, first, size_t, count)

void
list_batch_range_CALL(lace_worker* lace, nodes_table* dbs,
                      const uint64_t* indices, size_t first, size_t count)
{
    if (count > LIST_BATCH_BLOCK) {
        const size_t split = count / 2;
        list_batch_range_SPAWN(lace, dbs, indices, first, split);
        list_batch_range_CALL(lace, dbs, indices, first + split, count - split);
        list_batch_range_SYNC(lace);
        return;
    }

    const size_t end = first + count;
    for (size_t at = first; at < end; at++) {
        if (indices[at] < 2 || indices[at] >= dbs->table_size) {
            atomic_store_explicit(&dbs->batch_error, 1, memory_order_relaxed);
        }
    }
}

TASK(void, list_batch_remove, nodes_table*, dbs, const uint64_t*, indices,
     size_t, first, size_t, count)

void
list_batch_remove_CALL(lace_worker* lace, nodes_table* dbs,
                       const uint64_t* indices, size_t first, size_t count)
{
    if (count > LIST_BATCH_BLOCK) {
        const size_t split = count / 2;
        list_batch_remove_SPAWN(lace, dbs, indices, first, split);
        list_batch_remove_CALL(lace, dbs, indices, first + split, count - split);
        list_batch_remove_SYNC(lace);
        return;
    }

    const size_t end = first + count;
    for (size_t at = first; at < end; at++) {
        /* The range pass already passed, so the only way this fails is the
           caller error the contract lets us abort on. Returning here instead
           would report a failure after some of the set had been claimed. */
        if (nodes_remove(dbs, indices[at]) != SYLVAN_OK) {
            reorder_batch_abort(indices[at]);
        }
    }
}

TASK(void, list_batch_reinsert, nodes_table*, dbs, const uint64_t*, indices,
     size_t, first, size_t, count)

void
list_batch_reinsert_CALL(lace_worker* lace, nodes_table* dbs,
                         const uint64_t* indices, size_t first, size_t count)
{
    if (count > LIST_BATCH_BLOCK) {
        const size_t split = count / 2;
        list_batch_reinsert_SPAWN(lace, dbs, indices, first, split);
        list_batch_reinsert_CALL(lace, dbs, indices, first + split,
                                 count - split);
        list_batch_reinsert_SYNC(lace);
        return;
    }

    const size_t end = first + count;
    for (size_t at = first; at < end; at++) {
        /* Reinsertion keeps the contract it always had: a node that was not
           removed first is reported, not aborted on, exactly as the trie's
           batch does -- in a release build; nodes_reinsert asserts on it in a
           debug build, as the trie's does. It is the one failure the caller
           can act on, because it says the node is still in the index rather
           than missing from it. */
        if (nodes_reinsert(dbs, indices[at]) != SYLVAN_OK) {
            atomic_store_explicit(&dbs->batch_error, 1, memory_order_relaxed);
        }
    }
}

int
nodes_remove_batch_CALL(lace_worker* lace, nodes_table* dbs,
                        const uint64_t* indices, size_t count)
{
    if (count == 0) return SYLVAN_OK;
    if (nodes_reorder_caps(dbs) == 0) return SYLVAN_ERR_INVALID;

    /* Read-only, so a refusal here has mutated nothing. */
    atomic_store_explicit(&dbs->batch_error, 0, memory_order_relaxed);
    list_batch_range_CALL(lace, dbs, indices, 0, count);
    if (atomic_load_explicit(&dbs->batch_error, memory_order_relaxed) != 0) {
        return SYLVAN_ERR_INVALID;
    }

    list_batch_remove_CALL(lace, dbs, indices, 0, count);
    return SYLVAN_OK;
}

int
nodes_reinsert_batch_CALL(lace_worker* lace, nodes_table* dbs,
                          const uint64_t* indices, size_t count)
{
    if (count == 0) return SYLVAN_OK;
    if (nodes_reorder_caps(dbs) == 0) return SYLVAN_ERR_INVALID;

    atomic_store_explicit(&dbs->batch_error, 0, memory_order_relaxed);
    list_batch_reinsert_CALL(lace, dbs, indices, 0, count);
    return atomic_load_explicit(&dbs->batch_error, memory_order_relaxed) != 0
        ? SYLVAN_ERR_INVALID : SYLVAN_OK;
}
