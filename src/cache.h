#include <stdio.h>  // for fprintf
#include <stdint.h> // for uint32_t etc
#include <stdlib.h> // for exit
#include <sys/mman.h> // for mmap

#include <atomics.h>

#ifndef CACHE_INLINE_H
#define CACHE_INLINE_H

#ifndef CACHE_MASK
#define CACHE_MASK 1
#endif

/**
 * This cache is designed to store a,b,c->res, with a,b,c,res 64-bit integers.
 *
 * Each cache bucket takes 32 bytes, 2 per cache line.
 * Each cache status bucket takes 4 bytes, 16 per cache line.
 * Therefore, size 2^N = 36*(2^N) bytes.
 */

typedef struct __attribute__((packed)) cache_entry {
    uint64_t            a;
    uint64_t            b;
    uint64_t            c;
    uint64_t            res;
} * cache_entry_t;

extern size_t             cache_size;         // power of 2
extern size_t             cache_max;          // power of 2
#if CACHE_MASK
extern size_t             cache_mask;         // cache_size-1
#endif
extern cache_entry_t      cache_table;
extern uint32_t*          cache_status;

// status: 0x80000000 - bitlock
//         0x7fff0000 - hash (part of the 64-bit hash not used to position)
//         0x0000ffff - tag (every put increases tag field)

/* Rotating 64-bit FNV-1a hash */
static inline uint64_t __attribute__((unused))
cache_hash(uint64_t a, uint64_t b, uint64_t c)
{
    const uint64_t prime = 1099511628211;
    uint64_t hash = 14695981039346656037LLU;
    hash = (hash ^ a) * prime;
    hash = (hash ^ b) * prime;
    hash = (hash ^ c) * prime;
    return hash;
}

static int __attribute__((unused))
cache_get(uint64_t a, uint64_t b, uint64_t c, uint64_t *res)
{
    const uint64_t hash = cache_hash(a, b, c);
#if CACHE_MASK
    volatile uint32_t *s_bucket = cache_status + (hash & cache_mask);
#else
    volatile uint32_t *s_bucket = cache_status + (hash % cache_size);
#endif
    const uint32_t s = *s_bucket;
    // abort if locked
    if (s & 0x80000000) return 0;
    // abort if different hash
    if ((s ^ (hash>>32)) & 0x7fff0000) return 0;
    // abort if key different
#if CACHE_MASK
    cache_entry_t bucket = cache_table + (hash & cache_mask);
#else
    cache_entry_t bucket = cache_table + (hash % cache_size);
#endif
    if (bucket->a != a || bucket->b != b || bucket->c != c) return 0;
    *res = bucket->res;
    compiler_barrier();
    // abort if status field changed after compiler_barrier()
    return *s_bucket == s ? 1 : 0;
}

static int __attribute__((unused))
cache_put(uint64_t a, uint64_t b, uint64_t c, uint64_t res)
{
    const uint64_t hash = cache_hash(a, b, c);
#if CACHE_MASK
    volatile uint32_t *s_bucket = cache_status + (hash & cache_mask);
#else
    volatile uint32_t *s_bucket = cache_status + (hash % cache_size);
#endif
    const uint32_t s = *s_bucket;
    // abort if locked
    if (s & 0x80000000) return 0;
    // abort if same hash
    const uint32_t hash_mask = (hash>>32) & 0x7fff0000;
    if ((s & 0x7fff0000) == hash_mask) return 0;
    // use cas to claim bucket
    const uint32_t new_s = ((s+1) & 0x0000ffff) | hash_mask;
    if (!cas(s_bucket, s, new_s | 0x80000000)) return 0;
    // cas succesful: write data
#if CACHE_MASK
    cache_entry_t bucket = cache_table + (hash & cache_mask);
#else
    cache_entry_t bucket = cache_table + (hash % cache_size);
#endif
    bucket->a = a;
    bucket->b = b;
    bucket->c = c;
    bucket->res = res;
    compiler_barrier();
    // after compiler_barrier(), unlock status field
    *s_bucket = new_s;
    return 1;
}

static void __attribute__((unused))
cache_create(size_t _cache_size, size_t _max_size)
{
#if CACHE_MASK
    // Cache size must be a power of 2
    if (__builtin_popcountll(_cache_size) != 1 || __builtin_popcountll(_max_size) != 1) {
        fprintf(stderr, "cache: Table size must be a power of 2!\n");
        exit(1);
    }
#endif

    cache_size = _cache_size;
    cache_max  = _max_size;
#if CACHE_MASK
    cache_mask = cache_size - 1;
#endif

    cache_table = (cache_entry_t)mmap(0, cache_max * sizeof(struct cache_entry), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    cache_status = (uint32_t*)mmap(0, cache_max * sizeof(uint32_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (cache_table == (cache_entry_t)-1 || cache_status == (uint32_t*)-1) {
        fprintf(stderr, "cache: Unable to allocate memory!\n");
        exit(1);
    }
}

static inline void __attribute__((unused))
cache_free()
{
    munmap(cache_table, cache_max * sizeof(struct cache_entry));
    munmap(cache_status, cache_max * sizeof(uint32_t));
}

static void __attribute__((unused))
cache_clear()
{
    // a bit silly, but this works just fine, and does not require writing 0 everywhere...
    cache_free();
    cache_create(cache_size, cache_max);
}

static void __attribute__((unused))
cache_setsize(size_t size)
{
    // easy solution
    cache_free();
    cache_create(size, cache_max);
}

#endif
