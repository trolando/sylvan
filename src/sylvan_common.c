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

#include <sylvan_int.h>
#include <stdatomic.h>

static SYLVAN_NORETURN void
sylvan_default_table_full(const char *kind, size_t filled, size_t total)
{
    /* ZDD failures historically print "BDD"; preserve the default output. */
    const char *label = strcmp(kind, "ZDD") == 0 ? "BDD" : kind;
    fprintf(stderr, "%s Unique table full, %zu of %zu buckets filled!\n", label, filled, total);
    exit(1);
}

static _Atomic(sylvan_table_full_cb) table_full_handler = sylvan_default_table_full;

void
sylvan_set_table_full_handler(sylvan_table_full_cb handler)
{
    atomic_store_explicit(&table_full_handler, handler ? handler : sylvan_default_table_full,
                         memory_order_release);
}

SYLVAN_NORETURN void
sylvan_table_full(const char *kind)
{
    size_t filled = llmsset_count_marked(nodes);
    size_t total = llmsset_get_size(nodes);
    sylvan_table_full_cb handler = atomic_load_explicit(&table_full_handler, memory_order_acquire);
    handler(kind, filled, total);
    sylvan_default_table_full(kind, filled, total);
}

/**
 * Optional garbage collection diagnostics. Enable with the CMake option or
 * -DSYLVAN_GC_DIAGNOSTICS=1. No counting, clocks or output when disabled.
 */
#ifndef SYLVAN_GC_DIAGNOSTICS
#define SYLVAN_GC_DIAGNOSTICS 0
#endif

#if SYLVAN_GC_DIAGNOSTICS
#include <inttypes.h>
#if defined(_WIN32)
/* Exclude GDI: wingdi.h defines NEWFRAME, which conflicts with Lace. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#else
#include <time.h>
#endif

/* Collections are serialized by gc, including these diagnostic updates. */
static uint64_t gc_diag_count;

static double
gc_diag_now(void)
{
#if defined(_WIN32)
    LARGE_INTEGER ticks, frequency;
    if (!QueryPerformanceFrequency(&frequency) || !QueryPerformanceCounter(&ticks)) return 0.0;
    return (double)ticks.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
#else
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return 0.0;
#endif
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
#endif
}

static const char*
gc_diag_resize(size_t before, size_t after)
{
    return after > before ? "grew" : after < before ? "shrank" : "unchanged";
}
#endif

/**
 * Implementation of garbage collection
 */

/**
 * Whether garbage collection is enabled or not.
 */
static int gc_enabled = 1;

/**
 * Enable garbage collection (both automatic and manual).
 */
void
sylvan_gc_enable(void)
{
    gc_enabled = 1;
}

/**
 * Disable garbage collection (both automatic and manual).
 */
void
sylvan_gc_disable(void)
{
    gc_enabled = 0;
}

/**
 * This variable is used for a cas flag so only one gc runs at one time
 */
static _Atomic(int) gc;

/**
 * Whether the running collection was asked for with sylvan_gc(), and shrinks
 * the tables instead of calling the main hook. Set by the worker that starts
 * the collection, before its frame.
 */
static int gc_shrink;

/**
 * The minimum and maximum sizes of the nodes table and the operation cache
 */
static size_t table_min = 0, table_max = 0, cache_min = 0, cache_max = 0;

/**
 * Structures for the marking mechanisms
 */
typedef struct gc_hook_entry
{
    struct gc_hook_entry *next;
    gc_hook_cb cb;
} * gc_hook_entry_t;

static gc_hook_entry_t mark_list;
static gc_hook_entry_t pregc_list;
static gc_hook_entry_t postgc_list;
static gc_hook_cb main_hook;
static sylvan_gc_policy_t gc_policy;

void
sylvan_gc_hook_pregc(gc_hook_cb callback)
{
    gc_hook_entry_t e = (gc_hook_entry_t)malloc(sizeof(struct gc_hook_entry));
    e->cb = callback;
    e->next = pregc_list;
    pregc_list = e;
}

void
sylvan_gc_hook_postgc(gc_hook_cb callback)
{
    gc_hook_entry_t e = (gc_hook_entry_t)malloc(sizeof(struct gc_hook_entry));
    e->cb = callback;
    e->next = postgc_list;
    postgc_list = e;
}

void
sylvan_gc_add_mark(gc_hook_cb callback)
{
    gc_hook_entry_t e = (gc_hook_entry_t)malloc(sizeof(struct gc_hook_entry));
    e->cb = callback;
    e->next = mark_list;
    mark_list = e;
}

void
sylvan_gc_hook_main(gc_hook_cb callback)
{
    main_hook = callback;
}

