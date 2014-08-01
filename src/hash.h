#include <stdint.h>

#ifndef HASH_H
#define HASH_H

uint64_t rehash_mul(const void *key, const size_t len, const uint64_t seed);
uint64_t hash_mul(const void *key, const size_t len);

#endif
