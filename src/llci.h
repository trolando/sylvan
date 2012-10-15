#include "config.h"

#include <stdlib.h>
#include <stdio.h>  // for printf
#include <stdint.h> // for uint32_t etc
#include <string.h> // for memcpy
#include <assert.h> // for assert
#include <sys/mman.h> // for mmap

#include "atomics.h"

#ifdef HAVE_NUMA_H
#include "numa_tools.h"
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

// 64-bit hashing function, http://www.locklessinc.com (hash_mul.s)
unsigned long long hash_mul(const void* data, unsigned long long len);

typedef struct llci
{
    size_t             cache_size;  
    uint32_t           mask;         // size-1
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

static int __attribute__((unused)) llci_get(const llci_t dbs, void *data) 
{
    uint32_t hash = (uint32_t)(hash_mul(data, LLCI_KEYSIZE) & LLCI_MASK);
    if (hash == 0) hash++; // Do not use bucket 0.

    uint32_t idx = hash & dbs->mask;

    volatile uint32_t *bucket = &dbs->table[idx];
    register uint32_t v = *bucket & LLCI_MASK;

    if (v != hash) return 0;

    if (!cas(bucket, v, v|LLCI_LOCK)) return 0;

    // Lock acquired, compare
    const size_t data_idx = idx * LLCI_PDS;
    if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) == 0) {
        // Found existing
        memcpy(&((uint8_t*)data)[LLCI_KEYSIZE], &dbs->data[data_idx+LLCI_KEYSIZE], LLCI_DATASIZE-LLCI_KEYSIZE);
        *bucket = v;
        return 1;                    
    } else {
        // Did not match, release bucket again
        *bucket = v;
        return 0;
    }
}

static int __attribute__((unused)) llci_get_restart(const llci_t dbs, void *data) 
{
    uint32_t hash = (uint32_t)(hash_mul(data, LLCI_KEYSIZE) & LLCI_MASK);
    if (hash == 0) hash++; // Do not use bucket 0.

    uint32_t idx = hash & dbs->mask;

    volatile uint32_t *bucket = &dbs->table[idx];

    while (1) {
        register uint32_t v = *bucket;
        register uint32_t vh = v & LLCI_MASK;
        if (vh != hash) return 0;
        if (v == vh) { // v&LOCK == 0
            if (!cas(bucket, vh, vh|LLCI_LOCK)) continue; // Atomic restart
            
            // Lock acquired, compare
            const size_t data_idx = idx * LLCI_PDS;
            const uint8_t *bdata = &dbs->data[data_idx];
            if (memcmp(bdata, data, LLCI_KEYSIZE) == 0) {
                // Found existing
                memcpy(&((uint8_t*)data)[LLCI_KEYSIZE], &bdata[LLCI_KEYSIZE], LLCI_DATASIZE-LLCI_KEYSIZE);
                *bucket = vh;
                return 1;                    
            } else {
                // Did not match, release bucket again
                *bucket = vh;
                return 0;
            }
        }
    }
}

static int __attribute__((unused)) llci_get_seq(const llci_t dbs, void *data) 
{
    uint32_t hash = (uint32_t)(hash_mul(data, LLCI_KEYSIZE) & LLCI_MASK);
    if (hash == 0) hash++; // Do not use bucket 0.

    uint32_t idx = hash & dbs->mask;

    volatile uint32_t *bucket = &dbs->table[idx];
    register uint32_t v = *bucket & LLCI_MASK;

    if (v != hash) return 0;

    // Lock acquired, compare
    const size_t data_idx = idx * LLCI_PDS;
    if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) != 0) return 0;

    // Found existing
    memcpy(&((uint8_t*)data)[LLCI_KEYSIZE], &dbs->data[data_idx+LLCI_KEYSIZE], LLCI_DATASIZE-LLCI_KEYSIZE);
    return 1;                    
}

