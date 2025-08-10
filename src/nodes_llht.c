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
 * Lockless hash table (set) to store 16-byte keys.
 * Each unique key is associated with a 42-bit number.
 *
 * The set has support for stop-the-world garbage collection.
 * Methods nodes_clear, nodes_mark and nodes_rehash implement garbage collection.
 * During their execution, nodes_lookup is not allowed.
 *
 * WARNING: Originally, this table is designed to allow multiple tables.
 * However, this is not compatible with thread local storage for now.
 * Do not use multiple tables.
 */

typedef struct nodes
{
    _Atomic(uint64_t)* table;        // table with hashes
    uint8_t*           data;         // table with values
    _Atomic(uint64_t)* bitmap1;      // ownership bitmap (per 512 buckets)
    _Atomic(uint64_t)* bitmap2;      // bitmap for "contains data"
    uint64_t*          bitmapc;      // bitmap for "use custom functions"
    size_t             max_size;     // maximum size of the hash table (for resizing)
    size_t             table_size;   // size of the hash table (number of slots) --> power of 2!
#if LLMSSET_MASK
    size_t             mask;         // size-1
#endif
    size_t             f_size;
    nodes_hash_cb      hash_cb;      // custom hash function
    nodes_equals_cb    equals_cb;    // custom equals function
    nodes_create_cb    create_cb;    // custom create function
    nodes_destroy_cb   destroy_cb;   // custom destroy function
    _Atomic(int16_t)   threshold;    // number of iterations for insertion until returning error
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
#if LLMSSET_MASK
        /* Warning: if size is not a power of two, you will get interesting behavior */
        dbs->mask = dbs->table_size - 1;
#endif
        /* Set threshold: number of cache lines to probe before giving up on node insertion */
        dbs->threshold = 192 - 2 * __builtin_clzll(dbs->table_size);
    }
}

DECLARE_THREAD_LOCAL(my_region, uint64_t);

