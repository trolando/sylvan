/**
 * Regression test: several workers start garbage collection often and at the
 * same time, through the internal entry that operations use when the table is
 * full. Before this fix, a worker that did so just after another worker's
 * collection had finished could wait forever. CTest gives this test a timeout,
 * so that a hang fails the test. Two workers suffice to show the hang; more
 * workers than cores make every collection slow (all workers take part).
 */

#include <stdio.h>
#include <stdlib.h>

#include <lace.h>
#include <sylvan/sylvan.h>

TASK(int, gc_storm, int, depth)
int
gc_storm_CALL(lace_worker *lace, int depth)
{
    if (depth == 0) {
        for (int i=0; i<20; i++) sylvan_gc_collect_CALL(lace);
        return 1;
    }
    gc_storm_SPAWN(lace, depth-1);
    int a = gc_storm_CALL(lace, depth-1);
    sylvan_gc_collect_CALL(lace);
    int b = gc_storm_SYNC(lace);
    return a + b;
}

int main(int argc, char **argv)
{
    int workers = argc > 1 ? atoi(argv[1]) : 2;
    lace_start(workers, 0, 0);

    sylvan_set_sizes(1LL<<12, 1LL<<12, 1LL<<10, 1LL<<10);
    if (sylvan_init_package() != SYLVAN_OK || mtbdd_init() != SYLVAN_OK) {
        fprintf(stderr, "Sylvan initialization failed.\n");
        return 1;
    }

    for (int r=0; r<10; r++) {
        int n = gc_storm(8);
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
