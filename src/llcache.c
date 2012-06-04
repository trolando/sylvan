#include <stdlib.h>
#include <stdio.h>  // for printf
#include <stdint.h> // for uint32_t etc
#include <string.h> // for memcopy
#include <assert.h> // for assert

#include "config.h"

#include "atomics.h"
#include "llcache.h"
#include "memxchg.h"

struct llcache
{
    size_t             padded_data_length;
    size_t             key_length;
    size_t             data_length;
    size_t             cache_size;  
    uint32_t           mask;         // size-1
    uint32_t           *_table;      // table with hashes
    uint8_t            *_data;       // table with data
    uint32_t           *table;       // table with hashes
    uint8_t            *data;        // table with data
    llcache_delete_f   cb_delete;    // delete function (callback pre-delete)
    void               *cb_data;
};

static const uint32_t  EMPTY = 0x00000000;
static const uint32_t  LOCK  = 0x80000000;
static const uint32_t  MASK  = 0x7FFFFFFF;

// 64-bit hashing function, http://www.locklessinc.com (hash_mul.s)
unsigned long long hash_mul(const void* data, unsigned long long len);

static const int       HASH_PER_CL = ((LINE_SIZE) / 4);
static const uint32_t  CL_MASK     = ~(((LINE_SIZE) / 4) - 1); 
static const uint32_t  CL_MASK_R   = ((LINE_SIZE) / 4) - 1;

/* Example values with a LINE_SIZE of 64
 * HASH_PER_CL = 16
 * CL_MASK     = 0xFFFFFFF0
 * CL_MASK_R   = 0x0000000F
 */

/**
 * Calculate next index on a cache line walk
 * Returns 0 if the new index equals last
 */
static inline int next(uint32_t *cur, uint32_t last) 
{
    return (*cur = (*cur & CL_MASK) | ((*cur + 1) & CL_MASK_R)) != last;
}

inline void llcache_release(const llcache_t dbs, uint32_t index)
{
    dbs->table[index] &= ~LOCK;
}

inline int llcache_get(const llcache_t dbs, void *data)
{
    uint32_t index;
    int result = llcache_get_and_hold(dbs, data, &index);
    if (result) dbs->table[index] &= ~LOCK;
    return result;
}

inline int llcache_put(const llcache_t dbs, void *data)
{
    uint32_t index;
    int result = llcache_put_and_hold(dbs, data, &index);
    dbs->table[index] &= ~LOCK;
    return result;
}

int llcache_get_quicker(const llcache_t dbs, void *data)
{
    uint32_t hash = (uint32_t)(hash_mul(data, dbs->key_length) & MASK);
    if (hash == 0) hash++; // Do not use bucket 0.

    uint32_t idx = hash & dbs->mask;

    volatile uint32_t *bucket = &dbs->table[idx];
    register uint32_t v = *bucket & MASK;

    if (v != hash) return 0;

    if (!cas(bucket, v, v|LOCK)) return 0;

    // Lock acquired, compare
    const size_t data_idx = idx * dbs->padded_data_length;
    if (memcmp(&dbs->data[data_idx], data, dbs->key_length) == 0) {
        // Found existing
        memcpy(&data[dbs->key_length], &dbs->data[data_idx+dbs->key_length], dbs->data_length-dbs->key_length);
        *bucket = v;
        return 1;                    
    } else {
        // Did not match, release bucket again
        *bucket = v;
        return 0;
    }
}

