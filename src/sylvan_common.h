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

#ifndef SYLVAN_COMMON_H
#define SYLVAN_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Opaque pull iterator shared by BDDs, MTBDDs, ZDDs and LDDs.
 * Initialize the corresponding Sylvan subpackage before creation and destroy
 * all iterators before sylvan_quit(). One iterator must not be used by two
 * threads at once, including calls to destroy; separate iterators are independent.
 *
 * An iterator protects its diagram and variable set until destruction. Calling
 * create/destroy follows the same threading rules as mtbdd_protect and
 * mtbdd_unprotect (or the corresponding ZDD/LDD protection APIs). Calling
 * sylvan_gc() between next calls is safe, including when it shrinks the tables:
 * live node indices remain stable. No next operation creates nodes, performs
 * garbage collection or allocates traversal storage. Creation allocates ordinary
 * heap storage, but creates no diagram nodes. Keep inputs protected as usual
 * until creation completes, including while a spawned create task is pending.
 * Non-sentinel handles must refer to live diagrams of the indicated family;
 * stale handles and arbitrary node records are not supported.
 *
 * All create/next functions return 1 on success and 0 on error. Create leaves
 * its output unchanged on invalid arguments or iterator-storage allocation
 * failure; failed creation leaves nothing protected. Protection-table growth
 * follows the existing protection API's fatal allocation-failure policy. Next sets
 * has_item to 1 for an item, or 0 at exhaustion (also on repeated calls after
 * exhaustion). Values and leaf outputs are unchanged at exhaustion. Invalid
 * next arguments leave both iterator state and all outputs unchanged: NULL
 * iterator/has_item, wrong iterator family, wrong count, NULL values with a
 * nonzero count, or a NULL leaf output for MTBDDs. Arrays must have room for
 * count entries. NULL values is allowed for a zero-width item.
 */
typedef struct sylvan_iterator sylvan_iterator;

/** BDD/MTBDD cubes use 0, 1, 2 (don't-care); minterms use only 0 and 1. */
typedef enum {
    SYLVAN_ITERATOR_CUBES = 0,
    SYLVAN_ITERATOR_MINTERMS = 1
} sylvan_iterator_mode;

/**
 * Unprotect any iterator's roots, then free its storage, following the same
 * threading rules as the corresponding unprotect API. The iterator must not
 * be used after this call returns. Passing NULL is allowed.
 */
void sylvan_iterator_destroy(sylvan_iterator *iterator);

/**
 * Initialize the Sylvan parallel decision diagrams package.
 *
 * First, Sylvan must know how big the nodes table and cache may be.
 * Either use sylvan_set_sizes to explicitly set the table sizes, or use sylvan_set_limits
 * to let Sylvan compute the sizes for you.
 *
 * Then, call sylvan_init_package. This allocates the tables and other support structures.
 * Sylvan allocates virtual memory to accomodate the maximum sizes of both tables.
 * Initially, Sylvan only uses the minimum sizes.
 * During garbage collection, table sizes may be doubled until the maximum size is reached.
 *
 * Then, call initialization functions for the MTBDD/LDD modules like sylvan_init_mtbdd
 * and sylvan_init_ldd.
 *
 * Memory usage:
 * Every node requires 24 bytes memory. (16 bytes data + 8 bytes table overhead)
 * Every operation cache entry requires 36 bytes memory. (32 bytes data + 4 bytes table overhead)
 */
void sylvan_init_package(void);

/**
 * Explicitly set the sizes of the nodes table and the operation cache.
 * The sizes are in bytes, but they must be powers of two.
 * The minimum size is the size initially used.
 * The maximum size is the size allocated in virtual memory.
 */
void sylvan_set_sizes(size_t min_tablesize, size_t max_tablesize, size_t min_cachesize, size_t max_cachesize);

