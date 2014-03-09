#include <config.h>

#include <assert.h> // for assert
#include <stdint.h> // for uint64_t etc
#include <stdio.h>  // for printf
#include <stdlib.h>
#include <string.h> // for memcopy
#include <sys/mman.h> // for mmap

#include <atomics.h>
#include <llmsset.h>
#include <murmur.h>

#if USE_NUMA
#include <numa_tools.h>
#endif

/*
 *  1 bit  for data-filled
 *  1 bit  for hash-filled
 * 40 bits for the index
 * 22 bits for the hash
 */
#define DFILLED    ((uint64_t)0x8000000000000000)
#define HFILLED    ((uint64_t)0x4000000000000000)
#define MASK_INDEX ((uint64_t)0x000000ffffffffff)
#define MASK_HASH  ((uint64_t)0x3fffff0000000000)

static const uint8_t  HASH_PER_CL = ((LINE_SIZE) / 8);
static const uint64_t CL_MASK     = ~(((LINE_SIZE) / 8) - 1);
static const uint64_t CL_MASK_R   = ((LINE_SIZE) / 8) - 1;

/*
 * Example values with a LINE_SIZE of 64
 * HASH_PER_CL = 8
 * CL_MASK     = 0xFFFFFFFFFFFFFFF8
 * CL_MASK_R   = 0x0000000000000007
 */

// Calculate next index on a cache line walk
#define probe_sequence_next(cur, last) (((cur) = (((cur) & CL_MASK) | (((cur) + 1) & CL_MASK_R))) != (last))

/*
 * Note: garbage collection during lookup strictly forbidden
 * insert_index points to a starting point and is updated.
 */
void *
llmsset_lookup(const llmsset_t dbs, const void* data, uint64_t* insert_index, int* created, uint64_t* index)
{
    uint64_t hash_rehash = hash_mul(data, dbs->key_length);
    const uint64_t hash = hash_rehash & MASK_HASH;
    int i=0;

    for (;i<dbs->threshold;i++) {
        uint64_t idx = hash_rehash & dbs->mask;
        const uint64_t last = idx; // if next() sees idx again, stop.

        do {
            volatile uint64_t *bucket = &dbs->table[idx];
            uint64_t v = *bucket;

            if (!(v & HFILLED)) goto phase2;

            if (hash == (v & MASK_HASH)) {
                uint64_t d_idx = v & MASK_INDEX;
                register uint8_t *d_ptr = dbs->data + d_idx * dbs->padded_data_length;
                if (memcmp(d_ptr, data, dbs->key_length) == 0) {
                    if (index) *index = d_idx;
                    if (created) *created = 0;
                    return d_ptr;
                }
            }
        } while (probe_sequence_next(idx, last));

        hash_rehash = rehash_mul(data, dbs->key_length, hash_rehash);
    }

    uint64_t d_idx;
    uint8_t *d_ptr;
phase2:
    d_idx = *insert_index;
    while (1) {
        if (!d_idx) d_idx++; // do not use bucket 0 for data
        volatile uint64_t *ptr = dbs->table + d_idx;
        uint64_t h = *ptr;
        if (h & DFILLED) {
            d_idx = (d_idx+1) & dbs->mask;
        } else if (cas(ptr, h, h|DFILLED)) {
            d_ptr = dbs->data + d_idx * dbs->padded_data_length;
            memcpy(d_ptr, data, dbs->data_length);
            *insert_index = d_idx;
            break;
        }
    }

    // data has been inserted!
    uint64_t mask = hash | d_idx | HFILLED;

    // continue where we were...
    for (;i<dbs->threshold;i++) {
        uint64_t idx = hash_rehash & dbs->mask;
        const uint64_t last = idx; // if next() sees idx again, stop.

        do {
            volatile uint64_t *bucket = dbs->table + idx;
            uint64_t v;
phase2_restart:
            v = *bucket;

            if (!(v & HFILLED)) {
                uint64_t new_v = (v&DFILLED) | mask;
                if (!cas(bucket, v, new_v)) goto phase2_restart;

                if (index) *index = d_idx;
                if (created) *created = 1;
                return d_ptr;
            }

            if (hash == (v & MASK_HASH)) {
                uint64_t d2_idx = v & MASK_INDEX;
                register uint8_t *d2_ptr = dbs->data + d2_idx * dbs->padded_data_length;
                if (memcmp(d2_ptr, data, dbs->key_length) == 0) {
                    volatile uint64_t *ptr = dbs->table + d_idx;
                    uint64_t h = *ptr;
                    while (!cas(ptr, h, h&~(DFILLED))) { h = *ptr; } // uninsert data
                    if (index) *index = d2_idx;
                    if (created) *created = 0;
                    return d2_ptr;
                }
            }
        } while (probe_sequence_next(idx, last));

        hash_rehash = rehash_mul(data, dbs->key_length, hash_rehash);
    }

    return 0;
}

