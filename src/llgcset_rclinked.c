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
 * Note that 
 * 
 * The goal is that garbage collection is a rare event, so this list can be a resizable list. The
 * actual implementation is a linked list of blocks, each block N cache lines in size.
 * 
 * Alternative implementations of the garbage collection list are also possible, 
 * such as a (resizing or fixed-size) hash table or a resizing array.
 */

static const int        TABLE_SIZE = 24; // 1<<24 entries by default
static const uint32_t   EMPTY = 0;
static const uint32_t   WRITE_BIT = 1 << 31;           // 0x80000000
static const uint32_t   WRITE_BIT_R = ~(1 << 31);      // 0x7fffffff
static const uint32_t   TOMBSTONE = 0x7fffffff;

/**
 * Add uint32_t value to list, and return 1.
 * If this returns 0, it is likely an out-of-memory error.
 */
static int llgclist_put(llgcset_gclist_t *head, llgcset_gclist_t *tail, uint32_t *gclist_state, uint32_t value);

/**
 * Pop a value from the list, and return 0.
 * If this returns 0, there was no value to pop.
 */
static int llgclist_get(llgcset_gclist_t *head, llgcset_gclist_t *tail, uint32_t *gclist_state, uint32_t *value);

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
    dbs->gclist_head = 0;
    dbs->gclist_tail = 0;
    dbs->gclist_state = 0;
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
        llgclist_put(&dbs->gclist_head, &dbs->gclist_tail, &dbs->gclist_state, index);
    }
    return 1;
}

