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
 * The set has support for stop-the-world garbage collection.
 * Methods nodes_clear and nodes_reinsert implement garbage collection.
 */

typedef struct nodes_table
{
    uint8_t*           data;         // array with the nodes
    _Atomic(uint64_t)* first;        // array with the root of the trie
    _Atomic(uint64_t)* next;         // array with the 'next' entries of the trie
    _Atomic(uint64_t)* bitmap1;      // ownership bitmap (per 512 buckets)
    _Atomic(uint64_t)* bitmap2;      // bitmap for "contains data"
    _Atomic(uint64_t)* bitmapc;      // bitmap for "use custom functions"
    size_t             max_size;     // maximum size of the nodes table (for resizing)
    size_t             table_size;   // current size of the nodes table
    nodes_hash_cb      hash_cb;      // custom hash function
    nodes_equals_cb    equals_cb;    // custom equals function
    nodes_create_cb    create_cb;    // custom create function
    nodes_destroy_cb   destroy_cb;   // custom destroy function
    _Atomic(uint32_t)  reorder_lock; // serialises the reorder primitives
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

/**
 * Try to claim a free region.
 * start_region is the last region we worked on; search proceeds round-robin.
 * Returns the claimed region index, or UINT64_MAX if none available.
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
        for (;;) {
            if (v & mask) break;

            if (atomic_compare_exchange_weak_explicit(
                    ptr, &v, v | mask,
                    memory_order_relaxed, memory_order_relaxed)) {
                return region;
            }
        }
    }

    return UINT64_MAX;
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

            uint64_t claimed = claim_next_region(dbs, my_region + 1u);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        } else {
            const uint64_t regions = dbs->table_size / (64u * 8u);
            uint64_t start_region = 0;
            lace_worker *worker = lace_get_worker();

            if (regions != 0 && worker != NULL) {
                start_region =
                    ((uint64_t)worker->worker * regions) /
                    (uint64_t)lace_worker_count();
            }

            uint64_t claimed = claim_next_region(dbs, start_region);
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

static inline uint64_t nodes_lookup2(const nodes_table* dbs, uint64_t a, uint64_t b, int* created, const int custom)
{
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
        if (created_idx == UINT64_MAX) return 0; // failed to claim a data bucket!!
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
        value = atomic_load_explicit(&dbs->next[n_idx], memory_order_acquire);
        if (value == 0) {
            if (created_idx == 0) {
                // Claim data bucket and write data
                created_idx = claim_data_bucket(dbs);
                if (created_idx == UINT64_MAX) return 0; // failed to claim a data bucket!!
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
 *    strictly decreases in the happens-before order. Sequences are finite and
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

    if (dbs->data == 0 || dbs->first == 0 || dbs->next == 0 || dbs->bitmap1 == 0 || dbs->bitmap2 == 0 || dbs->bitmapc == 0) {
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

    // this design assumes there is only one nodes table...

    nodes_reset_region_TOGETHER();

    // initialize hashtab
    sylvan_init_hash();

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

    nodes_reset_region_TOGETHER();
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
 * This backend never requests maintenance: its index is rebuilt in place on
 * every insertion, so there is nothing to reorganize between collections.
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
 * SERIAL. nodes_remove and nodes_reinsert both take a per-table spin lock, so
 * they are serialised against each other across all workers. That is the
 * documented contract for this backend, and it is not a temporary shortcut
 * for removal alone:
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
 * Retirement volume per swap is the depth-1 dead set, so the uncontended cost
 * of the lock is irrelevant; partitioning by root family is the obvious way
 * to get parallelism back if it ever matters.
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
    return NODES_CAP_REMOVE | NODES_CAP_RELEASE;
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
        value = atomic_load_explicit(slot, memory_order_relaxed);
        if (value == 0) return NULL;
        if ((value & MASK_INDEX) == index) {
            *owner = index;
            return slot;
        }
        trie = value & MASK_INDEX;
    }
}

struct reorder_region {
    uint64_t* nodes;
    size_t count;
    size_t capacity;
};

static int
region_push(struct reorder_region* region, uint64_t index)
{
    if (region->count == region->capacity) {
        const size_t capacity =
            region->capacity == 0 ? 64u : region->capacity * 2u;
        uint64_t* nodes = (uint64_t*)realloc(
            region->nodes, capacity * sizeof(uint64_t));
        if (nodes == NULL) return -1;
        region->nodes = nodes;
        region->capacity = capacity;
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
    struct reorder_region region = { NULL, 0, 0 };
    int status = SYLVAN_OK;

    reorder_lock(dbs);

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
    free(region.nodes);
    return status;
}

int
nodes_reinsert(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    const struct reorder_key key = reorder_key_of(dbs, index);
    const uint64_t link = (key.hash & MASK_HASH) | index;
    uint64_t hash = key.hash;
    uint64_t trie = key.anchor;

    reorder_lock(dbs);

    /* The descent mirrors nodes_lookup2 exactly, including that the branch
       pair consumed after the family root belongs to the anchor and not to
       the root's occupant: <trie> starts following occupants only from the
       second slot on. */
    _Atomic(uint64_t)* slot = dbs->first + trie;
    uint64_t value = atomic_load_explicit(slot, memory_order_relaxed);
    for (;;) {
        if (value == 0) {
            /* Nothing else can be descending while the lock is held, so a
               plain release store is enough to publish the node. */
            atomic_store_explicit(slot, link, memory_order_release);
            reorder_unlock(dbs);
            return SYLVAN_OK;
        }

        const uint64_t occupant = value & MASK_INDEX;
        if (occupant == index) {
            assert(!"nodes_reinsert: the node is already in the index");
            reorder_unlock(dbs);
            return SYLVAN_ERR_INVALID;
        }
        if ((value & MASK_HASH) == (key.hash & MASK_HASH)) {
            const uint64_t* d_ptr =
                ((const uint64_t*)dbs->data) + 2 * occupant;
            const int equal = key.custom
                ? dbs->equals_cb(key.a, key.b, d_ptr[0], d_ptr[1])
                : (d_ptr[0] == key.a && d_ptr[1] == key.b);
            if (equal) {
                reorder_unlock(dbs);
                reorder_duplicate_abort(index, occupant);
            }
        }

        const uint64_t branch = trie * 2 + (hash & 1);
        hash = ror64(hash, 1);
        slot = dbs->next + branch;
        value = atomic_load_explicit(slot, memory_order_relaxed);
        trie = value & MASK_INDEX;
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
        assert(!"nodes_release: the node is still in the index");
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
 * regions no worker owns yet, because a worker exhausts the region it owns
 * before claiming another one and can only claim an unowned region. Free
 * slots inside owned regions are therefore reachable by exactly one worker
 * and are not counted, which is what makes the answer conservative rather
 * than optimistic and removes the need for a per-worker margin.
 *
 * The scan stops as soon as <count> is reached, so a small reservation on a
 * table with room costs a handful of popcounts.
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
    return reserve_free_slots(dbs, count) >= count
        ? SYLVAN_OK : SYLVAN_ERR_OOM;
}
