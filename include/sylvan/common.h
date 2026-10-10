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

/* The root visitor interface below names MTBDD. */
#include <sylvan/types.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** Status returned by fallible Sylvan operations. */
enum {
    SYLVAN_OK = 0,
    SYLVAN_ERR_OOM = -1,
    SYLVAN_ERR_INVALID = -2,
    SYLVAN_ERR_IO = -3,
    SYLVAN_ERR_CALLBACK = -4,
    SYLVAN_ERR_OVERFLOW = -5
};

/**
 * Return a stable human-readable description of a Sylvan status code.
 * The returned string is static and must not be freed.
 */
const char *sylvan_status_string(int status);

/** Apply callback control result: descend recursively instead of producing a result. */
enum {
    SYLVAN_APPLY_RECURSE = 1
};

/** Opaque state for BDD and MTBDD assignment iteration. */
typedef struct sylvan_iterator sylvan_iterator;

/** Output representation produced by a Sylvan iterator. */
typedef enum {
    SYLVAN_ITERATOR_CUBES = 0,
    SYLVAN_ITERATOR_MINTERMS = 1
} sylvan_iterator_mode;

/**
 * Destroy an iterator. Passing NULL is allowed.
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
 * Then, call initialization functions for the MTBDD/LDD modules like mtbdd_init
 * and listdd_init.
 *
 * Memory usage:
 * Every node requires 24 bytes memory. (16 bytes data + 8 bytes table overhead)
 * Every operation cache entry requires 44 bytes memory. (32 bytes data +
 * 8 bytes operation identifier + 4 bytes table overhead)
 *
 * Returns SYLVAN_OK, SYLVAN_ERR_INVALID when the table sizes were never set,
 * or SYLVAN_ERR_OOM. Nothing is initialized when it fails, and sylvan_quit()
 * must not be called.
 */
int sylvan_init_package(void);

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
static inline void sylvan_table_usage(size_t* filled, size_t* total);

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
 * 1) All installed pregc hooks are called.
 *    See sylvan_gc_hook_pregc to add hooks.
 * 2) The operation cache is cleared.
 * 3) The nodes table (data part) is cleared.
 * 4) Every root is visited and the nodes it reaches are marked (to be rehashed).
 *    See sylvan_gc_add_roots to add a root visitor.
 *    Afterwards, the ondead hook is called for all now-dead nodes with the custom flag set.
 * 5) The main gc hook is called. The function of this hook is to perform resizing.
 *    The default implementation doubles the nodes table and operation cache sizes.
 *    See sylvan_gc_hook_main to set the hook.
 * 6) The nodes table (hash part) is cleared.
 * 7) All marked nodes are rehashed.
 * 8) All installed postgc hooks are called.
 *    See sylvan_gc_hook_postgc to add hooks.
 * 9) After sylvan_gc() only, once the collection frame has ended: if automatic
 *    sifting is configured and the collection left a sift recommended, the
 *    forest is sifted in a frame of its own.
 *    See sylvan_gc_set_auto_sift in <sylvan/reorder.h>.
 *
 * For parts of the garbage collection process, specific methods exist.
 * - sylvan_clear_cache() clears the operation cache (step 2)
 * - sylvan_clear_and_mark() performs steps 3 and 4.
 * - sylvan_rehash_all() performs steps 6 and 7.
 */

/**
 * Trigger garbage collection manually.
 *
 * THE EXPLICIT ENTRY. Its caller owns the same precondition sylvan_sift()
 * states: no decision diagram operation is in flight. A caller here asks for a
 * collection between operations, rather than having one interrupt an
 * operation in the middle. That makes this the only place where an automatic
 * reordering may run; see sylvan_gc_set_auto_sift() in <sylvan/reorder.h>.
 * Collection itself needs no such precondition -- see sylvan_gc_collect
 * below, which is the entry that does not have it.
 *
 * When several workers call this at once, one runs the collection and the
 * others join its frame; only the one that ran it considers an automatic
 * reordering, because concurrent callers have already broken the precondition
 * as far as each other is concerned.
 */
