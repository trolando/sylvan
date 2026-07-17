#include <sylvan_platform.h>
#include <sylvan.h>

TASK_0(int, test_addition)
{
    MTBDD one = mtbdd_int64(1);
    MTBDD two = mtbdd_plus(one, one);
    return mtbdd_getint64(two) == 2 ? 0 : 1;
}

int main(void)
{
    lace_start(1, 0);
    sylvan_set_sizes(1U << 12, 1U << 12, 1U << 10, 1U << 10);
    sylvan_init_package();
    sylvan_init_mtbdd();

    int result = RUN(test_addition);

    sylvan_quit();
    lace_stop();
    return result;
}

