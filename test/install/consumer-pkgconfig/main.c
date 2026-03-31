#include <sylvan/platform.h>
#include <sylvan/sylvan.h>

TASK(int, test_addition)
int test_addition_CALL(lace_worker* lace)
{
    MTBDD one = mtbdd_int64(1);
    MTBDD two = mtbdd_plus(one, one);
    return mtbdd_getint64(two) == 2 ? 0 : 1;
}

int main(void)
{
    lace_start(1, 0, 0);
    sylvan_set_sizes(1U << 12, 1U << 12, 1U << 10, 1U << 10);
    sylvan_init_package();
    sylvan_init_mtbdd();

    int result = test_addition();

    sylvan_quit();
    lace_stop();
    return result;
}