VOID_TASK_0(nodes_reset_region)
void nodes_reset_region_CALL(lace_worker* lace)
{
    // we don't actually need Lace, but it's a Lace task to run for initialisation
    LOCALIZE_THREAD_LOCAL(my_region, uint64_t);
    my_region = (uint64_t)-1; // no region
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

/*
 * CL_MASK and CL_MASK_R are for the probe sequence calculation.
 * With 64 bytes per cache line, there are 8 64-bit values per cache line.
 * With 128 bytes per cache line, there are 16 64-bit values per cache line.
 */
static const uint64_t CL_MASK     = ~(((SYLVAN_CACHE_LINE_SIZE) / 8) - 1);
static const uint64_t CL_MASK_R   = ((SYLVAN_CACHE_LINE_SIZE) / 8) - 1;

/* 40 bits for the index, 24 bits for the hash */
#define MASK_INDEX ((uint64_t)0x000000ffffffffff)
#define MASK_HASH  ((uint64_t)0xffffff0000000000)

static inline uint64_t nodes_lookup2(const nodes_table* dbs, uint64_t a, uint64_t b, int* created, const int custom)
{
    // Compute the hash
    uint64_t hash = 14695981039346656037LLU;
    if (custom) hash = dbs->hash_cb(a, b, hash);
    else hash = sylvan_tabhash16(a, b, hash);

    //const uint64_t step = (((hash >> 20) | 1) << 3);
    const uint64_t step = SYLVAN_CACHE_LINE_SIZE/8;
    const uint64_t masked_hash = hash & MASK_HASH;
    uint64_t idx, last, cidx = 0;
    int i=0;

#if LLMSSET_MASK
    last = idx = hash & dbs->mask;
#else
    last = idx = hash % dbs->table_size;
#endif

    for (;;) {
        _Atomic(uint64_t)* bucket = dbs->table + idx;
        uint64_t v = atomic_load_explicit(bucket, memory_order_acquire);

        if (v == 0) {
            if (cidx == 0) {
                // Claim data bucket and write data
                cidx = claim_data_bucket(dbs);
                if (cidx == (uint64_t)-1) return 0; // failed to claim a data bucket
                if (custom) dbs->create_cb(&a, &b);
                // FIXME ensure a acquire_release fence
                uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*cidx;
                d_ptr[0] = a;
                d_ptr[1] = b;
            }
            if (atomic_compare_exchange_strong_explicit(bucket, &v, masked_hash | cidx, memory_order_acq_rel, memory_order_relaxed)) {
                if (custom) set_custom_bucket(dbs, cidx, custom);
                *created = 1;
                return cidx;
            }
        }

        if (masked_hash == (v & MASK_HASH)) {
            uint64_t d_idx = v & MASK_INDEX;
            uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*d_idx;
            if (custom) {
                if (dbs->equals_cb(a, b, d_ptr[0], d_ptr[1])) {
                    if (cidx != 0) {
                        dbs->destroy_cb(a, b);
                        release_data_bucket(dbs, cidx);
                    }
                    *created = 0;
                    return d_idx;
                }
            } else {
                if (d_ptr[0] == a && d_ptr[1] == b) {
                    if (cidx != 0) release_data_bucket(dbs, cidx);
                    *created = 0;
                    return d_idx;
                }
            }
        }

        sylvan_stats_count(LLMSSET_LOOKUP);

        // find next idx on probe sequence
        idx = (idx & CL_MASK) | ((idx+1) & CL_MASK_R);
        if (idx == last) {
            if (++i == dbs->threshold) return 0; // failed to find empty spot in probe sequence

            // go to next cache line in probe sequence

#if LLMSSET_MASK
            idx = (idx + step) & dbs->mask;
#else
            idx = (idx + step) & dbs->table_size;
#endif
            last = idx;
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

int nodes_rehash_bucket(nodes_table* dbs, uint64_t d_idx)
{
    const uint64_t * const d_ptr = ((uint64_t*)dbs->data) + 2*d_idx;
    const uint64_t a = d_ptr[0];
    const uint64_t b = d_ptr[1];

    uint64_t hash_rehash = 14695981039346656037LLU;
    const int custom = is_custom_bucket(dbs, d_idx) ? 1 : 0;
    if (custom) hash_rehash = dbs->hash_cb(a, b, hash_rehash);
    else hash_rehash = sylvan_tabhash16(a, b, hash_rehash);
    const uint64_t step = (((hash_rehash >> 20) | 1) << 3);
    const uint64_t new_v = (hash_rehash & MASK_HASH) | d_idx;
    int i=0;

    uint64_t idx, last;
#if LLMSSET_MASK
    last = idx = hash_rehash & dbs->mask;
#else
    last = idx = hash_rehash % dbs->table_size;
#endif

    for (;;) {
        _Atomic(uint64_t)* bucket = &dbs->table[idx];
        uint64_t v = atomic_load_explicit(bucket, memory_order_acquire);
        if (v == 0 && atomic_compare_exchange_strong(bucket, &v, new_v)) return 1;

        // find next idx on probe sequence
        idx = (idx & CL_MASK) | ((idx+1) & CL_MASK_R);
        if (idx == last) {
            if (++i == atomic_load_explicit(&dbs->threshold, memory_order_relaxed)) {
                // failed to find empty spot in probe sequence
                // solution: increase probe sequence length...
                atomic_fetch_add(&dbs->threshold, 1);
            }

            // go to next cache line in probe sequence
            hash_rehash += step;

#if LLMSSET_MASK
            last = idx = hash_rehash & dbs->mask;
#else
            last = idx = hash_rehash % dbs->table_size;
#endif
        }
    }
}

nodes_table* nodes_create(size_t initial_size, size_t max_size)
{
    nodes_table* dbs = alloc_aligned(sizeof(struct nodes));
    if (dbs == 0) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

#if LLMSSET_MASK
    /* Check if initial_size and max_size are powers of 2 */
    if (__builtin_popcountll(initial_size) != 1) {
        fprintf(stderr, "nodes_create: initial_size is not a power of 2!\n");
        exit(1);
    }

    if (__builtin_popcountll(max_size) != 1) {
        fprintf(stderr, "nodes_create: max_size is not a power of 2!\n");
        exit(1);
    }
#endif

    if (initial_size > max_size) {
        fprintf(stderr, "nodes_create: initial_size > max_size!\n");
        exit(1);
    }

    // minimum size is now 512 buckets (region size, but of course, n_workers * 512 is suggested as minimum)

    if (initial_size < 512) {
        fprintf(stderr, "nodes_create: initial_size too small!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    nodes_set_size(dbs, initial_size);

    /* This implementation of "resizable hash table" allocates the max_size table in virtual memory,
       but only uses the "actual size" part in real memory */

    dbs->table = (_Atomic(uint64_t)*) alloc_aligned(dbs->max_size * 8);
    dbs->data = (uint8_t*) alloc_aligned(dbs->max_size * 16);

    /* Also allocate bitmaps. Each region is 64*8 = 512 buckets.
       Overhead of bitmap1: 1 bit per 4096 bucket.
       Overhead of bitmap2: 1 bit per bucket.
       Overhead of bitmapc: 1 bit per bucket. */

    dbs->bitmap1 = (_Atomic(uint64_t)*)alloc_aligned(dbs->max_size / (512*8));
    dbs->bitmap2 = (_Atomic(uint64_t)*)alloc_aligned(dbs->max_size / 8);
    dbs->bitmapc = (uint64_t*)alloc_aligned(dbs->max_size / 8);

    if (dbs->table == 0 || dbs->data == 0 || dbs->bitmap1 == 0 || dbs->bitmap2 == 0 || dbs->bitmapc == 0) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

#if defined(madvise) && defined(MADV_RANDOM)
    madvise(dbs->table, dbs->max_size * 8, MADV_RANDOM);
#endif

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = 0xc000000000000000LL;

    dbs->hash_cb = NULL;
    dbs->equals_cb = NULL;
    dbs->create_cb = NULL;
    dbs->destroy_cb = NULL;

    // yes, ugly. for now, we use a global thread-local value.
    // that is a problem with multiple tables.
    // so, for now, do NOT use multiple tables!!

    INIT_THREAD_LOCAL(my_region);
    nodes_reset_region_TOGETHER();

    // initialize hashtab
    sylvan_init_hash();

    return dbs;
}

void nodes_free(nodes_table* dbs)
{
    free_aligned(dbs->table, dbs->max_size * 8);
    free_aligned(dbs->data, dbs->max_size * 16);
    free_aligned(dbs->bitmap1, dbs->max_size / (512*8));
    free_aligned(dbs->bitmap2, dbs->max_size / 8);
    free_aligned(dbs->bitmapc, dbs->max_size / 8);
    free_aligned(dbs, sizeof(struct nodes));
}

void nodes_clear_CALL(lace_worker* lace, nodes_table* dbs)
{
    clear_aligned(dbs->bitmap1, dbs->max_size / (512*8));
    clear_aligned(dbs->bitmap2, dbs->max_size / 8);
    clear_aligned(dbs->table, dbs->max_size * 8);

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

TASK_3(int, nodes_rehash_par, nodes_table*, dbs, size_t, first, size_t, count)

int nodes_rehash_par_CALL(lace_worker* lace, nodes_table* dbs, size_t first, size_t count)
{
    if (count > 512) {
        nodes_rehash_par_SPAWN(lace, dbs, first, count/2);
        int bad = nodes_rehash_par_CALL(lace, dbs, first + count/2, count - count/2);
        return bad + nodes_rehash_par_SYNC(lace);
    } else {
        int bad = 0;
        _Atomic(uint64_t)* ptr = dbs->bitmap2 + (first / 64);
        uint64_t mask = 0x8000000000000000LL >> (first & 63);
        for (size_t k=0; k<count; k++) {
            if (atomic_load_explicit(ptr, memory_order_relaxed) & mask) {
                if (nodes_rehash_bucket(dbs, first+k) == 0) bad++;
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
    return nodes_rehash_par_CALL(lace, dbs, 0, dbs->table_size);
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
