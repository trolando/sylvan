#include <stdio.h>
#include <stdint.h>

#include <lace.h>

#include "sylvan_platform.h"
#include "sylvan_table.h"
#include "test_assert.h"

int
main(void)
{
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

    llmsset_free(table);
    lace_stop();

    return 0;
}
