/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
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

#include <sylvan_int.h>
#include <sylvan_platform.h>

#include <errno.h>
#include <string.h>

/* Keep table_size and data_size's public types and layout unchanged. Access
 * them through C11 atomics here, including for C++ callers, whose _Atomic
 * maps to std::atomic.
 * Non-worker traversals can read these sizes while GC grows or shrinks the
 * table. Relaxed loads suffice: their protected nodes existed before the
 * traversal started and remain within the data range after a shrink.
 */
_Static_assert(sizeof(_Atomic(size_t)) == sizeof(size_t), "atomic size_t must preserve size");
_Static_assert(_Alignof(_Atomic(size_t)) <= _Alignof(size_t), "atomic size_t must preserve alignment");

/* For the hot paths in this file: a static inline function inlines also in a
 * shared library built with -fPIC, where an exported one may not. */
static inline size_t
table_size_relaxed(const llmsset_t dbs)
{
    return atomic_load_explicit((const _Atomic(size_t) *)&dbs->table_size, memory_order_relaxed);
}

size_t
llmsset_get_size(const llmsset_t dbs)
{
    return table_size_relaxed(dbs);
}

static inline size_t
data_size_relaxed(const llmsset_t dbs)
{
    return atomic_load_explicit((const _Atomic(size_t) *)&dbs->data_size, memory_order_relaxed);
}

size_t
llmsset_get_data_size(const llmsset_t dbs)
{
    return data_size_relaxed(dbs);
}

#if SYLVAN_CHECK_HANDLES
void
sylvan_handle_check_failed(size_t index, size_t size, const char *file, int line)
{
    fprintf(stderr,
            "[sylvan] %s:%d: decision diagram handle index %zu "
            "is outside the nodes data range (size %zu)\n",
            file, line, index, size);
    abort();
}
#endif

void
llmsset_set_size(llmsset_t dbs, size_t size)
{
    /* check bounds (don't be rediculous) */
    if (size > 128 && size <= dbs->max_size) {
        if (data_size_relaxed(dbs) < size) {
            atomic_store_explicit((_Atomic(size_t) *)&dbs->data_size, size, memory_order_relaxed);
        }
        atomic_store_explicit((_Atomic(size_t) *)&dbs->table_size, size, memory_order_relaxed);
#if LLMSSET_MASK
        /* Warning: if size is not a power of two, you will get interesting behavior */
        dbs->mask = size - 1;
#endif
        /* Set threshold: number of cache lines to probe before giving up on node insertion */
        dbs->threshold = (int16_t)(192 - 2 * clz_uint64((uint64_t)size));
    }
}

SYLVAN_TLS uint64_t my_region = UINT64_MAX;

VOID_TASK_0(llmsset_reset_region)
{
    // we don't actually need Lace, but it's a Lace task to run for initialisation
    my_region = UINT64_MAX; // no region
}

static uint64_t
claim_next_region(const llmsset_t dbs, uint64_t start_region)
{
    const uint64_t regions = data_size_relaxed(dbs) / (64u * 8u);
    if (regions == 0) return UINT64_MAX;

    start_region %= regions;

    for (uint64_t offset = 0; offset < regions; offset++) {
        const uint64_t region = (start_region + offset) % regions;
        _Atomic(uint64_t)*ptr = dbs->bitmap1 + (region / 64u);
        const uint64_t mask = UINT64_C(0x8000000000000000) >> (region & 63u);

        uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
        for (;;) {
            if (v & mask) break; // region already owned

            if (atomic_compare_exchange_weak_explicit(
                ptr,
                &v,
                v | mask,
                memory_order_acq_rel,
                memory_order_relaxed)) {
                return region;
            }

            // CAS failed; v has been updated. Try again unless the bit is now set.
        }
    }

    return UINT64_MAX;
}

static uint64_t
claim_data_bucket(const llmsset_t dbs)
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

            // Current region is full; claim the next available one.
            uint64_t claimed = claim_next_region(dbs, my_region + 1u);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        }
        else {
            // First use after startup or GC. Spread workers over the region space.
            const uint64_t regions = data_size_relaxed(dbs) / (64u * 8u);
            uint64_t start_region = 0;

            if (regions != 0) {
                start_region =
                    ((uint64_t)lace_get_worker()->worker * regions) /
                    (uint64_t)lace_workers();
            }

            uint64_t claimed = claim_next_region(dbs, start_region);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        }
    }
}

static void
release_data_bucket(const llmsset_t dbs, uint64_t index)
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
set_custom_bucket(const llmsset_t dbs, uint64_t index, int on)
{
    uint64_t *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    if (on) *ptr |= mask;
    else *ptr &= ~mask;
}

