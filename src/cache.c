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

#include <sylvan/internal.h>
#include <sylvan/platform.h>

#include <errno.h>  // for errno
#include <string.h> // for strerror

/**
 * This cache is designed to store a,b,c->res, with a,b,c,res 64-bit integers.
 *
 * Each cache bucket takes 32 bytes for its operands and result, plus 8 bytes
 * for an exact operation identifier.
 * Each cache status bucket takes 4 bytes, 16 per cache line.
 * Therefore, size 2^N = 44*(2^N) bytes.
 */

typedef struct cache6_entry {
    _Atomic(uint64_t)    a;
    _Atomic(uint64_t)    b;
    _Atomic(uint64_t)    c;
    _Atomic(uint64_t)    res;
    _Atomic(uint64_t)    d;
    _Atomic(uint64_t)    e;
    _Atomic(uint64_t)    f;
    _Atomic(uint64_t)    res2;
} cache6_entry;

static_assert(sizeof(struct cache6_entry) == 64, "cache6_entry should be a 64 byte struct");

typedef struct cache_entry {
    _Atomic(uint64_t)    a;
    _Atomic(uint64_t)    b;
    _Atomic(uint64_t)    c;
    _Atomic(uint64_t)    res;
} cache_entry;

static_assert(sizeof(struct cache_entry) == 32, "cache_entry should be a 32 byte struct");


static size_t             cache_size;         // power of 2
static size_t             cache_max;          // power of 2
#if CACHE_MASK
static size_t             cache_mask;         // cache_size-1
#endif
static cache_entry*      cache_table;
static _Atomic(uint64_t)* cache_op;
static uint32_t*          cache_status;

static _Atomic(uint64_t)  next_opid = 512;

uint64_t
cache_next_opid(void)
{
    return atomic_fetch_add_explicit(&next_opid, 1, memory_order_relaxed);
}

// status: 0x80000000 - bitlock
//         0x40000000 - part of a two-bucket entry
//         0x3fff0000 - hash (part of the 64-bit hash not used to position)
//         0x0000ffff - tag (every put increases tag field)

#define CACHE_STATUS_LOCK      UINT32_C(0x80000000)
#define CACHE_STATUS_PAIR      UINT32_C(0x40000000)
#define CACHE_STATUS_HASH_MASK UINT32_C(0x3fff0000)
#define CACHE_STATUS_TAG_MASK  UINT32_C(0x0000ffff)

static inline uint64_t rotl64(uint64_t x, unsigned k) {
    return (x << k) | (x >> (64 - k));
}

static inline uint64_t
cache_hash_op(uint64_t opid, uint64_t a, uint64_t b, uint64_t c)
{
    uint64_t h = opid ^ rotl64(a, 13) ^ rotl64(b, 31) ^ rotl64(c, 47);
    h ^= h >> 17;
    h *= UINT64_C(0x9e3779b97f4a7c15);
    h ^= h >> 31;
    return h;
}

static inline uint64_t cache_hash6(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    uint64_t h1 = a ^ rotl64(b, 19) ^ rotl64(c, 37);
    uint64_t h2 = d ^ rotl64(e, 11) ^ rotl64(f, 53);

    h1 ^= h1 >> 23;
    h2 ^= h2 << 7;

    uint64_t h = h1 ^ h2;
    h *= 0x9E3779B97F4A7C15ULL; // golden ratio constant
    h ^= h >> 29;
    return h;
}

int
cache_get6(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t *res1, uint64_t *res2)
{
    const uint64_t hash = cache_hash6(a, b, c, d, e, f);
#if CACHE_MASK
    _Atomic(uint64_t) *s_bucket = (_Atomic(uint64_t)*)cache_status + (hash & cache_mask)/2;
    cache6_entry* bucket = (cache6_entry*)cache_table + (hash & cache_mask)/2;
#else
    _Atomic(uint64_t) *s_bucket = (_Atomic(uint64_t)*)cache_status + (hash % cache_size)/2;
    cache6_entry* bucket = (cache6_entry*)cache_table + (hash % cache_size)/2;
#endif
    const uint64_t s = atomic_load_explicit(s_bucket, memory_order_relaxed);
    // abort if locked or second part of 2-part entry or if different hash
    uint64_t x = ((hash>>32) & CACHE_STATUS_HASH_MASK) | CACHE_STATUS_PAIR;
    x = x | (x<<32);
    if ((s & 0xffff0000ffff0000) != x) return 0;
    atomic_thread_fence(memory_order_acquire); // prevent LoadLoad reordering
    // abort if key different
    if (atomic_load_explicit(&bucket->a, memory_order_relaxed) != a ||
        atomic_load_explicit(&bucket->b, memory_order_relaxed) != b ||
        atomic_load_explicit(&bucket->c, memory_order_relaxed) != c) return 0;
    if (atomic_load_explicit(&bucket->d, memory_order_relaxed) != d ||
        atomic_load_explicit(&bucket->e, memory_order_relaxed) != e ||
        atomic_load_explicit(&bucket->f, memory_order_relaxed) != f) return 0;
    *res1 = atomic_load_explicit(&bucket->res, memory_order_relaxed);
    if (res2) *res2 = atomic_load_explicit(&bucket->res2, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire); // prevent LoadLoad reordering
    // abort if status field changed
    return atomic_load_explicit(s_bucket, memory_order_relaxed) == s ? 1 : 0;
}

