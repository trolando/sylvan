/* A protected failed result is not a node index (sylvan-2 222538e). */
#include <stdio.h>
#include <string.h>
#include <sylvan.h>
#include "test_assert.h"

TASK_1(int, check_invalid, int, use_zdd)
{
    MTBDD invalid = mtbdd_fraction(1, 0);
    test_assert(invalid == mtbdd_invalid);
    ZDD zinvalid = zdd_invalid;
    BDD live = sylvan_ithvar(7);
    mtbdd_protect(&live);
    if (use_zdd) zdd_protect(&zinvalid);
    else mtbdd_protect(&invalid);
    for (int i = 0; i < 3; i++) {
        CALL(sylvan_gc);
        test_assert(invalid == mtbdd_invalid);
        test_assert(zinvalid == zdd_invalid);
        test_assert(sylvan_ithvar(7) == live);
    }
    if (use_zdd) zdd_unprotect(&zinvalid);
    else mtbdd_unprotect(&invalid);
    mtbdd_unprotect(&live);
    return 0;
}

int main(int argc, char **argv)
{
    lace_start(2, 0);
    sylvan_set_sizes(1LL<<12, 1LL<<16, 1LL<<10, 1LL<<14);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_zdd();
    int result = RUN(check_invalid, argc > 1 && strcmp(argv[1], "zdd") == 0);
    sylvan_quit();
    lace_stop();
    return result;
}
