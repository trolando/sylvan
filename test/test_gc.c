/**
 * Regression test: several workers call sylvan_gc often and at the same time.
 * Before 1.12.2, a worker that called sylvan_gc just after another worker's
 * garbage collection had finished could wait forever. CTest gives this test a
 * timeout, so that a hang fails the test.
 */

#include <stdio.h>
#include <stdlib.h>

#include "sylvan.h"

#include <lace.h>

TASK_1(int, gc_storm, int, depth)
{
    if (depth == 0) {
        for (int i=0; i<20; i++) CALL(sylvan_gc);
        return 1;
    }
    SPAWN(gc_storm, depth-1);
    int a = CALL(gc_storm, depth-1);
    CALL(sylvan_gc);
    int b = SYNC(gc_storm);
    return a + b;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 4;
    lace_start(workers, 0);

    sylvan_set_sizes(1LL<<12, 1LL<<12, 1LL<<10, 1LL<<10);
    sylvan_init_package();
    sylvan_init_bdd();

    for (int r=0; r<20; r++) {
        int n = RUN(gc_storm, 8);
        if (n != 256) {
            fprintf(stderr, "gc_storm returned %d instead of 256\n", n);
            return 1;
        }
    }
    printf("Garbage collection by many workers at once: no hang.\n");

    sylvan_quit();
    lace_stop();

    return 0;
}
