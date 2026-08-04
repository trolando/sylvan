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

#include <errno.h>  // for errno
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
