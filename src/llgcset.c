#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include "llgcset.h"
#include "runtime.h"

// Employ 8 cache lines per GCLIST block.
#define GCLIST_CACHELINES 8

/**
 * LL-set with GC using Reference Counting. Note that this implementation is not cycle-safe.
 * 
 * This is a modification of the Lock-Less Set by Laarman et al.
 * It will only store the upper 2 bytes of the hash ("most significant bytes").
 * The lower 2 bytes are used as a reference counter, with 0xFFFE being the highest value.
 * 
 * When a key is about to be deleted, the reference counter is atomically set to 0xFFFF.
 * Once it is set to this value, the on_delete callback will be called if it is not NULL.
 * 
 * Because of internal references*, it is necessary to maintain a list of dead nodes, so there is 
 * a starting point for garbage collection. A node is dead when the reference counter is set to 0.
 * This list of dead nodes might contain nodes that are actually alive and it might contain duplicate nodes.
 * 
 * GC-list implemented as fixed block.
 */

static const int        TABLE_SIZE = 24; // 1<<24 entries by default
static const uint32_t   EMPTY = 0;
static const uint32_t   WRITE_BIT = 1 << 31;           // 0x80000000
static const uint32_t   WRITE_BIT_R = ~(1 << 31);      // 0x7fffffff
static const uint32_t   TOMBSTONE = 0x7fffffff;

/**
 * Initialize gc list... 
 */
static int llgclist_init(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock);

/**
 * Add uint32_t value to list, and return 1.
 * If this returns 0, it is likely an out-of-memory error.
 */
static int llgclist_put_head(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock, uint32_t value);
static int llgclist_put_tail(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock, uint32_t value);
/**
 * Pop a value from the list, and return 0.
 * If this returns 0, there was no value to pop.
 */
static int llgclist_pop_head(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock, uint32_t *value);
static int llgclist_pop_tail(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock, uint32_t *value);

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

// Calculate next index on a cache line walk
static inline int next(uint32_t line, uint32_t *cur, uint32_t last) 
{
    *cur = (((*cur)+1) & (CACHE_LINE_INT32_MASK_R)) | line;
    return *cur != last;
}

/**
 * Note: lookup_hash will increase the reference count!
 */
void *llgcset_lookup_hash(const llgcset_t dbs, const void* data, int* created, uint32_t* index, uint32_t* hash)
{
    size_t              seed = 0;
    size_t              l = dbs->length;
    size_t              b = dbs->bytes;

    uint32_t            hash_rehash = hash ? *hash : dbs->hash32(data, b, 0);

    /* hash_memo will be the key as stored in the table
     * WRITE_BITS: 0x80000000
     * HASH_BITS: 0x7ffff0000
     * RC_BITS: 0x0000ffff */  

    register uint32_t   hash_memo = hash_rehash & 0x7fff0000; 
    // avoid collision of hash with reserved values 
    while (EMPTY == hash_memo)
        hash_memo = dbs->hash32((char *)data, b, ++seed) & 0x7fff0000;

    uint32_t            WAIT = hash_memo;
    uint32_t            DONE = hash_memo | 0x80000000; // write bit
	
    uint32_t           *tomb_bucket = 0;
    uint32_t            tomb_idx = 0;

    while (seed < dbs->threshold)
    {
        uint32_t idx = hash_rehash & dbs->mask;
        size_t line = idx & CACHE_LINE_INT32_MASK;
        size_t last = idx; // if next() sees idx again, stop.
        do
        {
            // do not use slot 0
            if (idx == 0) continue;

            uint32_t *bucket = &dbs->table[idx];
            // If the bucket is still empty (i.e. not found! write!)
            if (EMPTY == *bucket)
            {
                if (tomb_bucket != 0) {
                    memcpy(&dbs->data[tomb_idx * l], data, b);
					// SFENCE; // future x86 without strong store ordering
                    atomic32_write(tomb_bucket, DONE | 0x00000001);

                    *index = tomb_idx;
                    *created = 1;
                    return &dbs->data[tomb_idx * l];
                }
                if (cas(bucket, EMPTY, WAIT))
                {
                    memcpy(&dbs->data[idx * l], data, b);
					// SFENCE; // future x86 without strong store ordering
                    atomic32_write(bucket, DONE | 0x00000001);

                    *index = idx;
                    *created = 1;
                    return &dbs->data[idx * l];
                }
            }
			LFENCE;///////////////////////////////////////////////////
            if (TOMBSTONE == atomic32_read(bucket)) {
                // we may want to use this slot!
                if (tomb_bucket == 0) {
                    if (cas(bucket, TOMBSTONE, WAIT))
                    {
                        tomb_bucket = bucket;
                        tomb_idx = idx;
                    }
                }
            }
            if (bucket != tomb_bucket && hash_memo == (atomic32_read(bucket) & 0x7fff0000))
            {
                // Wait until written or released (in case of tombstone)
                while (WAIT == atomic32_read(bucket)) cpu_relax();
                uint32_t v;
                if (((v=atomic32_read(bucket)) & 0xffff0000) == DONE) {
                    // first increase reference
                    while (1) {
                        if ((v & 0x0000ffff) == 0x0000ffff) {
                            // about to be deleted!
                            break;
                        }
                        else if ((v & 0x0000ffff) != 0x0000fffe) {
                            // not saturated, increase!
                            if (!(cas(bucket, v, v+1))) {
                                v = atomic32_read(bucket);
                                continue;// if failed, restart
                            }
                        }
                        // if here, either we increased refcount, or it is saturated
                        if (dbs->equals(&dbs->data[idx * l], data, b))
                        {
                            // Found existing!
                            if (tomb_bucket != 0) {
                                atomic32_write(tomb_bucket, TOMBSTONE);
                            }

                            *index = idx;
                            *created = 0;
                            return &dbs->data[idx * l];
                        }
                        // if here, then sadly we increased refcount for nothing
                        while (1) {
                            v = atomic32_read(bucket);
                            register uint32_t c = v & 0x0000ffff;
                            if (c == 0x0000ffff) break; // about to be deleted
                            if (c == 0x0000fffe) break; // saturated
                            if (c == 0x00000000) break; // ERROR STATE
                            if (cas(bucket, v, v-1)) break;
                        }
                        break;
                    }
                }
            }
        } while (next(line, &idx, last));
        hash_rehash = dbs->hash32(data, b, hash_rehash + (seed++));
    }

    // If we are here, then we are certain no entries exist!
    // if we have a tombstone, then the table is not full
    if (tomb_bucket != 0) {
        memcpy(&dbs->data[tomb_idx * l], data, b);
		// SFENCE; // future x86 without strong store ordering
        atomic32_write(tomb_bucket, DONE | 0x00000001);

        *index = tomb_idx;
        *created = 1;
        return &dbs->data[tomb_idx * l];

    }

    // table is full
    return 0;
}

