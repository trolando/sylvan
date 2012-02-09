#include "fast_hash.h"
#include "sylvan_runtime.h"
#include "llcache.h"

#ifndef LLGCSET_H
#define LLGCSET_H

typedef struct llgcset* llgcset_t;

//struct llgcset_gclist;
//typedef struct llgcset_gclist* llgcset_gclist_t;

// Reasons for calling gc
typedef enum {
  gc_user,
  gc_hashtable_full
} gc_reason;

// Callbacks
typedef int (*equals_f)(const void *a, const void *b, size_t length);
typedef void (*delete_f)(const llgcset_t set, const void* data);
typedef void (*pre_gc_f)(const llgcset_t set, gc_reason reason);

struct llgcset
{
    size_t    length;       // size of each data entry in the table (possibly padded)
    size_t    bytes;        // size of each data entry (actual size)
    size_t    size;         // size of the hash table (number of slots) --> power of 2!
    size_t    threshold;    // number of iterations until TABLE_FULL error is thrown
    uint32_t *table;        // table with hashes
    uint8_t  *data;         // table with values
    hash32_f  hash32;       // hash function
    equals_f  equals;       // equals function
    delete_f  cb_delete;    // delete function (callback pre-delete)
    pre_gc_f  pre_gc;       // function called when full...
    uint32_t  mask;         // size-1
    llcache_t deadlist;
    int       clearing;     // bit
};

#define llgcset_index_to_ptr(dbs, index) ((void*)&dbs->data[index*dbs->length])
#define llgcset_ptr_to_index(dbs, ptr) ((size_t)(((size_t)ptr-(size_t)dbs->data)/dbs->length))

void *llgcset_get_or_create(const llgcset_t dbs, const void* data, int *created, uint32_t* index);

llgcset_t llgcset_create(size_t key_size, size_t table_size, size_t gc_size, hash32_f hash32, equals_f equals, delete_f cb_delete, pre_gc_f pre_gc);

void llgcset_clear(llgcset_t dbs);

void llgcset_free(llgcset_t dbs);

void llgcset_ref(const llgcset_t dbs, uint32_t index);

void llgcset_deref(const llgcset_t dbs, uint32_t index);

void llgcset_gc(const llgcset_t dbs, gc_reason reason);

#endif