void
sylvan_gc_set_policy(sylvan_gc_policy_t policy)
{
    switch (policy) {
    case SYLVAN_GC_FEW_COLLECTIONS:
        sylvan_gc_hook_main(sylvan_gc_aggressive_resize_CALL);
        break;
    case SYLVAN_GC_LOW_MEMORY:
        sylvan_gc_hook_main(sylvan_gc_normal_resize_CALL);
        break;
    default:
        return;
    }
    gc_policy = policy;
}

sylvan_gc_policy_t
sylvan_gc_get_policy(void)
{
    return gc_policy;
}

/**
 * Clear the operation cache.
 */
VOID_TASK_IMPL_0(sylvan_clear_cache)
{
   cache_clear();
}

/**
 * Clear the nodes table and mark all referenced nodes.
 *
 * This does not clear the hash data or rehash the nodes.
 * After marking, the "destroy" hooks are called for all unmarked nodes,
 * for example to free data of custom MTBDD leaves.
 */
VOID_TASK_IMPL_0(sylvan_clear_and_mark)
{
    llmsset_clear_data(nodes);

    for (gc_hook_entry_t e = mark_list; e != NULL; e = e->next) {
        WRAP(e->cb);
    }

    llmsset_destroy_unmarked(nodes);
}

/**
 * Clear the hash array of the nodes table and rehash all marked buckets.
 */
VOID_TASK_IMPL_0(sylvan_rehash_all)
{
    // clear hash array
    llmsset_clear_hashes(nodes);

    // rehash marked nodes
    if (llmsset_rehash(nodes) != 0) {
        fprintf(stderr, "sylvan_gc_rehash error: not all nodes could be rehashed!\n");
        exit(1);
    }
}

/**
 * Logic for resizing the nodes table and operation cache
 */

/**
 * Helper routine to compute the next size....
 */
size_t
next_size(size_t current_size)
{
#if SYLVAN_SIZE_FIBONACCI
    size_t f1=1, f2=1;
    for (;;) {
        f2 += f1;
        if (f2 > current_size) return f2;
        f1 += f2;
        if (f1 > current_size) return f1;
    }
#else
    return current_size*2;
#endif
}

/**
 * Resizing heuristic that always doubles the tables when running gc (until max).
 * The nodes table and operation cache are both resized until their maximum size.
 */
VOID_TASK_IMPL_0(sylvan_gc_aggressive_resize)
{
    size_t nodes_size = llmsset_get_size(nodes);
    size_t nodes_max = llmsset_get_max_size(nodes);
    if (nodes_size < nodes_max) {
        size_t new_size = next_size(nodes_size);
        if (new_size > nodes_max) new_size = nodes_max;
        llmsset_set_size(nodes, new_size);
    }
    size_t cache_size = cache_getsize();
    size_t cache_max = cache_getmaxsize();
    if (cache_size < cache_max) {
        size_t new_size = next_size(cache_size);
        if (new_size > cache_max) new_size = cache_max;
        cache_setsize(new_size);
    }
}

/**
 * Resizing heuristic that only resizes when more than 50% is marked.
 * The operation cache is only resized if the nodes table is resized.
 */
VOID_TASK_IMPL_0(sylvan_gc_normal_resize)
{
    size_t nodes_size = llmsset_get_size(nodes);
    size_t nodes_max = llmsset_get_max_size(nodes);
    if (nodes_size < nodes_max) {
        size_t marked = llmsset_count_marked(nodes);
        if (marked*2 > nodes_size) {
            size_t new_size = next_size(nodes_size);
            if (new_size > nodes_max) new_size = nodes_max;
            llmsset_set_size(nodes, new_size);

            // also increase the operation cache
            size_t cache_size = cache_getsize();
            size_t cache_max = cache_getmaxsize();
            if (cache_size < cache_max) {
                new_size = next_size(cache_size);
                if (new_size > cache_max) new_size = cache_max;
                cache_setsize(new_size);
            }
        }
    }
}

/**
 * Shrink the nodes table to fit the marked nodes, for a collection asked for
 * with sylvan_gc(): to the smallest size, from the initial size up, with room
 * for twice the marked nodes. The operation cache shrinks in proportion, but
 * not below its initial size. Nodes do not move. The policy decides whether
 * to reduce the data range and release its empty stretches as well.
 */
VOID_TASK_0(sylvan_gc_shrink)
{
    size_t size = llmsset_get_size(nodes);
    size_t marked = llmsset_count_marked(nodes);
    size_t target = table_min;
    while (target < size && target < 2 * marked) target = next_size(target);
    if (target > size) return;
    if (gc_policy == SYLVAN_GC_LOW_MEMORY) {
        llmsset_shrink(nodes, target);
    } else if (target < size) {
        /* Setting a smaller hash table size leaves the data range and its
           pages intact, so future allocations can reuse them without faults. */
        llmsset_set_size(nodes, target);
    }

    size_t cache_size = cache_getsize();
    size_t cache_target = cache_size;
    for (size_t s = size; s > target && cache_target / 2 >= cache_min; s /= 2) cache_target /= 2;
    if (cache_target < cache_size) cache_setsize(cache_target);
}

