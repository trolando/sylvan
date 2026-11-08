/* Wide sibling lists must not require one native stack frame per value. */
#include <stdio.h>
#include <stdlib.h>

#include "sylvan.h"
#include "test_assert.h"

#define WIDTH 1000000u

TASK_0(int, test_wide)
{
    MDD rel = lddmc_false, set = lddmc_false, meta = lddmc_false;
    lddmc_refs_pushptr(&rel);
    lddmc_refs_pushptr(&set);
    lddmc_refs_pushptr(&meta);
    uint32_t levels[] = {1, 2, (uint32_t)-1};
    meta = lddmc_cube(levels, 3);
    set = lddmc_makenode(WIDTH-1, lddmc_true, lddmc_false);
    for (uint32_t i=WIDTH; i-- > 0;) {
        MDD write = lddmc_makenode(i, lddmc_true, lddmc_false);
        rel = lddmc_makenode(i, write, rel);
    }
    /* A fresh cache forces both relation-splitting helpers to visit every read. */
    sylvan_clear_cache();
    test_assert(CALL(lddmc_reach, set, rel, meta) == set);
    printf("REACH passed with %u identity transitions.\n", WIDTH);

    rel = lddmc_false;
    sylvan_gc();
    /* Removing the last write also used to recurse through the whole list.
     * The read does not match set, so relprod itself need not enumerate it. */
    for (uint32_t i=WIDTH; i-- > 0;) rel = lddmc_makenode(i, lddmc_true, rel);
    rel = lddmc_makenode(WIDTH-1, rel, lddmc_false);
    set = lddmc_makenode(WIDTH, lddmc_true, lddmc_false);
    sylvan_clear_cache();
    test_assert(CALL(lddmc_reach, set, rel, meta) == set);
    printf("REACH passed with %u writes under one read.\n", WIDTH);
    lddmc_refs_popptr(3);
    return 0;
}

int main(int argc, char **argv)
{
    lace_set_stacksize(8 * 1024 * 1024);
    lace_start(argc > 1 ? atoi(argv[1]) : 1, 0);
    /* Construction and the operation fit without collecting the wide lists. */
    sylvan_set_sizes(1LL<<23, 1LL<<23, 1LL<<16, 1LL<<16);
    sylvan_init_package();
    sylvan_init_ldd();
    int result = RUN(test_wide);
    sylvan_quit();
    lace_stop();
    return result;
}