static int
is_custom_bucket(const llmsset_t dbs, uint64_t index)
{
    uint64_t *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    return (*ptr & mask) ? 1 : 0;
}

/*
 * CL_MASK and CL_MASK_R are for the probe sequence calculation.
 * With 64 bytes per cacheline, there are 8 64-bit values per cacheline.
 */
#define CL_WORDS ((uint64_t)(SYLVAN_CACHE_LINE_SIZE / sizeof(uint64_t)))

static const uint64_t CL_MASK = ~(CL_WORDS - 1);
static const uint64_t CL_MASK_R = CL_WORDS - 1; 

/* 40 bits for the index, 24 bits for the hash */
#define MASK_INDEX ((uint64_t)0x000000ffffffffff)
#define MASK_HASH  ((uint64_t)0xffffff0000000000)

static inline uint64_t
llmsset_lookup2(const llmsset_t dbs, uint64_t a, uint64_t b, int* created, const int custom)
{
    uint64_t hash_rehash = 14695981039346656037LLU;
    if (custom) hash_rehash = dbs->hash_cb(a, b, hash_rehash);
    else hash_rehash = sylvan_tabhash16(a, b, hash_rehash);

    const uint64_t step = ((hash_rehash >> 20) | 1) * CL_WORDS;
    const uint64_t hash = hash_rehash & MASK_HASH;
    uint64_t idx, last, cidx = 0;
    int i=0;

#if LLMSSET_MASK
    last = idx = hash_rehash & dbs->mask;
#else
    last = idx = hash_rehash % table_size_relaxed(dbs);
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
                uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*cidx;
                d_ptr[0] = a;
                d_ptr[1] = b;
            }
            if (atomic_compare_exchange_strong_explicit(bucket, &v, hash | cidx, memory_order_release, memory_order_acquire)) {
                if (custom) set_custom_bucket(dbs, cidx, custom);
                *created = 1;
                return cidx;
            }
        }

        if (hash == (v & MASK_HASH)) {
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
            last = idx = hash_rehash & dbs->mask;
#else
            last = idx = hash_rehash % table_size_relaxed(dbs);
#endif
        }
    }
}

uint64_t
llmsset_lookup(const llmsset_t dbs, const uint64_t a, const uint64_t b, int* created)
{
    return llmsset_lookup2(dbs, a, b, created, 0);
}

uint64_t
llmsset_lookupc(const llmsset_t dbs, const uint64_t a, const uint64_t b, int* created)
{
    return llmsset_lookup2(dbs, a, b, created, 1);
}

int
llmsset_rehash_bucket(const llmsset_t dbs, uint64_t d_idx)
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
    last = idx = hash_rehash % table_size_relaxed(dbs);
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
            last = idx = hash_rehash % table_size_relaxed(dbs);
#endif
        }
    }
}

