/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sylvan_int.h>
#include <sylvan_align.h>

#include <errno.h>  // for errno
#include <string.h> // memset
#include <sys/mman.h> // for mmap

#define MASK_HASH    ((uint64_t)0xffffff0000000000) // 24 bits for the hash
#define MASK_INDEX   ((uint64_t)0x000000ffffffffff) // 40 bits for the index

DECLARE_THREAD_LOCAL(my_region, uint64_t);

VOID_TASK_0(llmsset_reset_region)
{
    LOCALIZE_THREAD_LOCAL(my_region, uint64_t);
    my_region = (uint64_t) -1; // no region
    SET_THREAD_LOCAL(my_region, my_region);
}

VOID_TASK_IMPL_0(llmsset_reset_all_regions)
{
    clear_aligned(nodes->bitmap1, nodes->max_size / (512 * 8));
    TOGETHER(llmsset_reset_region);
}

static uint64_t
claim_data_bucket(const llmsset_t dbs)
{
    // get my region, based on which worker are we
    // every worker has a region, and every region has 8 buckets
    LOCALIZE_THREAD_LOCAL(my_region, uint64_t);

    for (;;) {
        if (my_region != (uint64_t) -1) {
            // find empty bucket in region <my_region>
            _Atomic (uint64_t) *ptr = dbs->bitmap2 + (my_region * 8);
            int i = 0;
            // With 64 bytes per cacheline, there are 8 64-bit values per cacheline.
            for (; i < 8;) {
                uint64_t v = atomic_load_explicit(ptr, memory_order_relaxed);
                if (v != 0xffffffffffffffffLL) {
                    int j = __builtin_clzll(~v);
                    *ptr |= (0x8000000000000000LL >> j);
                    size_t index = (8 * my_region + i) * 64 + j;
                    return index;
                }
                i++;
                ptr++;
            }
        } else {
            // special case on startup or after garbage collection
            // several lines below, my_region will be increased by 1 and the entry claimed.
            my_region = -1 + (lace_get_worker()->worker * (dbs->table_size / (64 * 8))) / lace_workers();
        }

        const uint64_t n_regions = (dbs->table_size / (64 * 8));
        uint64_t count = n_regions;
        for (;;) {
            // check if table maybe full
            if (count-- == 0) {
                // at this point, we claim the table is full but in reality it just means
                // that there are no regions left to claim
                return (uint64_t) -1;
            }
            my_region += 1;
            if (my_region >= n_regions) my_region = 0;
            // try to claim the region <my_region>
            _Atomic (uint64_t) *ptr = dbs->bitmap1 + (my_region / 64);
            uint64_t mask = 0x8000000000000000LL >> (my_region & 63);
            uint64_t v;
            restart:
            v = atomic_load_explicit(ptr, memory_order_relaxed);
            if (v & mask) {
                continue; // <region> is already taken by some other worker
            }
            if (atomic_compare_exchange_weak(ptr, &v, v | mask)) {
                break; // success! <region> is now claimed by me
            } else {
                goto restart;
            }
        }
        // assign thread local variable <my_region> to me
        SET_THREAD_LOCAL(my_region, my_region);
    }
}

static void
release_data_bucket(const llmsset_t dbs, uint64_t index)
{
    _Atomic (uint64_t) *ptr = dbs->bitmap2 + (index / 64);
    uint64_t mask = 0x8000000000000000LL >> (index & 63);
    atomic_fetch_and(ptr, ~mask);
}

static void
set_custom_bucket(const llmsset_t dbs, uint64_t index, int on)
{
    uint64_t * ptr = dbs->bitmapc + (index / 64);
    uint64_t mask = 0x8000000000000000LL >> (index & 63);
    if (on) *ptr |= mask;
    else *ptr &= ~mask;
}

static int
is_custom_bucket(const llmsset_t dbs, uint64_t index)
{
    uint64_t * ptr = dbs->bitmapc + (index / 64);
    uint64_t mask = 0x8000000000000000LL >> (index & 63);
    return (*ptr & mask) ? 1 : 0;
}

static inline uint64_t
compute_hash(const llmsset_t dbs, uint64_t a, uint64_t b, const int custom)
{
    uint64_t hash = 14695981039346656037LLU;
    if (custom) hash = dbs->hash_cb(a, b, hash);
    else hash = sylvan_tabhash16(a, b, hash);
    return hash;
}

