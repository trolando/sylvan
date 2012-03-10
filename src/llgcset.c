#include <stdlib.h>
#include <stdio.h>  // for printf
#include <stdint.h> // for uint32_t etc
#include <string.h> // for memcopy
#include <assert.h> // for assert

#include "config.h"

#ifdef HAVE_NUMA_H
#include <numa.h>
#endif

#include "llgcset.h"

#define DEBUG_LLGCSET 0 // set to 1 to enable logic assertions

#ifndef LINE_SIZE
    #define LINE_SIZE 64 // default cache line
#endif

#define cpu_relax  asm volatile("pause\n": : :"memory")
#define cas(a,b,c) __sync_bool_compare_and_swap((a),(b),(c))
#define atomic_inc(P) __sync_add_and_fetch((P), 1)
#define atomic_dec(P) __sync_add_and_fetch((P), -1) 

// 64-bit hashing function, http://www.locklessinc.com (hash_mul.s)
unsigned long long hash_mul(const void* data, unsigned long long len);
unsigned long long rehash_mul(const void* data, unsigned long long len, unsigned long long seed);

/**
 * LL-set with GC using Reference Counting. Note that this implementation is not cycle-safe.
 * (That means you should not have graphs with cycles in this hash table)
 * 
 * This is a modification of the Lock-Less Set by Laarman et al.
 * It will only store the upper 2 bytes of the hash ("most significant bytes").
 * The lower 2 bytes are used as a reference counter, with 0xFFFE being the highest value.
 * 
 * The "0" slot will not be used in this hash table.
 * 
 * When a key is about to be deleted, the reference counter is atomically set to 0xFFFF.
 * Once it is set to this value, the on_delete callback will be called if set.
 * 
 * For garbage collection a list of once-dead nodes is maintained, which will be used during the gc process.
 * Any node that reaches count 0 is placed in this list.
 * This list of dead nodes might contain nodes that are actually alive and it might also contain duplicate nodes.
 * 
 * GC-list implemented as fixed block.
 */

static const uint32_t   EMPTY      = 0x00000000;
static const uint32_t   LOCK       = 0x80000000;
static const uint32_t   TOMBSTONE  = 0x7fffffff;

static const uint32_t   RC_MASK    = 0x0000ffff;
static const uint32_t   HL_MASK    = 0xffff0000;
static const uint32_t   HASH_MASK  = 0x7fff0000;
static const uint32_t   DELETING   = 0x0000ffff;
static const uint32_t   SATURATED  = 0x0000fffe;

static const int      HASH_PER_CL = ((LINE_SIZE) / 4);
static const uint32_t CL_MASK     = ~(((LINE_SIZE) / 4) - 1);
static const uint32_t CL_MASK_R   = ((LINE_SIZE) / 4) - 1;

/* Example values with a LINE_SIZE of 64
 * HASH_PER_CL = 16
 * CL_MASK     = 0xFFFFFFF0
 * CL_MASK_R   = 0x0000000F
 */

enum {
    REF_SUCCESS,
    REF_DELETING,
    REF_NOCAS,
    REF_NOWZERO
};

void llgcset_deadlist_ondelete(const llgcset_t dbs, const uint32_t index);

/* 
 * Try to increase ref 
 * returns REF_SUCCESS, REF_DELETING, REF_NOCAS, REF_LOCK.
 */
static inline int try_ref(volatile uint32_t *hashptr)
{
    register uint32_t hash = *hashptr;
    register uint32_t rc = hash & RC_MASK;
    if (rc == SATURATED) return REF_SUCCESS; // saturated, do not ref
    if (rc == DELETING) return REF_DELETING;
#if DEBUG_LLGCSET
    /* check */ assert( (rc + 1) == ((hash+1) & RC_MASK));
#endif
    if (!cas(hashptr, hash, hash+1)) return REF_NOCAS;
    return REF_SUCCESS;
}

