#include "fast_hash.h"

#ifndef LLSET_H
#define LLSET_H

typedef int (*equals_f)(const void *a, const void *b, size_t length);

struct llset
{
    size_t    length;
    size_t    bytes;
    size_t    size;
    size_t    threshold;
    uint32_t  mask;
    uint8_t  *data;
    uint32_t *table;
    hash32_f  hash32;
    equals_f  equals;
};

typedef struct llset* llset_t;

void *llset_get_or_create(const llset_t dbs, const void* data, int *created, uint32_t* index);

llset_t llset_create(size_t length, size_t size, hash32_f hash32, equals_f equals);

void *llset_index_to_ptr(const llset_t dbs, uint32_t index);

uint32_t llset_ptr_to_index(const llset_t dbs, void *ptr);

void llset_clear(llset_t dbs);

void llset_free(llset_t dbs);

void llset_delete(const llset_t dbs, uint32_t index);

#endif
