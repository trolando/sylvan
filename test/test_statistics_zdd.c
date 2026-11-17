/* Private reference-stack allocation injection and inspection. */
#include <sylvan_int.h>

void *statistics_test_realloc(void *ptr, size_t size);
#define realloc statistics_test_realloc
#include "../src/sylvan_zdd.c"

uint64_t *test_statistics_zdd_refs(size_t *count, size_t *capacity)
{
    *count = (size_t)(zdd_refs_key->rcur - zdd_refs_key->rbegin);
    *capacity = (size_t)(zdd_refs_key->rend - zdd_refs_key->rbegin);
    return zdd_refs_key->rbegin;
}