/**
 * Implicitly compute and set the sizes of the nodes table and the operation cache.
 *
 * This function computes max_tablesize and max_cachesize to fit the memory cap.
 * The memory cap is in bytes.
 *
 * The parameter table_ratio controls the ratio between the nodes table and the cache.
 * For the value 0, both tables are of the same size.
 * For values 1, 2, 3 ... the nodes table will be 2x, 4x, 8x ... as big as the cache
 * For values -1, -2, -3 ... the cache will be 2x, 4x, 8x ... as big as the nodes table
 *
 * The parameter initial_ratio controls how much smaller the initial table sizes are.
 * For values of 1, 2, 3, 4 the tables will initially be 2, 4, 8, 16 times smaller.
 */
void sylvan_set_limits(size_t memory_cap, int table_ratio, int initial_ratio);

/**
 * Handler for a node that could not be created after garbage collection at the
 * maximum nodes-table size. This is an out-of-memory condition, not a recovery hook.
 * It is also called if node creation still fails with garbage collection disabled.
 *
 * The handler runs on a Lace worker in the middle of an operation, while other
 * workers may be waiting for that operation. It must not call Sylvan operations
 * and must not return: it should exit or abort. If it returns anyway, Sylvan
 * prints its usual message to stderr and calls exit(1).
 *
 * exit() runs atexit handlers and static destructors on this worker. Neither the
 * handler nor those may stop Lace (lace_stop waits for every worker), quit
 * Sylvan, or wait for other workers, or the program can hang. If the
 * application's cleanup cannot avoid that, write the report, flush it, and call
 * _Exit() instead.
 *
 * Several workers can find the table full at the same time, so the handler can
 * be called concurrently. A handler that must report only once has to arrange
 * that itself (for example with an atomic flag).
 *
 * kind is "BDD" (including MTBDD), "MDD" (LDD), or "ZDD". filled and total are
 * the occupied and total bucket counts used in the usual message.
 */
typedef void (*sylvan_table_full_cb)(const char *kind, size_t filled, size_t total);

/**
 * Set the global nodes-table-full handler. NULL restores the default handler,
 * which prints the usual message to stderr and calls exit(1).
 * The handler pointer is stored atomically and may be set from any thread.
 * Install the handler before starting operations that may exhaust the table.
 * Replacing the handler does not wait for a call that is already running.
 */
void sylvan_set_table_full_handler(sylvan_table_full_cb handler);

/**
 * Frees all Sylvan data (also calls the quit() functions of BDD/LDD parts)
 */
void sylvan_quit(void);

/**
 * Registers a hook callback called during sylvan_quit()
 */
typedef void (*quit_cb)(void);
void sylvan_register_quit(quit_cb cb);

/**
 * Return number of occupied buckets in nodes table and total number of buckets.
 */
VOID_TASK_DECL_2(sylvan_table_usage, size_t*, size_t*);
#define sylvan_table_usage(filled, total) (RUN(sylvan_table_usage, filled, total))

/**
 * GARBAGE COLLECTION
 *
 * Garbage collection is performed in a new Lace frame, interrupting all ongoing work
 * until garbage collection is completed.
 *
 * By default, garbage collection is triggered when no new nodes can be added to the nodes table.
 * This is detected when there are no more available buckets in the bounded probe sequence.
 * Garbage collection can also be triggered manually with sylvan_gc()
 *
 * Garbage collection procedure:
 * 1) All installed pre_gc hooks are called.
 *    See sylvan_gc_hook_pre to add hooks.
 * 2) The operation cache is cleared.
 * 3) The nodes table (data part) is cleared.
 * 4) All nodes are marked (to be rehashed) using the various marking callbacks.
 *    See sylvan_gc_add_mark to add marking callbacks.
 *    Afterwards, the ondead hook is called for all now-dead nodes with the custom flag set.
 * 5) The main gc hook is called. The function of this hook is to perform resizing.
 *    The default implementation doubles the nodes table and operation cache sizes.
 *    See sylvan_gc_hook_main to set the hook.
 *    If allocation still fails after collection, further collections enforce
 *    growth of the nodes table up to its maximum, even with a custom main hook.
 *    A collection asked for with sylvan_gc() instead shrinks the tables to fit the
 *    live nodes; see sylvan_gc().
 * 5) The nodes table (hash part) is cleared.
 * 6) All marked nodes are rehashed.
 * 7) All installed post_gc hooks are called.
 *    See sylvan_gc_hook_post to add hooks.
 *
 * For parts of the garbage collection process, specific methods exist.
 * - sylvan_clear_cache() clears the operation cache (step 2)
 * - sylvan_clear_and_mark() performs steps 3 and 4.
 * - sylvan_rehash_all() performs steps 5 and 6.
 */