void llgcset_delete(const llgcset_t dbs, uint32_t index)
{
	// note that this is quite unsafe...
    dbs->table[index] = TOMBSTONE;
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



/******************************************
 * IMPLEMENTATION OF GCLIST
 * 
 * GCLIST is a FIFO queue implemented with
 * linked blocks aligned to the cache line
 ******************************************/
#define GCLIST_N_DATA   (GCLIST_CACHELINES*LINE_SIZE-12)/sizeof(uint32_t)

__attribute__ ((packed))
struct llgcset_gclist
{
    llgcset_gclist_t  next;  // pointer to next block
    uint16_t          start; // which can be read (0xffff = none)
    uint16_t          end;   // which can be written (GCLIST_N_DATA = full)
    char              pad[12-(sizeof(uint16_t)*2+sizeof(llgcset_gclist_t))];
    uint32_t          data[GCLIST_N_DATA];
};

/* gclist
 * 00 | 00: pointer
 * 08 | 04: start   
 * 10 | 06: end
 * 12 | 08: pad
 * 12 | 12: data[...] */

/**
 * Sanity check gclist
 * -> sizeof should be LINE_SIZE * GCLIST_CACHELINES
 */
struct sanity_check_gclist {
    int check_it[sizeof(struct llgcset_gclist)==GCLIST_CACHELINES*LINE_SIZE?0:-1];
};

/* The trick here is that we do not want to disturb other threads. We have to
 * assume that we are not the only one doing this, but let's just assume multiple processes
 * concurrently executing put and get. 
 * 
 * Some rules:
 * - The value "0" is reserved and cannot be used for data. If a value is "0", then there is
 *   no item at that spot.
 * - Start is 0xffff if there is no value in the list.
 * - Processes claim an item by using CAS to increase "start"
 *   After that, they can lazily claim the item (wait for it to be non-0). 
 *   Then, the item must be set to 0.
 * - Processes add an item by using CAS to increase "end". 
 *   After that, they can lazily set the "data" value.
 * - The thread that claims the last item will free() a block and update the HEAD reference.
 * - The procedure for creating extra blocks first CAS sets "next" to "0x1", then allocates memory, then
 *   lazily set "next" and "tail" to the new value. If the CAS fails, wait for the value to be set.
 */
 
 /**
  * To keep in mind: the ABA problem. This does not occur here. Hurray.
  */

static inline void llgclist_up_free(uint32_t *gclist_state) 
{
    // Up the free() Lock
    while (1) {
        uint32_t s = *gclist_state;
        while (s & 0xf0000000) {
            s = *(volatile uint32_t*)gclist_state;
            cpu_relax();
        }
        // assume the counter is big enough...
        if (cas(gclist_state, s, s+1)) break;
        cpu_relax();
    }
}

static inline void llgclist_down_free(uint32_t *gclist_state) 
{
    // Down the free() Lock
    while (1) {
        uint32_t s = *(volatile uint32_t*)gclist_state;
        if (cas(gclist_state, s, s-1)) break;
        cpu_relax();
    }
}

static inline void llgclist_lock_free(uint32_t *gclist_state)
{
    // Lock the free() Lock
    while (1) {
        uint32_t s = *(volatile uint32_t*)gclist_state;
        if (s == 1 && cas(gclist_state, 1, 0x80000001)) break;
        cpu_relax();
    }
}

static inline void llgclist_unlock_free(uint32_t *gclist_state)
{
    // Unlck the free() Lock
    *gclist_state = 1;
}


/**
 * Add uint32_t value to list, and return 1.
 * If this returns 0, it is likely an out-of-memory error.
 */
static int llgclist_put(llgcset_gclist_t *head, llgcset_gclist_t *tail, uint32_t *gclist_state, uint32_t value)
{
    llgclist_up_free(gclist_state);
    
    llgcset_gclist_t t = *tail;
    
    if (t==0) {
        // There is no tail yet. Ergo, there is also no head yet.
        if (cas(tail, 0, 1)) {
            t = (llgcset_gclist_t)rt_align(LINE_SIZE, sizeof(struct llgcset_gclist));
            memset(t, 0, sizeof(struct llgcset_gclist));
            t->start = 0xffff;
            // SFENCE; // Only relevant for architectures with out of order storing
            *tail = t;
            *head = t;
        } else {
            // wait for update by other process
            while ((t=*(volatile llgcset_gclist_t*)tail)<2) cpu_relax();
        }
    } else if (t==1) {
        // wait for update by other process
        while ((t=*(volatile llgcset_gclist_t*)tail)<2) cpu_relax();
    }
    
    LFENCE; // Prevent compiler/hardware optimizations around this place
    // Perhaps this is unnecessary, since there is a dependency relation.
    
    while (1) {
        uint16_t end = *(volatile uint16_t*)&t->end;
        if (end == GCLIST_N_DATA) {
            // This block is full! Create a new one!
            llgcset_gclist_t new_t;
            if (t->next==0) {
                if (cas(&t->next, 0, 1)) {
                    new_t = (llgcset_gclist_t)rt_align(LINE_SIZE, sizeof(struct llgcset_gclist));
                    memset(new_t, 0, sizeof(struct llgcset_gclist));
                    new_t->start = 0xffff;
                    // SFENCE; // Only relevant for architectures with out of order storing
                    t->next = new_t;
                } else {
                    // wait for update by other process
                    while ((new_t=*(volatile llgcset_gclist_t*)&t->next)<2) cpu_relax();
                }
            } else if (t->next==1) {
                // wait for update by other process
                while ((new_t=*(volatile llgcset_gclist_t*)&t->next)<2) cpu_relax();                
            } else new_t = t->next;
            t = new_t;
            continue; // go again
        } else {
            // Try to up it
            if (cas(&t->end, end, end+1)) {
                // We successfully upped it!
                t->data[end] = value;
                break;
            } else {
                // No success go back and try again
                continue;
            }
        }
    }
    
    llgclist_down_free(gclist_state);
    
    return 1;
}

/**
 * Pop a value from the list, and return 0.
 * If this returns 0, there was no value to pop.
 */
static int llgclist_get(llgcset_gclist_t *head, llgcset_gclist_t *tail, uint32_t *gclist_state, uint32_t *value)
{
    llgclist_up_free(gclist_state);
    
    int stop_0 = 0;

    llgcset_gclist_t t = *head;
    if (t == 0 || t == 1) stop_0 = 1; // either no head, or about to write (don't bother waiting)
    
    uint16_t s;
    while (!stop_0) {
        s = *(volatile uint16_t*)&t->start;
        if (s == 0xffff) { // empty
            stop_0 = 1;
        }
        else if (s == GCLIST_N_DATA) { // depleted, try next
            llgcset_gclist_t new_t = t->next;
            if (new_t == 0 || new_t == 1) { // either no next, or about to write
                stop_0 = 1;
            }
            else {
                cas(head, t, new_t); // someone has to update "head", don't care who
                // we used a cas to prevent 'bad' updates
                t = *head;
                s = t->start;
            }
        }
        else if (cas(&t->start, s, s+1)) break;
        else cpu_relax();
    }
    
    // We claimed s in t, OR stop_0 
    
    if (!stop_0) {
        // while it is 0, wait
        uint32_t v = 0;        
        while ((v=*(volatile uint32_t*)&t->data[s]) == 0) cpu_relax();
        *value = v;
        t->data[s] = 0;
        
        // If we are the last one, we have to free.
        if (s == (GCLIST_N_DATA-1)) {
            llgclist_lock_free(gclist_state);
            /* We are now in a lock. This must be BRIEF.
             * It is guaranteed that it impossible that any other process is
             * in get() or put() right now.
             * Thus, all we need to do is update head and tail */
            // MFENCE; // this is implicit in the locking
            if (*head == t) *head = t->next;
            if (*tail == t) *tail = t->next;
            // MFENCE; // this is implicit in the locking
            llgclist_unlock_free(gclist_state);
            free(t);
        }
    }

    llgclist_down_free(gclist_state);
    
    if (stop_0) return 0;
    return 1;
}
 
 