#include "config.h"

#include <stdlib.h>
#include <stdio.h>  // for printf
#include <stdint.h> // for uint32_t etc
#include <string.h> // for memset
#include <assert.h> // for assert
#include <sys/mman.h> // for mmap

#include "atomics.h"
#include "llsimplecache.h"

#ifdef HAVE_NUMA_H
#include "numa_tools.h"
#endif

struct llsimplecache
{
    size_t                 cache_size;  
    uint32_t               mask;         // size-1
    uint32_t               *table;       // table with data
    size_t                 fragment_size;
    llsimplecache_delete_f cb_delete;    // delete function (callback pre-delete)
    void                   *cb_data;
};

// 64-bit hashing function, http://www.locklessinc.com (hash_mul.s)
unsigned long long hash_mul(const void* data, unsigned long long len);

// We assume LINE_SIZE is a multiple of 4 and a power of 2
#define        HASH_PER_CL ((uint32_t)((LINE_SIZE) / 4))
#define        CL_MASK     ((uint32_t)(~(HASH_PER_CL - 1))) 
#define        CL_MASK_R   ((uint32_t)(HASH_PER_CL - 1))

#define        EMPTY       ((uint32_t)(0))

/* 
 * Example values with a LINE_SIZE of 64
 * HASH_PER_CL = 16
 * CL_MASK     = 0xFFFFFFF0
 * CL_MASK_R   = 0x0000000F
 */

// Calculate next index on a cache line walk
static inline int next(uint32_t *cur, uint32_t last) 
{
    return (*cur = (*cur & CL_MASK) | ((*cur + 1) & CL_MASK_R)) != last;
}

int llsimplecache_put(const llsimplecache_t dbs, uint32_t *data, uint64_t hash) 
{
    if (hash == 0) {
        hash = hash_mul(data, sizeof(uint32_t));
        if (hash == 0) hash++; // Avoid the value 0
    }

    uint32_t f_idx = hash & dbs->mask;
    uint32_t idx = f_idx;

    register const uint32_t d = *data;

    do {
        register volatile uint32_t *bucket = &dbs->table[idx];

        register uint32_t v;
restart_bucket:
        v = *bucket;

        // Check empty
        if (v == EMPTY) {
            if (cas(bucket, EMPTY, d)) return 1;
            goto restart_bucket;
        }

        // Check existing
        if (v == d) return 0;                    
    } while (next(&idx, f_idx));

    // If we are here, the cache line is full.
    // Claim first bucket
    register volatile uint32_t *bucket = &dbs->table[f_idx];
    while (1) {
        register const uint32_t v = *bucket;
        if (v == d) return 0;
        if (cas(bucket, v, d)) {
            *data = v;
            return 2;
        }
    }
}

static inline unsigned next_pow2(unsigned x)
{
    if (x <= 2) return x;
    return (1ULL << 32) >> __builtin_clz(x - 1);
}

llsimplecache_t llsimplecache_create(size_t cache_size, llsimplecache_delete_f cb_delete, void *cb_data)
{
    llsimplecache_t dbs;
    posix_memalign((void**)&dbs, LINE_SIZE, sizeof(struct llsimplecache));
    
    if (cache_size < HASH_PER_CL) cache_size = HASH_PER_CL;
    assert(next_pow2(cache_size) == cache_size);
    dbs->cache_size = cache_size;
    dbs->mask = dbs->cache_size - 1;

    dbs->table = mmap(0, dbs->cache_size * sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (dbs->table == (uint32_t*)-1) { fprintf(stderr, "Unable to allocate memory!"); exit(1); }

#ifdef HAVE_NUMA_H
    numa_interleave(dbs->table, dbs->cache_size * sizeof(uint32_t), &dbs->fragment_size);
#endif

    dbs->cb_delete = cb_delete; // can be NULL
    dbs->cb_data   = cb_data;

    return dbs;
}

void llsimplecache_clear(const llsimplecache_t dbs)
{
    llsimplecache_clear_partial(dbs, 0, dbs->cache_size);
}

void llsimplecache_clear_partial(const llsimplecache_t dbs, size_t first, size_t count)
{
    if (first < 0 || first >= dbs->cache_size) return;
    if (first + count > dbs->cache_size) count = dbs->cache_size - first;

    if (dbs->cb_delete == NULL) {
        memset(&dbs->table[first], 0, 4*count);
        return;
    }

    register volatile uint32_t *bucket = &dbs->table[first];
    while (count) {
        while(1) {
            register uint32_t data = *bucket;
            if (data == 0) break;
            *bucket = 0;
            dbs->cb_delete(dbs->cb_data, data);
        }
        bucket++; // next!
        count--;
    }
}

void llsimplecache_clear_multi(const llsimplecache_t dbs, size_t my_id, size_t n_workers)
{
#ifdef HAVE_NUMA_H
    int node, node_index, index, total;
    numa_worker_info(my_id, &node, &node_index, &index, &total);
    // we only clear that of our own node...
    size_t cachelines_total = (dbs->fragment_size + LINE_SIZE - 1) / (LINE_SIZE);
    size_t cachelines_each  = (cachelines_total   + total     - 1) / total;
    size_t first            = node_index * dbs->fragment_size + index * cachelines_each * LINE_SIZE;
    size_t max              = cachelines_total - index * cachelines_each;
    if (max > 0) {
        size_t count = max > cachelines_each ? cachelines_each : max;
        llsimplecache_clear_partial(dbs, first / 4, count / 4);
    }
#else
    size_t cachelines_total = (dbs->cache_size  + HASH_PER_CL - 1) / HASH_PER_CL;
    size_t cachelines_each  = (cachelines_total + n_workers   - 1) / n_workers;
    size_t first            = my_id * cachelines_each * HASH_PER_CL;
    llsimplecache_clear_partial(dbs, first, cachelines_each * HASH_PER_CL);
#endif
} 

void llsimplecache_free(const llsimplecache_t dbs)
{
    munmap(dbs->table, dbs->cache_size * sizeof(uint32_t));
    free(dbs);
}

void llsimplecache_print_size(const llsimplecache_t dbs, FILE *f)
{
    fprintf(f, "4 * %ld = %ld bytes", dbs->cache_size, dbs->cache_size * sizeof(uint32_t));
}
