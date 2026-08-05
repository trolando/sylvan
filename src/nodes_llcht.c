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
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/**
 * Lockless chained hash table (set) to store 16-byte keys.
 * Each unique key is associated with a 40-bit number.
 *
 * The head array is indexed by the full key hash. Each node has one immutable
 * link containing the 24-bit hash fingerprint and 40-bit index of the next
 * node. A lookup can therefore reject most chain entries without loading the
 * corresponding 16-byte key.
 *
 * Nodes are prepended with one successful compare-and-swap on the chain head.
 * Links are never changed while lookups are running; garbage collection clears
 * and rebuilds all chains while lookups are stopped.
 *
 * This global control allocates one head and one node slot per table bucket,
 * so its load factor cannot exceed one. Per-identity subtables must separate
 * head capacity from the global node capacity to exercise denser chaining.
 *
 * WARNING: Like the other nodes tables, this implementation currently uses a
 * global thread-local allocation region. Do not use multiple tables.
 */

typedef struct nodes_table
{
    _Atomic(uint64_t)* heads;        // hash-chain heads
    _Atomic(uint64_t)* next;         // hash/index links
    uint8_t*           data;         // table with values
    _Atomic(uint64_t)* bitmap1;      // ownership bitmap (per 512 buckets)
    _Atomic(uint64_t)* bitmap2;      // bitmap for "contains data"
    _Atomic(uint64_t)* bitmapc;      // bitmap for "use custom functions"
    size_t             max_size;     // maximum number of heads and nodes
    size_t             table_size;   // current number of heads and nodes
#if LLMSSET_MASK
    size_t             mask;         // table_size-1
#endif
    nodes_hash_cb      hash_cb;      // custom hash function
    nodes_equals_cb    equals_cb;    // custom equals function
    nodes_create_cb    create_cb;    // custom create function
    nodes_destroy_cb   destroy_cb;   // custom destroy function
} nodes_table;

/* 40 bits for the index, 24 bits for the hash fingerprint. */
#define MASK_INDEX UINT64_C(0x000000ffffffffff)
#define MASK_HASH  UINT64_C(0xffffff0000000000)

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

void*
nodes_get_pointer(const nodes_table* dbs, size_t index)
{
    return dbs->data + index * 16;
}

size_t
nodes_get_max_size(const nodes_table* dbs)
{
    return dbs->max_size;
}

size_t
nodes_get_size(const nodes_table* dbs)
{
    return dbs->table_size;
}

size_t
nodes_get_capacity_bytes(size_t capacity)
{
    return capacity * (2 * sizeof(uint64_t) + 2 * sizeof(uint64_t)) +
           region_bitmap_size(capacity) + 2 * node_bitmap_size(capacity);
}

void
nodes_set_size(nodes_table* dbs, size_t size)
{
    /* Check bounds (don't be ridiculous). */
    if (size > 128 && size >= dbs->table_size && size <= dbs->max_size) {
        dbs->table_size = size;
#if LLMSSET_MASK
        /* When masking is enabled, size must be a power of two. */
        dbs->mask = dbs->table_size - 1;
#endif
    }
}

static SYLVAN_TLS uint64_t my_region = UINT64_MAX;

TASK(void, nodes_reset_region)

void
nodes_reset_region_CALL(lace_worker* lace)
{
    (void)lace;
    /* We don't need Lace, but use a Lace task to initialize all workers. */
    my_region = UINT64_MAX;
}

static uint64_t
claim_next_region(const nodes_table* dbs, uint64_t start_region)
{
    const uint64_t regions = dbs->table_size / (64u * 8u);
    if (regions == 0) return UINT64_MAX;

    start_region %= regions;

    /* A claimed region remains owned by that worker for the complete GC
       epoch. Whole-region ownership makes the bitmap2 and bitmapc updates in
       the allocation path word-disjoint even though they are not RMWs. */
    for (uint64_t offset = 0; offset < regions; offset++) {
        const uint64_t region = (start_region + offset) % regions;
        _Atomic(uint64_t)* ptr = dbs->bitmap1 + (region / 64u);
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

            const uint64_t claimed = claim_next_region(dbs, my_region + 1u);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        } else {
            const uint64_t regions = dbs->table_size / (64u * 8u);
            uint64_t start_region = 0;
            lace_worker* worker = lace_get_worker();

            if (regions != 0 && worker != NULL) {
                start_region =
                    ((uint64_t)worker->worker * regions) /
                    (uint64_t)lace_worker_count();
            }

            const uint64_t claimed = claim_next_region(dbs, start_region);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        }
    }
}

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

static inline uint64_t
hash_key(const nodes_table* dbs, uint64_t a, uint64_t b, int custom)
{
    const uint64_t seed = UINT64_C(14695981039346656037);
    if (custom) return dbs->hash_cb(a, b, seed);
    return sylvan_tabhash16(a, b, seed);
}

static inline size_t
head_index(const nodes_table* dbs, uint64_t hash)
{
#if LLMSSET_MASK
    return (size_t)(hash & dbs->mask);
#else
    return (size_t)(hash % dbs->table_size);
#endif
}

/**
 * Search links in [link, stop). Chains only grow by prepending, so after a
 * failed head CAS this can search just the newly published prefix.
 */
static inline uint64_t
find_in_chain(const nodes_table* dbs, uint64_t link, uint64_t stop,
              uint64_t masked_hash, uint64_t a, uint64_t b, int custom)
{
    while (link != stop) {
        /* A nonzero stop must be an ancestor in this prepend-only chain. */
        assert(stop == 0 || link != 0);
        const uint64_t index = link & MASK_INDEX;

        if ((link & MASK_HASH) == masked_hash) {
            const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
            if (custom) {
                if (dbs->equals_cb(a, b, data[0], data[1])) return index;
            } else if (data[0] == a && data[1] == b) {
                return index;
            }
        }

        sylvan_stats_count(LLMSSET_LOOKUP);
        link = atomic_load_explicit(dbs->next + index, memory_order_relaxed);
    }

    return 0;
}

/* This function must not contain a Lace yield point. A worker can own an
   unpublished candidate and, for custom nodes, publication intentionally
   precedes setting its word-disjoint bitmapc bit. */
static inline uint64_t
nodes_lookup2(const nodes_table* dbs, uint64_t a, uint64_t b,
              int* created, const int custom)
{
    const uint64_t hash = hash_key(dbs, a, b, custom);
    const uint64_t masked_hash = hash & MASK_HASH;
    _Atomic(uint64_t)* head = dbs->heads + head_index(dbs, hash);

    uint64_t observed = atomic_load_explicit(head, memory_order_acquire);
    uint64_t found = find_in_chain(
        dbs, observed, 0, masked_hash, a, b, custom);
    if (found != 0) {
        *created = 0;
        return found;
    }

    const uint64_t candidate = claim_data_bucket(dbs);
    if (candidate == UINT64_MAX) return 0;

    if (custom) {
        const int status = dbs->create_cb(&a, &b);
        if (status != 0) {
            release_data_bucket(dbs, candidate);
            *created = status;
            return 0;
        }
    }

    uint64_t* data = ((uint64_t*)dbs->data) + 2 * candidate;
    data[0] = a;
    data[1] = b;

    const uint64_t candidate_link = masked_hash | candidate;

    for (;;) {
        /* The candidate is unpublished, so this link may still be changed.
           A successful CAS establishes next[index(new_head)] == old_head,
           which makes the failed-CAS prefix scan below complete. */
        atomic_store_explicit(
            dbs->next + candidate, observed, memory_order_relaxed);

        uint64_t expected = observed;
        if (atomic_compare_exchange_strong_explicit(
                head, &expected, candidate_link,
                memory_order_acq_rel, memory_order_acquire)) {
            if (custom) set_custom_bucket(dbs, candidate, 1);
            *created = 1;
            return candidate;
        }

        /* expected is the new head. Only its prefix can contain a new match. */
        found = find_in_chain(
            dbs, expected, observed, masked_hash, a, b, custom);
        if (found != 0) {
            if (custom) dbs->destroy_cb(a, b);
            release_data_bucket(dbs, candidate);
            *created = 0;
            return found;
        }

        observed = expected;
    }
}

uint64_t
nodes_lookup(const nodes_table* dbs, const uint64_t a, const uint64_t b,
             int* created)
{
    return nodes_lookup2(dbs, a, b, created, 0);
}

uint64_t
nodes_lookupc(const nodes_table* dbs, const uint64_t a, const uint64_t b,
              int* created)
{
    return nodes_lookup2(dbs, a, b, created, 1);
}