inline void *llgcset_get_or_create(const llgcset_t dbs, const void *data, int *created, uint32_t *index)
{
	int _created;
	uint32_t _index;
    void *result = llgcset_lookup_hash(dbs, data, &_created, &_index, NULL);
    if (created) *created=_created;
    if (index) *index=_index;
    return result;
}

llgcset_t llgcset_create(size_t length, size_t size, hash32_f hash32, equals_f equals, delete_f cb_delete)
{
    llgcset_t dbs = rt_align(CACHE_LINE_SIZE, sizeof(struct llgcset));
    dbs->hash32 = hash32 != NULL ? hash32 : hash_128_swapc; // default hash function is hash_128_swapc
    dbs->equals = equals != NULL ? equals : default_equals;
    dbs->cb_delete = cb_delete; // can be NULL
    dbs->bytes = length;
    dbs->size = 1 << size;
    dbs->threshold = dbs->size / 100;
    dbs->mask = dbs->size - 1;
    dbs->table = rt_align(CACHE_LINE_SIZE, sizeof(uint32_t) * dbs->size);
    dbs->data = rt_align(CACHE_LINE_SIZE, dbs->size * length);
    memset(dbs->table, 0, sizeof(uint32_t) * dbs->size);
    
    /* Initialize gclist */
    dbs->gc_size = 1024 * 1024;
    dbs->gc_list = rt_align(CACHE_LINE_SIZE, dbs->gc_size * sizeof(uint32_t));
    llgclist_init(dbs->gc_list, dbs->gc_size, &dbs->gc_head, &dbs->gc_tail, &dbs->gc_lock);
    
	// dont care about what is in "data" table
    return dbs;
}

int llgcset_ref(const llgcset_t dbs, uint32_t index)
{
    // We cannot use atomic fetch_and_add, because we need
    // to check for the values FFFE and FFFF
    while (1) {
        register uint32_t hash = *(volatile uint32_t *)&dbs->table[index];
        register uint32_t c = hash & 0xffff;
        if (c == 0x0000fffe) return 1; // saturated
        if (c == 0x0000ffff) return 0; // error: already deleted!
        c += 1;
        // c&=0xffff; not necessary because c != 0x0000ffff
        c |= (hash&0xffff0000);
        if (cas(&dbs->table[index], hash, c)) return 1; 
        // if we're here, someone else modified the value before us
    }
}

