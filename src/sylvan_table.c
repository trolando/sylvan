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

#include <errno.h>  // for errno
#include <string.h> // memset
#include <sys/mman.h> // for mmap

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#ifndef cas
#define cas(ptr, old, new) (__sync_bool_compare_and_swap((ptr),(old),(new)))
#endif

DECLARE_THREAD_LOCAL(my_region, uint64_t);

VOID_TASK_0(llmsset_reset_region)
{
    LOCALIZE_THREAD_LOCAL(my_region, uint64_t);
    my_region = (uint64_t)-1; // no region
    SET_THREAD_LOCAL(my_region, my_region);
}

static uint64_t
claim_data_bucket(const llmsset_t dbs)
{
    LOCALIZE_THREAD_LOCAL(my_region, uint64_t);

    for (;;) {
        if (my_region != (uint64_t)-1) {
            // find empty bucket in region <my_region>
            uint64_t *ptr = dbs->bitmap2 + (my_region*8);
            int i=0;
            for (;i<8;) {
                uint64_t v = *ptr;
                if (v != 0xffffffffffffffffLL) {
                    int j = __builtin_clzll(~v);
                    *ptr |= (0x8000000000000000LL>>j);
                    return (8 * my_region + i) * 64 + j;
                }
                i++;
                ptr++;
            }
        } else {
            // special case on startup or after garbage collection
            my_region += (lace_get_worker()->worker*(dbs->table_size/(64*8)))/lace_workers();
        }
        uint64_t count = dbs->table_size/(64*8);
        for (;;) {
            // check if table maybe full
            if (count-- == 0) return (uint64_t)-1;

            my_region += 1;
            if (my_region >= (dbs->table_size/(64*8))) my_region = 0;

            // try to claim it
            uint64_t *ptr = dbs->bitmap1 + (my_region/64);
            uint64_t mask = 0x8000000000000000LL >> (my_region&63);
            uint64_t v;
restart:
            v = *ptr;
            if (v & mask) continue; // taken
            if (cas(ptr, v, v|mask)) break;
            else goto restart;
        }
        SET_THREAD_LOCAL(my_region, my_region);
    }
}

static void
release_data_bucket(const llmsset_t dbs, uint64_t index)
{
    uint64_t *ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    *ptr &= ~mask;
}

static void
set_custom_bucket(const llmsset_t dbs, uint64_t index, int on)
{
    uint64_t *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    if (on) *ptr |= mask;
    else *ptr &= ~mask;
}

static int
is_custom_bucket(const llmsset_t dbs, uint64_t index)
{
    uint64_t *ptr = dbs->bitmapc + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    return (*ptr & mask) ? 1 : 0;
}

/* 40 bits for the index, 24 bits for the hash */
#define MASK_INDEX ((uint64_t)0x000000ffffffffff)
#define MASK_HASH  ((uint64_t)0xffffff0000000000)

static inline uint64_t
llmsset_lookup2(const llmsset_t dbs, uint64_t a, uint64_t b, int* created, const int custom)
{
    const uint64_t hash = custom ?
        dbs->hash_cb(a, b, 14695981039346656037LLU) :
        sylvan_tabhash16(a, b, 14695981039346656037LLU);

    const uint64_t hashm = hash & MASK_HASH;

#if LLMSSET_MASK
    volatile uint64_t *fptr = &dbs->table[hash & dbs->mask];
#else
    volatile uint64_t *fptr = &dbs->table[hash % dbs->table_size];
#endif

    uint64_t frst = *fptr;
    uint64_t cidx = 0; // stores where the new data [will be] stored
    uint64_t *cptr = 0;

    uint64_t idx = frst, end = 0;

    // stop when we encounter <end>

    for (;;) {
        if (idx == end) {
            // Try to insert now
            if (cidx == 0) {
                // Claim data bucket and write data
                cidx = claim_data_bucket(dbs);
                if (cidx == (uint64_t)-1) return 0; // failed to claim a data bucket
                if (custom) dbs->create_cb(&a, &b);
                cptr = ((uint64_t*)dbs->data) + 3*cidx;
                cptr[1] = a;
                cptr[2] = b;
            }
            // Set <next> and perform the CAS
            cptr[0] = hashm | frst;
            if (cas(fptr, frst, cidx)) {
                if (custom) set_custom_bucket(dbs, cidx, custom);
                *created = 1;
                return cidx;
            } else {
                end = frst;
                idx = frst = *fptr;
            }
        }

        uint64_t *dptr = ((uint64_t*)dbs->data) + 3*idx;
        uint64_t v = *dptr;

        if (hashm == (v & MASK_HASH)) {
            if (custom) {
                if (dbs->equals_cb(a, b, dptr[1], dptr[2])) {
                    if (cidx != 0) {
                        dbs->destroy_cb(a, b);
                        release_data_bucket(dbs, cidx);
                    }
                    *created = 0;
                    return idx;
                }
            } else {
                if (dptr[1] == a && dptr[2] == b) {
                    if (cidx != 0) release_data_bucket(dbs, cidx);
                    *created = 0;
                    return idx;
                }
            }
        }

        idx = v & MASK_INDEX; // next

        sylvan_stats_count(LLMSSET_LOOKUP);
    }
}

