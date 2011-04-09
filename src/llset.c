#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <assert.h>

#include "llset.h"
#include "runtime.h"

static const int        TABLE_SIZE = 24; // 1<<24 entries by default
static const uint32_t   EMPTY = 0;
static const uint32_t   WRITE_BIT = 1 << 31;
static const uint32_t   WRITE_BIT_R = ~(1 << 31);
static const uint32_t   TOMBSTONE = 0x7fffffff;

int default_equals(const void *a, const void *b, size_t length)
{
    return memcmp(a, b, length) == 0;
}

size_t inc (size_t x)
{
    return x + 1;
}

size_t dec (size_t x)
{
    return x - 1;
}
int lt (size_t x, size_t y)
{
    return x < y;
}

int gte (size_t x, size_t y)
{
    return x >= y;
}

typedef size_t  (*op_f)   (size_t x);
typedef int     (*comp_f) (size_t x, size_t y);

// Number to add to table index to get next cache line
static const size_t CACHE_LINE_INT32 = (1 << CACHE_LINE) / sizeof (uint32_t);

// MASK for determining our current cache line...
static const size_t CACHE_LINE_INT32_MASK = -((1 << CACHE_LINE) / sizeof (uint32_t));

/**
 * Returns operators to walk up or down a cache line, depending on the starting location.
 */
static inline size_t setup_walk_the_line (size_t idx, op_f* op, comp_f* comp)
{
    size_t line = (idx & CACHE_LINE_INT32_MASK);
    if (idx - line < (CACHE_LINE_INT32 >> 1))
    {
        *op = inc;
        *comp = lt;
        line += CACHE_LINE_INT32;
    }
    else
    {
        *op = dec;
        *comp = gte;
    }
    return line;
}

void *llset_lookup_hash(const llset_t dbs, const void* data, int* created, uint32_t* index, uint32_t* hash)
{
    size_t              seed = 0;
    size_t              l = dbs->length;
    size_t              b = dbs->bytes;

    uint32_t            hash_rehash = hash ? *hash : dbs->hash32(data, b, 0);

    uint32_t            hash_memo = hash_rehash;
    // avoid collision of memoized hash with reserved values EMPTY and WRITE_BIT
    while (EMPTY == hash_memo || WRITE_BIT & hash_memo || TOMBSTONE == hash_memo)
        hash_memo = dbs->hash32((char *)data, b, ++seed);
    uint32_t            WAIT = hash_memo & WRITE_BIT_R;
    uint32_t            DONE = hash_memo | WRITE_BIT;

    op_f                op;
    comp_f              comp;

    uint32_t           *tomb_bucket = 0;
    uint32_t            tomb_idx = 0;

    while (seed < dbs->threshold)
    {
        uint32_t idx = hash_rehash & dbs->mask;
        size_t line = setup_walk_the_line (idx, &op, &comp);
        for (; comp (idx, line); idx = op (idx))
        {
            // DO NOT ALLOW VALUES BELOW 2 -- RESERVED BDDFALSE AND BDDTRUE
            // This is a very ugly hack
            if (idx<2) continue;

            uint32_t *bucket = &dbs->table[idx];
            if (EMPTY == *bucket)
            {
                if (tomb_bucket != 0) {
                    memcpy(&dbs->data[tomb_idx * l], data, b);
                    atomic32_write(tomb_bucket, DONE);

                    if (index != NULL) *index = tomb_idx;
                    if (created != NULL) *created = 1;
                    return &dbs->data[tomb_idx * l];
                }
                if (cas(bucket, EMPTY, WAIT))
                {
                    memcpy(&dbs->data[idx * l], data, b);
                    atomic32_write(bucket, DONE);

                    if (index != NULL) *index = idx;
                    if (created != NULL) *created = 1;
                    return &dbs->data[idx * l];
                }
            }
            if (TOMBSTONE == atomic32_read(bucket)) {
                // We may want to use this!
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
                while (WAIT == atomic32_read(bucket)) {}

                if (atomic32_read(bucket) == DONE) {
                    if (dbs->equals(&dbs->data[idx * l], data, b))
                    {
                        // Found existing!
                        if (tomb_bucket != 0) {
                            atomic32_write(tomb_bucket, TOMBSTONE);
                        }

                        if (index != NULL) *index = idx;
                        if (created != NULL) *created = 0;
                        return &dbs->data[idx * l];
                    }
                }
            }
        }
        hash_rehash = dbs->hash32(data, b, hash_rehash + (seed++));
    }

    // cough.
    if (tomb_bucket != 0) {
        memcpy(&dbs->data[tomb_idx * l], data, b);
        atomic32_write(tomb_bucket, DONE);

        if (index != NULL) *index = tomb_idx;
        if (created != NULL) *created = 1;
        return &dbs->data[tomb_idx * l];

    }

    rt_report_and_exit(1, "Hash table full\n");
    return 0;
}

inline void *llset_index_to_ptr(const llset_t dbs, uint32_t index)
{
    assert (index >= 2 && index < dbs->size);
    return &dbs->data[index * dbs->length];
}

uint32_t llset_ptr_to_index(const llset_t dbs, void *ptr)
{
    size_t result = ((size_t)ptr - (size_t)dbs->data);
    result /= dbs->length;
    assert (result >= 2 && result < dbs->size);
    return result;
}

inline void *llset_get_or_create(const llset_t dbs, const void *data, int *created, uint32_t *index)
{
    return llset_lookup_hash(dbs, data, created, index, NULL);
}

llset_t llset_create(size_t length, size_t size, hash32_f hash32, equals_f equals)
{
    llset_t dbs = rt_align(CACHE_LINE_SIZE, sizeof(struct llset));
    dbs->length = length;
    dbs->hash32 = hash32 != NULL ? hash32 : SuperFastHash;
    dbs->equals = equals != NULL ? equals : default_equals;
    dbs->bytes = length;
    dbs->size = 1 << size;
    dbs->threshold = dbs->size / 100;
    dbs->mask = dbs->size - 1;
    dbs->table = rt_align(CACHE_LINE_SIZE, sizeof(uint32_t) * dbs->size);
    dbs->data = rt_align(CACHE_LINE_SIZE, dbs->size * length);
    memset(dbs->table, 0, sizeof(uint32_t) * dbs->size);
    return dbs;
}

void llset_delete(const llset_t dbs, uint32_t index)
{
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

