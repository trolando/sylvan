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

#include <sylvan/sylvan.h>
#include <sylvan/platform.h>
#include <sylvan/hash.h>
#include <refs.h>

#include <errno.h>  // for errno
#include <string.h> // for strerror

/**
 * Simple implementation of a 64-bit resizable hash-table
 * No idea if this is scalable... :( but it seems thread-safe
 */
static const uint64_t refs_ts = 0x7fffffffffffffff; // tombstone

#define fnvhash8(a) sylvan_fnvhash8(a, 14695981039346656037LLU)

/**
 * Counting active operations, so that a resize can wait until there are none.
 * A Lace worker counts its operations in its own counter, REFS_WORKER_STRIDE
 * bytes from the next, so that workers that protect and unprotect at the same
 * time do not contend on one cache line (128 bytes also covers the pairs of
 * 64-byte lines that some CPUs fetch together). Threads that are not Lace
 * workers, and workers without a counter, count theirs in the low bits of
 * refs_control. A resize sets a high bit of refs_control, then waits until
 * every count is 0.
 */
#define REFS_WORKER_STRIDE 128

/* Counters for at least this many workers, also for a table created before Lace starts */
#define REFS_MIN_WORKERS 256

static inline _Atomic(uint32_t)*
refs_worker_counter(refs_table_t *tbl, size_t worker)
{
    return (_Atomic(uint32_t)*)((char*)tbl->refs_workers + worker * REFS_WORKER_STRIDE);
}

static void
refs_create_workers(refs_table_t *tbl)
{
    size_t n = lace_worker_count();
    if (n < REFS_MIN_WORKERS) n = REFS_MIN_WORKERS;
    tbl->refs_workers = (_Atomic(uint32_t)*)sylvan_alloc_aligned(n * REFS_WORKER_STRIDE);
    // without counters, every thread counts its operations in refs_control
    tbl->refs_nworkers = tbl->refs_workers != NULL ? n : 0;
}

static void
refs_free_workers(refs_table_t *tbl)
{
    if (tbl->refs_workers != NULL) sylvan_free_aligned((void*)tbl->refs_workers, tbl->refs_nworkers * REFS_WORKER_STRIDE);
    tbl->refs_workers = NULL;
    tbl->refs_nworkers = 0;
}

/* For a resize that has set bit in refs_control: wait until no operation is active */
static void
refs_wait_inactive(refs_table_t *tbl, uint32_t bit)
{
    while (atomic_load_explicit(&tbl->refs_control, memory_order_seq_cst) != bit) continue;
    for (size_t i = 0; i < tbl->refs_nworkers; i++) {
        _Atomic(uint32_t) *c = refs_worker_counter(tbl, i);
        while (atomic_load_explicit(c, memory_order_seq_cst) != 0) continue;
    }
}

// Count number of entries
size_t
protect_count(refs_table_t *tbl)
{
    size_t count = 0;
    _Atomic(uint64_t)* bucket = tbl->refs_table;
    _Atomic(uint64_t)* const end = bucket + tbl->refs_size;
    while (bucket != end) {
        uint64_t d = atomic_load_explicit(bucket, memory_order_relaxed);
        if (d != 0 && d != refs_ts) count++;
        bucket++;
    }
    return count;
}

static inline void
protect_rehash(refs_table_t *tbl, uint64_t v)
{
    if (v == 0) return; // do not rehash empty value
    if (v == refs_ts) return; // do not rehash tombstone

    _Atomic(uint64_t)* bucket = tbl->refs_table + (fnvhash8(v) % tbl->refs_size);
    _Atomic(uint64_t)* const end = tbl->refs_table + tbl->refs_size;

    int i = 128; // try 128 times linear probing
    while (i--) {
        uint64_t d = atomic_load(bucket);
        if (d == 0 && atomic_compare_exchange_strong(bucket, &d, v)) return;
        if (++bucket == end) bucket = tbl->refs_table;
    }

    assert(0); // whoops!
}