static inline int
llmsset_rehash_bucket(const llmsset_t dbs, uint64_t d_idx)
{
    const uint8_t * const d_ptr = dbs->data + d_idx * dbs->padded_data_length;
    uint64_t hash_rehash = hash_mul(d_ptr, dbs->key_length);
    uint64_t mask = (hash_rehash & MASK_HASH) | d_idx | HFILLED;

    int i;
    for (i=0;i<dbs->threshold;i++) {
        uint64_t idx = hash_rehash & dbs->mask;
        const uint64_t last = idx; // if next() sees idx again, stop.

        // no need for atomic restarts
        // we can assume there are no collisions (GC rehash phase)
        do {
            volatile uint64_t *bucket = &dbs->table[idx];
            uint64_t v = *bucket;
            if (v & HFILLED) continue;
            if (cas(bucket, v, mask | (v&DFILLED))) return 1;
        } while (probe_sequence_next(idx, last));

        hash_rehash = rehash_mul(d_ptr, dbs->key_length, hash_rehash);
    }

    return 0;
}

llmsset_t
llmsset_create(size_t key_length, size_t data_length, size_t table_size)
{
    llmsset_t dbs;
    if (posix_memalign((void**)&dbs, LINE_SIZE, sizeof(struct llmsset)) != 0) {
        fprintf(stderr, "Unable to allocate memory!");
        exit(1);
    }

    assert(key_length <= data_length);

    dbs->key_length = key_length;
    dbs->data_length = data_length;
    dbs->padded_data_length = LLMSSET_PDS(data_length);

    if (table_size < HASH_PER_CL) table_size = HASH_PER_CL;
    dbs->table_size = table_size;
    dbs->mask = dbs->table_size - 1;

    dbs->threshold = (64 - __builtin_clzl(table_size)) + 4; // doubling table_size increases threshold by 1

    dbs->table = (uint64_t*)mmap(0, dbs->table_size * sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (dbs->table == (uint64_t*)-1) { fprintf(stderr, "Unable to allocate memory!"); exit(1); }
    dbs->data = (uint8_t*)mmap(0, dbs->table_size * dbs->padded_data_length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (dbs->data == (uint8_t*)-1) { fprintf(stderr, "Unable to allocate memory!"); exit(1); }

#if USE_NUMA
    size_t f_size=0;
    numa_interleave(dbs->table, dbs->table_size * sizeof(uint64_t), &f_size);
    dbs->f_size = (f_size /= sizeof(uint64_t));
    f_size *= dbs->padded_data_length;
    numa_interleave(dbs->data, dbs->table_size * dbs->padded_data_length, &f_size);
#endif

    return dbs;
}

void
llmsset_free(llmsset_t dbs)
{
    munmap(dbs->table, dbs->table_size * sizeof(uint64_t));
    munmap(dbs->data, dbs->table_size * dbs->padded_data_length);
    free(dbs);
}

static void
llmsset_compute_multi(const llmsset_t dbs, size_t my_id, size_t n_workers, size_t *_first_entry, size_t *_entry_count)
{
#if USE_NUMA
    size_t node, node_index, index, total;
    // We are on node <node>, which is the <node_index>th node that we can use.
    // Also we are the <index>th worker on that node, out of <total> workers.
    numa_worker_info(my_id, &node, &node_index, &index, &total);
    // On each node, there are <cachelines_total> cachelines, <cachelines_each> per worker.
    const size_t entries_total    = dbs->f_size;
    const size_t cachelines_total = (entries_total * sizeof(uint64_t) + LINE_SIZE - 1) / LINE_SIZE;
    const size_t cachelines_each  = (cachelines_total + total - 1) / total;
    const size_t entries_each     = cachelines_each * LINE_SIZE / sizeof(uint64_t);
    const size_t first_entry      = node_index * dbs->f_size + index * entries_each;
    const size_t cap_node         = entries_total - index * entries_each;
    const size_t cap_total        = dbs->table_size - first_entry;
    *_first_entry = first_entry;
    *_entry_count = entries_each < cap_node ? entries_each < cap_total ? entries_each : cap_total :
                                              cap_node     < cap_total ? cap_node     : cap_total ;
#else
    const size_t entries_total    = dbs->table_size;
    const size_t cachelines_total = (entries_total * sizeof(uint64_t) + LINE_SIZE - 1) / LINE_SIZE;
    const size_t cachelines_each  = (cachelines_total + n_workers - 1) / n_workers;
    const size_t entries_each     = cachelines_each * LINE_SIZE / sizeof(uint64_t);
    const size_t first_entry      = my_id * entries_each;
    const size_t cap_total        = dbs->table_size - first_entry;
    *_first_entry = first_entry;
    *_entry_count = entries_each < cap_total ? entries_each : cap_total;
#endif
}

size_t
llmsset_get_insertindex_multi(const llmsset_t dbs, size_t my_id, size_t n_workers)
{
    size_t first_entry, entry_count;
    llmsset_compute_multi(dbs, my_id, n_workers, &first_entry, &entry_count);
    return first_entry;
}

static inline void
llmsset_clear_range(const llmsset_t dbs, uint64_t start, uint64_t count)
{
    memset(dbs->table + start, 0, sizeof(uint64_t) * count);
}

void
llmsset_clear(const llmsset_t dbs)
{
    llmsset_clear_range(dbs, 0, dbs->table_size);
}

void
llmsset_clear_multi(const llmsset_t dbs, size_t my_id, size_t n_workers)
{
    size_t first_entry, entry_count;
    llmsset_compute_multi(dbs, my_id, n_workers, &first_entry, &entry_count);
    if (entry_count <= 0) return;
    llmsset_clear_range(dbs, first_entry, entry_count);
}

int
llmsset_mark_unsafe(const llmsset_t dbs, uint64_t index)
{
    uint64_t v = dbs->table[index];
    dbs->table[index] = DFILLED;
    return v & DFILLED ? 0 : 1;
}

int
llmsset_mark_safe(const llmsset_t dbs, uint64_t index)
{
    while (1) {
        uint64_t v = *(volatile uint64_t *)&(dbs->table[index]);
        if (v & DFILLED) return 0;
        if (cas(&dbs->table[index], v, v|DFILLED)) return 1;
    }
}

static inline void
llmsset_rehash_range(const llmsset_t dbs, uint64_t start, uint64_t count)
{
    while (count) {
        if (dbs->table[start]&DFILLED) llmsset_rehash_bucket(dbs, start);
        start++;
        count--;
    }
}

void
llmsset_rehash(const llmsset_t dbs)
{
    llmsset_rehash_range(dbs, 0, dbs->table_size);
}

void
llmsset_rehash_multi(const llmsset_t dbs, size_t my_id, size_t n_workers)
{
    size_t first_entry, entry_count;
    llmsset_compute_multi(dbs, my_id, n_workers, &first_entry, &entry_count);
    if (entry_count <= 0) return;
    llmsset_rehash_range(dbs, first_entry, entry_count);
}

void
llmsset_print_size(llmsset_t dbs, FILE *f)
{
    fprintf(f, "Hash: %ld * 8 = %ld bytes; Data: %ld * %d = %ld bytes ",
        dbs->table_size, dbs->table_size * 8, dbs->table_size,
        dbs->padded_data_length, dbs->table_size * dbs->padded_data_length);
}

size_t
llmsset_get_filled(const llmsset_t dbs)
{
    size_t count=0;

    size_t i;
    for (i=0; i<dbs->table_size; i++) {
        if (dbs->table[i] & DFILLED) count++;
    }

    return count;
}

size_t
llmsset_get_size(const llmsset_t dbs)
{
    return dbs->table_size;
}
