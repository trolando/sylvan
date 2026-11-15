#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sylvan_platform.h"
#include "test_assert.h"

/* The Linux fallback target runs these same content and range checks with
   MADV_DONTNEED forced to fail in its private copy of the allocator. */
static int
test_clear(void* (*alloc)(size_t), size_t size)
{
    uint8_t *memory = alloc(size);
    test_assert(memory != NULL);
    for (size_t i=0; i<size; i++) test_assert(memory[i] == 0);

    /* Repeated collections must clear both small and huge-page-sized tables. */
    for (int pass=0; pass<3; pass++) {
        memset(memory, 0xa5 + pass, size);
        sylvan_clear_aligned(memory, size);
        for (size_t i=0; i<size; i++) test_assert(memory[i] == 0);
    }

    if (size >= ((size_t)2 << 20)) {
        /* Shrinking clears 64 KiB-aligned empty stretches inside the data
           array. This may split a huge page, but must preserve its neighbors.
           The allocation itself need not be aligned to 64 KiB. */
        const uintptr_t chunk = 65536;
        uintptr_t from = ((uintptr_t)memory + 2 * chunk - 1) & ~(chunk - 1);
        size_t offset = (size_t)(from - (uintptr_t)memory);
        memset(memory, 0x5a, size);
        sylvan_clear_aligned(memory + offset, chunk);
        for (size_t i=0; i<size; i++) {
            test_assert(memory[i] == ((i >= offset && i < offset + chunk) ? 0 : 0x5a));
        }
    }

    sylvan_free_aligned(memory, size);
    return 0;
}

int
main(void)
{
    test_assert(sylvan_alloc_aligned(0) == NULL);
    test_assert(sylvan_alloc_sparse(0) == NULL);
    sylvan_clear_aligned(NULL, 0);
    sylvan_free_aligned(NULL, 0);

    const size_t sizes[] = {4096, (size_t)2 << 20, (size_t)4 << 20};
    for (size_t i=0; i<sizeof(sizes)/sizeof(sizes[0]); i++) {
        test_assert(test_clear(sylvan_alloc_aligned, sizes[i]) == 0);
        test_assert(test_clear(sylvan_alloc_sparse, sizes[i]) == 0);
    }
    return 0;
}
