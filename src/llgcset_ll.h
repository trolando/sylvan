#include "fast_hash.h"
#include "runtime.h"

#ifndef LLGCSET_H
#define LLGCSET_H

struct llgcset;
typedef struct llgcset* llgcset_t;

struct llgcset_gclist;
typedef struct llgcset_gclist* llgcset_gclist_t;

typedef int (*equals_f)(const void *a, const void *b, size_t length);
typedef void (*delete_f)(const llgcset_t set, const void* data);

struct llgcset
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
    delete_f  cb_delete;    // delete function (callback pre-delete)
    // PAD
    char      pad[PAD(sizeof(size_t)*4+
                      sizeof(uint32_t)+
                      sizeof(uint32_t*)+
                      sizeof(uint8_t*)+
                      sizeof(hash32_f)+
                      sizeof(equals_f)+
                      sizeof(delete_f), LINE_SIZE)];
    // BEGIN CL 2
    llgcset_gclist_t gclist_head; // list of gc candidates (head)
    llgcset_gclist_t gclist_tail; // list of gc candidates (tail)
    // PAD2
    char      pad2[PAD(sizeof(llgcset_gclist_t)*2, LINE_SIZE)];
    // BEGIN CL 3
    uint32_t  gclist_state; // number of writers
};

#define llgcset_index_to_ptr(dbs, index) ((void*)&dbs->data[index*dbs->length])
#define llgcset_ptr_to_index(dbs, ptr) ((size_t)(((size_t)ptr-(size_t)dbs->data)/dbs->length))

void *llgcset_get_or_create(const llgcset_t dbs, const void* data, int *created, uint32_t* index);

llgcset_t llgcset_create(size_t length, size_t size, hash32_f hash32, equals_f equals, delete_f cb_delete);

void llgcset_clear(llgcset_t dbs);

void llgcset_free(llgcset_t dbs);

int llgcset_ref(const llgcset_t dbs, uint32_t index);

int llgcset_deref(const llgcset_t dbs, uint32_t index);

/**
 * Note that deleting an object is very dangerous and should not be necessary!
 * Instead, dereference it!
 */
void llgcset_delete(const llgcset_t dbs, uint32_t index);

#endif
