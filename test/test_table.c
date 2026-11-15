#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sylvan.h"
#include "sylvan_cache.h"

#include <lace.h>

#include "sylvan_platform.h"
#include "sylvan_table.h"
#include "test_assert.h"

static uint64_t
collision_hash(uint64_t a, uint64_t b, uint64_t seed)
{
    (void)a; (void)b; (void)seed;
    return 0;
}

static int
same_key(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    return a == c && b == d;
}

static void
create_key(uint64_t *a, uint64_t *b)
{
    (void)a; (void)b;
}

static void
destroy_key(uint64_t a, uint64_t b)
{
    (void)a; (void)b;
}

TASK_0(int, test_rehash_shrink)
{
    llmsset_t table = llmsset_create(16384, 16384);
    llmsset_set_custom(table, collision_hash, same_key, create_key, destroy_key);
    /* 92 cache lines fit the initial probe limit (94), but exceed the limit
       after shrinking to 4096 buckets (90). Rebuild must keep them findable. */
    const size_t count = 92 * (SYLVAN_CACHE_LINE_SIZE / sizeof(uint64_t));
    uint64_t *indices = malloc(count * sizeof(uint64_t));
    test_assert(indices != NULL);
    for (size_t i=0; i<count; i++) {
        int created;
        indices[i] = llmsset_lookupc(table, i+1, 0, &created);
        test_assert(indices[i] > 1 && created);
    }
    llmsset_clear(table);
    for (size_t i=0; i<count; i++) llmsset_mark(table, indices[i]);
    llmsset_shrink(table, 4096);
    test_assert(llmsset_rehash(table) == 0);
    for (size_t i=0; i<count; i++) {
        int created;
        test_assert(llmsset_lookupc(table, i+1, 0, &created) == indices[i]);
        test_assert(!created);
    }
    free(indices);
    llmsset_free(table);
    return 0;
}

static _Atomic(unsigned) arrived, destroyed;

static void
create_together(uint64_t *a, uint64_t *b)
{
    (void)a; (void)b;
    /* Every worker must claim a bucket before any can publish the same key. */
    atomic_fetch_add_explicit(&arrived, 1, memory_order_relaxed);
    while (atomic_load_explicit(&arrived, memory_order_relaxed) != lace_workers()) {}
}

static void
destroy_loser(uint64_t a, uint64_t b)
{
    (void)a; (void)b;
    atomic_fetch_add_explicit(&destroyed, 1, memory_order_relaxed);
}

#define OWNED_KEYS 2048

static int
insert_owned(llmsset_t table, uint64_t *indices)
{
    const size_t worker = lace_get_worker()->worker;
    uint64_t *mine = indices + worker * (OWNED_KEYS + 1);
    for (size_t i=0; i<OWNED_KEYS; i++) {
        int created;
        if (i == 1023) {
            mine[OWNED_KEYS] = llmsset_lookupc(table, 0, 2, &created);
            test_assert(mine[OWNED_KEYS] > 1);
        }
        mine[i] = llmsset_lookup(table, worker * OWNED_KEYS + i + 1, 1, &created);
        test_assert(mine[i] > 1 && created);
    }
    return 0;
}

VOID_TASK_3(insert_owned_together, llmsset_t, table, uint64_t *, indices, int *, errors)
{
    errors[lace_get_worker()->worker] = insert_owned(table, indices);
}

TASK_0(int, test_owned_regions)
{
    const size_t workers = lace_workers();
    uint64_t *indices = malloc(workers * (OWNED_KEYS + 1) * sizeof(uint64_t));
    int *errors = calloc(workers, sizeof(int));
    test_assert(indices != NULL && errors != NULL);
    llmsset_t table = llmsset_create(65536, 65536);
    llmsset_set_custom(table, collision_hash, same_key, create_together, destroy_loser);
    TOGETHER(insert_owned_together, table, indices, errors);
    test_assert(atomic_load_explicit(&destroyed, memory_order_relaxed) == workers - 1);
    test_assert(llmsset_count_marked(table) == workers * OWNED_KEYS + 3);
    for (size_t w=0; w<workers; w++) {
        test_assert(!errors[w]);
        uint64_t *mine = indices + w * (OWNED_KEYS + 1);
        test_assert(mine[OWNED_KEYS] == indices[OWNED_KEYS]);
        for (size_t i=0; i<OWNED_KEYS; i++) {
            int created;
            test_assert(mine[i] < llmsset_get_data_size(table));
            test_assert(llmsset_is_marked(table, mine[i]));
            test_assert(llmsset_lookup(table, w * OWNED_KEYS + i + 1, 1, &created) == mine[i]);
            test_assert(!created);
        }
    }
    llmsset_free(table);
    free(errors);
    free(indices);
    return 0;
}

static int
test_cache_pair(void)
{
    /* These keys collide in the old status fingerprint. The second half of
     * a six-key entry must never be returned as a separate three-key entry. */
    cache_create(2, 2);
    uint64_t first = 0, second = 0;
    int inserted = 0;
    for (int i = 0; i < 1000 && !inserted; i++) {
        inserted = cache_put6(1, 2, 3, 105662, 5, 6, 123, 456);
    }
    test_assert(inserted);
    test_assert(cache_get6(1, 2, 3, 105662, 5, 6, &first, &second));
    test_assert(first == 123 && second == 456);
    test_assert(!cache_get(105662, 5, 6, &second));
    inserted = 0;
    for (int i = 0; i < 1000 && !inserted; i++) {
        inserted = cache_put(105662, 5, 6, 789);
    }
    test_assert(inserted);
    test_assert(cache_get(105662, 5, 6, &second) && second == 789);
    test_assert(!cache_get6(1, 2, 3, 105662, 5, 6, &first, &second));
    cache_free();
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "maxsize") == 0) {
        /* The maximum valid capacity is accepted without allocating it. */
        sylvan_set_sizes(4096, UINT64_C(1)<<40, 4096, 4096);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "oversized") == 0) {
        /* CTest expects rejection of a capacity that cannot fit in handles. */
        sylvan_set_sizes(4096, UINT64_C(1)<<41, 4096, 4096);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "cache") == 0) return test_cache_pair();
    if (argc > 1 && strcmp(argv[1], "ownership") == 0) {
        lace_start(8, 0);
        int result = RUN(test_owned_regions);
        lace_stop();
        return result;
    }
    lace_start(1, 0);

    uint64_t *memory = sylvan_alloc_aligned(4096);
    test_assert(memory != NULL);
    for (size_t i=0; i<512; i++) memory[i] = UINT64_MAX;
    sylvan_clear_aligned(memory, 4096);
    for (size_t i=0; i<512; i++) test_assert(memory[i] == 0);
    sylvan_free_aligned(memory, 4096);

    llmsset_t table = llmsset_create(4096, 4096);

    test_assert(llmsset_mark(table, 74));
    test_assert(llmsset_is_marked(table, 74));
    test_assert(!llmsset_is_marked(table, 73));
    test_assert(!llmsset_is_marked(table, 75));
    test_assert(llmsset_mark(table, 75));

    /* Reserved buckets are marked to prevent allocation, but must never
     * be inserted into the hash table by rehash. */
    test_assert(llmsset_rehash(table) == 0);
    for (size_t i = 0; i < llmsset_get_size(table); i++) {
        uint64_t entry = atomic_load_explicit(&table->table[i], memory_order_relaxed);
        test_assert(entry == 0 || (entry & UINT64_C(0xffffffffff)) > 1);
    }
    llmsset_free(table);

    int result = RUN(test_rehash_shrink);
    lace_stop();

    return result;
}