llmsset_t
llmsset_create(size_t initial_size, size_t max_size)
{
    llmsset_t dbs = sylvan_alloc_aligned(sizeof(struct llmsset));
    if (dbs == 0) {
        fprintf(stderr, "llmsset_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

#if LLMSSET_MASK
    /* Check if initial_size and max_size are powers of 2 */
    if (popcnt_uint64((uint64_t)initial_size) != 1) {
        fprintf(stderr, "llmsset_create: initial_size is not a power of 2!\n");
        exit(1);
    }

    if (popcnt_uint64((uint64_t)max_size) != 1) {
        fprintf(stderr, "llmsset_create: max_size is not a power of 2!\n");
        exit(1);
    }
#endif

    if (initial_size > max_size) {
        fprintf(stderr, "llmsset_create: initial_size > max_size!\n");
        exit(1);
    }

    // minimum size is now 512 buckets (region size, but of course, n_workers * 512 is suggested as minimum)

    if (initial_size < 512) {
        fprintf(stderr, "llmsset_create: initial_size too small!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    llmsset_set_size(dbs, initial_size);

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
        fprintf(stderr, "llmsset_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }


    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);

    dbs->hash_cb = NULL;
    dbs->equals_cb = NULL;
    dbs->create_cb = NULL;
    dbs->destroy_cb = NULL;

    // yes, ugly. for now, we use a global thread-local value.
    // that is a problem with multiple tables.
    // so, for now, do NOT use multiple tables!!

    TOGETHER(llmsset_reset_region);

    // initialize hashtab
    sylvan_init_hash();

    return dbs;
}

/**
 * Whether no bucket of a data region (512 buckets) holds a marked node.
 */
static int
region_is_empty(const llmsset_t dbs, size_t region)
{
    _Atomic(uint64_t)* ptr = dbs->bitmap2 + region * 8;
    for (int i = 0; i < 8; i++) {
        if (atomic_load_explicit(ptr + i, memory_order_relaxed) != 0) return 0;
    }
    return 1;
}

void
llmsset_shrink(llmsset_t dbs, size_t size)
{
    /* Even at the current hash size, a later collection may have dropped
       high nodes, or switched from retaining the arena to releasing it. */
    if (size > table_size_relaxed(dbs)) return;
    const size_t data_size = data_size_relaxed(dbs);

    /* one past the highest marked bucket (bit 63 is the first bucket of a word) */
    size_t top = 0;
    for (size_t w = data_size / 64; w-- > 0;) {
        uint64_t v = atomic_load_explicit(dbs->bitmap2 + w, memory_order_relaxed);
        if (v != 0) {
            top = w * 64 + 64 - ctz_uint64(v);
            break;
        }
    }

#if SYLVAN_USE_MMAP
    /* Give back each run of empty regions in 64 KiB-aligned chunks, rounding
       inward so live nodes are untouched. These boundaries accommodate the
       supported base page sizes, but can split huge pages. Reclaim the empty
       memory anyway; on Linux the mapping keeps its huge page advice. */
    const size_t region_bytes = 512 * 2 * sizeof(uint64_t);
    const uintptr_t chunk = 65536;
    const size_t regions = data_size / 512;
    size_t r = 0;
    while (r < regions) {
        if (!region_is_empty(dbs, r)) {
            r++;
            continue;
        }
        size_t first = r;
        while (r < regions && region_is_empty(dbs, r)) r++;
        uintptr_t from = (uintptr_t)(dbs->data + first * region_bytes);
        uintptr_t to = (uintptr_t)(dbs->data + r * region_bytes);
        from = (from + chunk - 1) & ~(chunk - 1);
        to &= ~(chunk - 1);
        if (from < to) sylvan_clear_aligned((void*)from, (size_t)(to - from));
    }
#endif

    /* the hash table shrinks to <size>; the data buckets in use only to the
       region above the highest marked node */
    size_t data = (top + 511) & ~(size_t)511;
    if (data < size) data = size;
    if (data > data_size) data = data_size;
    llmsset_set_size(dbs, size);
    atomic_store_explicit((_Atomic(size_t) *)&dbs->data_size, data, memory_order_relaxed);
}

void
llmsset_free(llmsset_t dbs)
{
    sylvan_free_aligned(dbs->table, dbs->max_size * sizeof(*dbs->table));
    sylvan_free_aligned(dbs->data, dbs->max_size * 2 * sizeof(uint64_t));
    sylvan_free_aligned(dbs->bitmap1, dbs->max_size / (512 * 8));
    sylvan_free_aligned(dbs->bitmap2, (dbs->max_size / 64) * sizeof(*dbs->bitmap2));
    sylvan_free_aligned(dbs->bitmapc, dbs->max_size / 8);
    sylvan_free_aligned(dbs, sizeof(struct llmsset));
}

VOID_TASK_IMPL_1(llmsset_clear, llmsset_t, dbs)
{
    CALL(llmsset_clear_data, dbs);
    CALL(llmsset_clear_hashes, dbs);
}

VOID_TASK_IMPL_1(llmsset_clear_data, llmsset_t, dbs)
{
    sylvan_clear_aligned(dbs->bitmap1, dbs->max_size / (512*8));
    sylvan_clear_aligned(dbs->bitmap2, dbs->max_size / 8);

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);

    TOGETHER(llmsset_reset_region);
}

VOID_TASK_IMPL_1(llmsset_clear_hashes, llmsset_t, dbs)
{
    sylvan_clear_aligned(dbs->table, dbs->max_size * 8);
}

int
llmsset_is_marked(const llmsset_t dbs, uint64_t index)
{
    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    return (atomic_load_explicit(ptr, memory_order_relaxed) & mask) ? 1 : 0;
}

int
llmsset_mark(const llmsset_t dbs, uint64_t index)
{
    _Atomic(uint64_t)* ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = UINT64_C(0x8000000000000000) >> (index&63);
    for (;;) {
        uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
        if (v & mask) return 0;
        if (atomic_compare_exchange_weak_explicit(
            ptr,
            &v,
            v | mask,
            memory_order_relaxed,
            memory_order_relaxed)) {
            return 1;
        }
    }
}

/**
 * The bits of bitmap word <w> that belong to buckets in [first, end). Bit 63
 * of a word is its first bucket. The passes over the data range below go
 * through the bitmaps a word at a time, so that empty parts of the range cost
 * one load per 64 buckets: after an explicit sylvan_gc() with the few
 * collections policy, the data range stays at its peak while few nodes live.
 */
static inline uint64_t
range_mask(size_t w, size_t first, size_t end)
{
    const size_t base = w * 64;
    uint64_t mask = UINT64_MAX;
    if (base < first) mask &= UINT64_MAX >> (first - base);
    if (end < base + 64) mask &= ~(UINT64_MAX >> (end - base));
    return mask;
}

/**
 * Where the passes below split a range of <count> buckets in two: about half,
 * on a bitmap word boundary. The passes start at bucket 0, so no two tasks
 * share a bitmap word, also when the data range is not a power of two (after
 * llmsset_shrink, a multiple of 512). destroy_par clears bits of bitmapc,
 * which is not atomic. Only called with count > 512, so the split is not 0.
 */
static inline size_t
word_split(size_t count)
{
    return (count / 2) & ~(size_t)63;
}

TASK_3(int, llmsset_rehash_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 512) {
        const size_t split = word_split(count);
        SPAWN(llmsset_rehash_par, dbs, first, split);
        int bad = CALL(llmsset_rehash_par, dbs, first + split, count - split);
        return bad + SYNC(llmsset_rehash_par);
    } else {
        int bad = 0;
        const size_t end = first + count;
        for (size_t w = first / 64; w * 64 < end; w++) {
            uint64_t v = atomic_load_explicit(dbs->bitmap2 + w, memory_order_relaxed);
            v &= range_mask(w, first, end);
            while (v != 0) {
                const unsigned int j = clz_uint64(v);
                v &= ~(UINT64_C(0x8000000000000000) >> j);
                const size_t k = w * 64 + j;
                if (k > 1 && llmsset_rehash_bucket(dbs, k) == 0) bad++;
            }
        }
        return bad;
    }
}

TASK_IMPL_1(int, llmsset_rehash, llmsset_t, dbs)
{
    return CALL(llmsset_rehash_par, dbs, 0, data_size_relaxed(dbs));
}

TASK_3(size_t, llmsset_count_marked_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 512) {
        const size_t split = word_split(count);
        SPAWN(llmsset_count_marked_par, dbs, first, split);
        size_t right = CALL(llmsset_count_marked_par, dbs, first + split, count - split);
        size_t left = SYNC(llmsset_count_marked_par);
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
            const size_t end = first + count;
            for (size_t w = first / 64; w * 64 < end; w++) {
                uint64_t v = atomic_load_explicit(dbs->bitmap2 + w, memory_order_relaxed);
                result += popcnt_uint64(v & range_mask(w, first, end));
            }
        }
        return result;
    }
}

