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

typedef struct nodes_table nodes_table;

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
 * Estimate the bytes reserved by this nodes backend for <capacity> buckets.
 * This includes its node/index arrays, allocation/custom bitmaps, and any
 * fixed routing metadata, but not allocator page rounding.
 */
size_t nodes_get_capacity_bytes(size_t capacity);

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
 * Internal canonical-index routes. Decision routes use the node's variable
 * identity (currently its var field). Auxiliary routes keep node families
 * that do not belong to an ordered decision variable out of those subtables.
 */
#define NODES_SUBTABLE_DECISION(variable) ((uint32_t)(variable))
#define NODES_SUBTABLE_LEAF             UINT32_C(0x01000000)
#define NODES_SUBTABLE_MAP              UINT32_C(0x01000001)
#define NODES_SUBTABLE_LISTDD           UINT32_C(0x01000002)
#define NODES_SUBTABLE_DEFAULT          UINT32_C(0x01000003)

/**
 * Find or insert a node using a canonical-index routing hint. Backends without
 * subtables deliberately ignore the hint and retain their normal behavior.
 */
uint64_t nodes_lookup_subtable(const nodes_table* dbs, uint32_t subtable,
                               uint64_t a, uint64_t b, int *created);
uint64_t nodes_lookupc_subtable(const nodes_table* dbs, uint32_t subtable,
                                uint64_t a, uint64_t b, int *created);

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
 * Check if an index is a valid node and/or is marked to be reinserted after
 * garbage collection.
 */
int nodes_is_marked(const nodes_table* dbs, uint64_t index);

/**
 * Recursively mark a node and its descendants.
 */
static inline void nodes_mark_rec(const nodes_table* dbs, uint64_t index);

/**
 * Rebuild the nodes table by reinserting/rehashing all marked nodes.
 * Returns 0 if successful, or the number of buckets that could not be
 * reinserted otherwise.
 */
static inline int nodes_rebuild(nodes_table* dbs);

/**
 * Retrieve number of nodes in the table.
 */
static inline size_t nodes_count_nodes(nodes_table* dbs);

/**
 * After garbage collection, this method calls the destroy callback
 * for all 'custom' data that is not kept.
 */
static inline void nodes_cleanup_custom(nodes_table* dbs);

/**
 * Perform table maintenance requested by the table itself.
 *
 * A backend may notice during a lookup that its internal index has degraded,
 * for example that a hash chain has grown far past its target length, and ask
 * for a maintenance pass by setting sylvan_maintenance_requested. Sylvan then
 * calls this from a stop-the-world Lace frame, exactly like garbage
 * collection, but without clearing the node bitmap, without marking and
 * without clearing the operation cache.
 *
 * Maintenance may only reorganize the backend's own index. It must not free,
 * move or renumber nodes: every node index that was valid before the pass is
 * still valid and still denotes the same node afterwards.
 *
 * Backends that never need this leave it empty and never set the flag.
 */
static inline void nodes_maintain(nodes_table* dbs);

// Definitions of tasks
TASK(void, nodes_clear, nodes_table*, dbs)
TASK(void, nodes_mark_rec, const nodes_table*, dbs, uint64_t, index)
TASK(int, nodes_rebuild, nodes_table*, dbs)
TASK(size_t, nodes_count_nodes, nodes_table*, dbs)
TASK(void, nodes_cleanup_custom, nodes_table*, dbs)
TASK(void, nodes_maintain, nodes_table*, dbs)

/**
 * Helper callbacks for dealing with custom leaves.
 * This is used by the MT support (mt.c)
 * hash(a, b, seed)
 * equals(lhs_a, lhs_b, rhs_a, rhs_b)
 * create(a, b) -- with a,b pointers, allows changing pointers on create of node,
 *                 but must keep hash/equals same; returns 0 on success and a
 *                 negative error code on failure
 * destroy(a, b)
 */
typedef uint64_t (*nodes_hash_cb)(uint64_t, uint64_t, uint64_t);
typedef int (*nodes_equals_cb)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef int (*nodes_create_cb)(uint64_t *, uint64_t *);
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