uint64_t
llmsset_lookup(const llmsset_t dbs, const uint64_t a, const uint64_t b, int* created)
{
    return llmsset_lookup2(dbs, a, b, created, 0);
}

uint64_t
llmsset_lookupc(const llmsset_t dbs, const uint64_t a, const uint64_t b, int* created)
{
    return llmsset_lookup2(dbs, a, b, created, 1);
}

int
llmsset_rehash_bucket(const llmsset_t dbs, uint64_t d_idx)
{
    uint64_t *dptr = ((uint64_t*)dbs->data) + 3*d_idx;

    const uint64_t hash = is_custom_bucket(dbs, d_idx) ?
        dbs->hash_cb(dptr[1], dptr[2], 14695981039346656037LLU) :
        sylvan_tabhash16(dptr[1], dptr[2], 14695981039346656037LLU);

#if LLMSSET_MASK
    volatile uint64_t *fptr = &dbs->table[hash & dbs->mask];
#else
    volatile uint64_t *fptr = &dbs->table[hash % dbs->table_size];
#endif

    for (;;) {
        uint64_t frst = *fptr;
        if (cas(fptr, frst, d_idx)) {
            *dptr = (hash & MASK_HASH) | frst;
            return 1;
        }
    }
}

#if 0
/**
 * Remove a single BDD from the table (do not run parallel with lookup!!!)
 * (for dynamic variable reordering)
 */
