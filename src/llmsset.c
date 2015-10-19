/*
 * Copyright 2011-2014 Formal Methods and Tools, University of Twente
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

#include <sylvan_config.h>

#include <assert.h> // for assert
#include <stdint.h> // for uint64_t etc
#include <stdio.h>  // for printf
#include <stdlib.h>
#include <string.h> // for memcopy
#include <sys/mman.h> // for mmap

#include <atomics.h>
#include <llmsset.h>
#include <stats.h>
#include <tls.h>

#ifndef USE_HWLOC
#define USE_HWLOC 0
#endif

#if USE_HWLOC
#include <hwloc.h>

static hwloc_topology_t topo;
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/*
 *  1 bit  for hash-filled
 * 40 bits for the index
 * 23 bits for the hash
 */
#define HFILLED    ((uint64_t)0x8000000000000000)
#define MASK_INDEX ((uint64_t)0x000000ffffffffff)
#define MASK_HASH  ((uint64_t)0x7fffff0000000000)

static const uint8_t  HASH_PER_CL = ((LINE_SIZE) / 8);
static const uint64_t CL_MASK     = ~(((LINE_SIZE) / 8) - 1);
static const uint64_t CL_MASK_R   = ((LINE_SIZE) / 8) - 1;

/*
 * Example values with a LINE_SIZE of 64
 * HASH_PER_CL = 8
 * CL_MASK     = 0xFFFFFFFFFFFFFFF8
 * CL_MASK_R   = 0x0000000000000007
 */

// Calculate next index on a cache line walk
#define probe_sequence_next(cur, last) (((cur) = (((cur) & CL_MASK) | (((cur) + 1) & CL_MASK_R))) != (last))

DECLARE_THREAD_LOCAL(my_region, uint64_t);

VOID_TASK_0(llmsset_reset_region)
{
    LOCALIZE_THREAD_LOCAL(my_region, uint64_t);
    my_region = (uint64_t)-1; // no region
    SET_THREAD_LOCAL(my_region, my_region);
}

VOID_TASK_0(llmsset_init_worker)
{
    // yes, ugly. for now, we use a global thread-local value.
    // that is a problem with multiple tables.
    // so, for now, do NOT use multiple tables!!
    INIT_THREAD_LOCAL(my_region);
    CALL(llmsset_reset_region);
}

/**
 * hash16
 */
#ifndef rotl64
static inline uint64_t
rotl64(uint64_t x, int8_t r)
{
    return ((x<<r) | (x>>(64-r)));
}
#endif

static uint64_t
rehash16_mul(const uint64_t a, const uint64_t b, const uint64_t seed)
{
    const uint64_t prime = 1099511628211;

    uint64_t hash = seed;
    hash = hash ^ a;
    hash = rotl64(hash, 47);
    hash = hash * prime;
    hash = hash ^ b;
    hash = rotl64(hash, 31);
    hash = hash * prime;

    return hash ^ (hash >> 32);
}

static uint64_t
hash16_mul(const uint64_t a, const uint64_t b)
{
    return rehash16_mul(a, b, 14695981039346656037LLU);
}

static uint64_t
claim_data_bucket(const llmsset_t dbs)
{
    LOCALIZE_THREAD_LOCAL(my_region, uint64_t);

    for (;;) {
        if (my_region != (uint64_t)-1) {
            // find empty bucket in region <my_region>
            uint64_t *ptr = dbs->bitmap2 + (my_region*8);
            int i=0;
            for (;i<8;) {
                uint64_t v = *ptr;
                if (v != 0xffffffffffffffffLL) {
                    int j = __builtin_clzl(~v);
                    *ptr |= (0x8000000000000000LL>>j);
                    uint64_t index = (8 * my_region + i) * 64 + j;
                    if (index <= 1) continue; // skip 0, 1
                    return index;
                }
                i++;
                ptr++;
            }
        }
        // if we're here, we need a new region...
        if (my_region == (uint64_t)-1) {
            my_region += (lace_get_worker()->worker*(dbs->table_size/(64*8)))/lace_workers();
        }
        uint64_t count = 0;
        for (;;) {
            // check if table maybe full
            if (count++ == dbs->table_size/(64*8)) return (uint64_t)-1;

            my_region += 1;
            if (my_region >= (dbs->table_size/(64*8))) my_region = 0;

            // try to claim it
            uint64_t *ptr = dbs->bitmap1 + (my_region/64);
            uint64_t mask = 0x8000000000000000LL >> (my_region&63);
            uint64_t v;
restart:
            v = *ptr;
            if (v & mask) continue; // taken
            if (cas(ptr, v, v|mask)) break;
            else goto restart;
        }
        SET_THREAD_LOCAL(my_region, my_region);
    }
}