static inline void sylvan_gc(void);

/**
 * Collect, and do nothing else.
 *
 * THE INTERNAL ENTRY, the one an operation uses when it cannot find a free
 * bucket: it calls from the middle of its own recursion, with sub-results
 * already computed and not yet combined. A collection there is sound, because
 * nothing moves and no handle changes. A reordering there would not be: the
 * suspended operation would resume and combine results it computed under the
 * previous rank order, under the new one. So this entry never reorders; that
 * is the only difference between it and sylvan_gc().
 *
 * Consumers want sylvan_gc().
 */
static inline void sylvan_gc_collect(void);

/**
 * Enable or disable garbage collection.
 *
 * This affects both automatic and manual garbage collection, i.e.,
 * calling sylvan_gc() while garbage collection is disabled does not have any effect.
 * If no new nodes can be added, Sylvan will write an error and abort.
 * Sifting is refused while garbage collection is disabled.
 */
void sylvan_gc_enable(void);
void sylvan_gc_disable(void);

/** Whether garbage collection is enabled; see sylvan_gc_disable(). */
int sylvan_gc_is_enabled(void);

/**
 * How many garbage collections have run since the process started.
 *
 * Monotone and never reset, not even by sylvan_quit(). It exists so that a
 * caller can establish that a collection it expects really happened, which is
 * otherwise unobservable outside the statistics build.
 */
uint64_t sylvan_gc_count(void);

/**
 * Set by the nodes table when it wants a stop-the-world maintenance pass.
 *
 * Maintenance is not garbage collection: it only lets the table reorganize its
 * own index (see nodes_maintain), so it neither collects nor invalidates
 * anything. The flag lives here, rather than next to the table, because
 * sylvan_gc_test is the safepoint that services it and is inlined into every
 * caller. Only the nodes table sets and clears it; Sylvan only reads it.
 * Backends that never request maintenance leave it zero forever.
 */
extern _Atomic(int) sylvan_maintenance_requested;

/**
 * Run a requested table maintenance pass in a new Lace frame.
 * Called by sylvan_gc_test; there is no reason to call this directly.
 */
void sylvan_maintenance(lace_worker* lace);

/**
 * Test if garbage collection must happen now.
 * This is just a call to the Lace framework to see if NEWFRAME has been used.
 * Before calling this, make sure all used BDDs are referenced.
 *
 * This is also where a pending table maintenance request is serviced, because
 * this is the one place every decision diagram operation passes through at a
 * point where interrupting it is known to be safe.
 */
static inline void sylvan_gc_test(lace_worker* lace)
{
    lace_check_yield(lace);
    if (LACE_UNLIKELY(atomic_load_explicit(
            &sylvan_maintenance_requested, memory_order_relaxed) != 0)) {
        sylvan_maintenance(lace);
    }
}

/**
 * Clear the operation cache.
 */

static inline void sylvan_clear_cache(void);

/**
 * Clear the nodes table (data part) and mark all nodes with the marking mechanisms.
 */

static inline void sylvan_clear_and_mark(void);

/**
 * Clear the nodes table (hash part) and rehash all marked nodes.
 */
static inline void sylvan_rehash_all(void);

/**
 * Callback type
 */
typedef void (*gc_hook_cb)(lace_worker* lace);

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
 */
void sylvan_gc_hook_main(gc_hook_cb callback);

