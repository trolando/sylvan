/**
 * Exercise GC policies with exact live counts, including the half-full boundary.
 * Raw table entries and a marking hook keep occupancy independent of DD sharing.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sylvan.h"
#include "sylvan_int.h"
#include "test_assert.h"

#if SYLVAN_USE_MMAP && (defined(__linux__) || defined(__APPLE__))
#include <sys/mman.h>
#include <unistd.h>
#endif

#define MIN_SIZE ((size_t)1 << 12)
#define MAX_SIZE ((size_t)1 << 16)
#define CHUNK ((size_t)65536)

struct root {
    uint64_t index, key;
};

static struct root roots[MAX_SIZE];
static size_t root_count;
static uint64_t next_key;
static unsigned hook_calls;

VOID_TASK_0(mark_roots)
{
    for (size_t i=0; i<root_count; i++) llmsset_mark(nodes, roots[i].index);
}

VOID_TASK_0(custom_main)
{
    hook_calls++;
}

static int
init_package(void)
{
    root_count = 0;
    next_key = 1;
    hook_calls = 0;
    sylvan_set_sizes(MIN_SIZE, MAX_SIZE, MIN_SIZE / 4, MAX_SIZE / 4);
    sylvan_init_package();
    sylvan_gc_add_mark(mark_roots_CALL);
#if SYLVAN_AGGRESSIVE_RESIZE
    test_assert(sylvan_gc_get_policy() == SYLVAN_GC_FEW_COLLECTIONS);
    sylvan_gc_collect();
    test_assert(llmsset_get_size(nodes) == 2 * MIN_SIZE);
#else
    test_assert(sylvan_gc_get_policy() == SYLVAN_GC_LOW_MEMORY);
    sylvan_gc_collect();
    test_assert(llmsset_get_size(nodes) == MIN_SIZE);
#endif
    sylvan_gc();
    test_assert(llmsset_get_size(nodes) == MIN_SIZE);
    return 0;
}

static int
add_nodes(size_t count, int keep)
{
    for (size_t i=0; i<count; i++) {
        uint64_t key = next_key++;
        int created;
        uint64_t index = llmsset_lookup(nodes, key, key + 1, &created);
        test_assert(index > 1 && created);
        if (keep) {
            test_assert(root_count < MAX_SIZE);
            roots[root_count++] = (struct root){index, key};
        }
    }
    return 0;
}

static int
check_tables(size_t size)
{
    test_assert(llmsset_get_size(nodes) == size);
    test_assert(cache_getsize() == size / 4);
    /* The two reserved node indices are counted by the resizing hook too. */
    test_assert(llmsset_count_marked(nodes) == root_count + 2);
    for (size_t i=0; i<root_count; i++) {
        int created;
        test_assert(roots[i].index < llmsset_get_data_size(nodes));
        test_assert(llmsset_is_marked(nodes, roots[i].index));
        const uint64_t *data = llmsset_index_to_ptr(nodes, roots[i].index);
        test_assert(data[0] == roots[i].key && data[1] == roots[i].key + 1);
        test_assert(llmsset_lookup(nodes, roots[i].key, roots[i].key + 1, &created) == roots[i].index);
        test_assert(!created);
    }
    return 0;
}

TASK_1(int, test_growth, sylvan_gc_policy_t, policy)
{
    sylvan_gc_set_policy(policy);
    test_assert(sylvan_gc_get_policy() == policy);
    test_assert(add_nodes(16, 1) == 0);
    for (size_t size=MIN_SIZE; size<MAX_SIZE; size*=2) {
        if (policy == SYLVAN_GC_LOW_MEMORY) {
            /* Repeatedly collect garbage with less than half the table live. */
            for (int pass=0; pass<3; pass++) {
                test_assert(add_nodes(64, 0) == 0);
                CALL(sylvan_gc_collect);
                test_assert(check_tables(size) == 0);
            }
            test_assert(add_nodes(size / 2 - 2 - root_count, 1) == 0);
            CALL(sylvan_gc_collect);
            test_assert(check_tables(size) == 0); // exactly half is not enough
            test_assert(add_nodes(1, 1) == 0);
        } else {
            test_assert(add_nodes(64, 0) == 0);
        }
        CALL(sylvan_gc_collect);
        test_assert(check_tables(2 * size) == 0);
    }
    if (policy == SYLVAN_GC_LOW_MEMORY) {
        /* Above half occupancy, growth must still stop at the maximum. */
        test_assert(add_nodes(MAX_SIZE / 2 + 1 - root_count, 1) == 0);
        test_assert(root_count > MAX_SIZE / 2);
    }
    for (int pass=0; pass<3; pass++) {
        test_assert(add_nodes(64, 0) == 0);
        CALL(sylvan_gc_collect);
        test_assert(check_tables(MAX_SIZE) == 0);
        test_assert(llmsset_get_data_size(nodes) == MAX_SIZE);
    }
    return 0;
}