int llgcset_deref(const llgcset_t dbs, uint32_t index)
{
    register int should_delete = 0;
    // We cannot use atomic fetch_and_add, because we need
    // to check for the values FFFE and FFFF
    while (1) {
        register uint32_t hash = *(volatile uint32_t *)&dbs->table[index];
        register uint32_t c = hash & 0xffff;
        if (c == 0x0000fffe) break; // saturated
        if (c == 0x0000ffff) return 0; // error: already deleted!
        if (c == 0x00000000) return 0; // error: already zero!
        c -= 1;
        if (c == 0) should_delete = 1;
        else should_delete = 0;
        // c&=0xffff; not necessary because c != 0x00000000
        c |= (hash&0xffff0000);
        if (cas(&dbs->table[index], hash, c)) break; 
        // if we're here, someone else modified the value before us
    }
    if (should_delete) {
        while (!llgclist_put_tail(dbs->gc_list, dbs->gc_size, &dbs->gc_head, &dbs->gc_tail, &dbs->gc_lock, index)) {
            // FULL
            llgcset_gc(dbs);
        }
    }
    return 1;
}

inline void llgcset_clear(llgcset_t dbs)
{
    memset(dbs->table, 0, sizeof(uint32_t) * dbs->size);
}

void llgcset_free(llgcset_t dbs)
{
    free(dbs->data);
    free(dbs->table);
    free(dbs);
}

void llgcset_gc(const llgcset_t dbs)
{
    uint32_t idx;
    while (llgclist_pop_head(dbs->gc_list, dbs->gc_size, &dbs->gc_head, &dbs->gc_tail, &dbs->gc_lock, &idx)) {
        register uint32_t hash = *(volatile uint32_t *)&dbs->table[idx];
        register uint32_t c = hash & 0xffff;
        if (c == 0x0000ffff) continue; // error: already being deleted!
        if (c != 0x00000000) continue; // error: not zero!
        c = hash | 0x0000ffff;
        if (!cas(&dbs->table[idx], hash, c)) continue; // error: it's changed by someone else!
        
        // if we're here, we can do our job!
        
        dbs->cb_delete(dbs, &dbs->data[idx * dbs->length]);
        dbs->table[idx] = TOMBSTONE;
    }
}

/******************************************
 * IMPLEMENTATION OF GCLIST
 ******************************************/
 
static int llgclist_init(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock)
{
    *head = 0; // head points to UNFILLED VALUE
    *tail = 0; // tail points to UNFILLED VALUE
    *lock = 0;
}

static int llgclist_put_head(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock, uint32_t value)
{
    // lock
    while (1) {
        if (cas(lock, 0, 1)) break;
        while (*((volatile uint8_t*)lock)>0) cpu_relax();
    }
    
    // no LFENCE needed, because of CAS lock.
    
    if ((*tail+1) == *head || (*tail == (size-1) && *head == 0)) {
        *lock = 0;
        return 0; // full
    }
    
    list[*head] = value;
    if (*head == 0) *head = size-1; 
    else *head = (*head)-1;
    
    // SFENCE; // needed in future x86 processors
    
    *lock = 0;
    return 1;
}

static int llgclist_put_tail(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock, uint32_t value)
{
    // lock
    while (1) {
        if (cas(lock, 0, 1)) break;
        while (*((volatile uint8_t*)lock)>0) cpu_relax();
    }
    
    // no LFENCE needed, because of CAS lock.
    
    if ((*tail+1) == *head || (*tail == (size-1) && *head == 0)) {
        *lock = 0;
        return 0; // full
    }
    
    list[*tail] = value;
    if (*tail == (size-1)) *tail = 0; 
    else *tail = (*tail)+1;
    
    // SFENCE; // needed in future x86 processors
    
    *lock = 0;
    return 1;
}

static int llgclist_pop_head(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock, uint32_t *value)
{
    // lock
    while (1) {
        if (cas(lock, 0, 1)) break;
        while (*((volatile uint8_t*)lock)>0) cpu_relax();
    }
    
    // no LFENCE needed, because of CAS lock.
    
    if (*head == *tail) {
        *lock = 0;
        return 0; // empty
    }
    
    if (*head == (size-1)) *head = 0;
    else *head = (*head) + 1;
    *value = list[*head];
    
    // SFENCE; // needed in future x86 processors
    
    *lock = 0;
    return 1;
}

static int llgclist_pop_tail(uint32_t *list, uint32_t size, uint32_t *head, uint32_t *tail, uint8_t *lock, uint32_t *value)
{
    // lock
    while (1) {
        if (cas(lock, 0, 1)) break;
        while (*((volatile uint8_t*)lock)>0) cpu_relax();
    }
    
    // no LFENCE needed, because of CAS lock.
    
    if (*head == *tail) {
        *lock = 0;
        return 0; // empty
    }
    
    if (*tail == 0) *tail = size - 1;
    else *tail = (*tail) - 1;
    *value = list[*tail];
    
    // SFENCE; // needed in future x86 processors
    
    *lock = 0;
    return 1;
}
 