/* 
 * Try to decrease ref 
 * returns REF_SUCCESS, REF_NOCAS, REF_NOWZERO, REF_LOCK.
 */
static inline int try_deref(volatile uint32_t *hashptr)
{
    register uint32_t hash = *hashptr;
    register uint32_t rc = hash & RC_MASK;
    if (rc == SATURATED) return REF_SUCCESS; // saturated, do not deref
    assert(rc != DELETING); // external logic check 
    assert(rc != 0);        // external logic check 
#if DEBUG_LLGCSET
    /* check */ assert( (rc - 1) == ((hash-1) & RC_MASK));
#endif
    if (!cas(hashptr, hash, hash-1)) return REF_NOCAS;
    if (rc == 1) return REF_NOWZERO; // we just decreased to zero
    return REF_SUCCESS;
}

static inline void lock(volatile uint32_t *bucket)
{
    while (1) {
        register uint32_t hash = *bucket;
        if (!(hash & LOCK)) { if (cas(bucket, hash, hash | LOCK)) { break; } }
        cpu_relax;
    }
}

static inline void unlock(volatile uint32_t *bucket)
{
    while (1) {
        register uint32_t hash = *bucket;
        if (cas(bucket, hash, hash & (~LOCK))) break;
        cpu_relax;
    }
}

// Calculate next index on a cache line walk
static inline int next(uint32_t *cur, uint32_t last) 
{
    return (*cur = (*cur & CL_MASK) | ((*cur + 1) & CL_MASK_R)) != last;
}

/**
 * Note: lookup_hash will increment the reference count (or set it to 1 if created).
 */
 
/* ALGORITHM:
 * - Walk lines and remember the first TOMBSTONE encountered
 * - PER BUCKET:
 * - If EMPTY: the element is not in the table.
 * -   Try to claim EMPTY bucket. If successful:
 * -     If tombstone: fill tombstone, set tombstone (wait) to DONE+1, set empty (wait) to TOMBSTONE, return.
 * -     Else: fill empty, set empty (wait) to DONE+1, return.
 * - If WAIT: 
 * -   If tombstone: release tombstone, wait for wait...
 * - If MATCH:
 * -   Increment ref, compare data. 
 * -     If equal, return.
 * -     If not equal, decrease reference counter and continue.
 * - If we released tombstone before (in WAIT step), fully restart.
 * - If TOMBSTONE and we have no claim yet: Try to claim TOMBSTONE.
 */
