#include <stdio.h>
#include <stdint.h>

#include <lace.h>

#include <sylvan/platform.h>
#include <sylvan/nodes.h>
#include "test_assert.h"

int
main(void)
{
    lace_start(1, 0, 0);

    uint64_t *memory = sylvan_alloc_aligned(4096);
    test_assert(memory != NULL);
    for (size_t i=0; i<512; i++) memory[i] = UINT64_MAX;
    sylvan_clear_aligned(memory, 4096);
    for (size_t i=0; i<512; i++) test_assert(memory[i] == 0);
    sylvan_free_aligned(memory, 4096);

    nodes_table *table = nodes_create(4096, 4096);
    int created = 0;
    uint64_t first = nodes_lookup(table, UINT64_C(0x400000000000004a), 0, &created);
    test_assert(first > 1 && created);
    uint64_t second = nodes_lookup(table, UINT64_C(0x400000000000004b), 0, &created);
    test_assert(second > 1 && second != first && created);

    nodes_clear(table);
    test_assert(!nodes_is_marked(table, first));
    test_assert(!nodes_is_marked(table, second));
    nodes_mark_rec(table, first);
    test_assert(nodes_is_marked(table, first));
    test_assert(!nodes_is_marked(table, second));
    nodes_mark_rec(table, second);
    test_assert(nodes_is_marked(table, second));

    nodes_free(table);
    lace_stop();

    return 0;
}
