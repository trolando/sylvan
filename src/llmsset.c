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

/*
 *  1 bit  for data-filled
 *  1 bit  for hash-filled
 * 40 bits for the index
 * 22 bits for the hash
 */
#define DFILLED    ((uint64_t)0x8000000000000000)
#define HFILLED    ((uint64_t)0x4000000000000000)
#define DNOTIFY    ((uint64_t)0x2000000000000000)
#define MASK_INDEX ((uint64_t)0x000000ffffffffff)
#define MASK_HASH  ((uint64_t)0x1fffff0000000000)

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

DECLARE_THREAD_LOCAL(insert_index, uint64_t);

VOID_TASK_1(llmsset_init_worker, llmsset_t, dbs)
{
    // yes, ugly. for now, we use a global thread-local value.
    // that is a problem with multiple tables.
    INIT_THREAD_LOCAL(insert_index);
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t);
    // take some start place
    insert_index = (dbs->table_size * LACE_WORKER_ID)/lace_workers();
    SET_THREAD_LOCAL(insert_index, insert_index);
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

/*
 * Note: garbage collection during lookup strictly forbidden
 * insert_index points to a starting point and is updated.
 */
uint64_t
llmsset_lookup(const llmsset_t dbs, const uint64_t a, const uint64_t b, int* created)
{
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t);

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

            if (!(v & HFILLED)) goto phase2;

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

    return 0; // failed to find empty spot

    uint64_t d_idx;
    uint64_t *d_ptr;
