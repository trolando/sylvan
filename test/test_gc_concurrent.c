/* Deterministic schedules for allocation recovery with four Lace workers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include <sylvan.h>
#include <sylvan_int.h>

/* Exercise the same collection task used by allocation recovery, with
 * different outstanding minima. This is an internal symbol, not a public API. */
VOID_TASK_DECL_2(sylvan_gc_start, int, size_t);

enum { RECOVER, GROWTH, SHRINK };
static int mode;
static uint64_t roots[8192], results[4];
static size_t root_count;
static size_t sizes[32];
static unsigned collections;
static _Atomic(unsigned) failed, entered;
static size_t minimum[4] = {2048, 4096, 8192, 4096};

/* A failure must end the process, including in VOID_TASKs and barriers. */
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static void
unexpected_full(const char *kind, size_t filled, size_t total)
{
    fprintf(stderr, "Unexpected table full: %s, %zu of %zu (maximum %zu)\n",
            kind, filled, total, llmsset_get_max_size(nodes));
    exit(1);
}

VOID_TASK_0(mark_roots)
{
    for (size_t i = 0; i < root_count; i++) llmsset_mark(nodes, roots[i]);
}

VOID_TASK_0(no_growth)
{
    /* Allocation recovery, including a joined request, must enforce growth. */
}

VOID_TASK_0(after_gc)
{
    CHECK(collections < sizeof(sizes) / sizeof(sizes[0]));
    sizes[collections++] = llmsset_get_size(nodes);
    if (collections == 1) {
        /* All three followers entered recovery while the leader's frame was
         * pending. None could merely arrive after that collection finished. */
        CHECK(atomic_load(&entered) == 4);
        CHECK(atomic_load(&failed) == 4);
        CHECK(sizes[0] == (mode == RECOVER ? 1024u : 2048u));
    }
}

static void
wait_for_collection(void)
{
    /* Deliberately do not yield: the announced frame cannot run until this
     * worker enters recovery and joins it. TOGETHER's own announcement has
     * already been cleared before any of these tasks starts. */
    while (atomic_load_explicit(&lace_newframe.t, memory_order_acquire) == NULL) {}
}

VOID_TASK_0(concurrent_recovery)
{
    unsigned worker = lace_get_worker()->worker;
    int created;
    CHECK(llmsset_lookup(nodes, UINT64_C(0xd000000000000000) + worker, 0, &created) == 0);
    atomic_fetch_add(&failed, 1);
    while (atomic_load(&failed) != 4) {}

    /* Worker 0 starts the collection; the others must join that exact frame.
     * In GROWTH they join a smaller (2048) request with larger minima. In
     * SHRINK an explicit collection starts from inside this task. */
    if (worker != 0) wait_for_collection();
    atomic_fetch_add(&entered, 1);
    if (mode == GROWTH || (mode == SHRINK && worker >= 2)) {
        CALL(sylvan_gc_start, 0, minimum[worker]);
        CHECK(llmsset_get_size(nodes) >= minimum[worker]);
    } else if (mode == SHRINK && worker == 0) {
        CALL(sylvan_gc);
    }

    /* RECOVER: all four constructors fail and enter the full recovery loop.
     * SHRINK: worker 1 enters it while the explicit shrink is pending;
     * workers 2 and 3 join that shrink with outstanding growth requests.
     * GROWTH: retry after enforcing each minimum, recovering further if the
     * remaining regions have already been claimed by the other workers. */
    results[worker] = mtbdd_makenode(100 + worker, mtbdd_false, mtbdd_true);
    CHECK(results[worker] > 1);
    CHECK(mtbdd_getvar(results[worker]) == 100 + worker);
    CHECK(mtbdd_getlow(results[worker]) == mtbdd_false);
    CHECK(mtbdd_gethigh(results[worker]) == mtbdd_true);
    /* TOGETHER keeps completed workers stealing, so they can join subsequent
     * collections. The result slots are protected before the tasks start. */
}

TASK_0(int, prepare)
{
    if (mode == SHRINK) llmsset_set_size(nodes, 8192);
    size_t size = llmsset_get_size(nodes);
    size_t count = 2;
    for (uint64_t key = 1; count < size && key < 100000; key++) {
        int created;
        uint64_t index = llmsset_lookup(nodes, UINT64_C(0xe000000000000000) + key, 0, &created);
        if (index != 0) {
            CHECK(created);
            /* The explicit collection keeps just the first 1022 nodes. The
             * low-memory policy also reduces the data range to 2048. */
            if (mode != SHRINK || root_count < 1022) roots[root_count++] = index;
            count++;
        }
    }
    CHECK(count == size);
    CHECK(llmsset_count_marked_no_yield(nodes) == size);
    CHECK(llmsset_count_marked(nodes) == size);
    return 0;
}

TASK_0(int, verify)
{
    CHECK(collections >= (mode == RECOVER ? 3u : 2u));
    for (unsigned i = 1; i < collections; i++) CHECK(sizes[i] >= sizes[i - 1]);
    for (unsigned worker = 0; worker < 4; worker++) {
        CHECK(mtbdd_makenode(100 + worker, mtbdd_false, mtbdd_true) == results[worker]);
        CHECK(mtbdd_getvar(results[worker]) == 100 + worker);
        CHECK(mtbdd_getlow(results[worker]) == mtbdd_false);
        CHECK(mtbdd_gethigh(results[worker]) == mtbdd_true);
        for (unsigned other = 0; other < worker; other++) CHECK(results[worker] != results[other]);
    }
    for (size_t i = 0; i < root_count; i++) {
        uint64_t *data = llmsset_index_to_ptr(nodes, roots[i]);
        int created;
        CHECK(llmsset_lookup(nodes, data[0], data[1], &created) == roots[i]);
        CHECK(!created);
    }
    CHECK(llmsset_count_marked_no_yield(nodes) == root_count + 6);
    CHECK(llmsset_count_marked(nodes) == root_count + 6);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc != 2) return 2;
    if (strcmp(argv[1], "recover") == 0) mode = RECOVER;
    else if (strcmp(argv[1], "growth") == 0) mode = GROWTH;
    else if (strcmp(argv[1], "shrink") == 0) mode = SHRINK;
    else return 2;

    lace_start(4, 8192);
    sylvan_set_sizes(1024, 16384, 1024, 16384);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_gc_set_policy(SYLVAN_GC_LOW_MEMORY);
    sylvan_gc_hook_main(no_growth_CALL);
    sylvan_gc_add_mark(mark_roots_CALL);
    sylvan_gc_hook_postgc(after_gc_CALL);
    sylvan_set_table_full_handler(unexpected_full);
    for (unsigned i = 0; i < 4; i++) mtbdd_protect(&results[i]);
    CHECK(RUN(prepare) == 0);
    TOGETHER(concurrent_recovery);
    CHECK(RUN(verify) == 0);
    for (unsigned i = 0; i < 4; i++) mtbdd_unprotect(&results[i]);
    sylvan_quit();
    lace_stop();
    return 0;
}