static int __attribute__((unused)) llci_put(const llci_t dbs, void *data) 
{
    uint32_t hash = (uint32_t)(hash_mul(data, LLCI_KEYSIZE) & LLCI_MASK);
    if (hash == 0) hash++; // Avoid 0.
    
    register uint32_t idx = hash & dbs->mask; // fast version of hash & tableSize
    register size_t data_idx = idx * LLCI_PDS;

    register volatile uint32_t *bucket = &dbs->table[idx];
    register uint32_t v = *bucket;

    if (v & LLCI_LOCK) return 0; // Not added
    v &= LLCI_MASK;

    if (v == LLCI_EMPTY) {
        if (cas(bucket, LLCI_EMPTY, hash|LLCI_LOCK)) {
            memcpy(&dbs->data[data_idx], data, LLCI_DATASIZE);
            *bucket = hash;
            return 1; // Added
        } else {
            return 0;
        }
    }

    if (v == hash) {
        if (memcmp(&dbs->data[data_idx], data, LLCI_KEYSIZE) == 0) {
            // Probably exists. (No lock)
            return 0;
        }
    }

    if (cas(bucket, v, hash|LLCI_LOCK)) {
        memcpy(&dbs->data[data_idx], data, LLCI_DATASIZE);
        *bucket = hash;
        return 1; // Added
    } else {
        // Claim failed, never mind
        return 0;
    }
}

static int __attribute__((unused)) llci_put_seq(const llci_t dbs, void *data) 
{
    uint32_t hash = (uint32_t)(hash_mul(data, LLCI_KEYSIZE) & LLCI_MASK);
    if (hash == 0) hash++; // Avoid 0.
    
    register uint32_t idx = hash & dbs->mask; // fast version of hash & tableSize
    register size_t data_idx = idx * LLCI_PDS;

    register volatile uint32_t *bucket = &dbs->table[idx];
    register uint32_t v = *bucket;

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
    posix_memalign((void**)&dbs, LINE_SIZE, sizeof(struct llci));
    
    assert(LLCI_KEYSIZE <= LLCI_DATASIZE);

    if (cache_size < LLCI_HASH_PER_CL) cache_size = LLCI_HASH_PER_CL;

    // Cache size must be a power of 2
    assert(llci_next_pow2(cache_size) == cache_size);
    dbs->cache_size = cache_size;

    dbs->mask = dbs->cache_size - 1;

    dbs->table = mmap(0, dbs->cache_size * sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    dbs->data = mmap(0, dbs->cache_size * LLCI_PDS, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (dbs->table == (uint32_t*)-1 || dbs->data == (uint8_t*)-11) { 
      fprintf(stderr, "Unable to allocate memory!"); 
      exit(1);
    }

#ifdef HAVE_NUMA_H
    size_t f_size=0;
    numa_interleave(dbs->table, dbs->cache_size * sizeof(uint32_t), &f_size);
    dbs->f_size = (f_size /= sizeof(uint32_t));
    f_size *= LLCI_PDS;
    numa_interleave(dbs->data, dbs->cache_size * LLCI_PDS, &f_size);
#endif

    return dbs;
}

static inline void __attribute__((unused)) 
llci_clear_partial(const llci_t dbs, size_t first, size_t count) 
{
    if (first >= 0 && first < dbs->cache_size) {
        if (count + first > dbs->cache_size) count = dbs->cache_size - first;
        memset(&dbs->table[first], 0, 4 * count);
    }
}

/*
 * Use llci_clear_multi when you have multiple workers to quickly clear the memoization table...
 */
static void __attribute__((unused))
llci_clear_multi(const llci_t dbs, size_t my_id, size_t n_workers)
{
#ifdef HAVE_NUMA_H
    int node, node_index, index, total;
    numa_worker_info(my_id, &node, &node_index, &index, &total);
    // we only clear that of our own node...
    size_t cachelines_total = (dbs->f_size      + LINE_SIZE - 1) / (LINE_SIZE);
    size_t cachelines_each  = (cachelines_total + total     - 1) / total;
    size_t first            = node_index * dbs->f_size + index * cachelines_each * LINE_SIZE;
    size_t max              = cachelines_total - index * cachelines_each;
    if (max > 0) {
        size_t count = max > cachelines_each ? cachelines_each : max;
        llci_clear_partial(dbs, first / 4, count / 4);
    }
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
    fprintf(f, "Hash: %ld * 4 = %ld bytes; Data: %ld * %ld = %ld bytes",
        dbs->cache_size, dbs->cache_size * 4, dbs->cache_size, 
        LLCI_PDS, dbs->cache_size * LLCI_PDS);
}

#endif
