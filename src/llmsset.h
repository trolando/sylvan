#ifndef LLMSSET_H
#define LLMSSET_H

typedef struct llmsset
{
    uint64_t          *table;       // table with hashes
    uint8_t           *data;        // table with values
    size_t            table_size;   // size of the hash table (number of slots) --> power of 2!
    size_t            mask;         // size-1
    size_t            f_size;
    int16_t           padded_data_length;
    int16_t           key_length;
    int16_t           data_length;
    int16_t           threshold;    // number of iterations for insertion until returning error
} *llmsset_t;

/**
 * Calculate size of buckets in data array (padded)
 */
#define LLMSSET_PDS(x) (((x) <= 2) ? (x) : ((x) <= 4) ? 4 : ((x) <= 8) ? 8 : (((x)+15)&(~15)))

/**
 * Translate an index to a pointer (data array)
 */
static inline void*
llmsset_index_to_ptr(const llmsset_t dbs, size_t index, size_t data_length)
{
    return dbs->data + index * LLMSSET_PDS(data_length);
}

/**
 * Translate a pointer (data array) to index
 */
static inline size_t
llmsset_ptr_to_index(const llmsset_t dbs, void* ptr, size_t data_length)
{
    return ((size_t)ptr - (size_t)dbs->data) / LLMSSET_PDS(data_length);
}

/**
 * Create and free a lockless MS set
 */
llmsset_t llmsset_create(size_t key_length, size_t data_length, size_t table_size);
void llmsset_free(llmsset_t dbs);

/**
 * Core function: find existing data, or add new
 */
void *llmsset_lookup(const llmsset_t dbs, const void *data, uint64_t *insert_index, int *created, uint64_t *index);

/**
 * To perform garbage collection, the user is responsible that no lookups are performed during the process.
 *
 * 1) call llmsset_clear or llmsset_clear_multi
 * 2) call llmsset_mark for each entry to keep
 * 3) call llmsset_rehash or llmsset_rehash_multi
 *
 * Note that the _multi variants use numa_tools
 */
void llmsset_clear(const llmsset_t dbs);
void llmsset_clear_multi(const llmsset_t dbs, size_t my_id, size_t n_workers);

/**
 * llmsset_mark_... returns a non-zero value when the node was unmarked
 * The _safe version uses a CAS operation, the _unsafe version a normal memory operation.
 * Use the _unsafe version unless you are bothered by false negatives
 */
int llmsset_mark_unsafe(const llmsset_t dbs, uint64_t index);
int llmsset_mark_safe(const llmsset_t dbs, uint64_t index);

void llmsset_rehash(const llmsset_t dbs);
void llmsset_rehash_multi(const llmsset_t dbs, size_t my_id, size_t n_workers);

/**
 * Some information retrieval methods
 */
void llmsset_print_size(llmsset_t dbs, FILE *f);
size_t llmsset_get_filled(const llmsset_t dbs);
size_t llmsset_get_size(const llmsset_t dbs);
size_t llmsset_get_insertindex_multi(const llmsset_t dbs, size_t my_id, size_t n_workers);
#endif