phase2:
    d_idx = insert_index;

    int count=0;
    for (;;) {
        if (count >= 2048) return 0; /* come on, just gc */
#if LLMSSET_MASK
        d_idx &= dbs->mask; // sanitize...
#else
        d_idx %= dbs->table_size; // sanitize...
#endif
        while (d_idx <= 1) d_idx++; // do not use bucket 0,1 for data
        volatile uint64_t *ptr = dbs->table + d_idx;
        uint64_t h = *ptr;
        if (h & DFILLED) {
            if ((++count & 127) == 0) {
                // random d_idx (lcg, and a shift)
                d_idx = 2862933555777941757ULL * d_idx + 3037000493ULL;
                d_idx ^= d_idx >> 32;
            } else {
                d_idx++;
            }
        } else if (cas(ptr, h, h|DFILLED)) {
            d_ptr = ((uint64_t*)dbs->data) + 2*d_idx;
            d_ptr[0] = a;
            d_ptr[1] = b;
            insert_index = d_idx;
            SET_THREAD_LOCAL(insert_index, insert_index);
            break;
        } else {
            d_idx++;
        }
    }

    sylvan_stats_add(LLMSSET_PHASE2, count);

    // data has been inserted!
    uint64_t mask = hash | d_idx | HFILLED;

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

            if (!(v & HFILLED)) {
                uint64_t new_v = (v&(DFILLED|DNOTIFY)) | mask;
                if (!cas(bucket, v, new_v)) goto phase2_restart;
                *created = 1;
                return d_idx;
            }

            if (hash == (v & MASK_HASH)) {
                uint64_t d2_idx = v & MASK_INDEX;
                register uint64_t *d2_ptr = ((uint64_t*)dbs->data) + 2*d2_idx;
                if (d2_ptr[0] == a && d2_ptr[1] == b) {
                    volatile uint64_t *ptr = dbs->table + d_idx;
                    uint64_t h = *ptr;
                    while (!cas(ptr, h, h&~(DFILLED|DNOTIFY))) { h = *ptr; } // uninsert data
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
    uint64_t mask = (hash_rehash & MASK_HASH) | d_idx | HFILLED;

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
            if (v & HFILLED) continue;
            if (cas(bucket, v, mask | (v&(DFILLED|DNOTIFY)))) return 1;
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
        fprintf(stderr, "Unable to allocate memory!\n");
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

    if (initial_size < HASH_PER_CL) {
        fprintf(stderr, "llmsset_create: initial_size too small!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    llmsset_set_size(dbs, initial_size);

    /* This implementation of "resizable hash table" allocates the max_size table in virtual memory,
       but only uses the "actual size" part in real memory */

#if USE_HWLOC
    dbs->table = (uint64_t*)hwloc_alloc_membind(topo, dbs->max_size * sizeof(uint64_t), hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_INTERLEAVE, 0);
    dbs->data = (uint8_t*)hwloc_alloc_membind(topo, dbs->max_size * 2 * sizeof(uint64_t), hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_FIRSTTOUCH, 0);
#else
    dbs->table = (uint64_t*)mmap(0, dbs->max_size * sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (dbs->table == (uint64_t*)-1) { fprintf(stderr, "Unable to allocate memory!"); exit(1); }
    dbs->data = (uint8_t*)mmap(0, dbs->max_size * 16, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (dbs->data == (uint8_t*)-1) { fprintf(stderr, "Unable to allocate memory!"); exit(1); }
#endif

    dbs->dead_cb = NULL;

    LACE_ME;
    TOGETHER(llmsset_init_worker, dbs);

    /* self-test */
    /* if (lace_workers() > 1) {
        size_t first, count, expected=0, i;
        for (i=0; i<lace_workers(); i++) {
            llmsset_compute_multi(dbs, i, lace_workers(), &first, &count);
            assert(expected == first);
            expected += count;
        }
        assert(expected == dbs->table_size);
    }*/

    return dbs;
}

void
llmsset_free(llmsset_t dbs)
{
#if USE_HWLOC
    hwloc_free(topo, dbs->table, dbs->max_size * sizeof(uint64_t));
    hwloc_free(topo, dbs->data, dbs->max_size * 16);
#else
    munmap(dbs->table, dbs->max_size * sizeof(uint64_t));
    munmap(dbs->data, dbs->max_size * 16);
#endif
    free(dbs);
}

static void
llmsset_compute_multi(const llmsset_t dbs, size_t my_id, size_t n_workers, size_t *_first_entry, size_t *_entry_count)
{
    const size_t entries_total    = dbs->table_size;
    const size_t cachelines_total = (entries_total * sizeof(uint64_t) + LINE_SIZE - 1) / LINE_SIZE;
    const size_t cachelines_each  = (cachelines_total + n_workers - 1) / n_workers;
    const size_t entries_each     = cachelines_each * LINE_SIZE / sizeof(uint64_t);
    const size_t first_entry      = my_id * entries_each;
    const size_t cap_total        = dbs->table_size - first_entry;
    if (first_entry > dbs->table_size) {
        *_first_entry = dbs->table_size;
        *_entry_count = 0;
    } else {
        *_first_entry = first_entry;
        *_entry_count = entries_each < cap_total ? entries_each : cap_total;
    }
}

VOID_TASK_3(llmsset_clear_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 1024) {
        SPAWN(llmsset_clear_par, dbs, first, count/2);
        CALL(llmsset_clear_par, dbs, first+count/2, count-count/2);
        SYNC(llmsset_clear_par);
    } else {
        uint64_t *ptr = dbs->table+first;
        for (size_t k=0; k<count; k++) {
            if (*ptr) *ptr &= DNOTIFY;
            ptr++;
        }
    }
}

VOID_TASK_IMPL_1(llmsset_clear, llmsset_t, dbs)
{
    if (dbs->dead_cb != NULL) {
        // slow clear
        CALL(llmsset_clear_par, dbs, 0, dbs->table_size);
    } else {
        // a bit silly, but this works just fine, and does not require writing 0 everywhere...
#if USE_HWLOC
        hwloc_free(topo, dbs->table, dbs->max_size * sizeof(uint64_t));
        dbs->table = (uint64_t*)hwloc_alloc_membind(topo, dbs->max_size * sizeof(uint64_t), hwloc_topology_get_allowed_cpuset(topo), HWLOC_MEMBIND_INTERLEAVE, 0);
#else
        munmap(dbs->table, dbs->max_size * sizeof(uint64_t));
        dbs->table = (uint64_t*)mmap(0, dbs->max_size * sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
#endif
        if (dbs->table == (uint64_t*)-1) { fprintf(stderr, "Unable to allocate memory!"); exit(1); }
    }
}

int
llmsset_is_marked(const llmsset_t dbs, uint64_t index)
{
    uint64_t v = dbs->table[index];
    return v & DFILLED ? 1 : 0;
}

int
llmsset_mark(const llmsset_t dbs, uint64_t index)
{
    uint64_t v = dbs->table[index];
    if (v & DFILLED) return 0;
    dbs->table[index] |= DFILLED;
    return 1;
}

VOID_TASK_3(llmsset_rehash_range, llmsset_t, dbs, size_t, first, size_t, count)
{
    while (count--) {
        if (dbs->table[first]&DFILLED) llmsset_rehash_bucket(dbs, first);
        first++;
    }
}

VOID_TASK_1(llmsset_rehash_task, llmsset_t, dbs)
{
    /* retrieve first entry and number of bucket */
    size_t first, count;
    llmsset_compute_multi(dbs, LACE_WORKER_ID, lace_workers(), &first, &count);

    // now proceed in blocks of 1024 buckets
    size_t spawn_count = 0;
    while (count > 1024) {
        SPAWN(llmsset_rehash_range, dbs, first, 1024);
        first += 1024;
        count -= 1024;
        spawn_count++;
    }
    if (count > 0) {
        CALL(llmsset_rehash_range, dbs, first, count);
    }
    while (spawn_count--) {
        SYNC(llmsset_rehash_range);
    }
}

VOID_TASK_IMPL_1(llmsset_rehash, llmsset_t, dbs)
{
    TOGETHER(llmsset_rehash_task, dbs);
    /* for now, call init_worker to reset "insert_index" */
    TOGETHER(llmsset_init_worker, dbs);
}

TASK_3(size_t, llmsset_count_marked_range, llmsset_t, dbs, size_t, first, size_t, count)
{
    size_t result = 0;
    while (count--) {
        if (dbs->table[first++] & DFILLED) result++;
    }
    return result;
}

TASK_IMPL_1(size_t, llmsset_count_marked, llmsset_t, dbs)
{
    size_t spawn_count = 0;
    size_t count = dbs->table_size, first=0;
    while (count > 4096) {
        SPAWN(llmsset_count_marked_range, dbs, first, 4096);
        first += 4096;
        count -= 4096;
        spawn_count++;
    }
    size_t marked_count = 0;
    if (count > 0) {
        marked_count = CALL(llmsset_count_marked_range, dbs, first, count);
    }
    while (spawn_count--) {
        marked_count += SYNC(llmsset_count_marked_range);
    }
    return marked_count;
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
    for (;;) {
        uint64_t v = dbs->table[index];
        if (v & DNOTIFY) return;
        if (cas(&dbs->table[index], v, v|DNOTIFY)) return;
    }
}

VOID_TASK_3(llmsset_notify_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 1024) {
        SPAWN(llmsset_notify_par, dbs, first, count/2);
        CALL(llmsset_notify_par, dbs, first+count/2, count-count/2);
        SYNC(llmsset_notify_par);
    } else {
        uint64_t *ptr = dbs->table+first;
        for (size_t k=first; k<first+count; k++) {
            uint64_t v = *ptr;
            if (!(v & DFILLED)) {
                if (v & DNOTIFY) {
                    if (WRAP(dbs->dead_cb, dbs->dead_ctx, k)) {
                        // mark it
                        *ptr = DNOTIFY | DFILLED;
                    } else {
                        *ptr = 0;
                    }
                }
            }
            ptr++;
        }
    }
}

VOID_TASK_IMPL_1(llmsset_notify_all, llmsset_t, dbs)
{
    if (dbs->dead_cb == NULL) return;
    CALL(llmsset_notify_par, dbs, 0, dbs->table_size);
}
