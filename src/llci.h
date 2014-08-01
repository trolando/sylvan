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

#include <stdlib.h>
#include <stdio.h>  // for printf
#include <stdint.h> // for uint32_t etc
#include <string.h> // for memcpy
#include <assert.h> // for assert
#include <sys/mman.h> // for mmap

#include <atomics.h>

#if USE_NUMA
#include <numa_tools.h>
#endif

#ifndef LLCACHE_INLINE_H
#define LLCACHE_INLINE_H

#ifndef LLCI_KEYSIZE
#error No LLCI_KEYSIZE set!
#endif

#ifndef LLCI_DATASIZE
#error No LLCI_DATASIZE set!
#endif

#define LLCI_PDS \
    (((LLCI_DATASIZE) <= 2) ? (LLCI_DATASIZE) : \
    ((LLCI_DATASIZE) <= 4) ? 4 : \
    ((LLCI_DATASIZE) <= 8) ? 8 : (((LLCI_DATASIZE)+15)&(~15)))

typedef struct llci {
    size_t             cache_size;
    size_t             mask;         // size-1
    uint32_t           *table;       // table with hashes
    uint8_t            *data;        // table with data
    size_t             f_size;
} *llci_t;

#define LLCI_EMPTY         ((uint32_t) 0x00000000)
#define LLCI_LOCK          ((uint32_t) 0x80000000)
#define LLCI_MASK          ((uint32_t) 0x7FFFFFFF)

// We assume that LINE_SIZE is a multiple of 4, which is reasonable.
#define LLCI_HASH_PER_CL   ((LINE_SIZE) / 4)
#define LLCI_CL_MASK       ((uint32_t)(~(LLCI_HASH_PER_CL-1)))
#define LLCI_CL_MASK_R     ((uint32_t)(LLCI_HASH_PER_CL-1))

static int __attribute__((unused)) llci_get_tag(const llci_t dbs, void *data)
{
    size_t hash = (size_t)hash_mul(data, LLCI_KEYSIZE);
    const size_t idx = hash & dbs->mask;
    const size_t data_idx = idx * LLCI_PDS;

    volatile uint32_t * const bucket = &dbs->table[idx];
    const uint32_t v = *bucket;

    hash >>= 32;

    // abort if locked or has a different hash or has a tag of 0
    if (v & LLCI_LOCK || ((v ^ hash) & 0x7FFFF000) || (v & 0x00000FFF) == 0) return 0;
    // abort if key different
    if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) != 0) return 0;

    memcpy(&((uint8_t*)data)[LLCI_KEYSIZE], &dbs->data[data_idx+LLCI_KEYSIZE], LLCI_DATASIZE-LLCI_KEYSIZE);
    return (*bucket == v) ? 1 : 0;
}

static int __attribute__((unused)) llci_put_tag(const llci_t dbs, void *data)
{
    size_t hash = (size_t)hash_mul(data, LLCI_KEYSIZE);
    const size_t idx = hash & dbs->mask;
    const size_t data_idx = idx * LLCI_PDS;

    volatile uint32_t * const bucket = &dbs->table[idx];
    const uint32_t v = *bucket;

    hash >>= 32;

    // abort if locked or exists
    if (v & LLCI_LOCK) return 0;
    if (!((v ^ hash) & 0x7FFFF000)) {
        if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) == 0) {
            // Probably exists. (No lock)
            return 0;
        }
    }

    uint32_t hh = (v+1) & 0x00000FFF;
    if (!hh) hh++; // Skip tag 0
    hh |= (hash & 0x7FFFF000);

    if (!cas(bucket, v, hh|LLCI_LOCK)) return 0;

    memcpy(&dbs->data[data_idx], data, LLCI_DATASIZE);
    *bucket = hh;
    return 1;
}

