/* Private reference-stack allocation injection and inspection. */
#include <sylvan_int.h>

void *statistics_test_realloc(void *ptr, size_t size);
#define realloc statistics_test_realloc
#include "../src/sylvan_mtbdd.c"

uint64_t *test_statistics_mtbdd_refs(size_t *count, size_t *capacity)
{
    *count = (size_t)(mtbdd_refs_key->rcur - mtbdd_refs_key->rbegin);
    *capacity = (size_t)(mtbdd_refs_key->rend - mtbdd_refs_key->rbegin);
    return mtbdd_refs_key->rbegin;
}