int
llmsset_clear_one(const llmsset_t dbs, uint64_t didx)
{
    uint64_t *dptr = ((uint64_t*)dbs->data) + 3*didx;

    const uint64_t hash = is_custom_bucket(dbs, didx) ?
        dbs->hash_cb(dptr[1], dptr[2], 14695981039346656037LLU) :
        sylvan_tabhash16(dptr[1], dptr[2], 14695981039346656037LLU);

#if LLMSSET_MASK
    volatile uint64_t *fptr = &dbs->table[hash & dbs->mask];
#else
    volatile uint64_t *fptr = &dbs->table[hash % dbs->table_size];
#endif

    uint64_t frst = *fptr;
    uint64_t cidx = 0; // stores where the new data [will be] stored
    uint64_t *cptr = 0;

    uint64_t idx = frst, end = 0;

    // stop when we encounter <end>

    for (;;) {
        if (idx == end) {
            return 0; // wasn't in???
        }

        uint64_t *ptr = ((uint64_t*)dbs->data) + 3*idx;
        if (*ptr 
        uint64_t v = *dptr;

        idx = v & MASK_INDEX; // next

        sylvan_stats_count(LLMSSET_LOOKUP);
    }

    // AFTER CHANGE, check if >>my<< next has changed!!

    for (;;) {
        uint64_t frst = *fptr;
        if (cas(fptr, frst, d_idx)) {
            *dptr = (hash & MASK_HASH) | frst;
            return 1;
        }
    }
}
#endif

llmsset_t
llmsset_create(size_t initial_size, size_t max_size)
{
    llmsset_t dbs = NULL;
    if (posix_memalign((void**)&dbs, LINE_SIZE, sizeof(struct llmsset)) != 0) {
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

    dbs->table = (uint64_t*)mmap(0, dbs->max_size * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    dbs->data = (uint8_t*)mmap(0, dbs->max_size * 24, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    /* Also allocate bitmaps. Each region is 64*8 = 512 buckets.
       Overhead of bitmap1: 1 bit per 4096 bucket.
       Overhead of bitmap2: 1 bit per bucket.
       Overhead of bitmapc: 1 bit per bucket. */

    dbs->bitmap1 = (uint64_t*)mmap(0, dbs->max_size / (512*8), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    dbs->bitmap2 = (uint64_t*)mmap(0, dbs->max_size / 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    dbs->bitmapc = (uint64_t*)mmap(0, dbs->max_size / 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (dbs->table == (uint64_t*)-1 || dbs->data == (uint8_t*)-1 || dbs->bitmap1 == (uint64_t*)-1 || dbs->bitmap2 == (uint64_t*)-1 || dbs->bitmapc == (uint64_t*)-1) {
        fprintf(stderr, "llmsset_create: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

#if defined(madvise) && defined(MADV_RANDOM)
    madvise(dbs->table, dbs->max_size * 8, MADV_RANDOM);
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
    munmap(dbs->table, dbs->max_size * 8);
    munmap(dbs->data, dbs->max_size * 16);
    munmap(dbs->bitmap1, dbs->max_size / (512*8));
    munmap(dbs->bitmap2, dbs->max_size / 8);
    munmap(dbs->bitmapc, dbs->max_size / 8);
    free(dbs);
}

VOID_TASK_IMPL_1(llmsset_clear, llmsset_t, dbs)
{
    CALL(llmsset_clear_data, dbs);
    CALL(llmsset_clear_hashes, dbs);
}

VOID_TASK_IMPL_1(llmsset_clear_data, llmsset_t, dbs)
{
    if (mmap(dbs->bitmap1, dbs->max_size / (512*8), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void*)-1) {
    } else {
        memset(dbs->bitmap1, 0, dbs->max_size / (512*8));
    }

    if (mmap(dbs->bitmap2, dbs->max_size / 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void*)-1) {
    } else {
        memset(dbs->bitmap2, 0, dbs->max_size / 8);
    }

    // forbid first two positions (index 0 and 1)
    dbs->bitmap2[0] = 0xc000000000000000LL;

    TOGETHER(llmsset_reset_region);
}

VOID_TASK_IMPL_1(llmsset_clear_hashes, llmsset_t, dbs)
{
    // just reallocate...
    if (mmap(dbs->table, dbs->max_size * 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void*)-1) {
#if defined(madvise) && defined(MADV_RANDOM)
        madvise(dbs->table, sizeof(uint64_t[dbs->max_size]), MADV_RANDOM);
#endif
    } else {
        // reallocate failed... expensive fallback
        memset(dbs->table, 0, dbs->max_size * 8);
    }
}

int
llmsset_is_marked(const llmsset_t dbs, uint64_t index)
{
    volatile uint64_t *ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    return (*ptr & mask) ? 1 : 0;
}

int
llmsset_mark(const llmsset_t dbs, uint64_t index)
{
    volatile uint64_t *ptr = dbs->bitmap2 + (index/64);
    uint64_t mask = 0x8000000000000000LL >> (index&63);
    for (;;) {
        uint64_t v = *ptr;
        if (v & mask) return 0;
        if (cas(ptr, v, v|mask)) return 1;
    }
}

TASK_3(int, llmsset_rehash_par, llmsset_t, dbs, size_t, first, size_t, count)
{
    if (count > 512) {
        SPAWN(llmsset_rehash_par, dbs, first, count/2);
        int bad = CALL(llmsset_rehash_par, dbs, first + count/2, count - count/2);
        return bad + SYNC(llmsset_rehash_par);
    } else {
        int bad = 0;
        uint64_t *ptr = dbs->bitmap2 + (first / 64);
        uint64_t mask = 0x8000000000000000LL >> (first & 63);
        for (size_t k=0; k<count; k++) {
            if (*ptr & mask) {
                if (llmsset_rehash_bucket(dbs, first+k) == 0) bad++;
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
        size_t split = count/2;
        SPAWN(llmsset_count_marked_par, dbs, first, split);
        size_t right = CALL(llmsset_count_marked_par, dbs, first + split, count - split);
        size_t left = SYNC(llmsset_count_marked_par);
        return left + right;
    } else {
        size_t result = 0;
        uint64_t *ptr = dbs->bitmap2 + (first / 64);
        if (count == 512) {
            result += __builtin_popcountll(ptr[0]);
            result += __builtin_popcountll(ptr[1]);
            result += __builtin_popcountll(ptr[2]);
            result += __builtin_popcountll(ptr[3]);
            result += __builtin_popcountll(ptr[4]);
            result += __builtin_popcountll(ptr[5]);
            result += __builtin_popcountll(ptr[6]);
            result += __builtin_popcountll(ptr[7]);
        } else {
            uint64_t mask = 0x8000000000000000LL >> (first & 63);
            for (size_t k=0; k<count; k++) {
                if (*ptr & mask) result += 1;
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
        size_t split = count/2;
        SPAWN(llmsset_destroy_par, dbs, first, split);
        CALL(llmsset_destroy_par, dbs, first + split, count - split);
        SYNC(llmsset_destroy_par);
    } else {
        for (size_t k=first; k<first+count; k++) {
            volatile uint64_t *ptr2 = dbs->bitmap2 + (k/64);
            volatile uint64_t *ptrc = dbs->bitmapc + (k/64);
            uint64_t mask = 0x8000000000000000LL >> (k&63);

            // if not marked but is custom
            if ((*ptr2 & mask) == 0 && (*ptrc & mask)) {
                uint64_t *d_ptr = ((uint64_t*)dbs->data) + 3*k;
                dbs->destroy_cb(d_ptr[1], d_ptr[2]);
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
void llmsset_set_custom(const llmsset_t dbs, llmsset_hash_cb hash_cb, llmsset_equals_cb equals_cb, llmsset_create_cb create_cb, llmsset_destroy_cb destroy_cb)
{
    dbs->hash_cb = hash_cb;
    dbs->equals_cb = equals_cb;
    dbs->create_cb = create_cb;
    dbs->destroy_cb = destroy_cb;
}
