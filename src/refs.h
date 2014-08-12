/*
 * Copyright 2011-2014 Formal Methods and Tools, University of Twente
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

#include <stdio.h>  // for fprintf
#include <stdint.h> // for uint32_t etc
#include <sys/mman.h> // for mmap

#include <atomics.h>

#if USE_NUMA
#include <numa_tools.h>
#endif

#ifndef REFS_INLINE_H
#define REFS_INLINE_H

static size_t refs_size;
static size_t *refs_table;

static const size_t refs_ts = 0x7fffffffffffffff;

/* for resizing */
static volatile uint32_t refs_control;
static size_t *refs_resize_table;
static size_t refs_resize_part;
static size_t refs_resize_done;
static size_t refs_resize_size;

/* FNV-1a hash */
static inline uint64_t
refs_hash(uint64_t a)
{
    const uint64_t prime = 1099511628211;
    uint64_t hash = 14695981039346656037LLU;
    hash = (hash ^ a) * prime;
    hash = (hash ^ ((a << 25) | (a >> 39))) * prime;
    return hash ^ (hash >> 32);
}

static inline size_t
refs_count()
{
    size_t count = 0;
    size_t *bucket = refs_table;
    while (bucket != refs_table + refs_size) {
        if (*bucket != 0 && *bucket != refs_ts) count++;
        bucket++;
    }
    return count;
}

static inline void
refs_rehash(size_t v)
{
    if (v == 0) return; // do not rehash empty
    if (v == refs_ts) return; // do not rehash tombstone

    volatile size_t *bucket = refs_table + (refs_hash(v & 0x000000ffffffffff) & (refs_size - 1));

    int i = 128; // try 128 times linear probing
    while (i--) {
        if (*bucket == 0) { if (cas(bucket, 0, v)) return; }
        if (++bucket == refs_table + refs_size) bucket = refs_table;
    }

    assert(0); // impossible!
}

static int
refs_resize_help()
{
    if (0 == (refs_control & 0xf0000000)) return 0; // no resize in progress (anymore)
    if (refs_control & 0x80000000) return 1; // still waiting for preparation

    if (refs_resize_part >= refs_resize_size / 128) return 1; // all parts claimed
    size_t part = __sync_fetch_and_add(&refs_resize_part, 1);
    if (part >= refs_resize_size/128) return 1; // all parts claimed

    // rehash all
    int i;
    volatile size_t *bucket = refs_resize_table + part * 128;
    for (i=0; i<128; i++) refs_rehash(*bucket++);

    __sync_fetch_and_add(&refs_resize_done, 1);
    return 1;
}