static int __attribute__((unused)) llci_get(const llci_t dbs, void *data)
{
    size_t hash = hash_mul(data, LLCI_KEYSIZE);
    const size_t idx = hash & dbs->mask;
    const size_t data_idx = idx * LLCI_PDS;

    volatile uint32_t * const bucket = &dbs->table[idx];
    register uint32_t v = *bucket & LLCI_MASK;

    hash >>= 32;
    hash &= LLCI_MASK;
    if (hash == 0) hash++;

    // if locked or if different hash...
    if (v != hash) return 0;

    // acquire lock
    if (!cas(bucket, v, v|LLCI_LOCK)) return 0;

    if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) == 0) {
        memcpy(&((uint8_t*)data)[LLCI_KEYSIZE], &dbs->data[data_idx+LLCI_KEYSIZE], LLCI_DATASIZE-LLCI_KEYSIZE);
        *bucket = v;
        return 1;
    } else {
        *bucket = v;
        return 0;
    }
}

static int __attribute__((unused)) llci_get_restart(const llci_t dbs, void *data)
{
    size_t hash = hash_mul(data, LLCI_KEYSIZE);
    const size_t idx = hash & dbs->mask;
    const size_t data_idx = idx * LLCI_PDS;

    volatile uint32_t * const bucket = &dbs->table[idx];

    hash >>= 32;
    hash &= LLCI_MASK;
    if (hash == 0) hash++;

    while (1) {
        const uint32_t v = *bucket;
        // if different hash, abort
        if ((v & LLCI_MASK) != hash) return 0;
        if (v & LLCI_LOCK) continue;

        // acquire lock
        if (!cas(bucket, v, v|LLCI_LOCK)) continue; // atomic restart

        if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) == 0) {
            memcpy(&((uint8_t*)data)[LLCI_KEYSIZE], &dbs->data[data_idx+LLCI_KEYSIZE], LLCI_DATASIZE-LLCI_KEYSIZE);
            *bucket = v;
            return 1;
        } else {
            *bucket = v;
            return 0;
        }
    }
}

static int __attribute__((unused)) llci_get_seq(const llci_t dbs, void *data)
{
    size_t hash = hash_mul(data, LLCI_KEYSIZE);
    const size_t idx = hash & dbs->mask;
    const size_t data_idx = idx * LLCI_PDS;

    volatile uint32_t * const bucket = &dbs->table[idx];
    const uint32_t v = *bucket & LLCI_MASK;

    hash >>= 32;
    hash &= LLCI_MASK;
    if (hash == 0) hash++;

    if (v != hash) return 0;

    if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) != 0) return 0;

    // Found existing
    memcpy(&((uint8_t*)data)[LLCI_KEYSIZE], &dbs->data[data_idx+LLCI_KEYSIZE], LLCI_DATASIZE-LLCI_KEYSIZE);
    return 1;
}

static int __attribute__((unused)) llci_put(const llci_t dbs, void *data)
{
    size_t hash = hash_mul(data, LLCI_KEYSIZE);
    const size_t idx = hash & dbs->mask;
    const size_t data_idx = idx * LLCI_PDS;

    volatile uint32_t * const bucket = &dbs->table[idx];
    register uint32_t v = *bucket;

    hash >>= 32;
    hash &= LLCI_MASK;
    if (hash == 0) hash++;

    if (v & LLCI_LOCK) return 0; // Not added

    if (v == hash) {
        if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) == 0) {
            // Probably exists. (No lock)
            return 0;
        }
    }

    if (!cas(bucket, v, hash|LLCI_LOCK)) return 0;
    memcpy(&dbs->data[data_idx], data, LLCI_DATASIZE);
    *bucket = hash;
    return 1; // Added
}

static int __attribute__((unused)) llci_put_seq(const llci_t dbs, void *data)
{
    size_t hash = hash_mul(data, LLCI_KEYSIZE);
    const size_t idx = hash & dbs->mask;
    const size_t data_idx = idx * LLCI_PDS;

    volatile uint32_t * const bucket = &dbs->table[idx];
    register uint32_t v = *bucket;

    hash >>= 32;
    hash &= LLCI_MASK;
    if (hash == 0) hash++;

    if (v == hash) {
        if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) == 0) {
            return 0;
        }
    }

    memcpy(&dbs->data[data_idx], data, LLCI_DATASIZE);
    *bucket = hash;
    return 1; // Added
}

static inline unsigned llci_next_pow2(unsigned x)
{
    if (x <= 2) return x;
    return (1ULL << 32) >> __builtin_clz(x - 1);
}

