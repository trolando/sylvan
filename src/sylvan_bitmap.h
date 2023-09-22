#ifndef SYLVAN_BITMAP_H
#define SYLVAN_BITMAP_H

#ifndef __cplusplus
  #include <stdatomic.h>
  #define memory_order memory_order
#else
  // Compatibility with C11
  #define memory_order std::memory_order
#endif

#include <stddef.h>
#include <stdint.h>
#include <limits.h>     // for CHAR_BIT


#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

// use uint64_t/ uint32_t to advantage the usual 64 bytes per cache line
typedef uint64_t bitmap_bucket_t;

typedef struct bitmap_s {
    bitmap_bucket_t *buckets;
    size_t size;
} bitmap_t;

typedef struct atomic_bitmap_s {
    _Atomic (bitmap_bucket_t) *container;
    size_t size;
} atomic_bitmap_t;

static const size_t npos = (bitmap_bucket_t)-1;
static const size_t NBITS_PER_BUCKET = sizeof(bitmap_bucket_t) * CHAR_BIT;

#define BUCKET_OFFSET(b)        ((b) / NBITS_PER_BUCKET)
#define BIT_OFFSET(b)           ((b) % NBITS_PER_BUCKET)
#define BIT_MASK(b)             (0x8000000000000000LL >> (BIT_OFFSET(b)))
#define NBUCKETS(b)             (((b) + NBITS_PER_BUCKET-1) / NBITS_PER_BUCKET)

/*
 * Allocate a new bitmap with the given size
 */
void bitmap_init(bitmap_t* bitmap, size_t new_size);

/*
 * Free the bitmap
 */
void bitmap_deinit(bitmap_t *bitmap);

/**
 * Set the bit at position n to 1, if it was 0.
 */
void bitmap_set(bitmap_t *bitmap, size_t pos);

/**
 * Set the bit at position n to 0, if it was 1.
 */
void bitmap_clear(bitmap_t *bitmap, size_t pos);

/**
 * Get the bit at position n.
 */
char bitmap_get(const bitmap_t *bitmap, size_t pos);

/**
 * Set the bit at position n to 0, if it was 1.
 */
void bitmap_clear_all(bitmap_t *bitmap);

/**
 * Get the first bit set to 1
 */
size_t bitmap_first(bitmap_t *bitmap);

/**
 * Get the first bit set to 1 (atomic version)
 */
size_t bitmap_first_from(bitmap_t *bitmap, size_t bucket_idx);

/**
 * Get the last bit set to 1
 */
size_t bitmap_last(bitmap_t *bitmap);

/**
 * Get the last 1-bit position from the given word index
 */
size_t bitmap_last_from(bitmap_t *bitmap, size_t pos);

/**
 * Get the next bit set to 1
 */
size_t bitmap_next(bitmap_t *bitmap, size_t pos);

/**
 * Get the previous bit set to 1
 */
size_t bitmap_prev(bitmap_t *bitmap, size_t pos);

/**
 * Count the number of bits set to 1
 */
size_t bitmap_count(bitmap_t *bitmap);

/*
 * Allocate a new bitmap with the given size (heap allocation)
 */
void atomic_bitmap_init(atomic_bitmap_t* bitmap, size_t new_size);

/*
 * Free the bitmap
 */
void atomic_bitmap_deinit(atomic_bitmap_t *bitmap);

/**
 * Set all bits to 0
 */
void atomic_bitmap_clear_all(atomic_bitmap_t *bitmap);

/**
 * Get the first bit set to 1 (atomic version)
 */
size_t atomic_bitmap_first(atomic_bitmap_t *bitmap);

/**
 * Get the first 1-bit position from the given word index (atomic version)
 */
size_t atomic_bitmap_first_from(atomic_bitmap_t *bitmap, size_t word_idx);

/**
 * Get the last bit set to 1
 */
size_t atomic_bitmap_last(atomic_bitmap_t *bitmap);

/*
 * Get the last 1-bit position from the given word index (atomic version)
 */
size_t atomic_bitmap_last_from(atomic_bitmap_t *bitmap, size_t pos);

/**
 * Get the next bit set to 1 (atomic version)
 */
size_t atomic_bitmap_next(atomic_bitmap_t *bitmap, size_t pos);

/**
 * Get the previous bit set to 1
 */
size_t atomic_bitmap_prev(atomic_bitmap_t *bitmap, size_t pos);

/**
 * Set the bit at position n to 1, if it was 0. (Atomic version)
 */
void atomic_bitmap_set(atomic_bitmap_t *bitmap, size_t pos, memory_order ordering);

/**
 * Set the bit at position n to 0, if it was 1. (Atomic version)
 */
void atomic_bitmap_clear(atomic_bitmap_t *bitmap, size_t pos, memory_order ordering);

/**
 * Get the bit at position n. (Atomic version)
 */
int atomic_bitmap_get(const atomic_bitmap_t *bitmap, size_t pos, memory_order ordering);


#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif // SYLVAN_BITMAP_H