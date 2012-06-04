#include "llsimplecache.h"
#include "ticketlock.h"

#ifndef LLGCSET_H
#define LLGCSET_H

typedef struct llgcset* llgcset_t;

// Callbacks
typedef void (*llgcset_delete_f)(const void* cb_data, const void* data);

struct llgcset
{
    size_t            padded_data_length;
    size_t            key_length;
    size_t            data_length;
    size_t            table_size;   // size of the hash table (number of slots) --> power of 2!
    size_t            threshold;    // number of iterations until TABLE_FULL error is thrown
    uint32_t          mask;         // size-1
    uint32_t          *_table;      // table with hashes
    uint8_t           *_data;       // table with values
    uint32_t          *table;       // table with hashes
    uint8_t           *data;        // table with values
    llgcset_delete_f  cb_delete;    // delete function (callback pre-delete)
    void              *cb_data;
    llsimplecache_t   deadlist;
    int               clearing;     // bit
    int               stack_lock;   // bitlock on
    ticketlock_t      stacklock;    // lock to protect stack access
    uint32_t          *stack;       // stack
    uint32_t          stacksize;
    uint32_t          stacktop;
};

#define llgcset_index_to_ptr(dbs, index) ((void*)&dbs->data[index*dbs->padded_data_length])
#define llgcset_ptr_to_index(dbs, ptr) ((size_t)(((size_t)ptr-(size_t)dbs->data)/dbs->padded_data_length))

void *llgcset_lookup(const llgcset_t dbs, const void *data, int *created, uint32_t *index);

llgcset_t llgcset_create(size_t key_length, size_t data_length, size_t table_size, llgcset_delete_f cb_delete, void *cb_data);

void llgcset_clear(llgcset_t dbs);

void llgcset_free(llgcset_t dbs);

void llgcset_ref(const llgcset_t dbs, uint32_t index);

void llgcset_deref(const llgcset_t dbs, uint32_t index);

void llgcset_gc(const llgcset_t dbs);

void llgcset_print_size(llgcset_t dbs, FILE *f);

size_t llgcset_get_filled(const llgcset_t dbs);

size_t llgcset_get_size(const llgcset_t dbs);
#endif
