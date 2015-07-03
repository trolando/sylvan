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

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <lace.h>
#include <sylvan_config.h>

#ifndef LLMSSET_H
#define LLMSSET_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef LLMSSET_MASK
#define LLMSSET_MASK 0 // set to 1 to use bit mask instead of modulo
#endif

/**
 * Lockless hash table (set) to store 16-byte keys.
 * Each unique key is associated with a 42-bit number.
 *
 * The set has support for stop-the-world garbage collection.
 * Methods llmsset_clear, llmsset_mark and llmsset_rehash implement garbage collection.
 * During their execution, llmsset_lookup is not allowed.
 */

/**
 * Callback for "notify on dead".
 * If the callback returns non-zero, then the node is marked for keeping (rehashing).
 * The main purpose of the callback is to allow freeing allocated memory for custom terminal nodes.
 */
LACE_TYPEDEF_CB(int, llmsset_dead_cb, void*, uint64_t);

typedef struct llmsset
{
    uint64_t          *table;       // table with hashes
    uint8_t           *data;        // table with values
    size_t            max_size;     // maximum size of the hash table (for resizing)
    size_t            table_size;   // size of the hash table (number of slots) --> power of 2!
#if LLMSSET_MASK
    size_t            mask;         // size-1
#endif
    size_t            f_size;
    llmsset_dead_cb   dead_cb;      // callback when certain nodes are dead
    void*             dead_ctx;     // context for dead_cb
    int16_t           threshold;    // number of iterations for insertion until returning error
} *llmsset_t;

/**
 * Retrieve a pointer to the data associated with the 42-bit value.
 */
static inline void*
llmsset_index_to_ptr(const llmsset_t dbs, size_t index)
{
    return dbs->data + index * 16;
}

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
static inline size_t
llmsset_get_size(const llmsset_t dbs)
{
    return dbs->table_size;
}

/**
 * Set the table size of the set.
 * Typically called during garbage collection, after clear and before rehash.
 * Returns 0 if dbs->table_size > dbs->max_size!
 */
static inline void
llmsset_set_size(llmsset_t dbs, size_t size)
{
    /* check bounds (don't be rediculous) */
    if (size > 128 && size <= dbs->max_size) {
        dbs->table_size = size;
#if LLMSSET_MASK
        /* Warning: if size is not a power of two, you will get interesting behavior */
        dbs->mask = dbs->table_size - 1;
#endif
        dbs->threshold = (64 - __builtin_clzl(dbs->table_size)) + 4; // doubling table_size increases threshold by 1
    }
}

/**
 * Core function: find existing data or add new.
 * Returns the unique 42-bit value associated with the data, or 0 when table is full.
 * Also, this value will never equal 0 or 1.
 */
uint64_t llmsset_lookup(const llmsset_t dbs, const uint64_t a, const uint64_t b, int *created);

/**
 * To perform garbage collection, the user is responsible that no lookups are performed during the process.
 *
 * 1) call llmsset_clear 
 * 2) call llmsset_mark for every bucket to rehash
 * 3) call llmsset_rehash 
 */
VOID_TASK_DECL_1(llmsset_clear, llmsset_t);
#define llmsset_clear(dbs) CALL(llmsset_clear, dbs)

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
 */
VOID_TASK_DECL_1(llmsset_rehash, llmsset_t);
#define llmsset_rehash(dbs) CALL(llmsset_rehash, dbs)

/**
 * Retrieve number of marked buckets.
 */
TASK_DECL_1(size_t, llmsset_count_marked, llmsset_t);
#define llmsset_count_marked(dbs) CALL(llmsset_count_marked, dbs)

/**
 * Install callback function for nodes that are dead after garbage collection
 * Only called for nodes for which llmsset_notify_ondead has been called
 * NOTE: if you set a callback, then the garbage collection will be much slower!
 */
void llmsset_set_ondead(const llmsset_t dbs, llmsset_dead_cb cb, void* ctx);

/**
 * Mark a node that the ondead callback will be called if it is not marked during garbage collection,
 * i.e. it is a dead node. This allows the user to either perform cleanup, or resurrect the node.
 */
void llmsset_notify_ondead(const llmsset_t dbs, uint64_t index);

/**
 * During garbage collection, this method calls the callback functions for all nodes
 * for which llmsset_notify_ondead has been called and a callback function has been set.
 */
VOID_TASK_DECL_1(llmsset_notify_all, llmsset_t);
#define llmsset_notify_all(dbs) CALL(llmsset_notify_all, dbs)

#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif
