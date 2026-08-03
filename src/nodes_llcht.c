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

#include <errno.h>
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
 * WARNING: Like the other nodes tables, this implementation currently uses a
 * global thread-local allocation region. Do not use multiple tables.
 */

typedef struct nodes_table
{
    _Atomic(uint64_t)* heads;        // hash-chain heads
    uint64_t*          next;         // immutable hash/index links
    uint8_t*           data;         // table with values
    _Atomic(uint64_t)* bitmap1;      // ownership bitmap (per 512 buckets)
    _Atomic(uint64_t)* bitmap2;      // bitmap for "contains data"
    uint64_t*          bitmapc;      // bitmap for "use custom functions"
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

void
nodes_set_size(nodes_table* dbs, size_t size)
{
    /* Check bounds (don't be ridiculous). */
    if (size > 128 && size <= dbs->max_size) {
        dbs->table_size = size;
#if LLMSSET_MASK
        /* When masking is enabled, size must be a power of two. */
        dbs->mask = dbs->table_size - 1;
#endif
    }
}

SYLVAN_TLS uint64_t my_region = UINT64_MAX;

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
 * bitmapc is non-atomic by design. During insertion, each worker owns a whole
 * data region via bitmap1, so no two workers write custom bits in the same
 * region concurrently. Cleanup/rebuild is stop-the-world relative to lookup.
 */
static void
set_custom_bucket(const nodes_table* dbs, uint64_t index, int on)
{
    uint64_t* ptr = dbs->bitmapc + (index / 64);
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63);
    if (on) *ptr |= mask;
    else *ptr &= ~mask;
}

static int
is_custom_bucket(const nodes_table* dbs, uint64_t index)
{
    const uint64_t* ptr = dbs->bitmapc + (index / 64);
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63);
    return (*ptr & mask) ? 1 : 0;
}

static uint64_t
hash_key(const nodes_table* dbs, uint64_t a, uint64_t b, int custom)
{
    const uint64_t seed = UINT64_C(14695981039346656037);
    if (custom) return dbs->hash_cb(a, b, seed);
    return sylvan_tabhash16(a, b, seed);
}

static size_t
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
static uint64_t
find_in_chain(const nodes_table* dbs, uint64_t link, uint64_t stop,
              uint64_t masked_hash, uint64_t a, uint64_t b, int custom)
{
    while (link != stop) {
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
        link = dbs->next[index];
    }

    return 0;
}

static uint64_t
nodes_lookup2(const nodes_table* dbs, uint64_t a, uint64_t b,
              int* created, int custom)
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
        /* The candidate is unpublished, so this link may still be changed. */
        dbs->next[candidate] = observed;

        uint64_t expected = observed;
        if (atomic_compare_exchange_strong_explicit(
                head, &expected, candidate_link,
                memory_order_release, memory_order_acquire)) {
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
        dbs->next[index] = observed;
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

    if (initial_size < 512) {
        fprintf(stderr, "nodes_create: initial_size too small!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    nodes_set_size(dbs, initial_size);

    /* Reserve maximum capacity in virtual memory, as in nodes_llht. */
    dbs->heads = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        dbs->max_size * sizeof(*dbs->heads));
    dbs->next = (uint64_t*)sylvan_alloc_aligned(
        dbs->max_size * sizeof(*dbs->next));
    dbs->data = (uint8_t*)sylvan_alloc_aligned(
        dbs->max_size * 2 * sizeof(uint64_t));

    dbs->bitmap1 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        region_bitmap_size(dbs->max_size));
    dbs->bitmap2 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        (dbs->max_size / 64) * sizeof(*dbs->bitmap2));
    dbs->bitmapc = (uint64_t*)sylvan_alloc_aligned(dbs->max_size / 8);

    if (dbs->heads == NULL || dbs->next == NULL || dbs->data == NULL ||
        dbs->bitmap1 == NULL || dbs->bitmap2 == NULL ||
        dbs->bitmapc == NULL) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n",
                strerror(errno));
        exit(1);
    }

#if defined(madvise) && defined(MADV_RANDOM)
    madvise(dbs->heads, dbs->max_size * sizeof(*dbs->heads), MADV_RANDOM);
    madvise(dbs->next, dbs->max_size * sizeof(*dbs->next), MADV_RANDOM);
#endif

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
        dbs->next, dbs->max_size * sizeof(*dbs->next));
    sylvan_free_aligned(
        dbs->data, dbs->max_size * 2 * sizeof(uint64_t));
    sylvan_free_aligned(
        (void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_free_aligned(
        (void*)dbs->bitmap2,
        (dbs->max_size / 64) * sizeof(*dbs->bitmap2));
    sylvan_free_aligned(dbs->bitmapc, dbs->max_size / 8);
    sylvan_free_aligned(dbs, sizeof(struct nodes_table));
}

void
nodes_clear_CALL(lace_worker* lace, nodes_table* dbs)
{
    (void)lace;
    sylvan_clear_aligned(
        (void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_clear_aligned((void*)dbs->bitmap2, dbs->max_size / 8);
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
    if (count == 512) {
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
        const uint64_t custom = dbs->bitmapc[word];
        uint64_t dead = custom & ~marked;

        while (dead != 0) {
            const unsigned int bit = clz_uint64(dead);
            const size_t index = word * 64 + bit;
            const uint64_t mask =
                UINT64_C(0x8000000000000000) >> bit;
            if (index < dbs->table_size) {
                uint64_t* data = ((uint64_t*)dbs->data) + 2 * index;
                dbs->destroy_cb(data[0], data[1]);
            }
            dead &= ~mask;
        }

        dbs->bitmapc[word] = custom & marked;
    }
}

void
nodes_cleanup_custom_CALL(lace_worker* lace, nodes_table* dbs)
{
    if (dbs->destroy_cb == NULL) return;
    nodes_destroy_par_CALL(
        lace, dbs, 0, (dbs->table_size + 63) / 64);
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