/**
 * Actual implementation of garbage collection
 */
VOID_TASK_0(sylvan_gc_go)
{
    sylvan_stats_count(SYLVAN_GC_COUNT);
    sylvan_timer_start(SYLVAN_GC);

    // call pre gc hooks
    for (gc_hook_entry_t e = pregc_list; e != NULL; e = e->next) {
        WRAP(e->cb);
    }

    /*
     * This simply clears the cache.
     * Alternatively, we could implement for example some strategy
     * where part of the cache is cleared and part is marked
     */
    CALL(sylvan_clear_cache);

#if SYLVAN_GC_DIAGNOSTICS
    const double diag_start = gc_diag_now();
    const size_t diag_used = CALL(llmsset_count_marked, nodes);
    const size_t diag_table_before = llmsset_get_size(nodes);
    const size_t diag_data_before = llmsset_get_data_size(nodes);
    const size_t diag_cache_before = cache_getsize();
#endif

    CALL(sylvan_clear_and_mark);

#if SYLVAN_GC_DIAGNOSTICS
    const size_t diag_live = CALL(llmsset_count_marked, nodes);
#endif

    // resize: shrink to fit for sylvan_gc(), else the main hook (by default, grow)
    if (gc_shrink) CALL(sylvan_gc_shrink);
    else WRAP(main_hook);

    CALL(sylvan_rehash_all);

#if SYLVAN_GC_DIAGNOSTICS
    {
        const size_t diag_table_after = llmsset_get_size(nodes);
        const size_t diag_cache_after = cache_getsize();
        fprintf(stderr,
                "[sylvan-gc] #%" PRIu64 " trigger=%s seconds=%.6f "
                "used=%zu live=%zu freed=%zu (%.1f%% live) "
                "table=%zu->%zu/%zu (%s) data=%zu->%zu "
                "cache=%zu->%zu/%zu (%s)\n",
                ++gc_diag_count, gc_shrink ? "sylvan_gc" : "sylvan_gc_collect",
                gc_diag_now() - diag_start, diag_used, diag_live, diag_used - diag_live,
                100.0 * (double)diag_live / (double)diag_table_before,
                diag_table_before, diag_table_after, llmsset_get_max_size(nodes),
                gc_diag_resize(diag_table_before, diag_table_after),
                diag_data_before, llmsset_get_data_size(nodes),
                diag_cache_before, diag_cache_after, cache_getmaxsize(),
                gc_diag_resize(diag_cache_before, diag_cache_after));
        fflush(stderr);
    }
#endif

    // call post gc hooks
    for (gc_hook_entry_t e = postgc_list; e != NULL; e = e->next) {
        WRAP(e->cb);
    }

    sylvan_timer_stop(SYLVAN_GC);
}

/**
 * Perform garbage collection, or wait for the one another worker runs.
 * Only the worker that starts the collection decides whether it shrinks.
 */
VOID_TASK_1(sylvan_gc_start, int, shrink)
{
    if (gc_enabled) {
        int zero = 0;
        if (atomic_compare_exchange_strong(&gc, &zero, 1)) {
            gc_shrink = shrink;
            NEWFRAME(sylvan_gc_go);
            gc_shrink = 0;
            gc = 0;
        } else {
            /* Another worker is collecting garbage. Wait until it is done,
             * and join its new frame if that has not started yet. Lace
             * announces the frame only until all workers have joined it, so
             * a worker that arrives after the frame has run must not wait
             * for it to appear. */
            while (atomic_load_explicit(&gc, memory_order_acquire) != 0) YIELD_NEWFRAME();
        }
    }
}

VOID_TASK_IMPL_0(sylvan_gc)
{
    CALL(sylvan_gc_start, 1);
}

VOID_TASK_IMPL_0(sylvan_gc_collect)
{
    CALL(sylvan_gc_start, 0);
}

/**
 * The unique table
 */

llmsset_t nodes;

static int
is_power_of_two(size_t size)
{
    return (size != 0) && ((size & (size-1)) == 0);
}