static void
report_residency(void *memory, size_t bytes)
{
#if SYLVAN_USE_MMAP && (defined(__linux__) || defined(__APPLE__))
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0 || bytes % (size_t)page_size != 0) {
        fprintf(stderr, "GC policy residency diagnostic: unsupported page size\n");
        return;
    }
    size_t pages = bytes / (size_t)page_size;
    unsigned char *resident = malloc(pages);
    /* Retained pages may be paged out, and clearing may fall back to memset.
       Residency is diagnostic only; even mincore failure is harmless.
       mincore takes unsigned char* on Linux and char* on macOS. */
    if (resident != NULL && mincore(memory, bytes, (void*)resident) == 0) {
        size_t count = 0;
        for (size_t i=0; i<pages; i++) count += resident[i] & 1;
        fprintf(stderr, "GC policy residency diagnostic: %zu/%zu pages resident\n", count, pages);
    } else {
        fprintf(stderr, "GC policy residency diagnostic: unavailable\n");
    }
    free(resident);
#else
    (void)memory; (void)bytes;
#endif
}

TASK_2(int, test_shrink, sylvan_gc_policy_t, policy, int, custom_hook)
{
    sylvan_gc_set_policy(SYLVAN_GC_FEW_COLLECTIONS);
    while (llmsset_get_size(nodes) < MAX_SIZE) CALL(sylvan_gc_collect);
    const size_t data_size = llmsset_get_data_size(nodes);
    test_assert(data_size == MAX_SIZE);

    /* Touch the whole arena, then retain just one low and one high node.
       This leaves both an interior hole and an unused tail to release. */
    memset(nodes->data, 0xa5, data_size * 16);
    test_assert(add_nodes(MAX_SIZE / 2 + 128, 1) == 0);
    struct root low = {0, 0}, high = {0, 0};
    for (size_t i=0; i<root_count; i++) {
        if (roots[i].index < MIN_SIZE) low = roots[i];
        if (roots[i].index >= MAX_SIZE / 2 && roots[i].index < 3 * MAX_SIZE / 4) high = roots[i];
    }
    test_assert(low.index > 1 && high.index >= MAX_SIZE / 2);
    struct root dead = roots[0];
    test_assert(dead.index != low.index && dead.index < high.index);
    test_assert(llmsset_is_marked(nodes, dead.index));
    roots[0] = low;
    roots[1] = high;
    root_count = 2;

    uintptr_t address = ((uintptr_t)nodes->data + 2 * CHUNK - 1) & ~(uintptr_t)(CHUNK - 1);
    unsigned char *hole = (unsigned char*)address;
    unsigned char *copy = malloc(CHUNK);
    test_assert(copy != NULL);
    memcpy(copy, hole, CHUNK);
    report_residency(hole, CHUNK);

    sylvan_gc_set_policy(policy);
    if (custom_hook) {
        sylvan_gc_hook_main(custom_main_CALL);
        CALL(sylvan_gc_collect);
        test_assert(hook_calls == 1);
        test_assert(check_tables(MAX_SIZE) == 0);
    }
    CALL(sylvan_gc);
    test_assert(check_tables(MIN_SIZE) == 0);
    test_assert(hook_calls == (unsigned)custom_hook);
    test_assert(sylvan_gc_get_policy() == policy);
    test_assert(high.index >= llmsset_get_size(nodes));

    size_t reduced_data = (high.index + 512) & ~(size_t)511;
    if (policy == SYLVAN_GC_FEW_COLLECTIONS) {
        test_assert(llmsset_get_data_size(nodes) == data_size);
        report_residency(hole, CHUNK);
        test_assert(memcmp(hole, copy, CHUNK) == 0);

        /* GC resets the worker's region, so its first dead bucket is reused.
           Verify retained contents and allocation bits before and after reuse. */
        test_assert(!llmsset_is_marked(nodes, dead.index));
        const uint64_t *data = llmsset_index_to_ptr(nodes, dead.index);
        test_assert(data[0] == dead.key && data[1] == dead.key + 1);
        uint64_t key = next_key++;
        int created;
        uint64_t index = llmsset_lookup(nodes, key, key + 1, &created);
        test_assert(index == dead.index && created);
        test_assert(llmsset_is_marked(nodes, index));
        test_assert(data[0] == key && data[1] == key + 1);
        roots[root_count++] = (struct root){index, key};
        test_assert(check_tables(MIN_SIZE) == 0);
    } else {
        test_assert(llmsset_get_data_size(nodes) == reduced_data);
        report_residency(hole, CHUNK);
#if SYLVAN_USE_MMAP
        for (size_t i=0; i<CHUNK; i++) test_assert(hole[i] == 0);
#endif
    }
    free(copy);

    if (custom_hook) {
        CALL(sylvan_gc_collect);
        test_assert(hook_calls == 2);
        test_assert(check_tables(MIN_SIZE) == 0);
        test_assert(sylvan_gc_get_policy() == policy);
    }

    /* Switching to low memory must release the arena even if a previous
       few-collections GC already shrank the hash table to its minimum. */
    sylvan_gc_set_policy(SYLVAN_GC_LOW_MEMORY);
    CALL(sylvan_gc);
    test_assert(check_tables(MIN_SIZE) == 0);
    test_assert(llmsset_get_data_size(nodes) == reduced_data);
    report_residency(hole, CHUNK);

    /* A later collection can release the high node without reducing the hash. */
    root_count = 1;
    CALL(sylvan_gc);
    test_assert(check_tables(MIN_SIZE) == 0);
    test_assert(llmsset_get_data_size(nodes) == MIN_SIZE);
    return 0;
}

