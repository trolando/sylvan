/* Replace the iterator translation unit in the dedicated static test library
 * for deterministic allocation failures. Do not link that library together
 * with the production library: each owns its complete Sylvan state. */
#include <sylvan_int.h>
#include <stdlib.h>

static long allocations_before_failure = -1;
void test_iterator_allocation(void);

void test_iterator_fail_after(long count)
{
    allocations_before_failure = count;
}

static int allocation_fails(void)
{
    test_iterator_allocation();
    if (allocations_before_failure < 0) return 0;
    if (allocations_before_failure == 0) return 1;
    allocations_before_failure--;
    return 0;
}

static void *iterator_test_malloc(size_t size)
{
    return allocation_fails() ? NULL : malloc(size);
}

static void *iterator_test_calloc(size_t count, size_t size)
{
    return allocation_fails() ? NULL : calloc(count, size);
}

static void *iterator_test_realloc(void *ptr, size_t size)
{
    return allocation_fails() ? NULL : realloc(ptr, size);
}

#define malloc iterator_test_malloc
#define calloc iterator_test_calloc
#define realloc iterator_test_realloc
#include "../src/sylvan_iterator.c"
