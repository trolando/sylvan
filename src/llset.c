#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include "llset.h"
#include "sylvan_runtime.h"

static const int        TABLE_SIZE = 24; // 1<<24 entries by default
static const uint32_t   EMPTY = 0;
static const uint32_t   WRITE_BIT = 1 << 31;           // 0x80000000
static const uint32_t   WRITE_BIT_R = ~(1 << 31);      // 0x7fffffff
static const uint32_t   TOMBSTONE = 0x7fffffff;

static int default_equals(const void *a, const void *b, size_t length)
{
    return memcmp(a, b, length) == 0;
}

// Number to add to table index to get next cache line
static const size_t CACHE_LINE_INT32 = (1 << CACHE_LINE) / sizeof (uint32_t);

// MASK for determining our current cache line...
// e.g. for 64 bytes cache on 4 byte uint32_t we get 16 per cache line, mask = 0xfffffff0
static const size_t CACHE_LINE_INT32_MASK = -((1 << CACHE_LINE) / sizeof (uint32_t));
//static const size_t CACHE_LINE_INT32_MASK_R = ~(-((1 << CACHE_LINE) / sizeof (uint32_t)));
static const size_t CACHE_LINE_INT32_MASK_R = (1 << CACHE_LINE) / sizeof(uint32_t) - 1;

static inline int next(uint32_t line, uint32_t *cur, uint32_t last) 
{
    *cur = (((*cur)+1) & (CACHE_LINE_INT32_MASK_R)) | line;
    return *cur != last;
}

void *llset_lookup_hash(const llset_t dbs, const void* data, int* created, uint32_t* index, uint32_t* hash)
{
    size_t              seed = 0;
    size_t              l = dbs->length;
    size_t              b = dbs->bytes;

    uint32_t            hash_rehash = hash ? *hash : dbs->hash32(data, b, 0);

	// hash_memo is the original hash for the data
    register uint32_t   hash_memo = hash_rehash & WRITE_BIT_R;
    // avoid collision of hash with reserved values 
    while (EMPTY == hash_memo /*|| WRITE_BIT & hash_memo*/ || TOMBSTONE == hash_memo)
        hash_memo = dbs->hash32((char *)data, b, ++seed) & WRITE_BIT_R;
    uint32_t            WAIT = hash_memo/* & WRITE_BIT_R*/;
    uint32_t            DONE = hash_memo | WRITE_BIT;
	// after this point, hash_memo is no longer used
	
    uint32_t           *tomb_bucket = 0;
    uint32_t            tomb_idx = 0;

    while (seed < dbs->threshold)
    {
        uint32_t idx = hash_rehash & dbs->mask;
        size_t line = idx & CACHE_LINE_INT32_MASK;
        size_t last = idx; // if next() sees idx again, stop.
        do
        {
            // do not use slots 0 and 1 (this is a hack)
            if (idx<2) continue;

            uint32_t *bucket = &dbs->table[idx];
            if (EMPTY == *bucket)
            {
                if (tomb_bucket != 0) {
                    memcpy(&dbs->data[tomb_idx * l], data, b);
					// SFENCE; // future x86 without strong store ordering
                    atomic32_write(tomb_bucket, DONE);

                    *index = tomb_idx;
                    *created = 1;
                    return &dbs->data[tomb_idx * l];
                }
                if (cas(bucket, EMPTY, WAIT))
                {
                    memcpy(&dbs->data[idx * l], data, b);
					// SFENCE; // future x86 without strong store ordering
                    atomic32_write(bucket, DONE);

                    *index = idx;
                    *created = 1;
                    return &dbs->data[idx * l];
                }
            }
			LFENCE;
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
            if (bucket != tomb_bucket && DONE == (atomic32_read(bucket) | WRITE_BIT))
            {
                // Wait until written or released (tombstone)
                while (WAIT == atomic32_read(bucket)) cpu_relax();

                if (atomic32_read(bucket) == DONE) {
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
        atomic32_write(tomb_bucket, DONE);

        *index = tomb_idx;
        *created = 1;
        return &dbs->data[tomb_idx * l];

    }

    // table is full
    return 0;
}

inline void *llset_get_or_create(const llset_t dbs, const void *data, int *created, uint32_t *index)
{
	int _created;
	uint32_t _index;
    void *result = llset_lookup_hash(dbs, data, &_created, &_index, NULL);
    if (created) *created=_created;
    if (index) *index=_index;
    return result;
}

llset_t llset_create(size_t length, size_t size, hash32_f hash32, equals_f equals)
{
    llset_t dbs = rt_align(CACHE_LINE_SIZE, sizeof(struct llset));
	dbs->length = length;
    dbs->hash32 = hash32 != NULL ? hash32 : SuperFastHash; // default hash function is hash_128_swapc
    dbs->equals = equals != NULL ? equals : default_equals;
    dbs->bytes = length;
    dbs->size = 1 << size;
    dbs->threshold = dbs->size / 100;
    dbs->mask = dbs->size - 1;
    dbs->table = rt_align(CACHE_LINE_SIZE, sizeof(uint32_t) * dbs->size);
    dbs->data = rt_align(CACHE_LINE_SIZE, dbs->size * length);
    memset(dbs->table, 0, sizeof(uint32_t) * dbs->size);
	// dont care about what is in "data" table
    return dbs;
}

void llset_delete(const llset_t dbs, uint32_t index)
{
	//  note that this may be quite unsafe...
	//  we need some sort of reference counting to do this properly...
	//  for now, it is sufficient if there is a guarantee that there is no mixing of deleting and adding
    dbs->table[index] = TOMBSTONE;
}

inline void llset_clear(llset_t dbs)
{
    memset(dbs->table, 0, sizeof(uint32_t) * dbs->size);
}

void llset_free(llset_t dbs)
{
    free(dbs->data);
    free(dbs->table);
    free(dbs);
}

