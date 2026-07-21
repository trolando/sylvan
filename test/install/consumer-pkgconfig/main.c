#include <sylvan/platform.h>
#include <sylvan/sylvan.h>

TASK(int, test_addition)
int test_addition_CALL(lace_worker* lace)
{
    MTBDD one = mtbdd_int64(1);
    MTBDD two = mtbdd_invalid;
    mtbdd_refs_pushptr(&one);
    mtbdd_refs_pushptr(&two);
    int result = mtbdd_add(&two, one, one) == SYLVAN_OK && mtbdd_leaf_int64(two) == 2 ? 0 : 1;
    mtbdd_refs_popptr(2);
    return result;
}

int main(void)
{
    lace_start(1, 0, 0);
    sylvan_set_sizes(1U << 12, 1U << 12, 1U << 10, 1U << 10);
    sylvan_init_package();
    mtbdd_init();

    int result = test_addition();

    sylvan_quit();
    lace_stop();
    return result;
}

