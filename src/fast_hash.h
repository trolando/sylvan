#ifndef FAST_HASH_H
#define FAST_HASH_H

#include <stdint.h>

typedef uint32_t (*hash32_f)(const void *key, int len, uint32_t seed);
typedef uint64_t (*hash64_f)(const void *key, int len, uint64_t seed);

extern uint32_t SuperFastHash (const void *data, int len, uint32_t hash);

extern uint32_t oat_hash(const void *data, int len, uint32_t seed);

extern int mix (int a, int b, int c);

#endif