void
sylvan_set_sizes(size_t min_tablesize, size_t max_tablesize, size_t min_cachesize, size_t max_cachesize)
{
    /* Some sanity checks */
    if (min_tablesize > max_tablesize) min_tablesize = max_tablesize;
    if (min_cachesize > max_cachesize) min_cachesize = max_cachesize;

    if (!is_power_of_two(min_tablesize) || !is_power_of_two(max_tablesize) ||
        !is_power_of_two(min_cachesize) || !is_power_of_two(max_cachesize)) {
        fprintf(stderr, "sylvan_set_sizes error: parameters not powers of 2!\n");
        exit(1);
    }

    if (max_tablesize > UINT64_C(0x0000010000000000)) {
        fprintf(stderr, "sylvan_set_sizes error: tablesize must be <= 40 bits!\n");
        exit(1);
    }

    table_min = min_tablesize;
    table_max = max_tablesize;
    cache_min = min_cachesize;
    cache_max = max_cachesize;
}

void
sylvan_set_limits(size_t memorycap, int table_ratio, int initial_ratio)
{
    /* This method ONLY limits the "unique nodes table" and the "operation cache" which are the main
     * datastructures of Sylvan.
     *
     * Suggested limits:
     * - memorycap to whatever you want at most, in bytes.
     * - table_ratio to a number between -3 (8x bigger cache) and 3 (8x bigger nodes table)
     * - inital_ratio to something like 10 (start with 1024x smaller tables initially)
     */
    if (table_ratio < -10 || table_ratio > 10) {
        fprintf(stderr, "sylvan_set_limits: table_ratio unreasonable (between -10 and 10)\n");
        exit(1);
    }

    size_t max_t = 1;
    size_t max_c = 1;
    if (table_ratio > 0) {
        max_t <<= table_ratio;
    } else {
        max_c <<= -table_ratio;
    }

    size_t cur = max_t * 24 + max_c * 36;
    if (cur > memorycap) {
        fprintf(stderr, "sylvan_set_limits: memory cap incompatible with requested table ratio\n");
    }

    while (2*cur < memorycap && max_t < UINT64_C(0x0000010000000000)) {
        max_t *= 2;
        max_c *= 2;
        cur *= 2;
    }

    if (initial_ratio < 0) {
        fprintf(stderr, "sylvan_set_limits: initial_ratio unreasonable (may not be negative)\n");
        exit(1);
    }

    size_t min_t = max_t, min_c = max_c;
    while (initial_ratio > 0 && min_t > 0x1000 && min_c > 0x1000) {
        min_t >>= 1;
        min_c >>= 1;
        initial_ratio--;
    }

    table_min = min_t;
    table_max = max_t;
    cache_min = min_c;
    cache_max = max_c;
}

/**
 * Initializes Sylvan.
 */
void
sylvan_init_package(void)
{
    if (table_max == 0) {
        fprintf(stderr, "sylvan_init_package error: table sizes not set (sylvan_set_sizes or sylvan_set_limits)!");
        exit(1);
    }

    /* Create tables */
    nodes = llmsset_create(table_min, table_max);
    cache_create(cache_min, cache_max);

    /* Initialize garbage collection */
    gc = 0;
#if SYLVAN_GC_DIAGNOSTICS
    gc_diag_count = 0;
#endif
#if SYLVAN_AGGRESSIVE_RESIZE
    sylvan_gc_set_policy(SYLVAN_GC_FEW_COLLECTIONS);
#else
    sylvan_gc_set_policy(SYLVAN_GC_LOW_MEMORY);
#endif

    sylvan_stats_init();
}

struct reg_quit_entry
{
    struct reg_quit_entry *next;
    quit_cb cb;
};

static struct reg_quit_entry *quit_register = NULL;

void
sylvan_register_quit(quit_cb cb)
{
    struct reg_quit_entry *e = (struct reg_quit_entry*)malloc(sizeof(struct reg_quit_entry));
    e->next = quit_register;
    e->cb = cb;
    quit_register = e;
}

void
sylvan_quit(void)
{
    while (quit_register != NULL) {
        struct reg_quit_entry *e = quit_register;
        quit_register = e->next;
        e->cb();
        free(e);
    }

    while (pregc_list != NULL) {
        gc_hook_entry_t e = pregc_list;
        pregc_list = e->next;
        free(e);
    }

    while (postgc_list != NULL) {
        gc_hook_entry_t e = postgc_list;
        postgc_list = e->next;
        free(e);
    }

    while (mark_list != NULL) {
        gc_hook_entry_t e = mark_list;
        mark_list = e->next;
        free(e);
    }

    cache_free();
    nodes_scratch_forget();
    llmsset_free(nodes);
}

/**
 * Calculate table usage (in parallel)
 */
VOID_TASK_IMPL_2(sylvan_table_usage, size_t*, filled, size_t*, total)
{
    size_t tot = llmsset_get_size(nodes);
    if (filled != NULL) *filled = llmsset_count_marked(nodes);
    if (total != NULL) *total = tot;
}


