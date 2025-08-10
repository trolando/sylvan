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
#include <sylvan/platform.h>

#include <errno.h>
#include <string.h>

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
        dbs->threshold = 192 - 2 * ctz_uint64(dbs->table_size);
    }
}

SYLVAN_TLS uint64_t my_region = UINT64_MAX;

TASK(void, nodes_reset_region)

void nodes_reset_region_CALL(lace_worker* lace)
{
    // we don't actually need Lace, but it's a Lace task to run for initialisation
    my_region = UINT64_MAX; // no region
}

static uint64_t
claim_next_region(const nodes_table* dbs, uint64_t start_region)
{
    const uint64_t regions = dbs->table_size / (64u * 8u);
    if (regions == 0) return UINT64_MAX;

    start_region %= regions;

    for (uint64_t offset = 0; offset < regions; offset++) {
        const uint64_t region = (start_region + offset) % regions;
        _Atomic(uint64_t)*ptr = dbs->bitmap1 + (region / 64u);
        const uint64_t mask = UINT64_C(0x8000000000000000) >> (region & 63u);

        uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
        for (;;) {
            if (v & mask) break;

            if (atomic_compare_exchange_weak_explicit(
                    ptr, &v, v | mask,
                    memory_order_acq_rel, memory_order_relaxed)) {
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
            // find empty bucket in current region
            _Atomic(uint64_t)*ptr = dbs->bitmap2 + (my_region * 8u);

            for (int i = 0; i < 8; i++) {
                uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
                if (v != UINT64_MAX) {
                    unsigned int j = clz_uint64(~v);
                    atomic_fetch_or_explicit(
                        ptr,
                        UINT64_C(0x8000000000000000) >> j,
                        memory_order_relaxed
                    );
                    return (8u * my_region + (uint64_t)i) * 64u + (uint64_t)j;
                }
                ptr++;
            }

            uint64_t claimed = claim_next_region(dbs, my_region + 1u);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        }
        else {
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
    _Atomic(uint64_t)*ptr = dbs->bitmap2 + (index / 64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index & 63);
    atomic_fetch_and_explicit(ptr, ~mask, memory_order_relaxed);
}

/*
 * bitmapc is non-atomic by design. During insertion, each worker owns a whole
 * data region via bitmap1, so no two workers write custom bits in the same
 * region concurrently. Cleanup/rehash is stop-the-world relative to insertion.
 */
static void
set_custom_bucket(const nodes_table* dbs, uint64_t index, int on)
{
    uint64_t *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    if (on) *ptr |= mask;
    else *ptr &= ~mask;
}

static int is_custom_bucket(const nodes_table* dbs, uint64_t index)
{
    uint64_t *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    return (*ptr & mask) ? 1 : 0;
}

/*
 * CL_MASK and CL_MASK_R are for the probe sequence calculation.
 * With 64 bytes per cache line, there are 8 64-bit values per cache line.
 * With 128 bytes per cache line, there are 16 64-bit values per cache line.
 */
#define CL_WORDS ((uint64_t)(SYLVAN_CACHE_LINE_SIZE / sizeof(uint64_t)))

static const uint64_t CL_MASK = ~(CL_WORDS - 1);
static const uint64_t CL_MASK_R = CL_WORDS - 1; 

/* 40 bits for the index, 24 bits for the hash */
#define MASK_INDEX ((uint64_t)0x000000ffffffffff)
#define MASK_HASH  ((uint64_t)0xffffff0000000000)

static inline uint64_t nodes_lookup2(const nodes_table* dbs, uint64_t a, uint64_t b, int* created, const int custom)
{
    // Compute the hash
    uint64_t hash_rehash = 14695981039346656037LLU;
    if (custom) hash_rehash = dbs->hash_cb(a, b, hash_rehash);
    else hash_rehash = sylvan_tabhash16(a, b, hash_rehash);

    const uint64_t step = ((hash_rehash >> 20) | 1) * CL_WORDS;
    const uint64_t masked_hash = hash_rehash & MASK_HASH;
    uint64_t idx, last, cidx = 0;
    int i=0;

#if LLMSSET_MASK
    last = idx = hash_rehash & dbs->mask;
#else
    last = idx = hash_rehash % dbs->table_size;
#endif

    for (;;) {
        _Atomic(uint64_t)* bucket = dbs->table + idx;
        uint64_t v = atomic_load_explicit(bucket, memory_order_acquire);

        if (v == 0) {
            if (cidx == 0) {
                // Claim data bucket and write data
                cidx = claim_data_bucket(dbs);
                if (cidx == (uint64_t)-1) return 0;
                if (custom) dbs->create_cb(&a, &b);
                // FIXME ensure a acquire_release fence
                uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*cidx;
                d_ptr[0] = a;
                d_ptr[1] = b;
            }
            if (atomic_compare_exchange_strong_explicit(bucket, &v, masked_hash | cidx, memory_order_release, memory_order_acquire)) {
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
            if (++i == atomic_load_explicit(&dbs->threshold, memory_order_relaxed)) {
                // failed to find empty spot in probe sequence
                if (cidx != 0) {
                    if (custom) dbs->destroy_cb(a, b);
                    release_data_bucket(dbs, cidx);
                }
                return 0;
            }

            // go to next cache line in probe sequence
            hash_rehash += step;

#if LLMSSET_MASK
            idx = hash_rehash & dbs->mask;
#else
            idx = hash_rehash % dbs->table_size;
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
    const uint64_t step = ((hash_rehash >> 20) | 1) * CL_WORDS;
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

        uint64_t v = atomic_load_explicit(bucket, memory_order_relaxed);
        if (v == 0 && atomic_compare_exchange_strong_explicit(
            bucket,
            &v,
            new_v,
            memory_order_release,
            memory_order_relaxed)) {
            return 1;
        }

        // find next idx on probe sequence
        idx = (idx & CL_MASK) | ((idx+1) & CL_MASK_R);
        if (idx == last) {
            if (++i == atomic_load_explicit(&dbs->threshold, memory_order_relaxed)) {
                // failed to find empty spot in probe sequence
                // solution: increase probe sequence length...
                atomic_fetch_add_explicit(&dbs->threshold, 1, memory_order_relaxed);
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
    nodes_table* dbs = sylvan_alloc_aligned(sizeof(struct nodes));
    if (dbs == 0) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

#if LLMSSET_MASK
    /* Check if initial_size and max_size are powers of 2 */
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

    // minimum size is now 512 buckets (region size, but of course, n_workers * 512 is suggested as minimum)

    if (initial_size < 512) {
        fprintf(stderr, "nodes_create: initial_size too small!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    nodes_set_size(dbs, initial_size);

    /* This implementation of "resizable hash table" allocates the max_size table in virtual memory,
       but only uses the "actual size" part in real memory */

    dbs->table = (_Atomic(uint64_t)*)sylvan_alloc_aligned(dbs->max_size * sizeof(*dbs->table));
    dbs->data = (uint8_t*)sylvan_alloc_aligned(dbs->max_size * 2 * sizeof(uint64_t));

    /* Also allocate bitmaps. Each region is 64*8 = 512 buckets.
       Overhead of bitmap1: 1 bit per 4096 bucket.
       Overhead of bitmap2: 1 bit per bucket.
       Overhead of bitmapc: 1 bit per bucket. */

    dbs->bitmap1 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(dbs->max_size / (512*8));
    dbs->bitmap2 = (_Atomic(uint64_t)*)sylvan_alloc_aligned((dbs->max_size / 64) * sizeof(*dbs->bitmap2));
    dbs->bitmapc = (uint64_t*)sylvan_alloc_aligned(dbs->max_size / 8);

    if (dbs->table == 0 || dbs->data == 0 || dbs->bitmap1 == 0 || dbs->bitmap2 == 0 || dbs->bitmapc == 0) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

#if defined(madvise) && defined(MADV_RANDOM)
    madvise(dbs->table, dbs->max_size * 8, MADV_RANDOM);
#endif

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);

    dbs->hash_cb = NULL;
    dbs->equals_cb = NULL;
    dbs->create_cb = NULL;
    dbs->destroy_cb = NULL;

    // yes, ugly. for now, we use a global thread-local value.
    // that is a problem with multiple tables.
    // so, for now, do NOT use multiple tables!!

    nodes_reset_region_TOGETHER();

    // initialize hashtab
    sylvan_init_hash();

    return dbs;
}

void nodes_free(nodes_table* dbs)
{
    sylvan_free_aligned(dbs->table, dbs->max_size * sizeof(*dbs->table));
    sylvan_free_aligned(dbs->data, dbs->max_size * 2 * sizeof(uint64_t));
    sylvan_free_aligned(dbs->bitmap1, dbs->max_size / (512 * 8));
    sylvan_free_aligned(dbs->bitmap2, (dbs->max_size / 64) * sizeof(*dbs->bitmap2));
    sylvan_free_aligned(dbs->bitmapc, dbs->max_size / 8);
    sylvan_free_aligned(dbs, sizeof(struct nodes));
}

void nodes_clear_CALL(lace_worker* lace, nodes_table* dbs)
{
    sylvan_clear_aligned(dbs->bitmap1, dbs->max_size / (512*8));
    sylvan_clear_aligned(dbs->bitmap2, dbs->max_size / 8);
    sylvan_clear_aligned(dbs->table, dbs->max_size * 8);

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
        }
    }
}

TASK(int, nodes_rehash_par, nodes_table*, dbs, size_t, first, size_t, count)

int nodes_rehash_par_CALL(lace_worker* lace, nodes_table* dbs, size_t first, size_t count)
{
    if (count > 512) {
        nodes_rehash_par_SPAWN(lace, dbs, first, count/2);
        int bad = nodes_rehash_par_CALL(lace, dbs, first + count/2, count - count/2);
        return bad + nodes_rehash_par_SYNC(lace);
    } else {
        int bad = 0;
        _Atomic(uint64_t)* ptr = dbs->bitmap2 + (first / 64);
        uint64_t mask = UINT64_C(0x8000000000000000) >> (first & 63);
        for (size_t k=0; k<count; k++) {
            if (atomic_load_explicit(ptr, memory_order_relaxed) & mask) {
                if (nodes_rehash_bucket(dbs, first+k) == 0) bad++;
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
    return nodes_rehash_par_CALL(lace, dbs, 0, dbs->table_size);
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
        if (count == 512) {
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

TASK(void, nodes_destroy_par, nodes_table*, dbs, size_t, first, size_t, count)

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
            uint64_t mask = UINT64_C(0x8000000000000000) >> (k&63);

            // if not marked but is custom
            uint64_t marked = atomic_load_explicit(ptr2, memory_order_relaxed);
            if ((marked & mask) == 0 && (*ptrc & mask)) {
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