static inline uint64_t
llmsset_lookup2(const llmsset_t dbs, uint64_t a, uint64_t b, int *created, const int custom)
{
    uint64_t hash = compute_hash(dbs, a, b, custom);
    uint64_t masked_hash = hash & MASK_HASH;

    // reminder
    // table.a = index of first entry
    // table.b = hash of entry + index of next entry

#if LLMSSET_MASK
    _Atomic (uint64_t) *first_ptr = &dbs->table[2 * (hash & dbs->mask)];
#else
    _Atomic(uint64_t)* first_ptr = &dbs->table[2*(hash % dbs->table_size)];
#endif

    uint64_t first_idx = atomic_load_explicit(first_ptr, memory_order_relaxed);
    uint64_t end = 0;
    uint64_t bucket_idx = first_idx;
    uint64_t claimed_idx = 0; // stores where the new data [will be] stored

    // stop when we encounter <end>
    for (;;) {
        if (bucket_idx == end) {
            // we did not find existing node in our table, and reached end of chain
            // or the chain was empty
            // thus, try to insert new node
            if (claimed_idx == 0) {
                // claim data bucket
                claimed_idx = claim_data_bucket(dbs);
                if (claimed_idx == (uint64_t) -1) return 0; // failed to claim a data bucket
                // call custom create callback (might update a and b, but should hash to the same!)
                if (custom) dbs->create_cb(&a, &b);
                // write the data
                uint64_t * dataptr = ((uint64_t *) dbs->data) + 2 * claimed_idx;
                dataptr[0] = a;
                dataptr[1] = b;
            }
            // Set <next> item index in the chain
            atomic_store_explicit(dbs->table + 2 * claimed_idx + 1, masked_hash | first_idx, memory_order_relaxed);

            // preserve the original first_idx (new "end" in case someone add something)
            end = first_idx;
            // now update the chain start (first_ptr) with claimed_idx
            if (atomic_compare_exchange_strong(first_ptr, &first_idx, claimed_idx)) {
                if (custom) set_custom_bucket(dbs, claimed_idx, custom);
                *created = 1;
                return claimed_idx;
            } else {
                // first_idx now holds the new value!
                bucket_idx = first_idx; // continue with the new first index
            }
        }

        uint64_t * dataptr = ((uint64_t *) dbs->data) + 2 * bucket_idx;
        uint64_t hash_chain = atomic_load_explicit(dbs->table + 2 * bucket_idx + 1, memory_order_relaxed);

        if (masked_hash == (hash_chain & MASK_HASH)) {
            // check if we already have this node in the table
            // if so, release the ownership if it was owned before, and return the bucket index
            if (custom) {
                // we do not use custom nodes for this test
                if (dbs->equals_cb(a, b, dataptr[0], dataptr[1])) {
                    if (claimed_idx != 0) {
                        dbs->destroy_cb(a, b);
                        release_data_bucket(dbs, claimed_idx);
                    }
                    *created = 0;
                    return bucket_idx;
                }
            } else {
                if (dataptr[0] == a && dataptr[1] == b) {
                    if (claimed_idx != 0) {
                        release_data_bucket(dbs, claimed_idx);
                    }
                    *created = 0;
                    return bucket_idx;
                }
            }
        }

        bucket_idx = hash_chain & MASK_INDEX; // next item index in the chain
        sylvan_stats_count(LLMSSET_LOOKUP);
    }
}

uint64_t
llmsset_lookup(const llmsset_t dbs, const uint64_t a, const uint64_t b, int *created)
{
    return llmsset_lookup2(dbs, a, b, created, 0);
}

uint64_t
llmsset_lookupc(const llmsset_t dbs, const uint64_t a, const uint64_t b, int *created)
{
    return llmsset_lookup2(dbs, a, b, created, 1);
}