uint64_t
nodes_lookup_subtable(const nodes_table* dbs, uint32_t subtable,
                      uint64_t a, uint64_t b, int* created)
{
    (void)subtable;
    return nodes_lookup2(dbs, a, b, created, 0);
}

uint64_t
nodes_lookupc_subtable(const nodes_table* dbs, uint32_t subtable,
                       uint64_t a, uint64_t b, int* created)
{
    (void)subtable;
    return nodes_lookup2(dbs, a, b, created, 1);
}

static int
nodes_rehash_bucket(nodes_table* dbs, uint64_t index)
{
    const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
    const uint64_t a = data[0];
    const uint64_t b = data[1];
    const int custom = is_custom_bucket(dbs, index);
    const uint64_t hash = hash_key(dbs, a, b, custom);
    const uint64_t link = (hash & MASK_HASH) | index;
    _Atomic(uint64_t)* head = dbs->heads + head_index(dbs, hash);

    uint64_t observed = atomic_load_explicit(head, memory_order_relaxed);
    do {
        atomic_store_explicit(
            dbs->next + index, observed, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(
        head, &observed, link, memory_order_release, memory_order_relaxed));

    return 1;
}

nodes_table*
nodes_create(size_t initial_size, size_t max_size)
{
    nodes_table* dbs = sylvan_alloc_aligned(sizeof(struct nodes_table));
    if (dbs == NULL) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n",
                strerror(errno));
        exit(1);
    }

#if LLMSSET_MASK
    if (popcnt_uint64((uint64_t)initial_size) != 1) {
        fprintf(stderr, "nodes_create: initial_size is not a power of 2!\n");
        exit(1);
    }

    if (popcnt_uint64((uint64_t)max_size) != 1) {
        fprintf(stderr, "nodes_create: max_size is not a power of 2!\n");
        exit(1);
    }
#endif

    if (initial_size > max_size) {
        fprintf(stderr, "nodes_create: initial_size > max_size!\n");
        exit(1);
    }

    if (max_size > UINT64_C(0x0000010000000000)) {
        fprintf(stderr, "nodes_create: max_size exceeds the 40-bit index!\n");
        exit(1);
    }

    if (initial_size < 512) {
        fprintf(stderr, "nodes_create: initial_size too small!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    nodes_set_size(dbs, initial_size);

    /* Reserve maximum capacity in virtual memory, as in nodes_llht. */
    dbs->heads = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        dbs->max_size * sizeof(*dbs->heads));
    dbs->next = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        dbs->max_size * sizeof(*dbs->next));
    dbs->data = (uint8_t*)sylvan_alloc_aligned(
        dbs->max_size * 2 * sizeof(uint64_t));

    dbs->bitmap1 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        region_bitmap_size(dbs->max_size));
    dbs->bitmap2 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        node_bitmap_size(dbs->max_size));
    dbs->bitmapc = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        node_bitmap_size(dbs->max_size));

    if (dbs->heads == NULL || dbs->next == NULL || dbs->data == NULL ||
        dbs->bitmap1 == NULL || dbs->bitmap2 == NULL ||
        dbs->bitmapc == NULL) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n",
                strerror(errno));
        exit(1);
    }

    /* Forbid first two node indices. */
    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);

    dbs->hash_cb = NULL;
    dbs->equals_cb = NULL;
    dbs->create_cb = NULL;
    dbs->destroy_cb = NULL;

    nodes_reset_region_TOGETHER();
    sylvan_init_hash();

    return dbs;
}