/**
 * Returns 1 while a resize is in progress, 0 when there is none (anymore).
 * Only the thread that resizes rehashes. Other threads used to help, claiming
 * parts of the old table, but a helper that was delayed between its checks and
 * its claim could claim a part of the next resize before that resize had its
 * new table, and rehash that part into the full old table: those values were
 * lost from the table.
 */
static int
protect_resize_busy(refs_table_t *tbl)
{
    return (atomic_load_explicit(&tbl->refs_control, memory_order_seq_cst) & 0xf0000000) != 0;
}

static void
protect_resize(refs_table_t *tbl)
{
    while (1) {
        uint32_t v = tbl->refs_control;
        if (v & 0xf0000000) {
            // someone else started resize: wait until it is done
            while (protect_resize_busy(tbl)) continue;
            return;
        }
        if (atomic_compare_exchange_weak(&tbl->refs_control, &v, 0x80000000 | v)) {
            // wait until all users gone
            refs_wait_inactive(tbl, 0x80000000);
            break;
        }
    }

    _Atomic(uint64_t)* old_table = tbl->refs_table;
    const size_t old_size = tbl->refs_size;

    // calculate new size
    size_t new_size = old_size;
    size_t count = protect_count(tbl);
    if (count*4 > old_size) new_size *= 2;

    // allocate new table
    _Atomic(uint64_t)* new_table = (_Atomic(uint64_t)*)sylvan_alloc_aligned(new_size * sizeof(uint64_t));
    if (new_table == 0) {
        fprintf(stderr, "refs: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }

    // set new data and rehash
    tbl->refs_table = new_table;
    tbl->refs_size = new_size;
    for (size_t i = 0; i < old_size; i++) {
        protect_rehash(tbl, atomic_load_explicit(old_table + i, memory_order_relaxed));
    }

    // done!
    atomic_store_explicit(&tbl->refs_control, 0, memory_order_seq_cst);

    // unmap old table
    sylvan_free_aligned((void*)old_table, old_size * sizeof(uint64_t));
}

/**
 * Enter protect_up or protect_down: first wait for a resize in progress to
 * finish. Returns the counter that protect_leave decrements. A worker's
 * increment of its counter and its load of refs_control are sequentially
 * consistent, as are a resize's update of refs_control and its loads of the
 * counters, so either the worker sees the resize or the resize sees the worker.
 */
static inline _Atomic(uint32_t)*
protect_enter(refs_table_t *tbl)
{
    lace_worker *w = lace_get_worker();
    if (w != NULL && (size_t)w->worker < tbl->refs_nworkers) {
        _Atomic(uint32_t) *c = refs_worker_counter(tbl, w->worker);
        for (;;) {
            atomic_fetch_add_explicit(c, 1, memory_order_seq_cst);
            if ((atomic_load_explicit(&tbl->refs_control, memory_order_seq_cst) & 0xf0000000) == 0) return c;
            atomic_fetch_sub_explicit(c, 1, memory_order_release);
            while (protect_resize_busy(tbl)) continue;
        }
    }
    for (;;) {
        uint32_t v = atomic_load_explicit(&tbl->refs_control, memory_order_seq_cst);
        if (v & 0xf0000000) {
            while (protect_resize_busy(tbl)) continue;
        } else {
            if (atomic_compare_exchange_weak(&tbl->refs_control, &v, v+1)) return &tbl->refs_control;
        }
    }
}

/* Leave protect_up or protect_down; c is what protect_enter returned */
static inline void
protect_leave(_Atomic(uint32_t) *c)
{
    atomic_fetch_sub_explicit(c, 1, memory_order_release);
}

void
protect_up(refs_table_t *tbl, uint64_t a)
{
    _Atomic(uint64_t)* bucket;
    _Atomic(uint64_t)* ts_bucket;
    uint64_t v;
    int i;

    _Atomic(uint32_t) *counter = protect_enter(tbl);

ref_retry:
    bucket = tbl->refs_table + (fnvhash8(a) & (tbl->refs_size - 1));
    ts_bucket = NULL; // tombstone
    i = 128; // try 128 times linear probing

    while (i--) {
ref_restart:
        v = *bucket;
        if (v == refs_ts) {
            if (ts_bucket == NULL) ts_bucket = bucket;
        } else if (v == 0) {
            // go go go
            if (ts_bucket != NULL) {
                v = refs_ts;
                if (atomic_compare_exchange_weak(ts_bucket, &v, a)) {
                    protect_leave(counter);
                    return;
                } else {
                    goto ref_retry;
                }
            } else {
                if (atomic_compare_exchange_weak(bucket, &v, a)) {
                    protect_leave(counter);
                    return;
                } else {
                    goto ref_restart;
                }
            }
        }

        if (++bucket == tbl->refs_table + tbl->refs_size) bucket = tbl->refs_table;
    }

    // not found after linear probing
    if (ts_bucket != NULL) {
        v = refs_ts;
        if (atomic_compare_exchange_weak(ts_bucket, &v, a)) {
            protect_leave(counter);
            return;
        } else {
            goto ref_retry;
        }
    } else {
        // hash table full
        protect_leave(counter);
        protect_resize(tbl);
        counter = protect_enter(tbl);
        goto ref_retry;
    }
}

void
protect_down(refs_table_t *tbl, uint64_t a)
{
    _Atomic(uint64_t)* bucket;
    _Atomic(uint32_t) *counter = protect_enter(tbl);

    bucket = tbl->refs_table + (fnvhash8(a) & (tbl->refs_size - 1));
    int i = 128; // try 128 times linear probing

    while (i--) {
        uint64_t d = atomic_load_explicit(bucket, memory_order_relaxed);
        if (d == a) {
            atomic_store_explicit(bucket, refs_ts, memory_order_relaxed);
            protect_leave(counter);
            return;
        }
        if (++bucket == tbl->refs_table + tbl->refs_size) bucket = tbl->refs_table;
    }

    // not found after linear probing: a was not protected
    protect_leave(counter);
    assert(0);
}

uint64_t*
protect_iter(refs_table_t *tbl, size_t first, size_t end)
{
    // assert(first < tbl->refs_size);
    // assert(end <= tbl->refs_size);

    _Atomic(uint64_t)* bucket = tbl->refs_table + first;
    while (bucket != tbl->refs_table + end) {
        uint64_t d = atomic_load_explicit(bucket, memory_order_relaxed);
        if (d != 0 && d != refs_ts) return (uint64_t*)bucket;
        bucket++;
    }
    return NULL;
}

uint64_t
protect_next(refs_table_t *tbl, uint64_t **_bucket, size_t end)
{
    _Atomic(uint64_t)* bucket = (_Atomic(uint64_t)*) *_bucket;
    // assert(bucket != NULL);
    // assert(end <= tbl->refs_size);
    uint64_t result = *bucket;
    bucket++;
    while (bucket != tbl->refs_table + end) {
        uint64_t d = atomic_load_explicit(bucket, memory_order_relaxed);
        if (d != 0 && d != refs_ts) {
            *_bucket = (uint64_t*)bucket;
            return result;
        }
        bucket++;
    }
    *_bucket = NULL;
    return result;
}

/**
 * NOTE: exits the process on failure. Reporting a status would have to reach
 * mtbdd_protect(), zdd_protect() and listdd_protect(), which create their
 * table lazily and have nowhere to report to.
 */
void
protect_create(refs_table_t *tbl, size_t _refs_size)
{
    if (popcnt_uint64(_refs_size) != 1) {
        fprintf(stderr, "refs: Table size must be a power of 2!\n");
        exit(1);
    }

    tbl->refs_size = _refs_size;
    tbl->refs_table = (_Atomic(uint64_t)*)sylvan_alloc_aligned(tbl->refs_size * sizeof(uint64_t));
    if (tbl->refs_table == 0) {
        fprintf(stderr, "refs: Unable to allocate memory: %s!\n", strerror(errno));
        exit(1);
    }
    refs_create_workers(tbl);
}

void
protect_free(refs_table_t *tbl)
{
    sylvan_free_aligned((void*)tbl->refs_table, tbl->refs_size * sizeof(uint64_t));
    tbl->refs_table = 0;
    refs_free_workers(tbl);
}
