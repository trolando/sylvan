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

#include <sylvan/internal/internal.h>
#include "align.h"

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

typedef struct nodes
{
    uint8_t*           data;         // array with the nodes
    _Atomic(uint64_t)* first;        // array with the root of the trie
    _Atomic(uint64_t)* next;         // array with the 'next' entries of the trie
    _Atomic(uint64_t)* bitmap1;      // ownership bitmap (per 512 buckets)
    _Atomic(uint64_t)* bitmap2;      // bitmap for "contains data"
    uint64_t*          bitmapc;      // bitmap for "use custom functions"
    size_t             max_size;     // maximum size of the nodes table (for resizing)
    size_t             table_size;   // current size of the nodes table
    nodes_hash_cb      hash_cb;      // custom hash function
    nodes_equals_cb    equals_cb;    // custom equals function
    nodes_create_cb    create_cb;    // custom create function
    nodes_destroy_cb   destroy_cb;   // custom destroy function
} nodes_table;

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

void nodes_set_size(nodes_table* dbs, size_t size)
{
    /* check bounds (don't be ridiculous) */
    if (size > 128 && size <= dbs->max_size) {
        dbs->table_size = size;
    }
}

DECLARE_THREAD_LOCAL(my_region, uint64_t);

VOID_TASK_0(nodes_reset_region)
void nodes_reset_region_CALL(lace_worker* lace)
{
    // we don't actually need Lace, but it's a Lace task to run for initialisation
    LOCALIZE_THREAD_LOCAL(my_region, uint64_t);
    my_region = UINT64_MAX; // no region
    SET_THREAD_LOCAL(my_region, my_region);
}

/**
 * Try to claim a free region.
 * start_region is the last region we worked on; search proceeds round-robin.
 * Returns the claimed region index, or UINT64_MAX if none available.
 */
static uint64_t claim_next_region(const nodes_table* dbs, uint64_t start_region)
{
    const uint64_t regions = dbs->table_size / (64u * 8u); // regions in table
    const uint64_t words   = (regions + 63u) / 64u;        // bitmap1 word count

    // start word index and bit index
    const uint64_t start_word = start_region / 64u;
    const uint64_t start_bit  = start_region % 64u;

    // Scan words
    //for (uint64_t w_offset = 0; w_offset < words; ++w_offset) {
    //    uint64_t w = (start_word + w_offset) % words;
    for (uint64_t w = start_word; w < words;) {
        _Atomic(uint64_t) *word = dbs->bitmap1 + w;

        uint64_t v = atomic_load_explicit(word, memory_order_relaxed);
        while (v != UINT64_MAX) {
            // There is at least one free bit
            int bit = __builtin_ctzll(~v); // least-significant free bit
            uint64_t mask = UINT64_C(1) << bit;

            // Try to claim
            if (atomic_compare_exchange_weak_explicit(
                         word, &v, v | mask,
                         memory_order_acq_rel, memory_order_relaxed)) {
                // Claimed successfully
                return w * 64 + bit;
            }
        }
        w++;
    }

    return UINT64_MAX; // full
}

static uint64_t claim_data_bucket(const nodes_table* dbs)
{
    LOCALIZE_THREAD_LOCAL(my_region, uint64_t);

    // First-time (or post-GC) init: everyone starts at region 0
    if (my_region == UINT64_MAX) {
        my_region = claim_next_region(dbs, 0);
        if (my_region == UINT64_MAX) return UINT64_MAX;
        SET_THREAD_LOCAL(my_region, my_region);
    }

    for (;;) {
        // find empty bucket in region <my_region>
        _Atomic(uint64_t)* ptr = dbs->bitmap2 + (my_region * 8u);
        for (int i=0; i<8; i++) {
            uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
            if (v != UINT64_MAX) {
                int j = __builtin_clzll(~v);
                *ptr |= UINT64_C(1) << (63 - j);
                return (8 * my_region + i) * 64 + j;
            }
            ptr++;
        }
        my_region = claim_next_region(dbs, my_region);
        if (my_region == UINT64_MAX) return UINT64_MAX;
        SET_THREAD_LOCAL(my_region, my_region);
    }
}

