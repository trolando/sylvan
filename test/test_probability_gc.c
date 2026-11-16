/* A private BDD implementation for deterministic GC injection. No hooks or
 * conditional instrumentation are added to the installed library. The normal
 * test_probability executable exercises the library implementation itself. */
#include <sylvan_int.h>
#include <stdatomic.h>

static _Atomic(unsigned) probability_gc_countdown;

void test_probability_gc_arm(unsigned visits)
{
    atomic_store(&probability_gc_countdown, visits);
}

VOID_TASK_0(probability_gc_safepoint)
{
    unsigned remaining = atomic_load(&probability_gc_countdown);
    while (remaining != 0) {
        if (atomic_compare_exchange_weak(&probability_gc_countdown, &remaining, remaining-1)) {
            if (remaining == 1) CALL(sylvan_gc);
            break;
        }
    }
    YIELD_NEWFRAME();
}

#undef sylvan_gc_test
#define sylvan_gc_test() CALL(probability_gc_safepoint)
#include "../src/sylvan_bdd.c"

void test_probability_exhaust_ids(void)
{
    atomic_store(&probability_call_id, UINT64_MAX-1);
}