static void
release_data_bucket(const llmsset_t dbs, uint64_t index)
{
    uint64_t *ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    *ptr &= ~mask;
}

/*
 * Note: garbage collection during lookup strictly forbidden
 * insert_index points to a starting point and is updated.
 */
uint64_t
llmsset_lookup(const llmsset_t dbs, const uint64_t a, const uint64_t b, int* created)
{
    uint64_t hash_rehash = hash16_mul(a, b);
    const uint64_t hash = hash_rehash & MASK_HASH;
    int i=0;

    for (;i<dbs->threshold;i++) {
#if LLMSSET_MASK
        uint64_t idx = hash_rehash & dbs->mask;
#else
        uint64_t idx = hash_rehash % dbs->table_size;
#endif
        const uint64_t last = idx; // if next() sees idx again, stop.

        do {
            volatile uint64_t *bucket = &dbs->table[idx];
            uint64_t v = *bucket;

            if (v == 0) goto phase2;

            if (hash == (v & MASK_HASH)) {
                uint64_t d_idx = v & MASK_INDEX;
                register uint64_t *d_ptr = ((uint64_t*)dbs->data) + 2*d_idx;
                if (d_ptr[0] == a && d_ptr[1] == b) {
                    *created = 0;
                    return d_idx;
                }
            }

            sylvan_stats_count(LLMSSET_PHASE1);
        } while (probe_sequence_next(idx, last));

        hash_rehash = rehash16_mul(a, b, hash_rehash);
    }

    return 0; // failed to find empty spot in probe sequence

    uint64_t d_idx;
phase2:
    d_idx = claim_data_bucket(dbs);
    if (d_idx == (uint64_t)-1) {
        return 0; // failed to claim a data bucket
    }

    uint64_t *d_ptr;
    d_ptr = ((uint64_t*)dbs->data) + 2*d_idx;
    d_ptr[0] = a;
    d_ptr[1] = b;

    // continue where we were...
    for (;i<dbs->threshold;i++) {
#if LLMSSET_MASK
        uint64_t idx = hash_rehash & dbs->mask;
#else
        uint64_t idx = hash_rehash % dbs->table_size;
#endif
        const uint64_t last = idx; // if next() sees idx again, stop.

        do {
            volatile uint64_t *bucket = dbs->table + idx;
            uint64_t v;
phase2_restart:
            v = *bucket;

            if (v == 0) {
                if (!cas(bucket, 0, HFILLED | hash | d_idx)) goto phase2_restart;
                *created = 1;
                return d_idx;
            }

            if (hash == (v & MASK_HASH)) {
                uint64_t d2_idx = v & MASK_INDEX;
                register uint64_t *d2_ptr = ((uint64_t*)dbs->data) + 2*d2_idx;
                if (d2_ptr[0] == a && d2_ptr[1] == b) {
                    release_data_bucket(dbs, d_idx);
                    *created = 0;
                    return d2_idx;
                }
            }

            sylvan_stats_count(LLMSSET_PHASE3);
        } while (probe_sequence_next(idx, last));

        hash_rehash = rehash16_mul(a, b, hash_rehash);
    }

    return 0;
}

static inline int
llmsset_rehash_bucket(const llmsset_t dbs, uint64_t d_idx)
{
    const uint64_t * const d_ptr = ((uint64_t*)dbs->data) + 2*d_idx;
    uint64_t hash_rehash = hash16_mul(d_ptr[0], d_ptr[1]);

    int i;
    for (i=0;i<dbs->threshold;i++) {
#if LLMSSET_MASK
        uint64_t idx = hash_rehash & dbs->mask;
#else
        uint64_t idx = hash_rehash % dbs->table_size;
#endif
        const uint64_t last = idx; // if next() sees idx again, stop.

        // no need for atomic restarts
        // we can assume there are no double inserts (GC rehash phase)
        do {
            volatile uint64_t *bucket = &dbs->table[idx];
            uint64_t v = *bucket;
            uint64_t new_v = (hash_rehash & MASK_HASH) | d_idx | HFILLED;
            if (v == 0 && cas(bucket, 0, new_v)) return 1;
        } while (probe_sequence_next(idx, last));

        hash_rehash = rehash16_mul(d_ptr[0], d_ptr[1], hash_rehash);
    }

    return 0;
}