void
nodes_free(nodes_table* dbs)
{
    sylvan_free_aligned(
        (void*)dbs->heads, dbs->max_size * sizeof(*dbs->heads));
    sylvan_free_aligned(
        (void*)dbs->next, dbs->max_size * sizeof(*dbs->next));
    sylvan_free_aligned(
        dbs->data, dbs->max_size * 2 * sizeof(uint64_t));
    sylvan_free_aligned(
        (void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_free_aligned(
        (void*)dbs->bitmap2, node_bitmap_size(dbs->max_size));
    sylvan_free_aligned(
        (void*)dbs->bitmapc, node_bitmap_size(dbs->max_size));
    sylvan_free_aligned(dbs, sizeof(struct nodes_table));
}

void
nodes_clear_CALL(lace_worker* lace, nodes_table* dbs)
{
    (void)lace;
    sylvan_clear_aligned(
        (void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_clear_aligned(
        (void*)dbs->bitmap2, node_bitmap_size(dbs->max_size));
    sylvan_clear_aligned(
        (void*)dbs->heads, dbs->max_size * sizeof(*dbs->heads));

    /* next is overwritten before every marked or newly allocated node is
       published, so clearing it would only create unnecessary memory traffic. */

    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);
    nodes_reset_region_TOGETHER();
}

int
nodes_is_marked(const nodes_table* dbs, uint64_t index)
{
    const _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index / 64);
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63);
    return (atomic_load_explicit(ptr, memory_order_relaxed) & mask) ? 1 : 0;
}

void
nodes_mark_rec_CALL(lace_worker* lace, const nodes_table* dbs, uint64_t index)
{
    if (index == 0 || index == 1) return;

    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index / 64);
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63);
    uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
    for (;;) {
        if (v & mask) return;
        if (atomic_compare_exchange_weak_explicit(
                ptr, &v, v | mask,
                memory_order_relaxed, memory_order_relaxed)) {
            const uint64_t* data =
                ((const uint64_t*)dbs->data) + 2 * index;
            const uint64_t a = data[0];
            const uint64_t b = data[1];
            if (a & UINT64_C(0x4000000000000000)) return;
            nodes_mark_rec_SPAWN(lace, dbs, b & MASK_INDEX);
            nodes_mark_rec_CALL(lace, dbs, a & MASK_INDEX);
            nodes_mark_rec_SYNC(lace);
            return;
        }
    }
}

TASK(int, nodes_rehash_par, nodes_table*, dbs, size_t, first, size_t, count)