/**
 * Policies for table growth and for releasing unused node memory.
 */
typedef enum {
    SYLVAN_GC_FEW_COLLECTIONS, /**< Always grow; keep the node arena on explicit GC. */
    SYLVAN_GC_LOW_MEMORY       /**< Grow above half full; release unused arena stretches. */
} sylvan_gc_policy_t;

/**
 * Select the garbage collection policy after sylvan_init_package().
 *
 * SYLVAN_GC_FEW_COLLECTIONS installs sylvan_gc_aggressive_resize: each automatic
 * collection doubles the tables up to their maximum sizes. sylvan_gc() shrinks
 * the hash table and operation cache, but keeps the node arena's data range and
 * does not release its empty stretches to the operating system.
 *
 * SYLVAN_GC_LOW_MEMORY installs sylvan_gc_normal_resize: an automatic collection
 * normally grows the tables only when more than half the nodes table is still in use.
 * sylvan_gc() also reduces the data range and releases empty arena stretches.
 * With either policy, allocation that still fails after collection forces
 * further node-table growth and retries, up to the configured maximum size.
 *
 * Each sylvan_init_package() resets the policy to SYLVAN_GC_FEW_COLLECTIONS
 * (SYLVAN_GC_LOW_MEMORY if compiled with SYLVAN_AGGRESSIVE_RESIZE=0).
 * The default preserves aggressive growth, but explicit collections now retain
 * the arena. Select SYLVAN_GC_LOW_MEMORY to release unused arena memory.
 *
 * This replaces any main hook, just like sylvan_gc_hook_main(). Installing a
 * main hook afterwards overrides the growth heuristic; the policy's shrink mode
 * stays. Allocation recovery can still force node-table growth after that hook.
 * Calling this again restores the policy's growth hook. No collection is run.
 * Change the policy only between operations, with no concurrent Sylvan calls.
 * Unrecognized policy values are ignored.
 */
void sylvan_gc_set_policy(sylvan_gc_policy_t policy);

/**
 * Return the last selected policy, even if a custom main hook overrides growth.
 */
sylvan_gc_policy_t sylvan_gc_get_policy(void);

/**
 * Trigger garbage collection manually.
 *
 * This also shrinks the nodes table to fit the live nodes: to the smallest size,
 * from the initial size up, with room for twice the live nodes, and the operation
 * cache in proportion. Nodes do not move. With SYLVAN_GC_LOW_MEMORY, the part of
 * the node arena in use shrinks only to just above the highest live node, and
 * the memory of its empty stretches is given back to the operating system in
 * 64 KiB chunks when virtual memory allocation is enabled. With
 * SYLVAN_GC_FEW_COLLECTIONS, the arena's data range and pages are retained.
 * The main gc hook is not called.
 * When the program needs more nodes again, the automatic collections grow the
 * tables as usual.
 */
VOID_TASK_DECL_0(sylvan_gc);
#define sylvan_gc() (RUN(sylvan_gc))

/**
 * Garbage collection when the nodes table is full: what Sylvan does itself when it
 * cannot add a node. It resizes with the main gc hook (by default, it doubles the
 * nodes table and the operation cache). The built-in main hooks never shrink
 * the tables; a custom hook may shrink them. This call can also join another
 * worker's explicit sylvan_gc(), which shrinks instead of calling the main hook.
 * Programs that want to collect garbage want sylvan_gc() instead.
 * Node constructors retry after this collection. If allocation still fails,
 * they collect again with an increasing minimum nodes-table size, up to the
 * configured maximum. The main hook still runs; only the nodes-table minimum
 * is enforced afterwards. If recovery joins a collection that leaves the table
 * below its requested minimum, it collects again to enforce that minimum before
 * retrying. Cache sizing is not constrained by recovery. With GC
 * disabled, no growth is attempted and a failed retry calls the full handler.
 */
VOID_TASK_DECL_0(sylvan_gc_collect);
#define sylvan_gc_collect() (RUN(sylvan_gc_collect))

/**
 * Enable or disable garbage collection.
 *
 * This affects both automatic and manual garbage collection, i.e.,
 * calling sylvan_gc() while garbage collection is disabled does not have any effect.
 * If no new nodes can be added, Sylvan calls the table-full handler.
 */
void sylvan_gc_enable(void);
void sylvan_gc_disable(void);

/**
 * Test if garbage collection must happen now.
 * This is just a call to the Lace framework to see if NEWFRAME has been used.
 * Before calling this, make sure all used BDDs are referenced.
 */
#define sylvan_gc_test() YIELD_NEWFRAME()

/**
 * Clear the operation cache.
 */
VOID_TASK_DECL_0(sylvan_clear_cache);
#define sylvan_clear_cache() RUN(sylvan_clear_cache)

/**
 * Clear the nodes table (data part) and mark all nodes with the marking mechanisms.
 */
VOID_TASK_DECL_0(sylvan_clear_and_mark);
#define sylvan_clear_and_mark() RUN(sylvan_clear_and_mark)

/**
 * Clear the nodes table (hash part) and rehash all marked nodes.
 */
VOID_TASK_DECL_0(sylvan_rehash_all);
#define sylvan_rehash_all() RUN(sylvan_rehash_all)

/**
 * Callback type
 */
LACE_TYPEDEF_CB(void, gc_hook_cb);

/**
 * Add a hook that is called before garbage collection begins.
 */
void sylvan_gc_hook_pregc(gc_hook_cb callback);

/**
 * Add a hook that is called after garbage collection is finished.
 */
void sylvan_gc_hook_postgc(gc_hook_cb callback);

/**
 * Replace the hook called between node marking and rehashing.
 * Typically, the hook resizes the hash table and operation cache according to some heuristic.
 * This does not change the policy's shrink mode or sylvan_gc_get_policy().
 * A later sylvan_gc_set_policy() replaces this hook with the policy's growth hook.
 * Explicit sylvan_gc() collections do not call the main hook.
 * If a node allocation fails again after an automatic collection, recovery
 * runs further collections with increasing minimum nodes-table sizes. This
 * hook still runs, but the minimum is enforced after it and before rehashing,
 * even if the hook did not grow the table. The hook alone controls cache sizing.
 * This guarantees progress to the configured nodes-table maximum before the
 * table-full handler is called (unless GC is disabled).
 */
void sylvan_gc_hook_main(gc_hook_cb callback);

/**
 * Add a marking mechanism.
 *
 * The mark_cb callback is called during garbage collection and should call the
 * appropriate recursive marking functions for the decision diagram nodes, for example
 * mtbdd_gc_mark_rec() for MTBDDs or lddmc_gc_mark_rec() for LDDs.
 *
 * The sylvan_count_refs() function uses the count_cb callbacks to compute the number
 * of references.
 */
void sylvan_gc_add_mark(gc_hook_cb mark_cb);

/**
 * One of the hooks for resizing behavior.
 * Default if SYLVAN_AGGRESSIVE_RESIZE is set.
 * Always double size on gc() until maximum reached.
 * Use sylvan_gc_hook_main() to set this heuristic.
 */
VOID_TASK_DECL_0(sylvan_gc_aggressive_resize);

/**
 * One of the hooks for resizing behavior.
 * Default if SYLVAN_AGGRESSIVE_RESIZE is not set.
 * Double size on gc() whenever >50% is used.
 * Use sylvan_gc_hook_main() to set this heuristic.
 */
VOID_TASK_DECL_0(sylvan_gc_normal_resize);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
