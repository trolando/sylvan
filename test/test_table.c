#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "sylvan.h"
#include "sylvan_cache.h"

#include <lace.h>

#include "sylvan_platform.h"
#include "sylvan_table.h"
#include "test_assert.h"

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

    lace_stop();

    return 0;
}