static inline llci_t llci_create(size_t cache_size)
{
    llci_t dbs;
    assert(posix_memalign((void**)&dbs, LINE_SIZE, sizeof(struct llci)) == 0);

    assert(LLCI_KEYSIZE <= LLCI_DATASIZE);

    if (cache_size < LLCI_HASH_PER_CL) cache_size = LLCI_HASH_PER_CL;

    // Cache size must be a power of 2
    if (llci_next_pow2(cache_size) != cache_size) {
        fprintf(stderr, "LLCI: Table size must be a power of 2!\n");
        exit(1);
    }

    dbs->cache_size = cache_size;
    dbs->mask = dbs->cache_size - 1;

    dbs->table = (uint32_t*)mmap(0, dbs->cache_size * sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    dbs->data = (uint8_t*)mmap(0, dbs->cache_size * LLCI_PDS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (dbs->table == (uint32_t*)-1 || dbs->data == (uint8_t*)-1) {
        fprintf(stderr, "LLCI: Unable to allocate memory!\n");
        exit(1);
    }

#if USE_NUMA
    size_t f_size=0;
    numa_interleave(dbs->table, dbs->cache_size * sizeof(uint32_t), &f_size);
    if (f_size % (sizeof(uint32_t)*(LINE_SIZE))) {
        fprintf(stderr, "LLCI: f_size not properly aligned to processor cache lines!");
    }
    dbs->f_size = (f_size /= sizeof(uint32_t));
    f_size *= LLCI_PDS;
    numa_interleave(dbs->data, dbs->cache_size * LLCI_PDS, &f_size);
#endif

    return dbs;
}

static inline void __attribute__((unused))
llci_clear_partial(const llci_t dbs, size_t first, size_t count)
{
    if (/*first >= 0 &&*/ first < dbs->cache_size) {
        if (count > dbs->cache_size - first) count = dbs->cache_size - first;
        memset(dbs->table + first, 0, 4 * count);
    }
}

/*
 * Use llci_clear_multi when you have multiple workers to quickly clear the memoization table...
 */
static void __attribute__((unused))
llci_clear_multi(const llci_t dbs, size_t my_id, size_t n_workers)
{
#if USE_NUMA
    size_t node, node_index, index, total;
    numa_worker_info(my_id, &node, &node_index, &index, &total);
    // we only clear that of our own node...
    // note f_size is the number of buckets, and is aligned to page_size...
    // with page_size 4096 bytes, f_size is aligned on 1024 buckets = 16 cachelines (typically)
    size_t cachelines_total = dbs->f_size / LLCI_HASH_PER_CL;
    size_t cachelines_each  = (cachelines_total + total - 1) / total;
    size_t first_line       = node_index * dbs->f_size / LLCI_HASH_PER_CL + index * cachelines_each;
    size_t max_lines        = cachelines_total - index * cachelines_each;
    if (max_lines > 0) {
        size_t count = max_lines > cachelines_each ? cachelines_each : max_lines;
        // Note that llci_clear_partial will fix count if overflow...
        llci_clear_partial(dbs, first_line * LLCI_HASH_PER_CL, count * LLCI_HASH_PER_CL);
    }
    (void)n_workers;
#else
    size_t cachelines_total = (dbs->cache_size  + LLCI_HASH_PER_CL - 1) / LLCI_HASH_PER_CL;
    size_t cachelines_each  = (cachelines_total + n_workers        - 1) / n_workers;
    size_t first            = my_id * cachelines_each * LLCI_HASH_PER_CL;
    // Note that llci_clear_partial will fix count if overflow...
    llci_clear_partial(dbs, first, cachelines_each * LLCI_HASH_PER_CL);
#endif
}

static void __attribute__((unused)) llci_clear(const llci_t dbs)
{
    llci_clear_partial(dbs, 0, dbs->cache_size);
}

static inline void llci_free(const llci_t dbs)
{
    munmap(dbs->table, dbs->cache_size * sizeof(uint32_t));
    munmap(dbs->data, dbs->cache_size * LLCI_PDS);
    free(dbs);
}

static inline void __attribute__((unused)) llci_print_size(const llci_t dbs, FILE *f)
{
    fprintf(f, "LLCI table with %ld entries; hash array = %ld bytes; data array = %ld bytes",
        dbs->cache_size, dbs->cache_size * 4, dbs->cache_size * LLCI_PDS);
}

#endif
