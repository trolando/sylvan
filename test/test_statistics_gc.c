/* Private translation unit: deterministic GC and allocation failure injection
 * without changing the installed library or relying on symbol interposition. */
#include <sylvan_int.h>
#include <stdatomic.h>

static unsigned gc_countdown;
static long allocations_before_failure = -1;

void test_statistics_gc_arm(unsigned visits)
{
    gc_countdown = visits;
}

void test_statistics_fail_after(long allocations)
{
    allocations_before_failure = allocations;
}

VOID_TASK_0(statistics_gc_safepoint)
{
    if (gc_countdown != 0 && --gc_countdown == 0) CALL(sylvan_gc);
    YIELD_NEWFRAME();
}

static int allocation_fails(void)
{
    if (allocations_before_failure < 0) return 0;
    if (allocations_before_failure == 0) return 1;
    allocations_before_failure--;
    return 0;
}

static void *statistics_test_calloc(size_t count, size_t size)
{
    return allocation_fails() ? NULL : calloc(count, size);
}

void *statistics_test_realloc(void *ptr, size_t size)
{
    return allocation_fails() ? NULL : realloc(ptr, size);
}

/* Hold the sampler while another external thread submits a cache writer. */
static atomic_int sampling_state;

void test_statistics_sampling_arm(void)
{
    atomic_store(&sampling_state, 1);
}

int test_statistics_sampling_active(void)
{
    return atomic_load(&sampling_state) == 2;
}

void test_statistics_sampling_release(void)
{
    atomic_store(&sampling_state, 3);
}

static size_t statistics_test_cache_getused(void)
{
    if (atomic_load(&sampling_state) == 1) {
        atomic_store(&sampling_state, 2);
        while (atomic_load(&sampling_state) == 2) {}
    }
    return cache_getused();
}

#define cache_getused statistics_test_cache_getused
#undef sylvan_gc_test
#define sylvan_gc_test() CALL(statistics_gc_safepoint)
#define calloc statistics_test_calloc
#define realloc statistics_test_realloc
#include "../src/sylvan_stats.c"