static void
refs_resize()
{
    while (1) {
        uint32_t v = refs_control;
        if (v & 0xf0000000) {
            // someone else started resize
            // just rehash blocks until done
            while (refs_resize_help()) continue;
            return;
        }
        if (cas(&refs_control, v, 0x80000000 | v)) {
            // wait until all users gone
            while (refs_control != 0x80000000) continue;
            break;
        }
    }

    refs_resize_table = refs_table;
    refs_resize_size = refs_size;
    refs_resize_part = 0;
    refs_resize_done = 0;

    // calculate new size
    size_t new_size = refs_size;
    size_t count = refs_count();
    if (count*4 > refs_size) new_size *= 2;

    // allocate new table
    size_t *new_table = (size_t*)mmap(0, new_size * sizeof(size_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (new_table == (size_t*)-1) {
        fprintf(stderr, "refs: Unable to allocate memory!\n");
        exit(1);
    }

#if USE_NUMA
    numa_interleave(new_table, new_size * sizeof(size_t), 0);
#endif

    // set new data and go
    refs_table = new_table;
    refs_size = new_size;
    compiler_barrier();
    refs_control = 0x40000000;

    // until all parts are done, rehash blocks
    while (refs_resize_done != refs_resize_size/128) refs_resize_help();

    // done!
    compiler_barrier();
    refs_control = 0;

    // unmap old table
    munmap(refs_resize_table, refs_resize_size * sizeof(size_t));
}

/* Enter refs_modify */
static inline void
refs_enter()
{
    for (;;) {
        uint32_t v = refs_control;
        if (v & 0xf0000000) {
            while (refs_resize_help()) continue;
        } else {
            if (cas(&refs_control, v, v+1)) return;
        }
    }
}

/* Leave refs_modify */
static inline void
refs_leave()
{
    for (;;) {
        uint32_t v = refs_control;
        if (cas(&refs_control, v, v-1)) return;
    }
}

static inline int
refs_modify(const uint64_t a, const int dir)
{
    volatile size_t *bucket;
    volatile size_t *ts_bucket;
    size_t v, new_v;
    int res, i;

    refs_enter();

ref_retry:
    bucket = refs_table + (refs_hash(a) & (refs_size - 1));
    ts_bucket = NULL; // tombstone
    i = 128; // try 128 times linear probing

    while (i--) {
ref_restart:
        v = *bucket;
        if (v == refs_ts) {
            if (ts_bucket == NULL) ts_bucket = bucket;
        } else if (v == 0) {
            // not found
            res = 0;
            if (dir < 0) goto ref_exit;
            if (ts_bucket != NULL) {
                bucket = ts_bucket;
                ts_bucket = NULL;
                v = refs_ts;
            }
            new_v = a | (1ULL << 40);
            goto ref_mod;
        } else if ((v & 0x000000ffffffffff) == a) {
            // found
            res = 1;
            size_t count = v >> 40;
            if (count == 0x7fffff) goto ref_exit;
            count += dir;
            if (count == 0) new_v = refs_ts;
            else new_v = a | (count << 40);
            goto ref_mod;
        }

        if (++bucket == refs_table + refs_size) bucket = refs_table;
    }

    // not found after linear probing
    if (dir < 0) {
        res = 0;
        goto ref_exit;
    } else if (ts_bucket != NULL) {
        bucket = ts_bucket;
        ts_bucket = NULL;
        v = refs_ts;
        new_v = a | (1ULL << 40);
        if (!cas(bucket, v, new_v)) goto ref_retry;
        res = 1;
        goto ref_exit;
    } else {
        // hash table full
        refs_leave();
        refs_resize();
        return refs_modify(a, dir);
    }

ref_mod:
    if (!cas(bucket, v, new_v)) goto ref_restart;

ref_exit:
    refs_leave();
    return res;
}

static void
refs_up(uint64_t a)
{
    refs_modify(a, 1);
}

static void
refs_down(uint64_t a)
{
    int res = refs_modify(a, -1);
    assert(res != 0);
}

static size_t*
refs_iter(size_t first, size_t end)
{
    // assert(first < refs_size);
    // assert(end <= refs_size);

    size_t *bucket = refs_table + first;
    while (bucket != refs_table + end) {
        if (*bucket != 0 && *bucket != refs_ts) return bucket;
        bucket++;
    }
    return NULL;
}

static size_t
refs_next(size_t **_bucket, size_t end)
{
    size_t *bucket = *_bucket;
    // assert(bucket != NULL);
    // assert(end <= refs_size);
    size_t result = *bucket & 0x000000ffffffffff;
    bucket++;
    while (bucket != refs_table + end) {
        if (*bucket != 0 && *bucket != refs_ts) {
            *_bucket = bucket;
            return result;
        }
        bucket++;
    }
    *_bucket = NULL;
    return result;
}

static void
refs_create(size_t _refs_size)
{
    if (__builtin_popcountll(_refs_size) != 1) {
        fprintf(stderr, "refs: Table size must be a power of 2!\n");
        exit(1);
    }

    refs_size = _refs_size;
    refs_table = (size_t*)mmap(0, refs_size * sizeof(size_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, 0, 0);
    if (refs_table == (size_t*)-1) {
        fprintf(stderr, "refs: Unable to allocate memory!\n");
        exit(1);
    }

#if USE_NUMA
    numa_interleave(refs_table, refs_size * sizeof(size_t), 0);
#endif
}

static void
refs_free()
{
    munmap(refs_table, refs_size * sizeof(size_t));
}

#endif
