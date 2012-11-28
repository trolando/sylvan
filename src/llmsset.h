#ifndef LLMSSET_H
#define LLMSSET_H

typedef struct llmsset* llmsset_t;
typedef struct llmsset_refset* llmsset_refset_t;

struct llmsset_refset
{
    llmsset_refset_t next;
    uint64_t index;
    int count;
};

struct llmsset
{
    uint64_t          *table;       // table with hashes
    uint8_t           *data;        // table with values
    llmsset_refset_t  refset;      // references
    size_t            table_size;   // size of the hash table (number of slots) --> power of 2!
    size_t            mask;         // size-1
    size_t            f_size;
    int16_t           padded_data_length;
    int16_t           key_length;
    int16_t           data_length;
    int16_t           threshold;    // number of iterations for insertion until returning error
};

// Padded Data Size (per entry) macro
#define LLMSSET_PDS(x) (((x) <= 2) ? (x) : ((x) <= 4) ? 4 : ((x) <= 8) ? 8 : (((x)+15)&(~15)))

static inline void *llmsset_index_to_ptr(const llmsset_t dbs, size_t index, size_t data_length)
{
    return dbs->data + index * LLMSSET_PDS(data_length);
}

static inline size_t llmsset_ptr_to_index(const llmsset_t dbs, void* ptr, size_t data_length)
{
    return ((size_t)ptr - (size_t)dbs->data) / LLMSSET_PDS(data_length);
}

void *llmsset_lookup(const llmsset_t dbs, const void *data, uint64_t *insert_index, int *created, uint64_t *index);

llmsset_t llmsset_create(size_t key_length, size_t data_length, size_t table_size);
void llmsset_free(llmsset_t dbs);

// The ref and deref functions implement an single linked list, head insertion
void llmsset_ref(const llmsset_t dbs, uint64_t index);
void llmsset_deref(const llmsset_t dbs, uint64_t index);

/*
 * To perform garbage collection, the user is responsible that no lookups are performed during the process.
 *
 * 1) call llmsset_clear
 * 2) for each index in the linked list "refset", call llmsset_mark
 * 3) for any externally managed nodes, call llmsset_mark
 * 4) call llmsset_rehash
 * 
 * Note that the _multi variants use numa_tools 
 */
void llmsset_clear(const llmsset_t dbs);
void llmsset_clear_multi(const llmsset_t dbs, size_t my_id, size_t n_workers);
// llmsset_mark returns a non-zero value when the node was unmarked
int llmsset_mark(const llmsset_t dbs, uint64_t index);
void llmsset_rehash(const llmsset_t dbs);
void llmsset_rehash_multi(const llmsset_t dbs, size_t my_id, size_t n_workers);

void llmsset_gc(const llmsset_t dbs);
void llmsset_gc_multi(const llmsset_t dbs, size_t my_id, size_t n_workers);

void llmsset_print_size(llmsset_t dbs, FILE *f);
size_t llmsset_get_filled(const llmsset_t dbs);
size_t llmsset_get_size(const llmsset_t dbs);
size_t llmsset_get_insertindex_multi(const llmsset_t dbs, size_t my_id, size_t n_workers);
#endif