int
llmsset_rehash_bucket(const llmsset_t dbs, uint64_t d_idx)
{
    // This is like lookup, except we assume that
    // - the data index is not duplicate
    // - the data is not yet in the table
    uint64_t * dataptr = ((uint64_t *) dbs->data) + 2 * d_idx;
    uint64_t hash = compute_hash(dbs, dataptr[0], dataptr[1], is_custom_bucket(dbs, d_idx));
    uint64_t masked_hash = hash & MASK_HASH;

#if LLMSSET_MASK
    _Atomic (uint64_t) *first_ptr = &dbs->table[2 * (hash & dbs->mask)];
#else
    _Atomic(uint64_t)* first_ptr = &dbs->table[2*(hash % dbs->table_size)];
#endif

    uint64_t first_idx = atomic_load_explicit(first_ptr, memory_order_relaxed);
    for (;;) {
        atomic_store_explicit(dbs->table + 2 * d_idx + 1, masked_hash | first_idx, memory_order_relaxed);
        if (atomic_compare_exchange_strong(first_ptr, &first_idx, d_idx)) return 1;
    }
}


/**
 * Clear a single bucket hash.
 * (do not run parallel with lookup!!!)
 * (for dynamic variable reordering)
 * (lock-free, but not wait-free)
 */
int
llmsset_clear_one_hash(const llmsset_t dbs, uint64_t d_idx)
{
    uint64_t * dataptr = ((uint64_t *) dbs->data) + 2 * d_idx;
    // compute the hash to find the ``head'' of the chain
    uint64_t hash = compute_hash(dbs, dataptr[0], dataptr[1], is_custom_bucket(dbs, d_idx));

#if LLMSSET_MASK
    _Atomic (uint64_t) *first_ptr = dbs->table + 2 * (hash & dbs->mask);
#else
    _Atomic(uint64_t)* first_ptr = dbs->table + 2 * (hash % dbs->table_size);
#endif

    // lock the head of the chain using CAS to -1 (-1 as magical value meaning LOCKED)
    uint64_t first_idx = atomic_load_explicit(first_ptr, memory_order_relaxed);
    for (;;) {
        while (first_idx == (uint64_t) -1) {
            // already locked, spin-wait until unlocked
            first_idx = atomic_load_explicit(first_ptr, memory_order_relaxed);
        }
        // not locked; check if not 0 (that would mean data is not in the hash table)
        if (first_idx == 0) return 0;
        // OK, use CAS to lock the chain
        if (atomic_compare_exchange_strong(first_ptr, &first_idx, (uint64_t) -1)) {
            break;
        }
    }

    // set next_idx to the next bucket in the chain
    uint64_t next_idx = atomic_load_explicit(dbs->table + 2 * d_idx + 1, memory_order_relaxed);
    next_idx &= MASK_INDEX;

    if (first_idx == d_idx) {
        // simple case: the head is d_idx
        atomic_store_explicit(first_ptr, next_idx, memory_order_seq_cst);
        return 1;
    } else {
        // the head is not d_idx, so follow the chain...
        uint64_t idx = first_idx;
        for (;;) {
            if (idx == 0) {
                // if idx equals 0, then the item was not in the hash table. return 0.
                // for example it was never created, or already removed...
                atomic_store_explicit(first_ptr, first_idx, memory_order_seq_cst);
                return 0;
            }

            _Atomic (uint64_t) *chain_ptr = dbs->table + 2 * idx + 1;
            uint64_t chain = atomic_load_explicit(chain_ptr, memory_order_relaxed);
            idx = chain & MASK_INDEX;

            if (idx == d_idx) { // found our predecessor
                // update the chain
                atomic_store_explicit(chain_ptr, (chain & MASK_HASH) | next_idx, memory_order_seq_cst);
                // unlock
                atomic_store_explicit(first_ptr, first_idx, memory_order_seq_cst);
                return 1;
            }
        }
    }
}

/**
 * Clear a single bucket data.
 */
void llmsset_clear_one_data(llmsset_t dbs, uint64_t index)
{
    release_data_bucket(dbs, index);
    if (is_custom_bucket(dbs, index)) {
        uint64_t * d_ptr = ((uint64_t *) dbs->data) + 2 * index;
        dbs->destroy_cb(d_ptr[0], d_ptr[1]);
    }
}