void *llgcset_lookup_hash(const llgcset_t dbs, const void* data, int* created, uint32_t* index)
{
    uint64_t            hash_rehash;
full_restart:

    hash_rehash = hash_mul(data, dbs->key_length);

    /* hash_memo will be the key as stored in the table */
    register uint32_t   hash_memo = hash_rehash & HASH_MASK; 
    // avoid collision of hash with reserved values 
    while (EMPTY == hash_memo || (TOMBSTONE & HASH_MASK) == hash_memo) 
        hash_memo = (hash_rehash = rehash_mul(data, dbs->key_length, hash_rehash)) & HASH_MASK;

    volatile uint32_t   *bucket = 0;
    volatile uint32_t   *tomb_bucket = 0;
    uint32_t            idx, tomb_idx;

    // Lock first bucket...
    uint32_t            first_idx = hash_rehash & dbs->mask;
    if (first_idx == 0) first_idx++; // do not use slot 0 (hack for sylvan)
    volatile uint32_t   *first_bucket = &dbs->table[first_idx];

    lock(first_bucket);

    // First bucket is ours, we can do our job!

    size_t rehash_count = 0;
    while (rehash_count < dbs->threshold)
    {
        idx = hash_rehash & dbs->mask;
        size_t last = idx; // if next() sees idx again, stop.
        do
        {
            // do not use slot 0 (hack for sylvan)
            if (idx == 0) continue;

            bucket = &dbs->table[idx];

            /* A bucket is either:          VALUE       | MASK
             * - E empty                    0x0000 0000 | 0xffff ffff
             * - L locked                   0x8000 0000 | 0x8000 0000
             * - T tombstone                0x7fff ffff | 0xffff ffff
             * - D deleting                 0x.... ffff | 0x0000 ffff
             * - F filled and matching      0x.... .... | 0x7fff 0000 
             * - F filled and unmatching    0x.... .... | 0x7fff 0000 
             * Unless we own a bucket (lock implies ownership) we
             * always use a CAS to modify buckets. Failed CAS means goto restart_bucket.
             */

            /** Valid transactions
             ** E cas> L -> E
             ** E cas> L -> F
             ** F cas> D -> T
             ** T cas> L -> T
             ** T cas> L -> F
             **/

            register uint32_t v;
restart_bucket:
            v = *bucket;

            // If the bucket is still empty, then our value is not yet in the table!
            if (EMPTY == (v & HASH_MASK))
            {
                if (tomb_bucket != 0) {
                    const size_t data_idx = tomb_idx * dbs->padded_data_length;
                    // We claimed a tombstone (using cas) earlier.
                    memcpy(&dbs->data[data_idx], data, dbs->data_length);
                    // SFENCE; // future x86 without strong store ordering
                    *tomb_bucket = hash_memo + 1; // also set RC to 1
                    if (tomb_bucket != first_bucket) unlock(first_bucket);

                    *index = tomb_idx;
                    *created = 1;
                    return &dbs->data[data_idx];
                }

                if (bucket == first_bucket) {
                    const size_t data_idx = idx * dbs->padded_data_length;
                    // The empty bucket is also the first bucket.
                    memcpy(&dbs->data[data_idx], data, dbs->data_length);
                    // SFENCE; // future x86 without strong store ordering
                    *bucket = hash_memo + 1;

                    *index = idx;
                    *created = 1;
                    return &dbs->data[data_idx];
                }

                // No claimed tombstone, so claim end of chain
                if (cas(bucket, EMPTY, LOCK)) {
                    const size_t data_idx = idx * dbs->padded_data_length;
                    memcpy(&dbs->data[data_idx], data, dbs->data_length);
                    // SFENCE; // future x86 without strong store ordering
                    *bucket = hash_memo + 1; // also set RC to 1
                    unlock(first_bucket);

                    *index = idx;
                    *created = 1;
                    return &dbs->data[data_idx];
                }
                
                // End of chain claim failed! We have to wait and restart!
                // Release all existing claims first...
                // tomb_bucket == 0
                unlock(first_bucket);
                while (*bucket & LOCK) cpu_relax;
                goto full_restart;
            }

            // Test if this bucket matches
            if (hash_memo == (v & HASH_MASK)) {
                // It matches: increase ref counter
                int ref_res = try_ref(bucket);
                if (ref_res != REF_SUCCESS) {
                    // Either "REF_DELETING" or "REF_NOCAS"
                    // REF_DELETING does not use the lock, so it can progress...
                    // REF_NOCAS is no problem.
                    goto restart_bucket;
                }
                // Compare data
                if (memcmp(&dbs->data[idx * dbs->padded_data_length], data, dbs->key_length) == 0)
                {
                    // Found existing!
                    if (tomb_bucket != 0) *tomb_bucket = TOMBSTONE;
                    if (tomb_bucket != first_bucket) unlock(first_bucket);

                    *index = idx;
                    *created = 0;
                    return &dbs->data[idx * dbs->padded_data_length];
                }
                // It was different, decrease counter again
                llgcset_deref(dbs, idx);
            }

            if (tomb_bucket == 0 && TOMBSTONE == (v & ~LOCK)) {
                if (bucket == first_bucket) {
                    tomb_bucket = first_bucket;
                    tomb_idx = first_idx;
                }
                // Claim bucket 
                else if (cas(bucket, TOMBSTONE, TOMBSTONE | LOCK)) {
                    tomb_bucket = bucket;
                    tomb_idx = idx;
                }
                // If it fails, no problem!
            }  
        } while (next(&idx, last));

        // Rehash, next cache line!
        hash_rehash = rehash_mul(data, dbs->key_length, hash_rehash);
        rehash_count++;
    }

    // If we are here, then we are certain no entries exist!
    // if we have a tombstone, then the table is not full
    if (tomb_bucket != 0) {
        const size_t data_idx = tomb_idx * dbs->padded_data_length;
        memcpy(&dbs->data[data_idx], data, dbs->data_length);
        // SFENCE; // future x86 without strong store ordering
        *tomb_bucket = hash_memo + 1;
        if (tomb_bucket != first_bucket) unlock(first_bucket);

        *index = tomb_idx;
        *created = 1;
        return &dbs->data[data_idx];
    }

    // table is full
    unlock(first_bucket);
    return 0;
}