int
cache_put6(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t res1, uint64_t res2)
{
    const uint64_t hash = cache_hash6(a, b, c, d, e, f);
#if CACHE_MASK
    _Atomic(uint64_t) *s_bucket = (_Atomic(uint64_t)*)cache_status + (hash & cache_mask)/2;
    cache6_entry* bucket = (cache6_entry*)cache_table + (hash & cache_mask)/2;
#else
    _Atomic(uint64_t) *s_bucket = (_Atomic(uint64_t)*)cache_status + (hash % cache_size)/2;
    cache6_entry* bucket = (cache6_entry*)cache_table + (hash % cache_size)/2;
#endif
    // can be relaxed, we use cas afterwards to claim it
    uint64_t s = atomic_load_explicit(s_bucket, memory_order_relaxed);
    // abort if locked
    if (s & UINT64_C(0x8000000080000000)) return 0;
    // create new
    uint64_t new_s = ((hash>>32) & CACHE_STATUS_HASH_MASK) | CACHE_STATUS_PAIR;
    new_s |= (new_s<<32);
    new_s |= (((s>>32)+1)&CACHE_STATUS_TAG_MASK)<<32;
    new_s |= (s+1)&CACHE_STATUS_TAG_MASK;
    // use cas to claim bucket
    if (!atomic_compare_exchange_weak_explicit(s_bucket, &s, new_s | UINT64_C(0x8000000080000000), memory_order_acq_rel, memory_order_relaxed)) return 0;
    // cas succesful: write data
    atomic_store_explicit(&bucket->a, a, memory_order_relaxed);
    atomic_store_explicit(&bucket->b, b, memory_order_relaxed);
    atomic_store_explicit(&bucket->c, c, memory_order_relaxed);
    atomic_store_explicit(&bucket->d, d, memory_order_relaxed);
    atomic_store_explicit(&bucket->e, e, memory_order_relaxed);
    atomic_store_explicit(&bucket->f, f, memory_order_relaxed);
    atomic_store_explicit(&bucket->res, res1, memory_order_relaxed);
    atomic_store_explicit(&bucket->res2, res2, memory_order_relaxed);
    // unlock status field
    atomic_store_explicit(s_bucket, new_s, memory_order_release);
    return 1;
}

int
cache_get_op(
    uint64_t opid, uint64_t a, uint64_t b, uint64_t c, uint64_t *res)
{
    const uint64_t hash = cache_hash_op(opid, a, b, c);
#if CACHE_MASK
    const size_t index = hash & cache_mask;
#else
    const size_t index = hash % cache_size;
#endif
    _Atomic(uint32_t) *s_bucket =
        (_Atomic(uint32_t)*)cache_status + index;
    cache_entry* bucket = cache_table + index;
    const uint32_t s = atomic_load_explicit(s_bucket, memory_order_relaxed);
    // abort if locked or if part of a 2-part cache entry
    if (s & (CACHE_STATUS_LOCK | CACHE_STATUS_PAIR)) return 0;
    // abort if different hash
    if ((s ^ (hash>>32)) & CACHE_STATUS_HASH_MASK) return 0;
    atomic_thread_fence(memory_order_acquire); // prevent LoadLoad reordering
    // abort if key different
    if (atomic_load_explicit(cache_op + index, memory_order_relaxed) != opid ||
        atomic_load_explicit(&bucket->a, memory_order_relaxed) != a ||
        atomic_load_explicit(&bucket->b, memory_order_relaxed) != b ||
        atomic_load_explicit(&bucket->c, memory_order_relaxed) != c) return 0;
    *res = atomic_load_explicit(&bucket->res, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire); // prevent LoadLoad reordering
    // abort if status field changed after compiler_barrier()
    return atomic_load_explicit(s_bucket, memory_order_relaxed) == s ? 1 : 0;
}

