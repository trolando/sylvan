#include <stdlib.h>
#include <stdio.h>  // for printf
#include <stdint.h> // for uint32_t etc
#include <string.h> // for memcopy
#include <assert.h> // for assert

#include "llgcset.h"
#include "sylvan_runtime.h"

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

static const int        TABLE_SIZE = 24; // 1<<24 entries by default
static const uint32_t   EMPTY = 0x00000000;
static const uint32_t   LOCK = 0x80000000;
static const uint32_t   TOMBSTONE = 0x7fffffff;

static const uint32_t   DELETING = 0x0000ffff;
static const uint32_t   SATURATED = 0x0000fffe;

#define RC_PART(s) (((uint32_t)s)&((uint32_t)0x0000ffff))

static int default_equals(const void *a, const void *b, size_t length)
{
    return memcmp(a, b, length) == 0;
}

// Number to of int32's per cache line (64/4 = 16)
static const size_t CACHE_LINE_INT32 = ((LINE_SIZE) / sizeof (uint32_t));
// Mask to determine our current cache line in an aligned int32 world (mask = 0xfffffff0)
static const size_t CACHE_LINE_INT32_MASK = -((LINE_SIZE) / sizeof (uint32_t));
// Reverse of CACHE_LINE_INT32_MASK, i.e. mask = 0x0000000f
static const size_t CACHE_LINE_INT32_MASK_R = ((LINE_SIZE) / sizeof (uint32_t)) - 1;

enum {
    REF_SUCCESS,
    REF_DELETING,
    REF_NOCAS,
    REF_NOWZERO
};

void llgcset_deadlist_ondelete(const llgcset_t dbs, const uint32_t *index);

/* 
 * Try to increase ref 
 * returns REF_SUCCESS, REF_DELETING, REF_NOCAS, REF_LOCK.
 */
static inline int try_ref(volatile uint32_t *hashptr)
{
    register uint32_t hash = *hashptr;
    //if (hash & LOCK) return REF_LOCK;
    register uint32_t rc = RC_PART(hash);
    if (rc == SATURATED) return REF_SUCCESS; // saturated, do not ref
    if (rc == DELETING) return REF_DELETING;
    /* check */ assert( (rc + 1) == RC_PART(hash+1));
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
    //if (hash & LOCK) return REF_LOCK;
    register uint32_t rc = RC_PART(hash);
    if (rc == SATURATED) return REF_SUCCESS; // saturated, do not deref
    assert(rc != DELETING);
    assert(rc != 0);
    /* check */ assert( (rc - 1) == RC_PART(hash-1));
    if (!cas(hashptr, hash, hash-1)) return REF_NOCAS;
    if (rc == 1) return REF_NOWZERO; // we just decreased to zero
    return REF_SUCCESS;
}

static inline void lock(volatile uint32_t *bucket)
{
    while (1) {
        register uint32_t hash = *bucket;
        if (!(hash & LOCK)) { if (cas(bucket, hash, hash | LOCK)) { break; } }
        cpu_relax();
    }
}

static inline void unlock(volatile uint32_t *bucket)
{
    while (1) {
        register uint32_t hash = *bucket;
        if (cas(bucket, hash, hash & (~LOCK))) break;
        cpu_relax();
    }
}