// This is a wrapper function. It allows NULL created, NULL index and will GC when table full.
inline void *llgcset_get_or_create(const llgcset_t dbs, const void *data, int *created, uint32_t *index)
{
    int _created;
    uint32_t _index;
    void *result = llgcset_lookup_hash(dbs, data, &_created, &_index);
    if (result == 0) {
        // Table full - gc then try again...
        llgcset_gc(dbs, gc_hashtable_full);
        result = llgcset_lookup_hash(dbs, data, &_created, &_index);
    }
    if (created) *created=_created;
    if (index) *index=_index;
    return result;
}

static inline unsigned next_pow2(unsigned x)
{
    if (x <= 2) return x;
    return (1ULL << 32) >> __builtin_clz(x - 1);
}

llgcset_t llgcset_create(size_t key_length, size_t data_length, size_t table_size, llgcset_delete_f cb_delete, llgcset_pregc_f cb_pregc, void *cb_data)
{
    llgcset_t dbs;
    posix_memalign((void**)&dbs, LINE_SIZE, sizeof(struct llgcset));

    assert(key_length <= data_length);  

    dbs->key_length = key_length;
    dbs->data_length = data_length;

    // For padded data length, we will just round up to 16 bytes.
    if (data_length == 1 || data_length == 2) dbs->padded_data_length = data_length;
    else if (data_length == 3 || data_length == 4) dbs->padded_data_length = 4;
    else if (data_length <= 8) dbs->padded_data_length = (data_length + 7) & ~7;
    else dbs->padded_data_length = (data_length + 15) & ~15;

    if (table_size < HASH_PER_CL) table_size = HASH_PER_CL;
    assert(next_pow2(table_size) == table_size);
    dbs->table_size = table_size;
    dbs->mask = dbs->table_size - 1;

    dbs->threshold = (64 - __builtin_clzl(table_size)) + 4; // doubling table_size increases threshold by 1

#ifdef HAVE_NUMA_H
    if (numa_available() >= 0) {
        dbs->table = (uint32_t*)numa_alloc_interleaved(dbs->table_size * sizeof(uint32_t));
        dbs->data = (uint8_t*)numa_alloc_interleaved(dbs->table_size * dbs->padded_data_length);
    } else {
#endif
    posix_memalign((void**)&dbs->table, LINE_SIZE, dbs->table_size * sizeof(uint32_t));
    posix_memalign((void**)&dbs->data, LINE_SIZE, dbs->table_size * dbs->padded_data_length);
#ifdef HAVE_NUMA_H
    }
#endif

    memset(dbs->table, 0, sizeof(uint32_t) * dbs->table_size);
    
    // dont care about what is in "data" table
 
    size_t cache_size = table_size >> 4; // table_size / 16
    dbs->deadlist = llsimplecache_create(cache_size, (llsimplecache_delete_f)&llgcset_deadlist_ondelete, dbs);

    dbs->clearing = 0;
 
    dbs->cb_delete = cb_delete; // can be NULL
    dbs->cb_pregc  = cb_pregc; // can be NULL
    dbs->cb_data   = cb_data;

    return dbs;
}

