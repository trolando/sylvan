#include <stdint.h>

#ifndef LLSIMPLECACHE_H
#define LLSIMPLECACHE_H

typedef struct llsimplecache* llsimplecache_t;

/*
 * Callback for deletion during llsimplecache_clear()
 * This is NOT used for overwritten entries in put!
 */
typedef void (*llsimplecache_delete_f)(const void *cb_data, const uint32_t data);

/**
 * LLCACHE_CREATE
 * Initialize new cache. The first <key_size> bytes of the data are used for hashing and to do
 * equality tests. The <table_size> is in bytes.
 */
llsimplecache_t llsimplecache_create(size_t table_size, llsimplecache_delete_f cb_delete, void* cb_data);

/**
 * LLCACHE_CLEAR
 * This method walks the entire cache and deletes every entry.
 * If you set a callback, it is used just before deletion.
 * It is safe to use the cache in other threads to get/put during a clean.
 * It is safe to have multiple threads using clean, but inefficient.
 */
void llsimplecache_clear(const llsimplecache_t dbs);
void llsimplecache_clear_partial(const llsimplecache_t dbs, size_t first, size_t count);
void llsimplecache_clear_multi(const llsimplecache_t dbs, size_t my_id, size_t n_workers);

/**
 * Free all used memory.
 */
void llsimplecache_free(const llsimplecache_t dbs);

/**
 * LLCACHE_PUT
 * Put an entry in the cache. The <<hash>> parameter is optional (default value: 0)
 * Returns 2 when overwriting an existing entry. In that case, "data" will contain the original data.
 * Returns 1 when successful.
 * Returns 0 when an existing entry exists. In that case, data will contain the original data.
 */
int llsimplecache_put(const llsimplecache_t dbs, uint32_t *data, uint64_t hash);

void llsimplecache_print_size(const llsimplecache_t dbs, FILE *f);

#endif
