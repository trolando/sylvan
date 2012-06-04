#include <stdint.h>

#ifndef LLCACHE_H
#define LLCACHE_H

typedef struct llcache* llcache_t;

/*
 * Callback for deletion during llcache_clear()
 * This is NOT used for overwritten entries in put!
 */
typedef void (*llcache_delete_f)(const void *cb_data, const void* key);

/**
 * DESIGN:
 * Use the hold and release functions if you want to do operations before releasing lock on cache entry.
 * Do not use get or put operations between hold and release.
 * Do not use get or put operations in the delete callback.
 * Doing this may cause deadlocks.
 */

/**
 * LLCACHE_CREATE
 * Initialize new cache. The first <key_size> bytes of the data are used for hashing and to do
 * equality tests. The <table_size> is in bytes.
 */
llcache_t llcache_create(size_t key_size, size_t data_size, size_t table_size, llcache_delete_f cb_delete, void* cb_data);

/**
 * LLCACHE_CLEAR
 * This method walks the entire cache and deletes every entry.
 * If you set a callback, it is used just before deletion.
 * It is safe to use the cache in other threads to get/put during a clean.
 * It is safe to have multiple threads using clean, but inefficient.
 */
void llcache_clear(llcache_t dbs);
void llcache_clear_partial(llcache_t dbs, size_t first, size_t count);
void llcache_clear_unsafe(llcache_t dbs);

/**
 * Free all used memory.
 */
void llcache_free(llcache_t dbs);

/** 
 * LLCACHE_GET
 * Try to retrieve an entry from the cache.
 * Returns 1 when successful, or 0 when it doesn't exist.
 */
int llcache_get(const llcache_t dbs, void *data);
int llcache_get_relaxed(const llcache_t dbs, void *data);
int llcache_get_quicker(const llcache_t dbs, void *data);
int llcache_get_quicker_restart(const llcache_t dbs, void *data);

int llcache_get_and_hold(const llcache_t dbs, void *data, uint32_t *index);

/**
 * LLCACHE_PUT
 * Put an entry in the cache. 
 * Returns 2 when overwriting an existing entry. In that case, "data" will contain the original data.
 * Returns 1 when successful.
 * Returns 0 when an existing entry exists. In that case, data will contain the original data.
 */
int llcache_put(const llcache_t dbs, void *data);
int llcache_put_relaxed(const llcache_t dbs, void *data);
int llcache_put_quicker(const llcache_t dbs, void *data);

int llcache_put_and_hold(const llcache_t dbs, void *data, uint32_t *index);

/**
 * LLCACHE_RELEASE
 * Use this method to release cache buckets that have been held.
 */
void llcache_release(const llcache_t dbs, uint32_t index);

void llcache_print_size(llcache_t dbs, FILE *f);

#endif
