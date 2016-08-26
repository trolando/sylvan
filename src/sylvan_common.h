/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016 Tom van Dijk, Johannes Kepler University Linz
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

#ifndef SYLVAN_COMMON_H
#define SYLVAN_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Initialize the Sylvan parallel decision diagrams package.
 *
 * After initialization, call sylvan_init_bdd and/or sylvan_init_ldd if you want to use
 * the BDD and/or LDD functionality.
 *
 * BDDs and LDDs share a common node table and operations cache.
 *
 * The node table is resizable.
 * The table is resized automatically when >50% of the table is filled during garbage collection.
 * This behavior can be customized by overriding the gc hook.
 * 
 * Memory usage:
 * Every node requires 24 bytes memory. (16 bytes data + 8 bytes overhead)
 * Every operation cache entry requires 36 bytes memory. (32 bytes data + 4 bytes overhead)
 *
 * Reasonable defaults: datasize of 1L<<26 (2048 MB), cachesize of 1L<<25 (1152 MB)
 */
void sylvan_init_package(size_t initial_tablesize, size_t max_tablesize, size_t initial_cachesize, size_t max_cachesize);

/**
 * Frees all Sylvan data (also calls the quit() functions of BDD/MDD parts)
 */
void sylvan_quit();

/**
 * Return number of occupied buckets in nodes table and total number of buckets.
 */
VOID_TASK_DECL_2(sylvan_table_usage, size_t*, size_t*);
#define sylvan_table_usage(filled, total) (CALL(sylvan_table_usage, filled, total))

/**
 * Perform garbage collection.
 *
 * Garbage collection is performed in a new Lace frame, interrupting all ongoing work
 * until garbage collection is completed.
 *
 * Garbage collection procedure:
 * 1) The operation cache is cleared and the hash table is reset.
 * 2) All live nodes are marked (to be rehashed). This is done by the "mark" callbacks.
 * 3) The "hook" callback is called.
 *    By default, this doubles the hash table size when it is >50% full.
 * 4) All live nodes are rehashed into the hash table.
 *
 * The behavior of garbage collection can be customized by adding "mark" callbacks and
 * replacing the "hook" callback.
 */
VOID_TASK_DECL_0(sylvan_gc);
#define sylvan_gc() (CALL(sylvan_gc))

/**
 * Enable or disable garbage collection.
 *
 * This affects both automatic and manual garbage collection, i.e.,
 * calling sylvan_gc() while garbage collection is disabled does not have any effect.
 */
void sylvan_gc_enable();
void sylvan_gc_disable();

/**
 * Add a "mark" callback to the list of callbacks.
 *
 * These are called during garbage collection to recursively mark nodes.
 *
 * Default "mark" functions that mark external references (via sylvan_ref) and internal
 * references (inside operations) are added by sylvan_init_bdd/sylvan_init_bdd.
 *
 * Functions are called in order.
 * level 10: marking functions of Sylvan (external/internal references)
 * level 20: call the hook function (for resizing)
 * level 30: rehashing
 */
LACE_TYPEDEF_CB(void, gc_mark_cb);
void sylvan_gc_add_mark(int order, gc_mark_cb callback);

/**
 * Set "hook" callback. There can be only one.
 *
 * The hook is called after the "mark" phase and before the "rehash" phase.
 * This allows users to perform certain actions, such as resizing the nodes table
 * and the operation cache. Also, dynamic resizing could be performed then.
 */
LACE_TYPEDEF_CB(void, gc_hook_cb);
void sylvan_gc_set_hook(gc_hook_cb new_hook);

/**
 * One of the hooks for resizing behavior.
 * Default if SYLVAN_AGGRESSIVE_RESIZE is set.
 * Always double size on gc() until maximum reached.
 */
VOID_TASK_DECL_0(sylvan_gc_aggressive_resize);

/**
 * One of the hooks for resizing behavior.
 * Default if SYLVAN_AGGRESSIVE_RESIZE is not set.
 * Double size on gc() whenever >50% is used.
 */
VOID_TASK_DECL_0(sylvan_gc_default_hook);

/**
 * Set "notify on dead" callback for the nodes table.
 * See also documentation in llmsset.h
 */
#define sylvan_set_ondead(cb, ctx) llmsset_set_ondead(nodes, cb, ctx)

/**
 * Global variables (number of workers, nodes table)
 */

extern llmsset_t nodes;

/* Garbage collection test task - t */
#define sylvan_gc_test() YIELD_NEWFRAME()

// BDD operations
#define CACHE_BDD_ITE             (0LL<<40)
#define CACHE_BDD_AND             (1LL<<40)
#define CACHE_BDD_XOR             (2LL<<40)
#define CACHE_BDD_EXISTS          (3LL<<40)
#define CACHE_BDD_AND_EXISTS      (4LL<<40)
#define CACHE_BDD_RELNEXT         (5LL<<40)
#define CACHE_BDD_RELPREV         (6LL<<40)
#define CACHE_BDD_SATCOUNT        (7LL<<40)
#define CACHE_BDD_COMPOSE         (8LL<<40)
#define CACHE_BDD_RESTRICT        (9LL<<40)
#define CACHE_BDD_CONSTRAIN       (10LL<<40)
#define CACHE_BDD_CLOSURE         (11LL<<40)
#define CACHE_BDD_ISBDD           (12LL<<40)
#define CACHE_BDD_SUPPORT         (13LL<<40)
#define CACHE_BDD_PATHCOUNT       (14LL<<40)

// MDD operations
#define CACHE_MDD_RELPROD         (20LL<<40)
#define CACHE_MDD_MINUS           (21LL<<40)
#define CACHE_MDD_UNION           (22LL<<40)
#define CACHE_MDD_INTERSECT       (23LL<<40)
#define CACHE_MDD_PROJECT         (24LL<<40)
#define CACHE_MDD_JOIN            (25LL<<40)
#define CACHE_MDD_MATCH           (26LL<<40)
#define CACHE_MDD_RELPREV         (27LL<<40)
#define CACHE_MDD_SATCOUNT        (28LL<<40)
#define CACHE_MDD_SATCOUNTL1      (29LL<<40)
#define CACHE_MDD_SATCOUNTL2      (30LL<<40)

// MTBDD operations
#define CACHE_MTBDD_APPLY         (40LL<<40)
#define CACHE_MTBDD_UAPPLY        (41LL<<40)
#define CACHE_MTBDD_ABSTRACT      (42LL<<40)
#define CACHE_MTBDD_ITE           (43LL<<40)
#define CACHE_MTBDD_AND_EXISTS    (44LL<<40)
#define CACHE_MTBDD_SUPPORT       (45LL<<40)
#define CACHE_MTBDD_COMPOSE       (46LL<<40)
#define CACHE_MTBDD_EQUAL_NORM    (47LL<<40)
#define CACHE_MTBDD_EQUAL_NORM_REL (48LL<<40)
#define CACHE_MTBDD_MINIMUM       (49LL<<40)
#define CACHE_MTBDD_MAXIMUM       (50LL<<40)
#define CACHE_MTBDD_LEQ           (51LL<<40)
#define CACHE_MTBDD_LESS          (52LL<<40)
#define CACHE_MTBDD_GEQ           (53LL<<40)
#define CACHE_MTBDD_GREATER       (54LL<<40)

/**
 * Registration of quit functions
 */
typedef void (*quit_cb)();
void sylvan_register_quit(quit_cb cb);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