int llcache_get_quicker_restart(const llcache_t dbs, void *data)
{
    uint32_t hash = (uint32_t)(hash_mul(data, dbs->key_length) & MASK);
    if (hash == 0) hash++; // Do not use bucket 0.
    uint32_t idx = hash & dbs->mask;

    volatile uint32_t *bucket = &dbs->table[idx];

    while (1) {
        register uint32_t v = *bucket;
        register uint32_t vh = v&MASK;
        if (vh != hash) {
            return 0;
        }
        if (v==vh) { // v&LOCK == 0
            if (cas(bucket, vh, vh|LOCK)) {
                // Lock acquired, compare
                const register size_t data_idx = idx * dbs->padded_data_length;
                const register uint8_t *bdata = &dbs->data[data_idx];
                if (memcmp(bdata, data, dbs->key_length) == 0) {
                    // Found existing
                    memcpy(&data[dbs->key_length], &bdata[dbs->key_length], dbs->data_length-dbs->key_length);
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
}

int llcache_put_quicker(const llcache_t dbs, void *data)
{
    uint32_t hash = (uint32_t)(hash_mul(data, dbs->key_length) & MASK);
    if (hash == 0) hash++; // Avoid 0.
    
    register uint32_t idx = hash & dbs->mask; // fast version of hash & tableSize
    register size_t data_idx = idx * dbs->padded_data_length;

    register volatile uint32_t *bucket = &dbs->table[idx];
    register uint32_t v = *bucket;

    if (v & LOCK) return 0; // Not added
    v &= MASK;

    if (v == EMPTY) {
        if (cas(bucket, EMPTY, hash|LOCK)) {
            memcpy(&dbs->data[data_idx], data, dbs->data_length);
            *bucket = hash;
            return 1; // Added
        } else {
            return 0;
        }
    } else if (v == hash) {
        if (memcmp(&dbs->data[data_idx], data, dbs->key_length) == 0) {
            // Probably exists. (No lock)
            return 0;
        }
    }

    if (cas(bucket, v, hash|LOCK)) {
        memxchg(&dbs->data[data_idx], data, dbs->data_length);
        *bucket = hash;
        return 2; // Overwritten
    } else {
        // Claim failed, never mind
        return 0;
    }
}

/**
 * This version uses the entire cacheline
 * However: it ignores locked buckets and does not recheck first, so 
 * false negatives are possible
 */
int llcache_get_relaxed(const llcache_t dbs, void *data)
{
    uint32_t hash = (uint32_t)(hash_mul(data, dbs->key_length) & MASK);

    if (hash == 0) hash++; // Do not use bucket 0.

    uint32_t f_idx = hash & dbs->mask;
    uint32_t idx;

    idx = f_idx;

    do {
        // do not use bucket 0
        if (idx == 0) continue;

        volatile uint32_t *bucket = &dbs->table[idx];
        register uint32_t v = *bucket;

        if (v & LOCK) continue; // skip locked (prevent expensive CAS)
        v &= MASK;

        if (v == EMPTY) return 0;

        if (v == hash) {
            if (cas(bucket, v, v|LOCK)) {
                // Lock acquired, compare
                const size_t data_idx = idx * dbs->padded_data_length;
                if (memcmp(&dbs->data[data_idx], data, dbs->key_length) == 0) {
                    // Found existing
                    memcpy(data, &dbs->data[data_idx], dbs->data_length);
                    *bucket = v;
                    return 1;                    
                } else {
                    // Did not match, release bucket again
                    *bucket = v;
                }
            } // else: CAS failed, ignore 
        }
    } while (next(&idx, f_idx));
  
    // If we are here, it is not in the cache line
    return 0;
}

/**
 * This version uses the entire cacheline
 * However: it ignores locked buckets and does not lock before memcmp.
 * False negatives are possible (resulting in duplicate insertion)
 * and false positives are possible (resulting in no insertion)
 */
int llcache_put_relaxed(const llcache_t dbs, void *data)
{
    uint32_t hash = (uint32_t)(hash_mul(data, dbs->key_length) & MASK);
    if (hash == 0) hash++; // Avoid 0.

    uint32_t f_idx = hash & dbs->mask;
    volatile uint32_t *f_bucket = &dbs->table[f_idx];

    uint32_t idx = f_idx;

    do {
        // do not use bucket 0
        if (idx == 0) continue;

        register volatile uint32_t *bucket = &dbs->table[idx];
        register uint32_t v = *bucket & MASK;

        if (v == EMPTY) {
            // EMPTY bucket!
            if (cas(bucket, EMPTY, hash|LOCK)) {
                register const size_t data_idx = idx * dbs->padded_data_length;
                // Claim successful!
                memcpy(&dbs->data[data_idx], data, dbs->data_length);
                *bucket = hash;
                return 1;
            }
        }

        if (v == hash) {
            register size_t data_idx = idx * dbs->padded_data_length;
            //if (memcmp(&dbs->data[data_idx], data, dbs->key_length) == 0) {
            if (memcmp(&dbs->data[data_idx], data, dbs->data_length) == 0) {
                // Probably found existing... (could be false positive)
                return 0;                    
            }
        }
    } while (next(&idx, f_idx));

    // If we are here, the cache line is full.
    // Claim first bucket
    const uint32_t v = *f_bucket;
    if (cas(f_bucket, v & MASK, hash | LOCK)) {
        register const size_t data_idx = f_idx * dbs->padded_data_length;
        memxchg(&dbs->data[data_idx], data, dbs->data_length);
        *f_bucket = hash;
        return 2;
    }

    // Claim failed, never mind
    return 0;
}


int llcache_get_and_hold(const llcache_t dbs, void *data, uint32_t *index) 
{
    uint32_t hash = (uint32_t)(hash_mul(data, dbs->key_length) & MASK);

    if (hash == 0) hash++; // blah. Just avoid 0, that's all.

    uint32_t f_idx = hash & dbs->mask;
    uint32_t idx;

    int only_check_first = 0;

restart_full:
    idx = f_idx;

    do {
        // do not use bucket 0
        if (idx == 0) continue;

        volatile uint32_t *bucket = &dbs->table[idx];

        register uint32_t v;

restart_bucket:
        v = *bucket;

        // Wait while locked...
        if (v & LOCK) {
            while ((v=*bucket) & LOCK) cpu_relax();
        }

        v &= MASK;

        if (v == EMPTY) return 0;

        if (v == hash) {
            if (cas(bucket, v, v|LOCK)) {
                // Lock acquired, compare
                const size_t data_idx = idx * dbs->padded_data_length;
                if (memcmp(&dbs->data[data_idx], data, dbs->key_length) == 0) {
                    // Found existing
                    memcpy(data, &dbs->data[data_idx], dbs->data_length);
                    *index = idx;
                    return 1;                    
                } else {
                    // Did not match, release bucket again
                    *bucket = v;
                }
            } else {
                // CAS failed
                goto restart_bucket;
            }
        }

        if (only_check_first) break;
    } while (next(&idx, f_idx));
  
    // If we are here, it is not in the cache line
    // BUT: perhaps it has been written in the first bucket!
    if (only_check_first == 0) {
        only_check_first = 1;
        goto restart_full;
    }

    return 0;
}

int llcache_put_and_hold(const llcache_t dbs, void *data, uint32_t *index) 
{
    uint32_t hash = (uint32_t)(hash_mul(data, dbs->key_length) & MASK);

    if (hash == 0) hash++; // blah. Just avoid 0, that's all.

    uint32_t f_idx = hash & dbs->mask;
    if (f_idx == 0) f_idx++; // do not use bucket 0
    volatile uint32_t *f_bucket = &dbs->table[f_idx];

    uint32_t idx;
    volatile uint32_t *bucket;

    int only_check_first = 0; // flag

restart_full:
    idx = f_idx;

    do {
        // do not use bucket 0
        if (idx == 0) continue;

        bucket = &dbs->table[idx];

        register uint32_t v;
restart_bucket:
        v = *bucket;

        // Wait while locked...
        if (v & LOCK) {
            while ((v=*bucket) & LOCK) cpu_relax();
        }

        v &= MASK;

        if (v == EMPTY) {
            // EMPTY bucket!
            if (cas(bucket, EMPTY, EMPTY|LOCK)) {
                register const size_t data_idx = idx * dbs->padded_data_length;
                // Claim successful!
                memcpy(&dbs->data[data_idx], data, dbs->data_length);
                *index = idx;
                *bucket = hash | LOCK;
                return 1;
            }

            // Claim unsuccessful! Fall back to lock-waiting
            goto restart_bucket;
        }

        if (v == hash) {
            if (cas(bucket, v, v|LOCK)) {
                register size_t data_idx = idx * dbs->padded_data_length;
                // Lock acquired, compare
                if (memcmp(&dbs->data[data_idx], data, dbs->key_length) == 0) {
                    // Found existing
                    register size_t b = dbs->data_length - dbs->key_length;
                    register void *dptr = &dbs->data[data_idx + dbs->key_length];
                    memxchg(dptr, &((uint8_t*)data)[dbs->key_length], b);
                    *index = idx;
                    return 0;                    
                } else {
                    *bucket = v;
                }
            } else {
                // CAS failed
                goto restart_bucket;
            }
        }

        if (only_check_first) break;
    } while (next(&idx, f_idx));

    // If we are here, the cache line is full.
    // Claim first bucket
    const uint32_t v = (*f_bucket) & MASK;
    if (cas(f_bucket, v, v|LOCK)) {
        register const size_t data_idx = f_idx * dbs->padded_data_length;
        register void *orig_data = alloca(dbs->data_length);

        memxchg(&dbs->data[data_idx], data, dbs->data_length);

        *index = f_idx;
        *f_bucket = hash | LOCK;
        return 2;
    }

    // Claim failed, restart, but only use first bucket
    only_check_first = 1;
    goto restart_full;
}

static inline unsigned next_pow2(unsigned x)
{
    if (x <= 2) return x;
    return (1ULL << 32) >> __builtin_clz(x - 1);
}

llcache_t llcache_create(size_t key_length, size_t data_length, size_t cache_size, llcache_delete_f cb_delete, void *cb_data)
{
    llcache_t dbs;
    posix_memalign((void**)&dbs, LINE_SIZE, sizeof(struct llcache));
    
    assert(key_length <= data_length);

    dbs->key_length = key_length;
    dbs->data_length = data_length;

    // For padded data length, we will just round up to 16 bytes.
    if (data_length == 1 || data_length == 2) dbs->padded_data_length = data_length;
    else if (data_length == 3 || data_length == 4) dbs->padded_data_length = 4;
    else if (data_length <= 8) dbs->padded_data_length = (data_length + 7) & ~7;
    else dbs->padded_data_length = (data_length + 15) & ~15;
    
    if (cache_size < HASH_PER_CL) cache_size = HASH_PER_CL;
    assert(next_pow2(cache_size) == cache_size);
    dbs->cache_size = cache_size;
    dbs->mask = dbs->cache_size - 1;

    dbs->_table = (uint32_t*)calloc(dbs->cache_size*sizeof(uint32_t)+LINE_SIZE, 1);
    dbs->table = ALIGN(dbs->_table);
    dbs->_data = (uint8_t*)malloc(dbs->cache_size*dbs->padded_data_length+LINE_SIZE);
    dbs->data = ALIGN(dbs->_data);

    // dont care about what is in "data" table - no need to clear it

    dbs->cb_delete = cb_delete; // can be NULL
    dbs->cb_data   = cb_data;

    return dbs;
}

inline void llcache_clear(llcache_t dbs)
{
    llcache_clear_partial(dbs, 0, dbs->cache_size);
}

inline void llcache_clear_partial(llcache_t dbs, size_t first, size_t count)
{
    // Clear per cacheline!
    size_t i, j;
    size_t last = first + count - 1;
    if (last >= dbs->cache_size) last = dbs->cache_size - 1;
    size_t i_max = last / HASH_PER_CL;

    for (i=first / HASH_PER_CL; i<=i_max; i++) {
        for (j=0;j<HASH_PER_CL;j++) {
            register volatile uint32_t *bucket = &dbs->table[i * HASH_PER_CL + j];
            while (1) {
                register uint32_t hash = (*bucket) & MASK;
                if (cas(bucket, hash, hash|LOCK)) break;
            }            
        }
        // Entire cacheline locked
        for (j=0;j<HASH_PER_CL;j++) {
            register volatile uint32_t *bucket = &dbs->table[i * HASH_PER_CL + j];
            if (((*bucket) & MASK) != EMPTY) {
                if (dbs->cb_delete != NULL) {
                    dbs->cb_delete(dbs->cb_data, &dbs->data[(i * HASH_PER_CL + j) * dbs->padded_data_length]);
                }
            }
            *bucket = EMPTY;
        } 
    }
}

inline void llcache_clear_unsafe(llcache_t dbs)
{
    // Just memset 0 the whole thing
    // No locking and no callbacks called...!
    printf("UNSAFE CLEARING LLCACHE\n");
    memset(dbs->table, 0, sizeof(uint32_t) * dbs->cache_size);
}

void llcache_free(llcache_t dbs)
{
    free(dbs->_data);
    free(dbs->_table);
    free(dbs);
}

void llcache_print_size(llcache_t dbs, FILE *f)
{
    fprintf(f, "Hash: %ld * 4 = %ld bytes; Data: %ld * %ld = %ld bytes",
        dbs->cache_size, dbs->cache_size * 4, dbs->cache_size, 
        dbs->padded_data_length, dbs->cache_size * dbs->padded_data_length);
}
