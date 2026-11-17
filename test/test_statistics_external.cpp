#include <sylvan_int.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "test_assert.h"

using namespace sylvan;

extern "C" {
void test_statistics_sampling_arm(void);
int test_statistics_sampling_active(void);
void test_statistics_sampling_release(void);
}

static std::atomic<bool> wrote_during_sampling(false);
static std::atomic<unsigned> writes(0);

VOID_TASK_0(external_cache_writer)
{
    if (test_statistics_sampling_active()) wrote_during_sampling.store(true);
    for (unsigned i=0; i<1000; i++) (void)cache_put(i, 2, 3, 4);
    writes.fetch_add(1);
}

int main(int argc, char **argv)
{
    lace_start(argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 4, 0);
    sylvan_set_sizes(1<<10, 1<<16, 1<<10, 1<<14);
    sylvan_init_package();
    for (unsigned repeat=0; repeat<5; repeat++) {
        test_statistics_sampling_arm();
        sylvan_statistics snapshot = SYLVAN_STATISTICS_INIT;
        int ok = 0;
        std::thread sampler([&] { ok = sylvan_statistics_snapshot(&snapshot); });
        while (!test_statistics_sampling_active()) std::this_thread::yield();
        std::atomic<bool> submitting(false);
        std::thread writer([&] {
            submitting.store(true);
            RUN(external_cache_writer);
        });
        while (!submitting.load()) std::this_thread::yield();
        /* Allow idle workers to accept the external RUN if the sampling
         * frame mistakenly leaves them stealing. No collection is involved. */
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        test_statistics_sampling_release();
        writer.join();
        sampler.join();
        test_assert(ok && !wrote_during_sampling.load());
        test_assert(writes.load() == repeat+1);
        test_assert(snapshot.cache_used <= snapshot.cache_capacity);
    }
    sylvan_quit();
    lace_stop();
    return 0;
}
