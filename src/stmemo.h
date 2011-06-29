#include "fast_hash.h"

#ifndef LLSET_H
#define LLSET_H

typedef int (*equals_f)(const void *a, const void *b, size_t length);

struct stmemo
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

typedef struct stmemo* stmemo_t;

#define stmemo_index_to_ptr(dbs, index) ((void*)&dbs->data[index*dbs->length])
#define stmemo_ptr_to_index(dbs, ptr) ((size_t)(((size_t)ptr-(size_t)dbs->data)/dbs->length))

void *stmemo_get_or_create(const stmemo_t dbs, const void* data, int *created, uint32_t* index);

stmemo_t stmemo_create(size_t length, size_t size, hash32_f hash32, equals_f equals);

//void *stmemo_index_to_ptr(const stmemo_t dbs, uint32_t index);

//uint32_t stmemo_ptr_to_index(const stmemo_t dbs, void *ptr);

void stmemo_clear(stmemo_t dbs);

void stmemo_free(stmemo_t dbs);

void stmemo_delete(const stmemo_t dbs, uint32_t index);

#endif