static void release_data_bucket(const nodes_table* dbs, uint64_t index)
{
    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    atomic_fetch_and(ptr, ~mask);
    // FIXME should not be seq_cst when just local, only when reinserting
}

static void set_custom_bucket(const nodes_table* dbs, uint64_t index, int on)
{
    uint64_t *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    if (on) *ptr |= mask;
    else *ptr &= ~mask;
}

static int is_custom_bucket(const nodes_table* dbs, uint64_t index)
{
    uint64_t *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    return (*ptr & mask) ? 1 : 0;
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
        if (custom) dbs->create_cb(&a, &b);
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
                if (custom) dbs->create_cb(&a, &b);
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

    uint64_t value = atomic_load_explicit(&dbs->first[trie], memory_order_relaxed);
    if (value == 0) {
        if (atomic_compare_exchange_strong_explicit(
                &dbs->first[trie], &value, masked_hash | d_idx,
                memory_order_relaxed, memory_order_relaxed)) return 1;
    }

    for (;;) {
        uint64_t n_idx = trie*2 + (hash&1);
        hash = ror64(hash, 1);
        value = atomic_load_explicit(&dbs->next[n_idx], memory_order_relaxed);
        if (value == 0) {
            if (atomic_compare_exchange_strong_explicit(
                    &dbs->next[n_idx], &value, masked_hash | d_idx,
                    memory_order_relaxed, memory_order_relaxed)) return 1;
        }
        trie = value&0x000000ffffffffff;
    }
}

nodes_table* nodes_create(size_t initial_size, size_t max_size)
{
    nodes_table* dbs = alloc_aligned(sizeof(struct nodes));
    if (dbs == 0) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

    if (initial_size > max_size) {
        fprintf(stderr, "nodes_create: initial_size > max_size!\n");
        exit(1);
    }

    // minimum size is 512 buckets (region size)

    if (initial_size < 512) {
        fprintf(stderr, "nodes_create: initial_size too small!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    nodes_set_size(dbs, initial_size);

    dbs->data = (uint8_t*) alloc_aligned(dbs->max_size * 16);
    dbs->first = (_Atomic(uint64_t)*) alloc_aligned(dbs->max_size * 8);
    dbs->next = (_Atomic(uint64_t)*) alloc_aligned(dbs->max_size * 16);

    /* Allocate bitmaps. Each region is 64*8 = 512 buckets.
       Overhead of bitmap1: 1 bit per 4096 bucket.
       Overhead of bitmap2: 1 bit per bucket.
       Overhead of bitmapc: 1 bit per bucket. */

    dbs->bitmap1 = (_Atomic(uint64_t)*)alloc_aligned(dbs->max_size / (512*8));
    dbs->bitmap2 = (_Atomic(uint64_t)*)alloc_aligned(dbs->max_size / 8);
    dbs->bitmapc = (uint64_t*)alloc_aligned(dbs->max_size / 8);

    if (dbs->data == 0 || dbs->first == 0 || dbs->next == 0 || dbs->bitmap1 == 0 || dbs->bitmap2 == 0 || dbs->bitmapc == 0) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

#if defined(madvise) && defined(MADV_RANDOM)
    madvise(dbs->first, dbs->max_size * 8, MADV_RANDOM);
    madvise(dbs->next, dbs->max_size * 16, MADV_RANDOM);
#endif

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = 0xc000000000000000LL;

    dbs->hash_cb = NULL;
    dbs->equals_cb = NULL;
    dbs->create_cb = NULL;
    dbs->destroy_cb = NULL;

    // this design assumes there is only one nodes table...

    INIT_THREAD_LOCAL(my_region);
    nodes_reset_region_TOGETHER();

    // initialize hashtab
    sylvan_init_hash();

    return dbs;
}

void nodes_free(nodes_table* dbs)
{
    free_aligned(dbs->data, dbs->max_size * 16);
    free_aligned(dbs->first, dbs->max_size * 8);
    free_aligned(dbs->next, dbs->max_size * 16);
    free_aligned(dbs->bitmap1, dbs->max_size / (512*8));
    free_aligned(dbs->bitmap2, dbs->max_size / 8);
    free_aligned(dbs->bitmapc, dbs->max_size / 8);
    free_aligned(dbs, sizeof(struct nodes));
}

void nodes_clear_CALL(lace_worker* lace, nodes_table* dbs)
{
    clear_aligned(dbs->bitmap1, dbs->max_size / (512*8));
    clear_aligned(dbs->bitmap2, dbs->max_size / 8);
    clear_aligned(dbs->first, dbs->max_size * 8);
    clear_aligned(dbs->next, dbs->max_size * 16);

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = 0xc000000000000000LL;

    nodes_reset_region_TOGETHER();
}

int nodes_is_marked(const nodes_table* dbs, uint64_t index)
{
    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    return (atomic_load_explicit(ptr, memory_order_relaxed) & mask) ? 1 : 0;
}

void nodes_mark_rec_CALL(lace_worker* lace, const nodes_table* dbs, uint64_t index)
{
    if (index == 0 || index == 1) return; // reserved for true/false

    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
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
        }
    }
}

