#include "fast_hash.h"

#ifndef LLSET_H
#define LLSET_H

typedef int (*equals_f)(const void *a, const void *b, size_t length);

struct llset
{
    size_t    length;       // size of each data entry in the table (possibly padded)
    size_t    bytes;        // size of each data entry (actual size)
    size_t    size;         // size of the hash table (number of slots) --> power of 2!
    size_t    threshold;    // number of iterations until TABLE_FULL error is thrown
    uint32_t  mask;         // size-1
    uint32_t *table;        // table with hashes
    uint8_t  *data;         // table with values
    hash32_f  hash32;       // hash function
    equals_f  equals;       // equals function
};

typedef struct llset* llset_t;

#define llset_index_to_ptr(dbs, index) ((void*)&dbs->data[index*dbs->length])
#define llset_ptr_to_index(dbs, ptr) ((size_t)(((size_t)ptr-(size_t)dbs->data)/dbs->length))

void *llset_get_or_create(const llset_t dbs, const void* data, int *created, uint32_t* index);

llset_t llset_create(size_t length, size_t size, hash32_f hash32, equals_f equals);

void llset_clear(llset_t dbs);

void llset_free(llset_t dbs);

void llset_deref(const llset_t dbs, uint32_t index);

void llset_delete(const llset_t dbs, uint32_t index);

#endif
