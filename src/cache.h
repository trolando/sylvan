#include <stdio.h>  // for fprintf
#include <stdint.h> // for uint32_t etc
#include <sys/mman.h> // for mmap

#include <atomics.h>

#if USE_NUMA
#include <numa_tools.h>
#endif

#ifndef CACHE_INLINE_H
#define CACHE_INLINE_H

/**
 * This cache is designed to store (a,b,c)->d,
 * with a,b,c 64-bit integers, and d is a 40-bit integer
 *
 * Each cache bucket takes 32 bytes, 2 buckets per cache line.
 */

typedef struct __attribute__((packed)) cache_entry {
    volatile uint64_t   res;
    uint64_t            a;
    uint64_t            b;
    uint64_t            c;
} * cache_entry_t;

static size_t             cache_size;         // power of 2
static size_t             cache_mask;         // cache_size-1
static cache_entry_t      cache_table;

static inline uint64_t
cache_rotl64(uint64_t x, int8_t r)
{
    return (x << r) | (x >> (64 - r));
}

// FNV1A_Hash_Jesteress
static inline uint64_t
cache_hash(uint64_t a, uint64_t b, uint64_t c)
{
    const uint64_t prime = 1099511628211;
    uint64_t hash = 14695981039346656037LLU;

    hash = (hash ^ cache_rotl64(a, 5)) * prime;
    hash = (hash ^ cache_rotl64(b, 7)) * prime;
    hash = (hash ^ cache_rotl64(c, 11)) * prime;

    return hash ^ (hash >> 32);
}

static cache_entry_t
cache_bucket(uint64_t a, uint64_t b, uint64_t c)
{
    return cache_table + (cache_hash(a, b, c) & cache_mask);
}

// Geduld, mijn vriend, geduld, zei de slak tegen de schildpad...

static inline int __attribute__((unused))
cache_get(const cache_entry_t bucket, uint64_t a, uint64_t b, uint64_t c, uint64_t *res)
{
    const uint64_t _res = bucket->res;
    // abort if locked
    if (_res & 0x8000000000000000) return 0;
    // abort if key different
    if (bucket->a != a || bucket->b != b || bucket->c != c) return 0;
    compiler_barrier();
    if (bucket->res != _res) return 0;
    *res = _res & 0x000000ffffffffff;
    return 1;
}

static inline int __attribute__((unused))
cache_put(const cache_entry_t bucket, uint64_t a, uint64_t b, uint64_t c, uint64_t res)
{
    const uint64_t _res = bucket->res;
    // abort if locked or may exist
    if (_res & 0x8000000000000000) return 0;
    if (bucket->a == a && bucket->b == b && bucket->c == c) return 0;

    uint64_t new_res = ((_res & 0x7fffff0000000000) + 0x0000010000000000) & 0x7fffff0000000000;
    new_res |= res;
    if (!cas(&bucket->res, _res, new_res | 0x8000000000000000)) return 0;

    bucket->a = a;
    bucket->b = b;
    bucket->c = c;
    compiler_barrier();
    bucket->res = new_res;

    return 1;
}

static void
cache_create(size_t _cache_size)
{
    // Cache size must be a power of 2
    if (__builtin_popcountll(_cache_size) != 1) {
        fprintf(stderr, "cache: Table size must be a power of 2!\n");
        exit(1);
    }

    cache_size = _cache_size;
    cache_mask = cache_size - 1;

    cache_table = (cache_entry_t)mmap(0, cache_size * sizeof(struct cache_entry), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (cache_table == (cache_entry_t)-1) {
        fprintf(stderr, "cache: Unable to allocate memory!\n");
        exit(1);
    }

#if USE_NUMA
    numa_interleave(cache_table, cache_size * sizeof(struct cache_entry), 0);
#endif
}

static void __attribute__((unused))
cache_clear()
{
    munmap(cache_table, cache_size * sizeof(struct cache_entry));
    cache_table = (cache_entry_t)mmap(0, cache_size * sizeof(struct cache_entry), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
#if USE_NUMA
    numa_interleave(cache_table, cache_size * sizeof(struct cache_entry), 0);
#endif
}

static inline void __attribute__((unused))
cache_free()
{
    munmap(cache_table, cache_size * sizeof(struct cache_entry));
}

#endif