/**
 * Increase reference counter
 */
void llgcset_ref(const llgcset_t dbs, uint32_t index)
{
    assert(index != 0 && index < dbs->table_size);

    int ref_res;
    register volatile uint32_t *hashptr = &dbs->table[index];
    do {
        ref_res = try_ref(hashptr);
#if DEBUG_LLGCSET
        assert(ref_res != REF_DELETING);
#endif
        // ref_res is REF_LOCK or REF_NOCAS or REF_SUCCESS
    } while (ref_res != REF_SUCCESS);
}

void try_delete_item(const llgcset_t dbs, uint32_t index)
{
    register volatile uint32_t *hashptr = &dbs->table[index];
    register uint32_t hash = *hashptr;

    // Check if still 0 and then try to claim it...
    while ((hash & RC_MASK) == 0) {
        if (cas(hashptr, hash, hash | DELETING)) {
            // if we're here, we can safely delete it!
            if (dbs->cb_delete != NULL) dbs->cb_delete(dbs->cb_data, &dbs->data[index * dbs->padded_data_length]);

            // We do not want to interfere with locks...
            while (!cas(hashptr, hash, hash | TOMBSTONE)) {
                hash = *hashptr;
                cpu_relax;
            }
            break;
        }
        hash = *hashptr;
        cpu_relax;
    }   
}

/**
 * Decrease reference counter
 */
void llgcset_deref(const llgcset_t dbs, uint32_t index)
{
    assert(index != 0 && index < dbs->table_size);

    int ref_res;
    register volatile uint32_t *hashptr = &dbs->table[index];
    do { 
        ref_res = try_deref(hashptr);
        // ref_res is REF_NOCAS or REF_SUCCESS or REF_NOWZERO
    } while (ref_res != REF_NOWZERO && ref_res != REF_SUCCESS);

    if (ref_res == REF_NOWZERO) {
        // Add it to the deadlist, then return.
        if (dbs->clearing != 0) try_delete_item(dbs, index);
        else if(llsimplecache_put(dbs->deadlist, &index, index) == 2) {
            try_delete_item(dbs, index);
        }
    } 
}

inline void llgcset_clear(llgcset_t dbs)
{
    // TODO MODIFY so the callback is properly called and all???
    // Or is that simply a matter of gc()...
    memset(dbs->table, 0, sizeof(uint32_t) * dbs->table_size);
}

void llgcset_free(llgcset_t dbs)
{
    llsimplecache_free(dbs->deadlist);
#ifdef HAVE_NUMA_H
    if (numa_available() >= 0) {
        numa_free(dbs->data, dbs->table_size * dbs->padded_data_length);
        numa_free(dbs->table, dbs->table_size * sizeof(uint32_t));
    } else {
#endif
    free(dbs->data);
    free(dbs->table);
#ifdef HAVE_NUMA_H
    }
#endif
    free(dbs);
}

/**
 * Execute garbage collection
 */
void llgcset_gc(const llgcset_t dbs, gc_reason reason)
{
    // Call dbs->pre_gc first
    if (dbs->cb_pregc != NULL) dbs->cb_pregc(dbs->cb_data, reason);

    atomic_inc(&dbs->clearing);
    llsimplecache_clear(dbs->deadlist);
    atomic_dec(&dbs->clearing);
}

void llgcset_deadlist_ondelete(const llgcset_t dbs, const uint32_t index)
{
    try_delete_item(dbs, index);
}

void llgcset_print_size(llgcset_t dbs, FILE *f)
{
    fprintf(f, "Hash: %ld * 4 = %ld bytes; Data: %ld * %ld = %ld bytes ",
        dbs->table_size, dbs->table_size * 4, dbs->table_size, 
        dbs->padded_data_length, dbs->table_size * dbs->padded_data_length);
    fprintf(f, "(Deadlist: ");
    llsimplecache_print_size(dbs->deadlist, f);
    fprintf(f, ")");
}
