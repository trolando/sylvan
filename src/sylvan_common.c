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

TASK_2(size_t, sylvan_table_usage_par, size_t, start, size_t, end)
{
    if (end - start <= 128) {
        return llmsset_get_filled_partial(nodes, start, end);
    } else {
        size_t part = (end-start)/2;
        if (part < 128) part = 128;
        SPAWN(sylvan_table_usage_par, start, start+part);
        size_t end2 = start+2*part;
        if (end2 > end) end2 = end;
        size_t res = CALL(sylvan_table_usage_par, start+part, end2);;
        res += SYNC(sylvan_table_usage_par);
        return res;
    }
}

VOID_TASK_IMPL_2(sylvan_table_usage, size_t*, filled, size_t*, total)
{
    size_t tot = llmsset_get_size(nodes);
    if (filled != NULL) *filled = CALL(sylvan_table_usage_par, 0, tot);
    if (total != NULL) *total = tot;
}

/**
 * Thread-local insert index for LLMSset
 */
DECLARE_THREAD_LOCAL(insert_index, uint64_t*);

uint64_t*
initialize_insert_index()
{
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    insert_index = (uint64_t*)malloc(LINE_SIZE);
    LACE_ME;
    size_t my_id = LACE_WORKER_ID;
    *insert_index = llmsset_get_insertindex_multi(nodes, my_id, workers);
    SET_THREAD_LOCAL(insert_index, insert_index);
    return insert_index;
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

VOID_TASK_0(sylvan_gc_clear_llmsset)
{
    llmsset_clear_multi(nodes, LACE_WORKER_ID, workers);
}

VOID_TASK_0(sylvan_gc_rehash)
{
    LOCALIZE_THREAD_LOCAL(insert_index, uint64_t*);
    if (insert_index == NULL) insert_index = initialize_insert_index();
    *insert_index = llmsset_get_insertindex_multi(nodes, LACE_WORKER_ID, workers);

    llmsset_rehash_multi(nodes, LACE_WORKER_ID, workers);
}

VOID_TASK_0(sylvan_gc_go)
{
    // clear cache
    cache_clear();

    // clear hash array (parallel)
    TOGETHER(sylvan_gc_clear_llmsset);

    // call mark functions
    struct reg_gc_mark_entry *e = gc_mark_register;
    while (e != NULL) {
        WRAP(e->cb);
        e = e->next;
    }

    // maybe resize
    if (!llmsset_is_maxsize(nodes)) {
        size_t filled, total;
        sylvan_table_usage(&filled, &total);
        if (filled > total/2) {
            llmsset_sizeup(nodes);
        }
    }

    // rehash
    TOGETHER(sylvan_gc_rehash);
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
sylvan_init_package(size_t tablesize, size_t maxsize, size_t cachesize)
{
    workers = lace_workers();

    INIT_THREAD_LOCAL(insert_index);

#if USE_NUMA
    if (numa_available() != -1) {
        numa_set_interleave_mask(numa_all_nodes_ptr);
    }
#endif

    if (tablesize > 40 || maxsize > 40) {
        fprintf(stderr, "sylvan_init error: tablesize must be <= 40!\n");
        exit(1);
    }

    if (cachesize > 40) {
        fprintf(stderr, "sylvan_init error: cachesize must be <= 40!\n");
        exit(1);
    }

    nodes = llmsset_create(1LL<<tablesize, 1LL<<maxsize);
    cache_create(1LL<<cachesize, 1LL<<cachesize);

    gc = 0;
    barrier_init(&gcbar, lace_workers());

    // Another sanity check
    llmsset_test_multi(nodes, workers);
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
