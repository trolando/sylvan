#include <sylvan.h>
#include "test_assert.h"

using namespace sylvan;

/* Check all public wrappers from a non-worker C++ caller. These buffers use
 * fixed version-1 layouts, independent of the growing Sylvan_Counters enum. */
int main(int argc, char **argv)
{
    lace_start(argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 1, 0);
    sylvan_set_sizes(1<<10, 1<<16, 1<<10, 1<<14);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_zdd();
    sylvan_statistics statistics = SYLVAN_STATISTICS_INIT;
    test_assert(sylvan_statistics_snapshot(&statistics));
    size_t count;
    test_assert(sylvan_statistics_read(nullptr, 0, &count) && count > 0);
    sylvan_statistic *entries = new sylvan_statistic[count];
    test_assert(sylvan_statistics_read(entries, count, &count));
    sylvan_statistic gc;
    uint64_t value;
    sylvan_gc();
    test_assert(sylvan_statistics_read_name("GC executions", &gc));
    test_assert(sylvan_statistics_read_counter(SYLVAN_GC_COUNT, &value));
    test_assert(value == gc.value && value == (SYLVAN_STATS ? 1u : 0u));
    MTBDD bdd = sylvan_true;
    ZDD zdd = zdd_true;
    sylvan_level_statistic level;
    test_assert(mtbdd_level_statistics(&bdd, 1, &level, 1, &count, &value));
    test_assert(count == 0 && value == 1);
    test_assert(zdd_level_statistics(&zdd, 1, &level, 1, &count, &value));
    test_assert(count == 0 && value == 1);
    delete[] entries;
    sylvan_quit();
    lace_stop();
    return 0;
}