TASK_0(int, test_switch)
{
    sylvan_gc_set_policy(SYLVAN_GC_FEW_COLLECTIONS);
    CALL(sylvan_gc_collect);
    test_assert(check_tables(2 * MIN_SIZE) == 0);
    sylvan_gc_set_policy(SYLVAN_GC_LOW_MEMORY);
    CALL(sylvan_gc_collect);
    test_assert(check_tables(2 * MIN_SIZE) == 0);
    sylvan_gc_hook_main(custom_main_CALL);
    CALL(sylvan_gc_collect);
    test_assert(hook_calls == 1);
    test_assert(sylvan_gc_get_policy() == SYLVAN_GC_LOW_MEMORY);

    /* Selecting a policy replaces the custom hook, even for the same policy. */
    sylvan_gc_set_policy(SYLVAN_GC_LOW_MEMORY);
    CALL(sylvan_gc_collect);
    test_assert(hook_calls == 1);
    sylvan_gc_set_policy(SYLVAN_GC_FEW_COLLECTIONS);
    CALL(sylvan_gc_collect);
    test_assert(check_tables(4 * MIN_SIZE) == 0);
    test_assert(hook_calls == 1);
    sylvan_gc_set_policy((sylvan_gc_policy_t)123);
    test_assert(sylvan_gc_get_policy() == SYLVAN_GC_FEW_COLLECTIONS);
    CALL(sylvan_gc_collect);
    test_assert(check_tables(8 * MIN_SIZE) == 0);
    return 0;
}

int
main(int argc, char **argv)
{
    lace_start(argc > 1 ? atoi(argv[1]) : 1, 0);
    const sylvan_gc_policy_t policies[] = {SYLVAN_GC_FEW_COLLECTIONS, SYLVAN_GC_LOW_MEMORY};
    for (size_t i=0; i<sizeof(policies)/sizeof(policies[0]); i++) {
        test_assert(init_package() == 0);
        test_assert(RUN(test_growth, policies[i]) == 0);
        sylvan_quit();
        for (int custom_hook=0; custom_hook<2; custom_hook++) {
            test_assert(init_package() == 0);
            test_assert(RUN(test_shrink, policies[i], custom_hook) == 0);
            sylvan_quit();
        }
    }
    test_assert(init_package() == 0);
    test_assert(RUN(test_switch) == 0);
    sylvan_quit();
    lace_stop();
    return 0;
}