TASK_3(int, nodes_rebuild_par, nodes_table*, dbs, size_t, first, size_t, count)

int nodes_rebuild_par_CALL(lace_worker* lace, nodes_table* dbs, size_t first, size_t count)
{
    if (count > 512) {
        nodes_rebuild_par_SPAWN(lace, dbs, first, count/2);
        int bad = nodes_rebuild_par_CALL(lace, dbs, first + count/2, count - count/2);
        return bad + nodes_rebuild_par_SYNC(lace);
    } else {
        int bad = 0;
        _Atomic(uint64_t)* ptr = dbs->bitmap2 + (first / 64);
        uint64_t mask = 0x8000000000000000LL >> (first & 63);
        for (size_t k=0; k<count; k++) {
            if (atomic_load_explicit(ptr, memory_order_relaxed) & mask) {
                if (nodes_reinsert_bucket(dbs, first+k) == 0) bad++;
            }
            mask >>= 1;
            if (mask == 0) {
                ptr++;
                mask = 0x8000000000000000LL;
            }
        }
        return bad;
    }
}

int nodes_rebuild_CALL(lace_worker* lace, nodes_table* dbs)
{
    return nodes_rebuild_par_CALL(lace, dbs, 0, dbs->table_size);
}

TASK_3(size_t, nodes_count_nodes_par, nodes_table*, dbs, size_t, first, size_t, count)

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
        if (count == 512) {
            result += __builtin_popcountll(atomic_load_explicit(ptr+0, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr+1, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr+2, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr+3, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr+4, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr+5, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr+6, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr+7, memory_order_relaxed));
        } else {
            uint64_t mask = 0x8000000000000000LL >> (first & 63);
            for (size_t k=0; k<count; k++) {
                if (atomic_load_explicit(ptr, memory_order_relaxed) & mask) result += 1;
                mask >>= 1;
                if (mask == 0) {
                    ptr++;
                    mask = 0x8000000000000000LL;
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

VOID_TASK_3(nodes_destroy_par, nodes_table*, dbs, size_t, first, size_t, count)

void nodes_destroy_par_CALL(lace_worker* lace, nodes_table* dbs, size_t first, size_t count)
{
    if (count > 1024) {
        size_t split = count/2;
        nodes_destroy_par_SPAWN(lace, dbs, first, split);
        nodes_destroy_par_CALL(lace, dbs, first + split, count - split);
        nodes_destroy_par_SYNC(lace);
    } else {
        for (size_t k=first; k<first+count; k++) {
            _Atomic(uint64_t)* ptr2 = dbs->bitmap2 + (k/64);
            uint64_t *ptrc = dbs->bitmapc + (k/64);
            uint64_t mask = 0x8000000000000000LL >> (k&63);

            // if not marked but is custom
            if ((*ptr2 & mask) == 0 && (*ptrc & mask)) {
                uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*k;
                dbs->destroy_cb(d_ptr[0], d_ptr[1]);
                *ptrc &= ~mask;
            }
        }
    }
}

void nodes_cleanup_custom_CALL(lace_worker* lace, nodes_table* dbs)
{
    if (dbs->destroy_cb == NULL) return; // no custom function
    nodes_destroy_par_CALL(lace, dbs, 0, dbs->table_size);
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
