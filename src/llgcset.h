#include "llcache.h"

#ifndef LLGCSET_H
#define LLGCSET_H

typedef struct llgcset* llgcset_t;

// Reasons for calling gc
typedef enum {
  gc_user,
  gc_hashtable_full
} gc_reason;

// Callbacks
typedef void (*llgcset_delete_f)(const void* cb_data, const void* data);
typedef void (*llgcset_pregc_f)(const void* cb_data, gc_reason reason);

struct llgcset
{
    size_t            padded_data_length;
    size_t            key_length;
    size_t            data_length;
    size_t            table_size;   // size of the hash table (number of slots) --> power of 2!
    size_t            threshold;    // number of iterations until TABLE_FULL error is thrown
    uint32_t          mask;         // size-1
    uint32_t          *table;       // table with hashes
    uint8_t           *data;        // table with values
    llgcset_delete_f  cb_delete;    // delete function (callback pre-delete)
    llgcset_pregc_f   cb_pregc;     // function called when full...
    void              *cb_data;
    llcache_t         deadlist;
    int               clearing;     // bit
};

#define llgcset_index_to_ptr(dbs, index) ((void*)&dbs->data[index*dbs->padded_data_length])
#define llgcset_ptr_to_index(dbs, ptr) ((size_t)(((size_t)ptr-(size_t)dbs->data)/dbs->padded_data_length))

void *llgcset_get_or_create(const llgcset_t dbs, const void* data, int *created, uint32_t *index);

llgcset_t llgcset_create(size_t key_length, size_t data_length, size_t table_size, llgcset_delete_f cb_delete, llgcset_pregc_f cb_pregc, void *cb_data);

void llgcset_clear(llgcset_t dbs);

void llgcset_free(llgcset_t dbs);

void llgcset_ref(const llgcset_t dbs, uint32_t index);

void llgcset_deref(const llgcset_t dbs, uint32_t index);

void llgcset_gc(const llgcset_t dbs, gc_reason reason);

void llgcset_print_size(llgcset_t dbs, FILE *f);

#endif
