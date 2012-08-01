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
    int               gc;           // counter
    int               stack_lock;   // bitlock on
    ticketlock_t      stacklock;    // lock to protect stack access
    uint32_t          *stack;       // stack
    uint32_t          stacksize;
    uint32_t          stacktop;
};

// Padded Data Size (per entry) macro
#define LLGCSET_PDS(x) \
    (((x) <= 2) ? (x) : ((x) <= 4) ? 4 : ((x) <= 8) ? 8 : (((x)+15)&(~15)))

static __attribute__((always_inline)) inline void* llgcset_index_to_ptr(const llgcset_t dbs, size_t index, size_t data_length)
{
    return ((void*)&dbs->data[index * LLGCSET_PDS(data_length)]);
}

static __attribute__((always_inline)) inline size_t llgcset_ptr_to_index(const llgcset_t dbs, void* ptr, size_t data_length)
{
    return ((size_t) ((((size_t)ptr) - ((size_t)dbs->data)) / LLGCSET_PDS(data_length)));
}

/*
// These are the old inline functions.
// Basically, the above optimizes generated code from IMUL with a memory read, to SAL 
// Use llgcset_XXX_to_XXX(dbs, XXX, sizeof(struct data_struct)) 

static __attribute__((always_inline)) inline void* llgcset_index_to_ptr(const llgcset_t dbs, size_t index)
{
    return ((void*)&dbs->data[index * dbs->padded_data_length]);
}

static __attribute__((always_inline)) inline size_t llgcset_ptr_to_index(const llgcset_t dbs, void* ptr)
{
    return ((size_t) ((((size_t)ptr) - ((size_t)dbs->data)) / dbs->padded_data_length));
}
*/

void *llgcset_lookup(const llgcset_t dbs, const void *data, int *created, uint32_t *index);

// Use lookup2 when GC is mutually exclusive with lookup2
void *llgcset_lookup2(const llgcset_t dbs, const void *data, int *created, uint32_t *index);
void *llgcset_lookup2_seq(const llgcset_t dbs, const void *data, int *created, uint32_t *index);

llgcset_t llgcset_create(size_t key_length, size_t data_length, size_t table_size, llgcset_delete_f cb_delete, void *cb_data);

void llgcset_clear(llgcset_t dbs);

void llgcset_free(llgcset_t dbs);

void llgcset_ref(const llgcset_t dbs, uint32_t index);

void llgcset_deref(const llgcset_t dbs, uint32_t index);

void llgcset_gc(const llgcset_t dbs);

// Use this one to do garbage collection with multiple workers...
// The caller is responsible for proper barriers, etc.
void llgcset_gc_multi(const llgcset_t dbs, size_t my_id, size_t n_workers);

void llgcset_print_size(llgcset_t dbs, FILE *f);

size_t llgcset_get_filled(const llgcset_t dbs);

size_t llgcset_get_size(const llgcset_t dbs);
#endif