llmsset_t
llmsset_create(size_t initial_size, size_t max_size)
{
#if USE_HWLOC
    hwloc_topology_init(&topo);
    hwloc_topology_load(topo);
#endif

    llmsset_t dbs = NULL;
    if (posix_memalign((void**)&dbs, LINE_SIZE, sizeof(struct llmsset)) != 0) {
        fprintf(stderr, "llmsset_create: Unable to allocate memory!\n");
        exit(1);
    }

#if LLMSSET_MASK
    /* Check if initial_size and max_size are powers of 2 */
    if (__builtin_popcountll(initial_size) != 1) {
        fprintf(stderr, "llmsset_create: initial_size is not a power of 2!\n");
        exit(1);
    }

    if (__builtin_popcountll(max_size) != 1) {
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

    dbs->table = (uint64_t*)mmap(0, dbs->max_size * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    dbs->data = (uint8_t*)mmap(0, dbs->max_size * 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

    /* Also allocate bitmaps. Each region is 64*8 = 512 buckets.
       Overhead of bitmap1: 1 bit per 4096 bucket.
       Overhead of bitmap2: 1 bit per bucket.
       Overhead of bitmap3: 1 bit per bucket. */

    dbs->bitmap1 = (uint64_t*)mmap(0, dbs->max_size / (512*8), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    dbs->bitmap2 = (uint64_t*)mmap(0, dbs->max_size / 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
    dbs->bitmap3 = (uint64_t*)mmap(0, dbs->max_size / 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);

    if (dbs->table == (uint64_t*)-1 || dbs->data == (uint8_t*)-1 || dbs->bitmap1 == (uint64_t*)-1 || dbs->bitmap2 == (uint64_t*)-1 || dbs->bitmap3 == (uint64_t*)-1) {
        fprintf(stderr, "llmsset_create: Unable to allocate memory!\n");
        exit(1);
    }

#if defined(madvise) && defined(MADV_RANDOM)
    madvise(dbs->table, dbs->max_size * 8, MADV_RANDOM);
#endif

#if USE_HWLOC
    hwloc_set_area_membind(topo, dbs->table, dbs->max_size * 8, hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_INTERLEAVE, 0);
    hwloc_set_area_membind(topo, dbs->data, dbs->max_size * 16, hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_FIRSTTOUCH, 0);
    hwloc_set_area_membind(topo, dbs->bitmap1, dbs->max_size / (512*8), hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_INTERLEAVE, 0);
    hwloc_set_area_membind(topo, dbs->bitmap2, dbs->max_size / 8, hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_FIRSTTOUCH, 0);
    hwloc_set_area_membind(topo, dbs->bitmap3, dbs->max_size / 8, hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_FIRSTTOUCH, 0);
#endif

    dbs->dead_cb = NULL;

    LACE_ME;
    TOGETHER(llmsset_init_worker);

    return dbs;
}

void
llmsset_free(llmsset_t dbs)
{
    munmap(dbs->table, dbs->max_size * 8);
    munmap(dbs->data, dbs->max_size * 16);
    munmap(dbs->bitmap1, dbs->max_size / (512*8));
    munmap(dbs->bitmap2, dbs->max_size / 8);
    munmap(dbs->bitmap3, dbs->max_size / 8);
    free(dbs);
}

VOID_TASK_IMPL_1(llmsset_clear, llmsset_t, dbs)
{
    // just reallocate...
    if (mmap(dbs->table, dbs->max_size * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void*)-1) {
#if defined(madvise) && defined(MADV_RANDOM)
        madvise(dbs->table, sizeof(uint64_t[dbs->max_size]), MADV_RANDOM);
#endif
#if USE_HWLOC
        hwloc_set_area_membind(topo, dbs->table, sizeof(uint64_t[dbs->max_size]), hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_INTERLEAVE, 0);
#endif
    } else {
        // reallocate failed... expensive fallback
        memset(dbs->table, 0, dbs->max_size * 8);
    }

    if (mmap(dbs->bitmap1, dbs->max_size / (512*8), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void*)-1) {
#if USE_HWLOC
        hwloc_set_area_membind(topo, dbs->bitmap1, dbs->max_size / (512*8), hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_INTERLEAVE, 0);
#endif
    } else {
        memset(dbs->bitmap1, 0, dbs->max_size / (512*8));
    }

    if (mmap(dbs->bitmap2, dbs->max_size / 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void*)-1) {
#if USE_HWLOC
        hwloc_set_area_membind(topo, dbs->bitmap2, dbs->max_size / 8, hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_FIRSTTOUCH, 0);
#endif
    } else {
        memset(dbs->bitmap2, 0, dbs->max_size / 8);
    }

    TOGETHER(llmsset_reset_region);
}

int
llmsset_is_marked(const llmsset_t dbs, uint64_t index)
{
    volatile uint64_t *ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    return (*ptr & mask) ? 1 : 0;
}

int
llmsset_mark(const llmsset_t dbs, uint64_t index)
{
    volatile uint64_t *ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    for (;;) {
        uint64_t v = *ptr;
        if (v & mask) return 0;
        if (cas(ptr, v, v|mask)) return 1;
    }
}

VOID_TASK_3(llmsset_rehash_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 1024) {
        size_t split = count/2;
        SPAWN(llmsset_rehash_par, dbs, first, split);
        CALL(llmsset_rehash_par, dbs, first + split, count - split);
        SYNC(llmsset_rehash_par);
    } else {
        uint64_t *ptr = dbs->bitmap2 + (first / 64);
        uint64_t mask = 0x8000000000000000LL >> (first & 63);
        for (size_t k=0; k<count; k++) {
            if (*ptr & mask) llmsset_rehash_bucket(dbs, first+k);
            mask >>= 1;
            if (mask == 0) {
                ptr++;
                mask = 0x8000000000000000LL;
            }
        }
    }
}

VOID_TASK_IMPL_1(llmsset_rehash, llmsset_t, dbs)
{
    CALL(llmsset_rehash_par, dbs, 0, dbs->table_size);
}

TASK_3(size_t, llmsset_count_marked_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 1024) {
        size_t split = count/2;
        SPAWN(llmsset_count_marked_par, dbs, first, split);
        size_t right = CALL(llmsset_count_marked_par, dbs, first + split, count - split);
        size_t left = SYNC(llmsset_count_marked_par);
        return left + right;
    } else {
        size_t result = 0;
        uint64_t *ptr = dbs->bitmap2 + (first / 64);
        uint64_t mask = 0x8000000000000000LL >> (first & 63);
        for (size_t k=0; k<count; k++) {
            if (*ptr & mask) result += 1;
            mask >>= 1;
            if (mask == 0) {
                ptr++;
                mask = 0x8000000000000000LL;
            }
        }
        return result;
    }
}

TASK_IMPL_1(size_t, llmsset_count_marked, llmsset_t, dbs)
{
    return CALL(llmsset_count_marked_par, dbs, 0, dbs->table_size);
}

void
llmsset_set_ondead(const llmsset_t dbs, llmsset_dead_cb cb, void* ctx)
{
    dbs->dead_cb = cb;
    dbs->dead_ctx = ctx;
}

void
llmsset_notify_ondead(const llmsset_t dbs, uint64_t index)
{
    volatile uint64_t *ptr = dbs->bitmap3 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    for (;;) {
        uint64_t v = *ptr;
        if (v & mask) return;
        if (cas(ptr, v, v|mask)) return;
    }
}

VOID_TASK_3(llmsset_notify_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 1024) {
        size_t split = count/2;
        SPAWN(llmsset_notify_par, dbs, first, split);
        CALL(llmsset_notify_par, dbs, first + split, count - split);
        SYNC(llmsset_notify_par);
    } else {
        for (size_t k=first; k<first+count; k++) {
            volatile uint64_t *ptr2 = dbs->bitmap2 + (k/64);
            volatile uint64_t *ptr3 = dbs->bitmap3 + (k/64);
            uint64_t mask = 0x8000000000000000LL >> (k&63);

            // if not filled but has notify
            if ((*ptr2 & mask) == 0 && (*ptr3 & mask)) {
                if (WRAP(dbs->dead_cb, dbs->dead_ctx, k)) {
                    // keep it
                    for (;;) {
                        uint64_t v = *ptr2;
                        if (cas(ptr2, v, v|mask)) break;
                    }
                } else {
                    // unnotify it
                    for (;;) {
                        uint64_t v = *ptr3;
                        if (cas(ptr3, v, v&(~mask))) break;
                    }
                }
            }
        }
    }
}

VOID_TASK_IMPL_1(llmsset_notify_all, llmsset_t, dbs)
{
    if (dbs->dead_cb == NULL) return;
    CALL(llmsset_notify_par, dbs, 0, dbs->table_size);
}
