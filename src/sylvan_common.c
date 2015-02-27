/*
 * Copyright 2011-2015 Formal Methods and Tools, University of Twente
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

#include <sylvan_config.h>

#include <barrier.h>
#include <sylvan_common.h>

#if USE_NUMA
#include <numa.h>
#endif

/**
 * Static global variables
 */

int workers;
llmsset_t nodes;

/**
 * Variables for operation cache
 */

size_t             cache_size;         // power of 2
size_t             cache_max;          // power of 2
#if CACHE_MASK
size_t             cache_mask;         // cache_size-1
#endif
cache_entry_t      cache_table;
uint32_t*          cache_status;

/**
 * Retrieve nodes
 */

llmsset_t
__sylvan_get_internal_data()
{
    return nodes;
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

/**
 * Implementation of garbage collection
 */
static int gc_enabled = 1;
static barrier_t gcbar; // gc in progress
static volatile int gc; // barrier

struct reg_gc_mark_entry
{
    struct reg_gc_mark_entry *next;
    gc_mark_cb cb;
};

static struct reg_gc_mark_entry *gc_mark_register = NULL;

void
sylvan_gc_add_mark(gc_mark_cb cb)
{
    struct reg_gc_mark_entry *e = (struct reg_gc_mark_entry*)malloc(sizeof(struct reg_gc_mark_entry));
    e->next = gc_mark_register;
    e->cb = cb;
    gc_mark_register = e;
}

static gc_hook_cb gc_hook;

void
sylvan_gc_set_hook(gc_hook_cb new_hook)
{
    gc_hook = new_hook;
}

void
sylvan_gc_enable()
{
    gc_enabled = 1;
}

void
sylvan_gc_disable()
{
    gc_enabled = 0;
}

/* Default hook */
VOID_TASK_0(sylvan_gc_default_hook)
{
    /**
     * Default behavior:
     * if we can resize the nodes set, and if we use more than 50%, then increase size
     */
    size_t max_size = llmsset_get_max_size(nodes);
    size_t size = llmsset_get_size(nodes);
    if (size < max_size) {
        size_t marked = llmsset_count_marked(nodes);
        if (marked*2 > size) {
            llmsset_set_size(nodes, size*2 < max_size ? size*2 : max_size);
            if (cache_size < cache_max) {
                // current design: just increase cache_size
                if (cache_size*2 < cache_max) cache_size *= 2;
                else cache_size = cache_max;
            }
        }
    }
}

VOID_TASK_0(sylvan_gc_go)
{
    // clear cache
    cache_clear();

    // clear hash array
    llmsset_clear(nodes);

    // call mark functions
    struct reg_gc_mark_entry *e = gc_mark_register;
    while (e != NULL) {
        WRAP(e->cb);
        e = e->next;
    }

    // call hook function (resizing, reordering, etc)
    WRAP(gc_hook);

    // rehash marked nodes
    llmsset_rehash(nodes);
}

/* Perform garbage collection */
VOID_TASK_IMPL_0(sylvan_gc)
{
    if (!gc_enabled) return;
    if (cas(&gc, 0, 1)) {
        NEWFRAME(sylvan_gc_go);
        gc = 0;
    } else {
        /* wait for new frame to appear */
        while (*(volatile Task**)&lace_newframe.t == 0) {}
        YIELD_NEWFRAME();
    }
}

/**
 * Package init and quit functions
 */
void
sylvan_init_package(size_t tablesize, size_t maxsize, size_t cachesize, size_t max_cachesize)
{
    workers = lace_workers();

#if USE_NUMA
    if (numa_available() != -1) {
        numa_set_interleave_mask(numa_all_nodes_ptr);
    }
#endif

    if (tablesize > maxsize) tablesize = maxsize;
    if (cachesize > max_cachesize) cachesize = max_cachesize;

    if (maxsize > 0x000003ffffffffff) {
        fprintf(stderr, "sylvan_init_package error: tablesize must be <= 42 bits!\n");
        exit(1);
    }

    nodes = llmsset_create(tablesize, maxsize);
    cache_create(cachesize, max_cachesize);

    gc = 0;
    barrier_init(&gcbar, lace_workers());
    gc_hook = TASK(sylvan_gc_default_hook);
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
sylvan_quit()
{
    while (quit_register != NULL) {
        struct reg_quit_entry *e = quit_register;
        quit_register = e->next;
        e->cb();
        free(e);
    }

    while (gc_mark_register != NULL) {
        struct reg_gc_mark_entry *e = gc_mark_register;
        gc_mark_register = e->next;
        free(e);
    }

    cache_free();
    llmsset_free(nodes);
    barrier_destroy(&gcbar);
}