TASK_IMPL_1(size_t, llmsset_count_marked, llmsset_t, dbs)
{
    return CALL(llmsset_count_marked_par, dbs, 0, data_size_relaxed(dbs));
}

VOID_TASK_3(llmsset_destroy_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 1024) {
        const size_t split = word_split(count);
        SPAWN(llmsset_destroy_par, dbs, first, split);
        CALL(llmsset_destroy_par, dbs, first + split, count - split);
        SYNC(llmsset_destroy_par);
    } else {
        const size_t end = first + count;
        for (size_t w = first / 64; w * 64 < end; w++) {
            // custom leaves that are not marked
            uint64_t v = dbs->bitmapc[w] & ~atomic_load_explicit(dbs->bitmap2 + w, memory_order_relaxed);
            v &= range_mask(w, first, end);
            while (v != 0) {
                const unsigned int j = clz_uint64(v);
                const uint64_t mask = UINT64_C(0x8000000000000000) >> j;
                v &= ~mask;
                const uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*(w * 64 + j);
                dbs->destroy_cb(d_ptr[0], d_ptr[1]);
                dbs->bitmapc[w] &= ~mask;
            }
        }
    }
}

VOID_TASK_IMPL_1(llmsset_destroy_unmarked, llmsset_t, dbs)
{
    if (dbs->destroy_cb == NULL) return; // no custom function
    CALL(llmsset_destroy_par, dbs, 0, data_size_relaxed(dbs));
}

/**
 * Set custom functions
 */
void llmsset_set_custom(const llmsset_t dbs, llmsset_hash_cb hash_cb, llmsset_equals_cb equals_cb, llmsset_create_cb create_cb, llmsset_destroy_cb destroy_cb)
{
    dbs->hash_cb = hash_cb;
    dbs->equals_cb = equals_cb;
    dbs->create_cb = create_cb;
    dbs->destroy_cb = destroy_cb;
}