int
nodes_rehash_par_CALL(lace_worker* lace, nodes_table* dbs,
                      size_t first, size_t count)
{
    if (count > 512) {
        nodes_rehash_par_SPAWN(lace, dbs, first, count / 2);
        const int bad = nodes_rehash_par_CALL(
            lace, dbs, first + count / 2, count - count / 2);
        return bad + nodes_rehash_par_SYNC(lace);
    }

    int bad = 0;
    const _Atomic(uint64_t)* ptr = dbs->bitmap2 + (first / 64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (first & 63);
    for (size_t k = 0; k < count; k++) {
        if (first + k > 1 &&
            (atomic_load_explicit(ptr, memory_order_relaxed) & mask)) {
            if (nodes_rehash_bucket(dbs, first + k) == 0) bad++;
        }
        mask >>= 1;
        if (mask == 0) {
            ptr++;
            mask = UINT64_C(0x8000000000000000);
        }
    }
    return bad;
}

int
nodes_rebuild_CALL(lace_worker* lace, nodes_table* dbs)
{
    return nodes_rehash_par_CALL(lace, dbs, 0, dbs->table_size);
}

TASK(size_t, nodes_count_nodes_par,
     nodes_table*, dbs, size_t, first, size_t, count)

size_t
nodes_count_nodes_par_CALL(lace_worker* lace, nodes_table* dbs,
                           size_t first, size_t count)
{
    if (count > 512) {
        const size_t split = count / 2;
        nodes_count_nodes_par_SPAWN(lace, dbs, first, split);
        const size_t right = nodes_count_nodes_par_CALL(
            lace, dbs, first + split, count - split);
        const size_t left = nodes_count_nodes_par_SYNC(lace);
        return left + right;
    }

    size_t result = 0;
    const _Atomic(uint64_t)* ptr = dbs->bitmap2 + (first / 64);
    if (count == 512 && (first & 63) == 0) {
        result += popcnt_uint64(atomic_load_explicit(ptr + 0, memory_order_relaxed));
        result += popcnt_uint64(atomic_load_explicit(ptr + 1, memory_order_relaxed));
        result += popcnt_uint64(atomic_load_explicit(ptr + 2, memory_order_relaxed));
        result += popcnt_uint64(atomic_load_explicit(ptr + 3, memory_order_relaxed));
        result += popcnt_uint64(atomic_load_explicit(ptr + 4, memory_order_relaxed));
        result += popcnt_uint64(atomic_load_explicit(ptr + 5, memory_order_relaxed));
        result += popcnt_uint64(atomic_load_explicit(ptr + 6, memory_order_relaxed));
        result += popcnt_uint64(atomic_load_explicit(ptr + 7, memory_order_relaxed));
    } else {
        uint64_t mask = UINT64_C(0x8000000000000000) >> (first & 63);
        for (size_t k = 0; k < count; k++) {
            if (atomic_load_explicit(ptr, memory_order_relaxed) & mask) {
                result++;
            }
            mask >>= 1;
            if (mask == 0) {
                ptr++;
                mask = UINT64_C(0x8000000000000000);
            }
        }
    }
    return result;
}

size_t
nodes_count_nodes_CALL(lace_worker* lace, nodes_table* dbs)
{
    return nodes_count_nodes_par_CALL(lace, dbs, 0, dbs->table_size);
}

TASK(void, nodes_destroy_par,
     nodes_table*, dbs, size_t, first_word, size_t, word_count)

void
nodes_destroy_par_CALL(lace_worker* lace, nodes_table* dbs,
                       size_t first_word, size_t word_count)
{
    if (word_count > 16) {
        const size_t split = word_count / 2;
        nodes_destroy_par_SPAWN(lace, dbs, first_word, split);
        nodes_destroy_par_CALL(
            lace, dbs, first_word + split, word_count - split);
        nodes_destroy_par_SYNC(lace);
        return;
    }

    for (size_t word = first_word; word < first_word + word_count; word++) {
        const uint64_t marked = atomic_load_explicit(
            dbs->bitmap2 + word, memory_order_relaxed);
        const uint64_t custom = atomic_load_explicit(
            dbs->bitmapc + word, memory_order_relaxed);
        uint64_t dead = custom & ~marked;

        while (dead != 0) {
            const unsigned int bit = clz_uint64(dead);
            const size_t index = word * 64 + bit;
            const uint64_t mask =
                UINT64_C(0x8000000000000000) >> bit;
            if (index < dbs->table_size && dbs->destroy_cb != NULL) {
                uint64_t* data = ((uint64_t*)dbs->data) + 2 * index;
                dbs->destroy_cb(data[0], data[1]);
            }
            dead &= ~mask;
        }

        atomic_store_explicit(
            dbs->bitmapc + word, custom & marked, memory_order_relaxed);
    }
}

void
nodes_cleanup_custom_CALL(lace_worker* lace, nodes_table* dbs)
{
    nodes_destroy_par_CALL(
        lace, dbs, 0, (dbs->table_size + 63) / 64);
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

void
nodes_set_custom(nodes_table* dbs,
                 nodes_hash_cb hash_cb, nodes_equals_cb equals_cb,
                 nodes_create_cb create_cb, nodes_destroy_cb destroy_cb)
{
    dbs->hash_cb = hash_cb;
    dbs->equals_cb = equals_cb;
    dbs->create_cb = create_cb;
    dbs->destroy_cb = destroy_cb;
}

/**
 * Reorder primitives.
 *
 * Removal splices the node out of its own chain with compare-and-swap, and
 * reinsertion prepends it again under its new key. Both derive the chain from
 * the record: the head slot is head_index(hash(a, b)), exactly as a lookup
 * would compute it.
 *
 * Concurrency. Other session workers may run nodes_remove and nodes_reinsert
 * on the same chain at the same time. The hazard is the classic one for
 * CAS-based list deletion: if p precedes d and both are removed, the remover
 * of p may swing p's predecessor onto d after the remover of d has already
 * updated next[p], leaving d linked. The fix is the delete-in-progress link,
 * as on the historical sift branch, except that our links live in next[]
 * rather than in the record:
 *
 *   1. A remover first replaces next[d] with LINK_REMOVING, keeping the
 *      successor it read. Nothing else ever writes next[d], so this claims d.
 *   2. It then walks the chain from the head looking for d's predecessor and
 *      swings that link to the saved successor. A walker that lands on a node
 *      whose next is LINK_REMOVING restarts from the head instead of
 *      following a link that is about to be spliced out; a failed
 *      compare-and-swap also restarts.
 *
 * That restart cannot livelock. Splicing preserves the order of the surviving
 * nodes, so "waits for" follows the chain order, which is a strict total
 * order and therefore acyclic: the earliest in-progress removal on a chain
 * never waits, and always completes.
 *
 * A remover's walk also survives a concurrent reinsertion, which is subtler,
 * because reinsertion reuses the very link word a walker may be standing on.
 * Say the walker read next[p] naming x, and x is then removed and reinserted
 * before the walker loads next[x]. It now reads the head that x was prepended
 * in front of, and jumps back to the front of the chain. That is harmless in
 * both directions: the walker can never step *past* its own node d, because
 * passing d means reading a link at d's predecessor that does not name d, and
 * that predecessor's link either still names d (found), or holds
 * LINK_REMOVING (restart), or was reused by a reinsertion (jump backwards);
 * and it can never splice wrongly, because the compare-and-swap only replaces
 * a link that literally names d, which makes its holder d's predecessor at
 * that instant. Reaching the end of a chain therefore really does mean the
 * node was not in it. Each backward jump costs one more traversal but needs
 * another worker to have completed a reinsertion, so the walk terminates.
 *
 * None of this is safe against a concurrent nodes_lookup, which would follow
 * LINK_REMOVING as if it were a link to node 0.
 */

/* A link with an all-ones fingerprint and a zero index. No published link can
   look like this: a link names a node index >= 2, and a chain end is exactly
   zero. */
#define LINK_REMOVING MASK_HASH

uint32_t
nodes_reorder_caps(const nodes_table* dbs)
{
    (void)dbs;
    return NODES_CAP_REMOVE | NODES_CAP_RELEASE;
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

static _Atomic(uint64_t)*
reorder_head(const nodes_table* dbs, uint64_t index)
{
    const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
    const uint64_t hash = hash_key(
        dbs, data[0], data[1], is_custom_bucket(dbs, index));
    return dbs->heads + head_index(dbs, hash);
}

int
nodes_remove(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    /* Claim the node and take its successor with it. The compare-and-swap
       retries because next[index] is not private: when the node behind this
       one is removed, its remover splices by writing this very link. Only a
       second removal of this node itself can install LINK_REMOVING. */
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

    _Atomic(uint64_t)* head = reorder_head(dbs, index);

    for (;;) {
        uint64_t link = atomic_load_explicit(head, memory_order_acquire);
        if ((link & MASK_INDEX) == index) {
            if (atomic_compare_exchange_strong_explicit(
                    head, &link, successor,
                    memory_order_acq_rel, memory_order_acquire)) {
                return SYLVAN_OK;
            }
            continue;
        }

        for (;;) {
            const uint64_t previous = link & MASK_INDEX;
            if (previous == 0) {
                /* Not in its own chain: either it was never inserted, or the
                   caller is removing it twice. */
                assert(!"nodes_remove: node is not in its own chain");
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
 * Walk a chain looking for a node equal to the given key.
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
            const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
            if (custom) {
                if (dbs->equals_cb(a, b, data[0], data[1])) return index;
            } else if (data[0] == a && data[1] == b) {
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

    const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
    const uint64_t a = data[0];
    const uint64_t b = data[1];
    const int custom = is_custom_bucket(dbs, index);
    const uint64_t hash = hash_key(dbs, a, b, custom);
    const uint64_t masked_hash = hash & MASK_HASH;
    _Atomic(uint64_t)* head = dbs->heads + head_index(dbs, hash);
    const uint64_t link = masked_hash | index;

    uint64_t observed = atomic_load_explicit(head, memory_order_acquire);
    for (;;) {
        const uint64_t equal = reorder_find_equal(
            dbs, observed, masked_hash, a, b, custom);
        if (equal == UINT64_MAX) {
            observed = atomic_load_explicit(head, memory_order_acquire);
            continue;
        }
        if (equal != 0) reorder_duplicate_abort(index, equal);

        /* Publish this node's own link before the node itself: the release
           compare-and-swap on the head orders the store in front of it, so a
           walker can never reach the node with a stale successor. */
        atomic_store_explicit(
            dbs->next + index, observed, memory_order_relaxed);
        uint64_t expected = observed;
        if (atomic_compare_exchange_strong_explicit(
                head, &expected, link,
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

    /* Leave the slot looking unused. An insertion here writes the link before
       it publishes the node, so this backend does not depend on it, but a
       released slot that still says "removal in progress" is a trap for
       anything that reads the link array without that knowledge. */
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
