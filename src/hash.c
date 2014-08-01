#include <stddef.h>
#include <stdint.h>
#include <hash.h>

uint64_t
FNV1A_Hash_Jesteress(const void *key, const size_t _len, const uint64_t seed)
{
    size_t len = _len;
    const uint64_t prime = 1099511628211;
    uint64_t hash = seed;
    const uint8_t *p = (const uint8_t *)key;

    for (; len >= 8; len-=8, p+=8) {
        hash = (hash ^ *(uint64_t*)p) * prime;
    }

    if (len & 4) {
        hash = (hash ^ *(uint32_t*)p) * prime;
        p += 4;
    }

    if (len & 2) {
        hash = (hash ^ *(uint16_t*)p) * prime;
        p += 2;
    }

    if (len & 1) hash = (hash ^ *p) * prime;

    return hash ^ (hash >> 32);
}

uint64_t
rehash_mul(const void *key, const size_t len, const uint64_t seed)
{
    return FNV1A_Hash_Jesteress(key, len, seed);
}

uint64_t
hash_mul(const void *key, const size_t len)
{
    return rehash_mul(key, len, 14695981039346656037LLU);
}
