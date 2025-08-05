/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2018 Tom van Dijk, Johannes Kepler University Linz
 * Copyright 2019-2025 Tom van Dijk, Johannes Kepler University Linz
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

#ifndef SYLVAN_NODES_H
#define SYLVAN_NODES_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Functions for managing decision diagram nodes.
 */

typedef struct nodes nodes_table;

/**
 * Retrieve a pointer to the data associated with the index
 */
void* nodes_get_pointer(const nodes_table* dbs, size_t index);

/**
 * Create the table for the decision diagram nodes.
 * This will allocate a set of <max_size> buckets in virtual memory.
 * The actual space used is <initial_size> buckets.
 */
nodes_table* nodes_create(size_t initial_size, size_t max_size);

/**
 * Free the table.
 */
void nodes_free(nodes_table* dbs);

/**
 * Retrieve the maximum size of the nodes table.
 */
size_t nodes_get_max_size(const nodes_table* dbs);

/**
 * Retrieve the current size of the nodes table.
 */
size_t nodes_get_size(const nodes_table* dbs);

/**
 * Find or insert a decision diagram node.
 * Returns the index if successful, or 0 if the table is full!!
 * @ensures 2 <= \result < nodes_get_size(dbs);
 */
uint64_t nodes_lookup(const nodes_table* dbs, const uint64_t a, const uint64_t b, int *created);

/**
 * Find or insert a decision diagram node using the custom callbacks.
 * This is mainly used for multi-terminal custom leaf support.
 * Returns the index if successful, or 0 if the table is full!!
 * @ensures 2 <= \result < nodes_get_size(dbs);
 */
uint64_t nodes_lookupc(const nodes_table* dbs, const uint64_t a, const uint64_t b, int *created);

/**
 * To perform garbage collection, the user is responsible that no lookups are performed during the process.
 *
 * 1) call nodes_clear
 * 2) call nodes_mark for every bucket to rehash
 * 3) call nodes_rehash
 */

/**
 * Frees all nodes, but do not change any data
 * Clears all data to prepare for reinsertion/rehashing
 */
static inline void nodes_clear(nodes_table* dbs);

/**
 * Check if a certain data bucket is marked (in use).
 */
int nodes_is_marked(const nodes_table* dbs, uint64_t index);

/**
 * During garbage collection, buckets are marked (for rehashing) with this function.
 * Returns 0 if the node was already marked, or non-zero if it was not marked.
 * May also return non-zero if multiple workers marked at the same time.
 */
int nodes_mark(const nodes_table* dbs, uint64_t index);

/**
 * Rehash all marked buckets.
 * Returns 0 if successful, or the number of buckets not rehashed if not.
 */
static inline int nodes_rehash(nodes_table* dbs);

/**
 * Rehash a single bucket.
 * Returns 0 if successful, or 1 if not.
 */
int nodes_rehash_bucket(nodes_table* dbs, uint64_t d_idx);

/**
 * Retrieve number of marked buckets.
 */
static inline size_t nodes_count_marked(nodes_table* dbs);

/**
 * After garbage collection, this method calls the destroy callback
 * for all 'custom' data that is not kept.
 */
static inline void nodes_destroy_unmarked(nodes_table* dbs);

VOID_TASK_1(nodes_clear, nodes_table*, dbs)
TASK_1(int, nodes_rehash, nodes_table*, dbs)
TASK_1(size_t, nodes_count_marked, nodes_table*, dbs)
VOID_TASK_1(nodes_destroy_unmarked, nodes_table*, dbs)

/**
 * Helper callbacks for dealing with custom leaves.
 * This is used by the MT support (mt.c)
 * hash(a, b, seed)
 * equals(lhs_a, lhs_b, rhs_a, rhs_b)
 * create(a, b) -- with a,b pointers, allows changing pointers on create of node,
 *                 but must keep hash/equals same!
 * destroy(a, b)
 */
typedef uint64_t (*nodes_hash_cb)(uint64_t, uint64_t, uint64_t);
typedef int (*nodes_equals_cb)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef void (*nodes_create_cb)(uint64_t *, uint64_t *);
typedef void (*nodes_destroy_cb)(uint64_t, uint64_t);

/**
 * Set custom functions
 */
void nodes_set_custom(nodes_table* dbs, nodes_hash_cb hash_cb, nodes_equals_cb equals_cb, nodes_create_cb create_cb, nodes_destroy_cb destroy_cb);

/**
 * Set the table size of the set.
 * This should only be called internally during garbage collection.
 */
void nodes_set_size(nodes_table* dbs, size_t size);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
