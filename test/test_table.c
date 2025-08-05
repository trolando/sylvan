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

    test_assert(nodes_mark(table, 74));
    test_assert(nodes_is_marked(table, 74));
    test_assert(!nodes_is_marked(table, 73));
    test_assert(!nodes_is_marked(table, 75));
    test_assert(nodes_mark(table, 75));

    nodes_free(table);
    lace_stop();

    return 0;
}
