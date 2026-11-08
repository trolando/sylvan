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

/* Do not include this file directly. Instead, include sylvan.h */

#ifndef REFS_INLINE_H
#define REFS_INLINE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Implementation of external references ("protect pointers")
 * Based on a hash table for 64-bit non-null values, linear probing
 * Use tombstones for deleting
 */
/*
 * A resize waits until no operation is active. Each Lace worker counts its
 * active operations in its own counter (refs_workers), on its own cache line,
 * so that workers that protect and unprotect at the same time do not contend
 * on one shared word. Other threads count theirs in the low bits of
 * refs_control, whose high bits hold the state of a resize.
 */
typedef struct
{
    /* read by every operation; written only by a resize, when no operation is active */
    _Atomic(uint64_t) *refs_table;        // table itself
    size_t refs_size;                     // number of buckets
    _Atomic(uint32_t) *refs_workers;      // active operations per Lace worker, one counter per 128 bytes
    size_t refs_nworkers;                 // number of per-worker counters

    /* keeps refs_control off the cache line of the fields above */
    char refs_pad[128];

    /* control field and helpers during resize operation */
    _Atomic(uint32_t) refs_control;       // resize state (high bits), active operations of other threads (low bits)
    _Atomic(uint64_t) *refs_resize_table; // previous table
    size_t refs_resize_size;              // size of previous table
    _Atomic(size_t) refs_resize_part;     // which part is next
    _Atomic(size_t) refs_resize_done;     // how many parts are done
} refs_table_t;

// Count number of entries
size_t protect_count(refs_table_t *tbl);

// Add or remove the 64-bit value a
// Will fail (assertion) if a is not present when removing
void protect_up(refs_table_t *tbl, uint64_t a);
void protect_down(refs_table_t *tbl, uint64_t a);

// Return a bucket or NULL to start iterating
uint64_t *protect_iter(refs_table_t *tbl, size_t first, size_t end);

// Continue iterating, set bucket to next bucket or NULL
uint64_t protect_next(refs_table_t *tbl, uint64_t **bucket, size_t end);

// User must supply a pointer, protect_create and protect_free handle
// initialization/destruction
void protect_create(refs_table_t *tbl, size_t _refs_size);
void protect_free(refs_table_t *tbl);

#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif
