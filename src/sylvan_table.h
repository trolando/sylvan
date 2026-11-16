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

/* Do not include this file directly. Instead, include sylvan_int.h */

#ifndef SYLVAN_TABLE_H
#define SYLVAN_TABLE_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Lockless hash table (set) to store 16-byte keys.
 * Each unique key is associated with a 40-bit number.
 *
 * The set has support for stop-the-world garbage collection.
 * Methods llmsset_clear, llmsset_mark and llmsset_rehash implement garbage collection.
 * During their execution, llmsset_lookup is not allowed.
 *
 * WARNING: Originally, this table is designed to allow multiple tables.
 * However, this is not compatible with thread local storage for now.
 * Do not use multiple tables.
 */

/**
 * hash(a, b, seed)
 * equals(lhs_a, lhs_b, rhs_a, rhs_b)
 * create(a, b) -- with a,b pointers, allows changing pointers on create of node,
 *                 but must keep hash/equals same!
 * destroy(a, b)
 */
typedef uint64_t (*llmsset_hash_cb)(uint64_t, uint64_t, uint64_t);
typedef int (*llmsset_equals_cb)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*llmsset_create_cb)(uint64_t *, uint64_t *);
typedef void (*llmsset_destroy_cb)(uint64_t, uint64_t);

typedef struct llmsset
{
    _Atomic(uint64_t)* table;        // table with hashes
    uint8_t*           data;         // table with values
    _Atomic(uint64_t)* bitmap1;      // ownership bitmap (per 512 buckets)
    _Atomic(uint64_t)* bitmap2;      // bitmap for "contains data"
    uint64_t*          bitmapc;      // bitmap for "use custom functions"
    size_t             max_size;     // maximum size of the hash table (for resizing)
    size_t             table_size;   // size of the hash table (number of slots) --> power of 2!
    size_t             data_size;    // nodes are in the data buckets below this; >= table_size
#if LLMSSET_MASK
    size_t             mask;         // size-1
#endif
    size_t             f_size;
    llmsset_hash_cb    hash_cb;      // custom hash function
    llmsset_equals_cb  equals_cb;    // custom equals function
    llmsset_create_cb  create_cb;    // custom create function
    llmsset_destroy_cb destroy_cb;   // custom destroy function
    _Atomic(int16_t)   threshold;    // number of iterations for insertion until returning error
} *llmsset_t;

/**
 * Create the set.
 * This will allocate a set of <max_size> buckets in virtual memory.
 * The actual space used is <initial_size> buckets.
 */
llmsset_t llmsset_create(size_t initial_size, size_t max_size);

/**
 * Free the set.
 */
void llmsset_free(llmsset_t dbs);

/**
 * Retrieve the maximum size of the set.
 */
static inline size_t
llmsset_get_max_size(const llmsset_t dbs)
{
    return dbs->max_size;
}

/**
 * Retrieve the current size of the lockless MS set.
 */
size_t llmsset_get_size(const llmsset_t dbs);

/**
 * Retrieve the number of data buckets in use: new nodes go into the buckets
 * below it, and every live node is below it. This is the size of the hash
 * table, unless the hash table shrank while the data range was retained or
 * live nodes remained in higher buckets (nodes never move).
 */
size_t llmsset_get_data_size(const llmsset_t dbs);

/**
 * Optional handle bounds checking, also enabled in Release builds.
 * Check the data range, not the hash table size: shrinking does not move nodes.
 * MTBDD_GETNODE, ZDD_GETNODE and LDD_GETNODE all use this pointer helper.
 * With the option off, its code is unchanged and no bounds are loaded.
 */
#ifndef SYLVAN_CHECK_HANDLES
#define SYLVAN_CHECK_HANDLES 0
#endif

#if SYLVAN_CHECK_HANDLES
SYLVAN_NORETURN void sylvan_handle_check_failed(size_t index, size_t size, const char *file, int line);
#endif

/**
 * Retrieve a pointer to the data associated with the 40-bit value.
 */
static inline void*
llmsset_index_to_ptr(const llmsset_t dbs, size_t index)
{
#if SYLVAN_CHECK_HANDLES
    const size_t size = llmsset_get_data_size(dbs);
    if (index >= size) sylvan_handle_check_failed(index, size, __FILE__, __LINE__);
#endif
    return dbs->data + index * 16;
}

/**
 * Set the table size of the set.
 * Typically called during garbage collection, after clear and before rehash.
 * Also grows the data range when needed. A smaller size retains the data range
 * and its pages; use llmsset_shrink to reduce it and release empty stretches.
 * Ignores sizes <= 128 or above the maximum size.
 */
void llmsset_set_size(llmsset_t dbs, size_t size);

/**
 * Shrink the hash table to <size> slots. Call during garbage collection, after
 * marking and before rehashing, with room for the marked nodes. Nodes keep
 * their place: the data buckets in use shrink only to just above the highest
 * marked node, and the memory of data regions without marked nodes is given
 * back to the operating system when virtual memory allocation is enabled.
 * Also releases unused data when <size> equals the current hash table size.
 */
void llmsset_shrink(llmsset_t dbs, size_t size);

/**
 * Core function: find existing data or add new.
 * Returns the unique 40-bit value associated with the data, or 0 when table is full.
 * Also, this value will never equal 0 or 1.
 * Note: garbage collection during lookup strictly forbidden
 */
uint64_t llmsset_lookup(const llmsset_t dbs, const uint64_t a, const uint64_t b, int *created);

/**
 * Same as lookup, but use the custom functions
 */
uint64_t llmsset_lookupc(const llmsset_t dbs, const uint64_t a, const uint64_t b, int *created);

/**
 * To perform garbage collection, the user is responsible that no lookups are performed during the process.
 *
 * 1) call llmsset_clear 
 * 2) call llmsset_mark for every bucket to rehash
 * 3) call llmsset_rehash 
 */
VOID_TASK_DECL_1(llmsset_clear, llmsset_t);
#define llmsset_clear(dbs) RUN(llmsset_clear, dbs)

VOID_TASK_DECL_1(llmsset_clear_data, llmsset_t);
#define llmsset_clear_data(dbs) RUN(llmsset_clear_data, dbs)

VOID_TASK_DECL_1(llmsset_clear_hashes, llmsset_t);
#define llmsset_clear_hashes(dbs) RUN(llmsset_clear_hashes, dbs)

/**
 * Check if a certain data bucket is marked (in use).
 */
int llmsset_is_marked(const llmsset_t dbs, uint64_t index);

/**
 * During garbage collection, buckets are marked (for rehashing) with this function.
 * Returns 0 if the node was already marked, or non-zero if it was not marked.
 * May also return non-zero if multiple workers marked at the same time.
 */
int llmsset_mark(const llmsset_t dbs, uint64_t index);

/**
 * Rehash all marked buckets.
 * Returns 0 if successful, or the number of buckets not rehashed if not.
 */
TASK_DECL_1(int, llmsset_rehash, llmsset_t);
#define llmsset_rehash(dbs) RUN(llmsset_rehash, dbs)

/**
 * Rehash a single bucket.
 * Returns 0 if successful, or 1 if not.
 */
int llmsset_rehash_bucket(const llmsset_t dbs, uint64_t d_idx);

/**
 * Retrieve number of marked buckets.
 */
TASK_DECL_1(size_t, llmsset_count_marked, llmsset_t);
#define llmsset_count_marked(dbs) RUN(llmsset_count_marked, dbs)

/* Serial count with no Lace safepoint, for a terminal allocation failure.
 * Like the parallel count, includes the whole data range and reserved buckets.
 * Concurrent allocations may increase the count while it is being read. */
size_t llmsset_count_marked_no_yield(const llmsset_t dbs);

/**
 * During garbage collection, this method calls the destroy callback
 * for all 'custom' data that is not kept.
 */
VOID_TASK_DECL_1(llmsset_destroy_unmarked, llmsset_t);
#define llmsset_destroy_unmarked(dbs) RUN(llmsset_destroy_unmarked, dbs)

/**
 * Set custom functions
 */
void llmsset_set_custom(const llmsset_t dbs, llmsset_hash_cb hash_cb, llmsset_equals_cb equals_cb, llmsset_create_cb create_cb, llmsset_destroy_cb destroy_cb);

/**
 * Default hashing functions.
 */
#define llmsset_hash sylvan_tabhash16
#define llmsset_fnvhash sylvan_fnvhash16

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