/**
 * ROOT VISITORS
 *
 * The escape hatch for a consumer whose roots have no stable address, so that
 * mtbdd_protect() does not apply to them: a language binding on a moving or
 * copying heap, a handle table that reallocates, a container that renumbers.
 * Sylvan calls the registered hook whenever it needs to know what the consumer
 * considers a root, and the hook answers by yielding those roots.
 *
 * IT YIELDS VALUES, NOT ADDRESSES, and it yields them in arrays. Values,
 * because that is all Sylvan needs, and a stable address is what the consumer
 * does not have. Arrays, so that a consumer holding its roots contiguously
 * hands over a whole block in one call, and one that does not fills a small
 * buffer of its own and flushes it. <count> may be zero, and a hook that owns
 * nothing right now may yield nothing at all. The array is borrowed for the
 * duration of the yield call and Sylvan does not retain the pointer.
 *
 * WHY NOT A MARKING CALLBACK: a callback that marks can keep roots alive but
 * cannot say what they are, and variable reordering needs to know. A reorder
 * session pins every root it is shown so that no swap retires it, and it
 * cannot pin what it cannot enumerate, so a marking hook would lock reordering
 * out of the process entirely. A visitor does not: roots yielded here are
 * marked by garbage collection and pinned by sifting alike, through the one
 * hook.
 *
 * The hook runs with all workers stopped, once per collection and once more
 * when a reorder session opens. It must not create, protect, unprotect or
 * otherwise touch decision diagrams, and it must not itself register or remove
 * a visitor; yielding is the only thing it may do. Asked twice within one
 * stopped-world frame it must yield the same set both times.
 *
 * The values are MTBDD handles, which is to say BDD handles too. ZDD and
 * ListDD roots are not covered: use zdd_protect() and listdd_protect(), which
 * both families require anyway, since neither can be reordered.
 */
typedef void (*sylvan_roots_yield)(void *arg, const MTBDD *roots, size_t count);
typedef void (*sylvan_roots_hook)(
    void *context, sylvan_roots_yield yield, void *arg);

/**
 * Register a root visitor. <context> is handed back to the hook unchanged and
 * is never interpreted; it may be NULL. Registering the same <hook, context>
 * pair twice makes it visited twice, which is harmless and pointless.
 *
 * Returns SYLVAN_OK, SYLVAN_ERR_INVALID for a NULL hook, or SYLVAN_ERR_OOM.
 * Every registration is released by sylvan_quit(), so a consumer that
 * reinitialises Sylvan registers again.
 */
int sylvan_gc_add_roots(sylvan_roots_hook hook, void *context);

/**
 * Remove one registration of the <hook, context> pair. Returns SYLVAN_OK, or
 * SYLVAN_ERR_INVALID when that pair is not registered.
 */
int sylvan_gc_remove_roots(sylvan_roots_hook hook, void *context);

/**
 * One of the hooks for resizing behavior.
 * Default if SYLVAN_AGGRESSIVE_RESIZE is set.
 * Always double size on gc() until maximum reached.
 * Use sylvan_gc_hook_main() to set this heuristic.
 */
static inline void sylvan_gc_aggressive_resize(void);

/**
 * One of the hooks for resizing behavior.
 * Default if SYLVAN_AGGRESSIVE_RESIZE is not set.
 * Double size on gc() whenever >50% is used.
 * Use sylvan_gc_hook_main() to set this heuristic.
 */
static inline void sylvan_gc_normal_resize(void);

/**
 * The body of a garbage collection, without the frame around it.
 *
 * sylvan_gc() takes the collection flag and enters this in a new Lace frame.
 * Call it directly only from code that already runs with the world stopped
 * and wants a collection as one step of what it is doing; a reorder session
 * opening is the one such caller. Calling it directly bypasses
 * sylvan_gc_disable() on purpose: the caller is not asking for a collection
 * to be scheduled, it is performing one.
 */
TASK(void, sylvan_gc_go)

TASK(void, sylvan_gc_aggressive_resize)
TASK(void, sylvan_gc_normal_resize)
TASK(void, sylvan_table_usage, size_t*, filled, size_t*, total)
TASK(void, sylvan_gc)
TASK(void, sylvan_gc_collect)
TASK(void, sylvan_clear_cache)
TASK(void, sylvan_clear_and_mark)
TASK(void, sylvan_rehash_all)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