llmsset_t
llmsset_create(size_t initial_size, size_t max_size)
{
    llmsset_t dbs = (llmsset_t) alloc_aligned(sizeof(struct llmsset));
    if (dbs == 0) {
        fprintf(stderr, "llmsset_create: Unable to allocate memory!\n");
        exit(1);
    }

#if LLMSSET_MASK
    /* Check if initial_size and max_size are powers of 2 */
    if (__builtin_popcountll(initial_size) != 1) {
        fprintf(stderr, "llmsset_create: initial_size is not a power of 2!\n");
        exit(1);
    }

    if (__builtin_popcountll(max_size) != 1) {
        fprintf(stderr, "llmsset_create: max_size is not a power of 2!\n");
        exit(1);
    }
#endif

    if (initial_size > max_size) {
        fprintf(stderr, "llmsset_create: initial_size > max_size!\n");
        exit(1);
    }

    // minimum size is now 512 buckets (region size, but of course, n_workers * 512 is suggested as minimum)

    if (initial_size < 512) {
        fprintf(stderr, "llmsset_create: initial_size too small!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    llmsset_set_size(dbs, initial_size);

    /* This implementation of "resizable hash table" allocates the max_size table in virtual memory,
       but only uses the "actual size" part in real memory */

    dbs->table = (_Atomic (uint64_t) *) alloc_aligned(dbs->max_size * 16);
    dbs->data = (uint8_t *) alloc_aligned(dbs->max_size * 16);

    /* Also allocate bitmaps. Each region is 64*8 = 512 buckets.
       Overhead of bitmap1: 1 bit per 4096 bucket.
       Overhead of bitmap2: 1 bit per bucket.
       Overhead of bitmapc: 1 bit per bucket. */

    dbs->bitmap1 = (_Atomic (uint64_t) *) alloc_aligned(dbs->max_size / (512 * 8));
    dbs->bitmap2 = (_Atomic (uint64_t) *) alloc_aligned(dbs->max_size / 8);
    dbs->bitmapc = (uint64_t *) alloc_aligned(dbs->max_size / 8);

    if (dbs->table == 0 || dbs->data == 0 || dbs->bitmap1 == 0 || dbs->bitmap2 == 0 || dbs->bitmapc == 0) {
        fprintf(stderr, "llmsset_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

#if defined(madvise) && defined(MADV_RANDOM)
    madvise(dbs->table, dbs->max_size * 16, MADV_RANDOM);
#endif

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = 0xc000000000000000LL;

    dbs->hash_cb = NULL;
    dbs->equals_cb = NULL;
    dbs->create_cb = NULL;
    dbs->destroy_cb = NULL;

    // yes, ugly. for now, we use a global thread-local value.
    // that is a problem with multiple tables.
    // so, for now, do NOT use multiple tables!!

    INIT_THREAD_LOCAL(my_region);
    TOGETHER(llmsset_reset_region);

    // initialize hashtab
    sylvan_init_hash();

    return dbs;
}

void
llmsset_free(llmsset_t dbs)
{
    free_aligned(dbs->table, dbs->max_size * 16);
    free_aligned(dbs->data, dbs->max_size * 16);
    free_aligned(dbs->bitmap1, dbs->max_size / (512 * 8));
    free_aligned(dbs->bitmap2, dbs->max_size / 8);
    free_aligned(dbs->bitmapc, dbs->max_size / 8);
    free_aligned(dbs, sizeof(struct llmsset));
}

VOID_TASK_IMPL_1(llmsset_clear, llmsset_t, dbs)
{
    CALL(llmsset_clear_data, dbs);
    CALL(llmsset_clear_hashes, dbs);
}

VOID_TASK_IMPL_1(llmsset_clear_data, llmsset_t, dbs)
{
    clear_aligned(dbs->bitmap1, dbs->max_size / (512 * 8));
    clear_aligned(dbs->bitmap2, dbs->max_size / 8);

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = 0xc000000000000000LL;

    TOGETHER(llmsset_reset_region);
}

VOID_TASK_IMPL_1(llmsset_clear_hashes, llmsset_t, dbs)
{
    clear_aligned(dbs->table, dbs->max_size * 16);
}

int
llmsset_is_marked(const llmsset_t dbs, uint64_t index)
{
    _Atomic (uint64_t) *ptr = dbs->bitmap2 + (index / 64);
    uint64_t mask = 0x8000000000000000LL >> (index & 63);
    return (atomic_load_explicit(ptr, memory_order_relaxed) & mask) ? 1 : 0;
}

int
llmsset_mark(const llmsset_t dbs, uint64_t index)
{
    _Atomic (uint64_t) *ptr = dbs->bitmap2 + (index / 64);
    uint64_t mask = 0x8000000000000000LL >> (index & 63);
    for (;;) {
        uint64_t v = *ptr;
        if (v & mask) return 0;
        if (atomic_compare_exchange_weak(ptr, &v, v | mask)) return 1;
    }
}

TASK_3(int, llmsset_rehash_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 512) {
        SPAWN(llmsset_rehash_par, dbs, first, count / 2);
        int bad = CALL(llmsset_rehash_par, dbs, first + count / 2, count - count / 2);
        return bad + SYNC(llmsset_rehash_par);
    } else {
        int bad = 0;
        _Atomic (uint64_t) *ptr = dbs->bitmap2 + (first / 64);
        uint64_t mask = 0x8000000000000000LL >> (first & 63);
        for (size_t k = 0; k < count; k++) {
            if (atomic_load_explicit(ptr, memory_order_relaxed) & mask) {
                if (llmsset_rehash_bucket(dbs, first + k) == 0) bad++;
            }
            mask >>= 1;
            if (mask == 0) {
                ptr++;
                mask = 0x8000000000000000LL;
            }
        }
        return bad;
    }
}

TASK_IMPL_1(int, llmsset_rehash, llmsset_t, dbs)
{
    return CALL(llmsset_rehash_par, dbs, 0, dbs->table_size);
}

TASK_3(size_t, llmsset_count_marked_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 512) {
        size_t split = count / 2;
        SPAWN(llmsset_count_marked_par, dbs, first, split);
        size_t right = CALL(llmsset_count_marked_par, dbs, first + split, count - split);
        size_t left = SYNC(llmsset_count_marked_par);
        return left + right;
    } else {
        size_t result = 0;
        _Atomic (uint64_t) *ptr = dbs->bitmap2 + (first / 64);
        if (count == 512) {
            result += __builtin_popcountll(atomic_load_explicit(ptr + 0, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr + 1, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr + 2, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr + 3, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr + 4, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr + 5, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr + 6, memory_order_relaxed));
            result += __builtin_popcountll(atomic_load_explicit(ptr + 7, memory_order_relaxed));
        } else {
            uint64_t mask = 0x8000000000000000LL >> (first & 63);
            for (size_t k = 0; k < count; k++) {
                if (atomic_load_explicit(ptr, memory_order_relaxed) & mask) result += 1;
                mask >>= 1;
                if (mask == 0) {
                    ptr++;
                    mask = 0x8000000000000000LL;
                }
            }
        }
        return result;
    }
}

TASK_IMPL_1(size_t, llmsset_count_marked, llmsset_t, dbs)
{
    return CALL(llmsset_count_marked_par, dbs, 0, dbs->table_size);
}

VOID_TASK_3(llmsset_destroy_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 1024) {
        size_t split = count / 2;
        SPAWN(llmsset_destroy_par, dbs, first, split);
        CALL(llmsset_destroy_par, dbs, first + split, count - split);
        SYNC(llmsset_destroy_par);
    } else {
        for (size_t k = first; k < first + count; k++) {
            _Atomic (uint64_t) *ptr2 = dbs->bitmap2 + (k / 64);
            uint64_t * ptrc = dbs->bitmapc + (k / 64);
            uint64_t mask = 0x8000000000000000LL >> (k & 63);

            // if not marked but is custom
            if ((*ptr2 & mask) == 0 && (*ptrc & mask)) {
                uint64_t * d_ptr = ((uint64_t *) dbs->data) + 2 * k;
                dbs->destroy_cb(d_ptr[0], d_ptr[1]);
                *ptrc &= ~mask;
            }
        }
    }
}

VOID_TASK_IMPL_1(llmsset_destroy_unmarked, llmsset_t, dbs)
{
    if (dbs->destroy_cb == NULL) return; // no custom function
    CALL(llmsset_destroy_par, dbs, 0, dbs->table_size);
}

/**
 * Set custom functions
 */
void llmsset_set_custom(const llmsset_t dbs, llmsset_hash_cb hash_cb, llmsset_equals_cb equals_cb,
                        llmsset_create_cb create_cb, llmsset_destroy_cb destroy_cb)
{
    dbs->hash_cb = hash_cb;
    dbs->equals_cb = equals_cb;
    dbs->create_cb = create_cb;
    dbs->destroy_cb = destroy_cb;
}