// Calculate next index on a cache line walk
static inline int next(uint32_t line, uint32_t *cur, uint32_t last) 
{
    *cur = (((*cur)+1) & (CACHE_LINE_INT32_MASK_R)) | line;
    return *cur != last;
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
void *llgcset_lookup_hash(const llgcset_t dbs, const void* data, int* created, uint32_t* index, uint32_t* hash)
{
    const size_t        l = dbs->length;
    const size_t        b = dbs->bytes;

    size_t              seed;
full_restart:
    seed = 0;

    uint32_t            hash_rehash = hash ? *hash : dbs->hash32(data, b, 0);

    /* hash_memo will be the key as stored in the table */
    register uint32_t   hash_memo = hash_rehash & 0x7fff0000; 
    // avoid collision of hash with reserved values 
    while (EMPTY == hash_memo || 0x7fff0000 == hash_memo)
        hash_memo = dbs->hash32((char *)data, b, ++seed) & 0x7fff0000;

    volatile uint32_t   *bucket = 0;
    volatile uint32_t   *tomb_bucket = 0;
    uint32_t            idx, tomb_idx;

    // Lock first bucket...
    uint32_t            first_idx = hash_rehash & dbs->mask;
    if (first_idx == 0) first_idx++; // do not use slot 0 (hack for sylvan)
    volatile uint32_t   *first_bucket = &dbs->table[first_idx];

    lock(first_bucket);

    // First bucket is ours, we can do our job!

    while (seed < dbs->threshold)
    {
        idx = hash_rehash & dbs->mask;
        size_t line = idx & CACHE_LINE_INT32_MASK;
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

restart_bucket:
            // If the bucket is still empty, then our value is not yet in the table!
            if (EMPTY == (*bucket & 0x7fff0000))
            {
                if (tomb_bucket != 0) {
                    // We claimed a tombstone (using cas) earlier.
                    memcpy(&dbs->data[tomb_idx * l], data, b);
                    // SFENCE; // future x86 without strong store ordering
                    *tomb_bucket = hash_memo + 1; // also set RC to 1
                    if (tomb_bucket != first_bucket) unlock(first_bucket);

                    *index = tomb_idx;
                    *created = 1;
                    return &dbs->data[tomb_idx * l];
                }

                if (bucket == first_bucket) {
                    // The empty bucket is also the first bucket.
                    memcpy(&dbs->data[idx * l], data, b);
                    // SFENCE; // future x86 without strong store ordering
                    *bucket = hash_memo + 1;

                    *index = idx;
                    *created = 1;
                    return &dbs->data[idx * l];
                }

                // No claimed tombstone, so claim end of chain
                if (cas(bucket, EMPTY, LOCK)) {
                    memcpy(&dbs->data[idx * l], data, b);
                    // SFENCE; // future x86 without strong store ordering
                    *bucket = hash_memo + 1; // also set RC to 1
                    unlock(first_bucket);

                    *index = idx;
                    *created = 1;
                    return &dbs->data[idx * l];
                }
                
                // End of chain claim failed! We have to wait and restart!
                // Release all existing claims first...
                // tomb_bucket == 0
                unlock(first_bucket);
                while (*bucket & LOCK) cpu_relax();
                goto full_restart;
            }

            // Test if this bucket matches
            if (hash_memo == (*bucket & 0x7fff0000)) {
                // It matches: increase ref counter
                int ref_res = try_ref(bucket);
                if (ref_res != REF_SUCCESS) {
                    // Either "REF_DELETING" or "REF_NOCAS"
                    // REF_DELETING does not use the lock, so it can progress...
                    // REF_NOCAS is no problem.
                    goto restart_bucket;
                }
                // Compare data
                if (dbs->equals(&dbs->data[idx * l], data, b))
                {
                    // Found existing!
                    if (tomb_bucket != 0) *tomb_bucket = TOMBSTONE;
                    if (tomb_bucket != first_bucket) unlock(first_bucket);

                    *index = idx;
                    *created = 0;
                    return &dbs->data[idx * l];
                }
                // It was different, decrease counter again
                llgcset_deref(dbs, idx);
            }

            if (tomb_bucket == 0 && TOMBSTONE == (*bucket & 0x7fffffff)) {
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
        } while (next(line, &idx, last));

        // Rehash, next cache line!
        hash_rehash = dbs->hash32(data, b, hash_rehash + (++seed));
    }

    // If we are here, then we are certain no entries exist!
    // if we have a tombstone, then the table is not full
    if (tomb_bucket != 0) {
        memcpy(&dbs->data[tomb_idx * l], data, b);
        // SFENCE; // future x86 without strong store ordering
        *tomb_bucket = hash_memo + 1;
        if (tomb_bucket != first_bucket) unlock(first_bucket);

        *index = tomb_idx;
        *created = 1;
        return &dbs->data[tomb_idx * l];
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
    void *result = llgcset_lookup_hash(dbs, data, &_created, &_index, NULL);
    if (result == 0) {
        // Table full - gc then try again...
        llgcset_gc(dbs, gc_hashtable_full);
        result = llgcset_lookup_hash(dbs, data, &_created, &_index, NULL);
    }
    if (created) *created=_created;
    if (index) *index=_index;
    return result;
}

llgcset_t llgcset_create(size_t key_size, size_t table_size, size_t gc_size, hash32_f hash32, equals_f equals, delete_f cb_delete, pre_gc_f pre_gc)
{
    llgcset_t dbs = rt_align(CACHE_LINE_SIZE, sizeof(struct llgcset));
    dbs->hash32 = hash32 != NULL ? hash32 : SuperFastHash;
    dbs->equals = equals != NULL ? equals : default_equals;
    dbs->cb_delete = cb_delete; // can be NULL
    dbs->pre_gc = pre_gc; // can be NULL

    // MINIMUM TABLE SIZE
    if (table_size < 4) table_size = 4;
    
    dbs->bytes = key_size; 
    dbs->length = key_size;
    dbs->size = 1 << table_size;
    dbs->threshold = 2*table_size; // e.g. 40 cache lines in a 1<<20 table
    dbs->mask = dbs->size - 1;
    dbs->table = rt_align(CACHE_LINE_SIZE, sizeof(uint32_t) * dbs->size);
    dbs->data = rt_align(CACHE_LINE_SIZE, dbs->size * key_size);
    memset(dbs->table, 0, sizeof(uint32_t) * dbs->size);
    
    // dont care about what is in "data" table
 
    int cache_size = table_size - 4;
    if (cache_size<4) cache_size=4;
    dbs->deadlist = llcache_create(4, 4, 1<<cache_size, (llcache_delete_f)&llgcset_deadlist_ondelete, dbs);

    dbs->clearing = 0;
 
    return dbs;
}

/**
 * Increase reference counter
 */
void llgcset_ref(const llgcset_t dbs, uint32_t index)
{
    assert(index < dbs->size);
    assert(index != 0);

    int ref_res;
    register volatile uint32_t *hashptr = &dbs->table[index];
    do {
        ref_res = try_ref(hashptr);
        assert(ref_res != REF_DELETING);
        // ref_res is REF_LOCK or REF_NOCAS or REF_SUCCESS
    } while (ref_res != REF_SUCCESS);
}

void try_delete_item(const llgcset_t dbs, uint32_t index)
{
    register volatile uint32_t *hashptr = &dbs->table[index];
    register uint32_t hash = *hashptr;

    // Check if still 0 and then try to claim it...
    while (RC_PART(hash) == 0) {
        if (cas(hashptr, hash, hash | DELETING)) {
            // if we're here, we can safely delete it!
            if (dbs->cb_delete != NULL) dbs->cb_delete(dbs, &dbs->data[index * dbs->length]);

            // We do not want to interfere with locks...
            while (!cas(hashptr, hash, hash | TOMBSTONE)) {
                hash = *hashptr;
                cpu_relax();
            }
            break;
        }
        hash = *hashptr;
        cpu_relax();
    }   
}

/**
 * Decrease reference counter
 */
void llgcset_deref(const llgcset_t dbs, uint32_t index)
{
    assert(index < dbs->size);
    assert(index != 0);

    int ref_res;
    register volatile uint32_t *hashptr = &dbs->table[index];
    do { 
        ref_res = try_deref(hashptr);
        // ref_res is REF_NOCAS or REF_SUCCESS or REF_NOWZERO
    } while (ref_res != REF_NOWZERO && ref_res != REF_SUCCESS);

    if (ref_res == REF_NOWZERO) {
        // Add it to the deadlist, then return.
        if (dbs->clearing != 0) try_delete_item(dbs, index);
        else if(llcache_put(dbs->deadlist, &index) == 2) {
            try_delete_item(dbs, index);
        }
    } 
}

inline void llgcset_clear(llgcset_t dbs)
{
    // TODO MODIFY so the callback is properly called and all???
    // Or is that simply a matter of gc()...
    memset(dbs->table, 0, sizeof(uint32_t) * dbs->size);
}

void llgcset_free(llgcset_t dbs)
{
    llcache_free(dbs->deadlist);
    free(dbs->data);
    free(dbs->table);
    free(dbs);
}

/**
 * Execute garbage collection
 */
void llgcset_gc(const llgcset_t dbs, gc_reason reason)
{
    // Call dbs->pre_gc first
    if (dbs->pre_gc != NULL) dbs->pre_gc(dbs, reason);

    atomic_inc(&dbs->clearing);
    llcache_clear(dbs->deadlist);
    atomic_dec(&dbs->clearing);
}

void llgcset_deadlist_ondelete(const llgcset_t dbs, const uint32_t *index)
{
    try_delete_item(dbs, *index);
}