int
cache_put_op(
    uint64_t opid, uint64_t a, uint64_t b, uint64_t c, uint64_t res)
{
    const uint64_t hash = cache_hash_op(opid, a, b, c);
#if CACHE_MASK
    const size_t index = hash & cache_mask;
#else
    const size_t index = hash % cache_size;
#endif
    _Atomic(uint32_t) *s_bucket =
        (_Atomic(uint32_t)*)cache_status + index;
    cache_entry* bucket = cache_table + index;
    uint32_t s = atomic_load_explicit(s_bucket, memory_order_relaxed);
    // abort if locked
    if (s & CACHE_STATUS_LOCK) return 0;
    // abort if hash identical -> no: in iscasmc this occasionally causes timeouts?!
    const uint32_t hash_mask = (hash>>32) & CACHE_STATUS_HASH_MASK;
    // if ((s & 0x7fff0000) == hash_mask) return 0;
    // use cas to claim bucket
    const uint32_t new_s = ((s+1) & CACHE_STATUS_TAG_MASK) | hash_mask;
    if (!atomic_compare_exchange_weak_explicit(s_bucket, &s, new_s | CACHE_STATUS_LOCK, memory_order_acq_rel, memory_order_relaxed)) return 0;
    // cas succesful: write data
    atomic_store_explicit(cache_op + index, opid, memory_order_relaxed);
    atomic_store_explicit(&bucket->a, a, memory_order_relaxed);
    atomic_store_explicit(&bucket->b, b, memory_order_relaxed);
    atomic_store_explicit(&bucket->c, c, memory_order_relaxed);
    atomic_store_explicit(&bucket->res, res, memory_order_relaxed);
    // unlock status field
    atomic_store_explicit(s_bucket, new_s, memory_order_release);
    return 1;
}

int
cache_get(uint64_t a, uint64_t b, uint64_t c, uint64_t *res)
{
    return cache_get_op(0, a, b, c, res);
}

int
cache_put(uint64_t a, uint64_t b, uint64_t c, uint64_t res)
{
    return cache_put_op(0, a, b, c, res);
}


#define IS_POWER_OF_TWO(x) ((x) != 0 && ((x) & ((x) - 1)) == 0)


void
cache_create(size_t _cache_size, size_t _max_size)
{
#if CACHE_MASK
    // Cache size must be a power of 2
    if (!IS_POWER_OF_TWO(_cache_size) || !IS_POWER_OF_TWO(_max_size)) {
        fprintf(stderr, "cache_create: Table size must be a power of 2!\n");
        exit(1);
    }
#endif

    cache_size = _cache_size;
    cache_max  = _max_size;
#if CACHE_MASK
    cache_mask = cache_size - 1;
#endif

    if (cache_size > cache_max) {
        fprintf(stderr, "cache_create: Table size must be <= max size!\n");
        exit(1);
    }

    cache_table = (cache_entry*)sylvan_alloc_aligned(cache_max * sizeof(struct cache_entry));
    cache_op = (_Atomic(uint64_t)*)sylvan_alloc_aligned(cache_max * sizeof(*cache_op));
    cache_status = (uint32_t*)sylvan_alloc_aligned(cache_max * sizeof(uint32_t));
    if (cache_table == 0 || cache_op == 0 || cache_status == 0) {
        fprintf(stderr, "cache_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }
}

void
cache_free(void)
{
    sylvan_free_aligned(cache_table, cache_max * sizeof(struct cache_entry));
    sylvan_free_aligned((void*)cache_op, cache_max * sizeof(*cache_op));
    sylvan_free_aligned(cache_status, cache_max * sizeof(uint32_t));
}

void
cache_clear(void)
{
    /* A zero status makes the corresponding payload unreachable. */
    sylvan_clear_aligned(cache_status, cache_size * sizeof(*cache_status));
}

void
cache_setsize(size_t size)
{
    if (size == 0 || size > cache_max) {
        fprintf(stderr, "cache_setsize: Table size must be nonzero and <= max size!\n");
        exit(1);
    }
#if CACHE_MASK
    if (!IS_POWER_OF_TWO(size)) {
        fprintf(stderr, "cache_setsize: Table size must be a power of 2!\n");
        exit(1);
    }
#endif

    cache_size = size;
#if CACHE_MASK
    cache_mask = cache_size - 1;
#endif
    cache_clear();
}

size_t
cache_getsize(void)
{
    return cache_size;
}

size_t
cache_getused(void)
{
    size_t result = 0;
    for (size_t i=0;i<cache_size;i++) {
        uint32_t s = cache_status[i];
        if (s & 0x80000000) fprintf(stderr, "cache_getuser: cache in use during cache_getused()\n");
        if (s) result++;
    }
    return result;
}

size_t
cache_getmaxsize(void)
{
    return cache_max;
}
