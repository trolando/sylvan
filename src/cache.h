#include <stdint.h> // for uint32_t etc

#ifndef CACHE_H
#define CACHE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef CACHE_MASK
#define CACHE_MASK 1
#endif

typedef struct cache_entry *cache_entry_t;

int cache_get(uint64_t a, uint64_t b, uint64_t c, uint64_t *res);

int cache_put(uint64_t a, uint64_t b, uint64_t c, uint64_t res);

void cache_create(size_t _cache_size, size_t _max_size);

void cache_free();

void cache_clear();

void cache_setsize(size_t size);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
