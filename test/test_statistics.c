#include <sylvan_int.h>
#include "test_assert.h"

static unsigned workers, collections;
static uint32_t random_state = 0x891abcde;
static const uint32_t variables[] = {0, 1, 9, 100003, 0xfffffe, 0xffffff};

#ifdef SYLVAN_TEST_STATISTICS_GC
void test_statistics_gc_arm(unsigned visits);
void test_statistics_fail_after(long allocations);
uint64_t *test_statistics_mtbdd_refs(size_t *count, size_t *capacity);
uint64_t *test_statistics_zdd_refs(size_t *count, size_t *capacity);
#endif

VOID_TASK_0(count_collection)
{
    collections++;
}

static uint32_t random_u32(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

VOID_TASK_0(count_on_every_worker)
{
    sylvan_stats_add(BDD_EVAL, 7);
}

TASK_0(int, test_snapshots)
{
    CALL(sylvan_stats_reset);
    struct { sylvan_statistics data; uint64_t guard[2]; } sample = {
        SYLVAN_STATISTICS_INIT, {UINT64_C(0x123456789abcdef0), UINT64_MAX}
    };
    sample.data.size = sizeof(sample); /* A future caller's larger structure. */
    test_assert(CALL(sylvan_statistics_snapshot, &sample.data));
    test_assert(sample.data.size == sizeof(sample));
    test_assert(sample.guard[0] == UINT64_C(0x123456789abcdef0) && sample.guard[1] == UINT64_MAX);
    test_assert(sample.data.gc_count == 0 && sample.data.gc_time_ns == 0);
    test_assert(sample.data.worker_count == workers);
    test_assert(sample.data.statistics_enabled == (SYLVAN_STATS ? 1u : 0u));
    test_assert(sample.data.nodes_used <= sample.data.nodes_capacity + 2);
    test_assert(sample.data.nodes_capacity <= sample.data.nodes_max_capacity);
    test_assert(sample.data.cache_used <= sample.data.cache_capacity);
    test_assert(sample.data.cache_capacity <= sample.data.cache_max_capacity);
    sample.data.size = SYLVAN_STATISTICS_V1_SIZE;
    test_assert(CALL(sylvan_statistics_snapshot, &sample.data));
    test_assert(sample.guard[1] == UINT64_MAX);
    sylvan_statistics saved = sample.data;
    test_assert(!CALL(sylvan_statistics_snapshot, NULL));
    sample.data.version++;
    saved = sample.data;
    test_assert(!CALL(sylvan_statistics_snapshot, &sample.data));
    test_assert(memcmp(&sample.data, &saved, sizeof(saved)) == 0);
    sample.data.version = SYLVAN_STATISTICS_VERSION;
    sample.data.size = SYLVAN_STATISTICS_V1_SIZE - 1;
    saved = sample.data;
    test_assert(!CALL(sylvan_statistics_snapshot, &sample.data));
    test_assert(memcmp(&sample.data, &saved, sizeof(saved)) == 0);

    sylvan_statistic before, after;
    test_assert(CALL(sylvan_statistics_read_name, "BDD and", &before));
    BDD a = mtbdd_refs_push(sylvan_ithvar(3));
    BDD b = mtbdd_refs_push(sylvan_ithvar(7));
    for (unsigned i=0; i<8; i++) test_assert(sylvan_and(a, b) != sylvan_invalid);
    test_assert(CALL(sylvan_statistics_read_name, "BDD and", &after));
    test_assert(after.kind == SYLVAN_STATISTIC_OPERATION && after.reserved == 0);
#if SYLVAN_STATS
    test_assert(after.value > before.value && after.cache_hits > before.cache_hits);
    test_assert(after.cache_puts > before.cache_puts);
#else
    test_assert(after.value == 0 && after.cache_hits == 0 && after.cache_puts == 0);
#endif
    uint64_t value;
    test_assert(CALL(sylvan_statistics_read_counter, BDD_AND, &value) && value == after.value);
    test_assert(CALL(sylvan_statistics_read_counter, BDD_AND_CACHED, &value) && value == after.cache_hits);
    test_assert(CALL(sylvan_statistics_read_counter, BDD_AND_CACHEDPUT, &value) && value == after.cache_puts);
    TOGETHER(count_on_every_worker);
    test_assert(CALL(sylvan_statistics_read_counter, BDD_EVAL, &value));
    test_assert(value == (SYLVAN_STATS ? 7 * workers : 0));

    CALL(sylvan_gc);
    sylvan_statistics current = SYLVAN_STATISTICS_INIT;
    test_assert(CALL(sylvan_statistics_snapshot, &current));
    test_assert(current.gc_count == (SYLVAN_STATS ? 1u : 0u));
#if SYLVAN_STATS
    test_assert(current.gc_time_ns > 0);
#else
    test_assert(current.gc_time_ns == 0);
#endif
    test_assert(CALL(sylvan_statistics_read_name, "GC executions", &after));
    test_assert(after.kind == SYLVAN_STATISTIC_COUNTER && after.value == current.gc_count);
    test_assert(after.cache_hits == 0 && after.cache_puts == 0);
    test_assert(CALL(sylvan_statistics_read_name, "Total time spent", &after));
    test_assert(after.kind == SYLVAN_STATISTIC_TIMER && after.value == current.gc_time_ns);

    size_t count = 0, written;
    test_assert(CALL(sylvan_statistics_read, NULL, 0, &count) && count > 0);
    sylvan_statistic *entries = malloc((count+1) * sizeof(*entries));
    sylvan_statistic *unchanged = malloc((count+1) * sizeof(*unchanged));
    test_assert(entries != NULL && unchanged != NULL);
    memset(entries, 0xa5, (count+1) * sizeof(*entries));
    memcpy(unchanged, entries, (count+1) * sizeof(*entries));
    test_assert(!CALL(sylvan_statistics_read, entries, count-1, &written) && written == count);
    test_assert(memcmp(entries, unchanged, (count+1) * sizeof(*entries)) == 0);
    test_assert(!CALL(sylvan_statistics_read, entries, 0, &written) && written == count);
    test_assert(!CALL(sylvan_statistics_read, entries, count, NULL));
    written = 123;
    test_assert(!CALL(sylvan_statistics_read, NULL, 1, &written) && written == 123);
    test_assert(CALL(sylvan_statistics_read, entries, count, &written) && written == count);
    test_assert(memcmp(entries+count, unchanged+count, sizeof(*entries)) == 0);
    for (size_t i=0; i<count; i++) {
        test_assert(entries[i].name != NULL && entries[i].reserved == 0);
        test_assert(CALL(sylvan_statistics_read_name, entries[i].name, &after));
        test_assert(after.kind == entries[i].kind && after.value == entries[i].value);
        test_assert(after.cache_hits == entries[i].cache_hits && after.cache_puts == entries[i].cache_puts);
        for (size_t j=0; j<i; j++) test_assert(strcmp(entries[j].name, entries[i].name) != 0);
        if (!SYLVAN_STATS) test_assert(after.value == 0 && after.cache_hits == 0 && after.cache_puts == 0);
    }
    /* The legacy snapshot still writes its original complete layout, including
     * all counters, and agrees with every raw id read through the new API. */
    struct { sylvan_stats_t data; uint64_t guard; } legacy;
    legacy.guard = UINT64_MAX;
    CALL(sylvan_stats_snapshot, &legacy.data);
    test_assert(legacy.guard == UINT64_MAX);
    for (size_t i=0; i<SYLVAN_COUNTER_COUNTER; i++) {
        test_assert(CALL(sylvan_statistics_read_counter, i, &value));
        test_assert(value == legacy.data.counters[i]);
    }
    value = 123;
    test_assert(!CALL(sylvan_statistics_read_counter, SYLVAN_COUNTER_COUNTER, &value) && value == 123);
    test_assert(!CALL(sylvan_statistics_read_counter, SIZE_MAX, &value) && value == 123);
    test_assert(!CALL(sylvan_statistics_read_counter, BDD_AND, NULL));
    memset(&after, 0xa5, sizeof(after));
    memcpy(&before, &after, sizeof(after));
    const char *unknown[] = {NULL, "", "bdd and", "not a statistic", "Tables"};
    for (size_t i=0; i<sizeof(unknown)/sizeof(unknown[0]); i++) {
        test_assert(!CALL(sylvan_statistics_read_name, unknown[i], &after));
        test_assert(memcmp(&after, &before, sizeof(after)) == 0);
    }
    test_assert(!CALL(sylvan_statistics_read_name, "BDD and", NULL));
    free(unchanged);
    free(entries);
    mtbdd_refs_pop(2);
    return 0;
}

/* Independent oracle: enumerate paths through public accessors, recording
 * physical handles in a linear array. No hashing, scratch marks or cache. */
struct oracle {
    uint64_t seen[8192];
    size_t seen_count, count;
    uint64_t leaves;
    sylvan_level_statistic levels[4096];
};

static void enumerate_nodes(struct oracle *oracle, uint64_t dd, int zdd)
{
    const uint64_t physical = dd & ~mtbdd_complement;
    for (size_t i=0; i<oracle->seen_count; i++) if (oracle->seen[i] == physical) return;
    assert(oracle->seen_count < 8192);
    oracle->seen[oracle->seen_count++] = physical;
    if (zdd ? zdd_isleaf(dd) : mtbdd_isleaf(dd)) { oracle->leaves++; return; }
    uint32_t level = zdd ? zdd_getvar(dd) : mtbdd_getvar(dd);
    size_t i = 0;
    while (i < oracle->count && oracle->levels[i].level != level) i++;
    if (i == oracle->count) {
        assert(i < 4096);
        oracle->levels[i].level = level;
        oracle->count++;
    }
    oracle->levels[i].node_count++;
    enumerate_nodes(oracle, zdd ? zdd_getlow(dd) : mtbdd_getlow(dd), zdd);
    enumerate_nodes(oracle, zdd ? zdd_gethigh(dd) : mtbdd_gethigh(dd), zdd);
}

TASK_3(int, check_profile, const uint64_t*, roots, size_t, root_count, int, zdd)
{
    struct oracle *oracle = calloc(1, sizeof(*oracle));
    test_assert(oracle != NULL);
    for (size_t i=0; i<root_count; i++) enumerate_nodes(oracle, roots[i], zdd);
    size_t count = SIZE_MAX;
    uint64_t leaves = UINT64_MAX;
    int ok = zdd ? CALL(zdd_level_statistics, roots, root_count, NULL, 0, &count, &leaves)
                 : CALL(mtbdd_level_statistics, roots, root_count, NULL, 0, &count, &leaves);
    test_assert(ok && count == oracle->count && leaves == oracle->leaves);
    sylvan_level_statistic *entries = malloc((count+1) * sizeof(*entries));
    test_assert(entries != NULL);
    memset(entries, 0xa5, (count+1) * sizeof(*entries));
    sylvan_level_statistic sentinel;
    memcpy(&sentinel, entries, sizeof(sentinel));
    if (count != 0) {
        ok = zdd ? CALL(zdd_level_statistics, roots, root_count, entries, count-1, &count, &leaves)
                 : CALL(mtbdd_level_statistics, roots, root_count, entries, count-1, &count, &leaves);
        test_assert(!ok && count == oracle->count && leaves == oracle->leaves);
        for (size_t i=0; i<=count; i++) test_assert(memcmp(entries+i, &sentinel, sizeof(sentinel)) == 0);
    }
    ok = zdd ? CALL(zdd_level_statistics, roots, root_count, entries, count, &count, &leaves)
             : CALL(mtbdd_level_statistics, roots, root_count, entries, count, &count, &leaves);
    test_assert(ok && count == oracle->count && leaves == oracle->leaves);
    test_assert(memcmp(entries+count, &sentinel, sizeof(sentinel)) == 0);
    for (size_t i=0; i<count; i++) {
        test_assert(entries[i].reserved == 0 && entries[i].node_count > 0);
        if (i != 0) test_assert(entries[i-1].level < entries[i].level);
        size_t j = 0;
        while (j < oracle->count && oracle->levels[j].level != entries[i].level) j++;
        test_assert(j < oracle->count && entries[i].node_count == oracle->levels[j].node_count);
    }
    free(entries);
    free(oracle);
    return 0;
}

static MTBDD from_table(const MTBDD *table, unsigned depth, unsigned assignment, int zdd)
{
    if (depth == 6) return table[assignment];
    MTBDD low = from_table(table, depth+1, assignment, zdd);
    if (zdd) zdd_refs_push(low); else mtbdd_refs_push(low);
    MTBDD high = from_table(table, depth+1, assignment | (1u << depth), zdd);
    MTBDD result = zdd ? zdd_makenode(variables[depth], low, high) : mtbdd_makenode(variables[depth], low, high);
    if (zdd) zdd_refs_pop(1); else mtbdd_refs_pop(1);
    return result;
}

TASK_0(int, test_levels)
{
    for (int zdd=0; zdd<2; zdd++) {
        uint64_t leaves[] = {zdd ? zdd_false : mtbdd_false, zdd ? zdd_true : mtbdd_true, 0, 0, 0};
        for (size_t i=0; i<5; i++) {
            if (zdd) zdd_refs_pushptr(leaves+i); else mtbdd_refs_pushptr(leaves+i);
        }
        leaves[2] = mtbdd_int64(0);
        leaves[3] = mtbdd_int64(-7);
        leaves[4] = mtbdd_double(3.25);
        test_assert(CALL(check_profile, NULL, 0, zdd) == 0);
        for (size_t i=0; i<5; i++) test_assert(CALL(check_profile, leaves+i, 1, zdd) == 0);
        test_assert(CALL(check_profile, leaves, 5, zdd) == 0);
        for (unsigned repeat=0; repeat<80; repeat++) {
            uint64_t roots[6];
            for (size_t r=0; r<3; r++) {
                MTBDD table[64];
                for (size_t i=0; i<64; i++) table[i] = leaves[random_u32() % (repeat % 2 ? 5 : 2)];
                roots[r] = from_table(table, 0, 0, zdd);
                if (zdd) zdd_refs_push(roots[r]); else mtbdd_refs_push(roots[r]);
            }
            roots[3] = roots[0];
            roots[4] = roots[1] ^ ((!zdd || ZDD_COMPLEMENT_EDGES) ? mtbdd_complement : 0);
            roots[5] = zdd ? zdd_gethigh(roots[2]) : mtbdd_gethigh(roots[2]);
            /* Existing scratch users may have marks set around this call. */
            nodes_scratch_acquire(nodes);
            nodes_scratch_mark(nodes, MTBDD_GETINDEX(roots[0]));
            test_assert(CALL(check_profile, roots, 6, zdd) == 0);
            test_assert(nodes_scratch_ismarked(nodes, MTBDD_GETINDEX(roots[0])));
            nodes_scratch_unmark(nodes, MTBDD_GETINDEX(roots[0]));
            nodes_scratch_release(nodes);
            CALL(sylvan_gc);
            SPAWN(check_profile, roots, 6, zdd);
            test_assert(CALL(check_profile, roots+1, 4, zdd) == 0);
            test_assert(SYNC(check_profile) == 0);
            if (zdd) zdd_refs_pop(3); else mtbdd_refs_pop(3);
        }
        if (zdd) zdd_refs_popptr(5); else mtbdd_refs_popptr(5);
    }
    return 0;
}

TASK_0(int, test_invalid)
{
    MTBDD map = mtbdd_refs_push(mtbdd_map_add(mtbdd_map_empty(), 7, mtbdd_true));
    MTBDD invalid[] = {mtbdd_invalid, 1, UINT64_C(1)<<45, llmsset_get_data_size(nodes), map};
    for (int zdd=0; zdd<2; zdd++) {
        for (unsigned i=0; i<10; i++) {
            if (i < 5 && zdd && invalid[i] == zdd_true) continue;
            const uint64_t *roots = i < 5 ? invalid+i : &map;
            if (i == 5) roots = NULL;
            sylvan_level_statistic entry = {19, 23, 29};
            size_t count = 31;
            uint64_t leaves = 37;
            sylvan_level_statistic *out = i == 6 ? NULL : &entry;
            size_t *n = i == 7 ? NULL : &count;
            uint64_t *l = i == 8 ? NULL : &leaves;
            /* A mixed array must also reject a late invalid root. */
            uint64_t mixed[] = {zdd ? zdd_true : mtbdd_true, mtbdd_invalid};
            if (i == 9) roots = mixed;
            int ok = zdd ? CALL(zdd_level_statistics, roots, i == 9 ? 2 : 1, out, 1, n, l)
                         : CALL(mtbdd_level_statistics, roots, i == 9 ? 2 : 1, out, 1, n, l);
            test_assert(!ok && count == 31 && leaves == 37);
            test_assert(entry.level == 19 && entry.reserved == 23 && entry.node_count == 29);
        }
    }
    mtbdd_refs_pop(1);
    return 0;
}

VOID_TASK_2(statistics_work, MTBDD, a, MTBDD, b)
{
    for (unsigned i=0; i<5000; i++) {
        MTBDD node = mtbdd_refs_push(mtbdd_makenode(i, a, b));
        (void)sylvan_and(node, a);
        mtbdd_refs_pop(1);
    }
}

TASK_0(int, test_concurrent_snapshot)
{
    MTBDD a = mtbdd_refs_push(sylvan_ithvar(100000));
    MTBDD b = mtbdd_refs_push(sylvan_ithvar(100001));
    SPAWN(statistics_work, a, b);
    int valid = 1;
    for (unsigned i=0; i<20; i++) {
        sylvan_statistics snapshot = SYLVAN_STATISTICS_INIT;
        valid &= CALL(sylvan_statistics_snapshot, &snapshot);
        /* The bitmap includes reserved slots 0 and 1, which have no hash
         * entries. After shrinking, the larger arena can fill every hash
         * slot as well as retaining those two reserved slots. */
        valid &= snapshot.nodes_used <= snapshot.nodes_capacity + 2;
        valid &= snapshot.cache_used <= snapshot.cache_capacity;
        if (i % 4 == 0) CALL(sylvan_gc);
    }
    /* Always join outstanding work, even if an assertion will fail. */
    SYNC(statistics_work);
    mtbdd_refs_pop(2);
    test_assert(valid);
    return 0;
}

TASK_0(int, test_collection)
{
    const size_t count = 12000;
    MTBDD *garbage = malloc(count * sizeof(*garbage));
    test_assert(garbage != NULL);
    unsigned before = collections;
    for (size_t i=0; i<count; i++) garbage[i] = mtbdd_ref(mtbdd_int64((int64_t)i+100000));
    test_assert(collections > before); /* Automatic collection with small tables. */
    MTBDD bdd = mtbdd_refs_push(sylvan_ithvar(0xfffffe));
    ZDD zdd = zdd_refs_push(zdd_makenode(0xffffff, zdd_true, zdd_true));
    for (size_t i=0; i<count; i++) mtbdd_deref(garbage[i]);
    free(garbage);
    CALL(sylvan_gc);
    test_assert(MTBDD_GETINDEX(bdd) >= llmsset_get_size(nodes));
    test_assert(ZDD_GETINDEX(zdd) >= llmsset_get_size(nodes));
    test_assert(CALL(check_profile, &bdd, 1, 0) == 0);
    test_assert(CALL(check_profile, &zdd, 1, 1) == 0);
    for (unsigned i=0; i<4; i++) {
        SPAWN(check_profile, &bdd, 1, 0);
        CALL(sylvan_gc);
        test_assert(CALL(check_profile, &zdd, 1, 1) == 0);
        test_assert(SYNC(check_profile) == 0);
    }
    mtbdd_refs_pop(1);
    zdd_refs_pop(1);

    /* More levels and handles than the initial hash/stack capacities, with
     * periodic safepoints inside the traversal and a sparse 24-bit domain. */
    for (int family=0; family<2; family++) {
        uint64_t root = family ? zdd_true : mtbdd_true;
        if (family) zdd_refs_pushptr(&root); else mtbdd_refs_pushptr(&root);
        for (unsigned i=1500; i>0; i--) {
            root = family ? zdd_makenode(i*100, zdd_false, root) : mtbdd_makenode(i*100, mtbdd_false, root);
        }
        test_assert(CALL(check_profile, &root, 1, family) == 0);
#ifdef SYLVAN_TEST_STATISTICS_GC
        /* At the second safepoint, only the operation's own root references
         * protect this graph. GC happens after its visited set is populated. */
        if (family) zdd_refs_popptr(1); else mtbdd_refs_popptr(1);
        before = collections;
        test_statistics_gc_arm(2);
        size_t n;
        uint64_t leaves;
        int ok = family ? CALL(zdd_level_statistics, &root, 1, NULL, 0, &n, &leaves)
                        : CALL(mtbdd_level_statistics, &root, 1, NULL, 0, &n, &leaves);
        test_assert(ok && n == 1500 && leaves == (family && !ZDD_COMPLEMENT_EDGES ? 2u : 1u));
        test_assert(collections == before+1);
        if (family) zdd_refs_pushptr(&root); else mtbdd_refs_pushptr(&root);
        test_assert(CALL(check_profile, &root, 1, family) == 0);

        /* Fail every successive allocation, including hash-table resizes,
         * and verify atomic outputs and reference-stack cleanup. */
        int succeeded = 0;
        for (long fail=0; fail<100; fail++) {
            sylvan_level_statistic entry = {19, 23, 29};
            n = 31;
            leaves = 37;
            test_statistics_fail_after(fail);
            ok = family ? CALL(zdd_level_statistics, &root, 1, NULL, 0, &n, &leaves)
                        : CALL(mtbdd_level_statistics, &root, 1, NULL, 0, &n, &leaves);
            test_statistics_fail_after(-1);
            if (ok) { succeeded = 1; break; }
            test_assert(n == 31 && leaves == 37);
            test_statistics_fail_after(fail);
            ok = family ? CALL(zdd_level_statistics, &root, 1, &entry, 1, &n, &leaves)
                        : CALL(mtbdd_level_statistics, &root, 1, &entry, 1, &n, &leaves);
            test_statistics_fail_after(-1);
            test_assert(!ok && n == 31 && leaves == 37);
            test_assert(entry.level == 19 && entry.reserved == 23 && entry.node_count == 29);
        }
        test_assert(succeeded);
#endif
        if (family) zdd_refs_popptr(1); else mtbdd_refs_popptr(1);
    }
    return 0;
}

#ifdef SYLVAN_TEST_STATISTICS_GC
TASK_0(int, test_reference_reservation)
{
    for (int family=0; family<2; family++) {
        uint64_t saved = family ? zdd_refs_push(zdd_makenode(8, zdd_false, zdd_true))
                                : mtbdd_refs_push(sylvan_ithvar(8));
        uint64_t *(*inspect)(size_t*, size_t*) = family ? test_statistics_zdd_refs : test_statistics_mtbdd_refs;
        size_t used, capacity;
        uint64_t *begin = inspect(&used, &capacity);
        test_assert(used > 0 && begin[used-1] == saved);
        /* Filling the final slot also triggers legacy refs_push's growth. */
        size_t root_count = capacity - used;
        uint64_t *roots = malloc(root_count * sizeof(*roots));
        test_assert(roots != NULL);
        for (size_t i=0; i<root_count; i++) roots[i] = family ? zdd_true : mtbdd_true;
        for (long fail=0; fail<=1; fail++) {
            sylvan_level_statistic entry = {19, 23, 29};
            size_t count = 31;
            uint64_t leaves = 37;
            test_statistics_fail_after(fail);
            int ok = family ? CALL(zdd_level_statistics, roots, root_count, &entry, 1, &count, &leaves)
                            : CALL(mtbdd_level_statistics, roots, root_count, &entry, 1, &count, &leaves);
            test_statistics_fail_after(-1);
            test_assert(!ok && count == 31 && leaves == 37);
            test_assert(entry.level == 19 && entry.reserved == 23 && entry.node_count == 29);
            size_t current_used, current_capacity;
            uint64_t *current = inspect(&current_used, &current_capacity);
            test_assert(current_used == used && current[used-1] == saved);
            if (fail == 0) test_assert(current == begin && current_capacity == capacity);
            else test_assert(current_capacity > capacity);
            /* Existing references survive both the failed reservation and
             * a later traversal allocation failure after successful growth. */
            CALL(sylvan_gc);
            test_assert(CALL(check_profile, &saved, 1, family) == 0);
        }
        test_assert(CALL(check_profile, roots, root_count, family) == 0);
        size_t current_used, current_capacity;
        (void)inspect(&current_used, &current_capacity);
        test_assert(current_used == used);
        test_assert(!(family ? zdd_refs_reserve(SIZE_MAX) : mtbdd_refs_reserve(SIZE_MAX)));
        test_assert(inspect(&current_used, &current_capacity)[used-1] == saved && current_used == used);
        free(roots);
        if (family) zdd_refs_pop(1); else mtbdd_refs_pop(1);
    }
    return 0;
}
#endif

TASK_0(int, runtests)
{
#ifdef SYLVAN_TEST_STATISTICS_GC
    if (CALL(test_reference_reservation)) return 1;
#endif
    if (CALL(test_snapshots)) return 1;
    if (CALL(test_levels)) return 1;
    if (CALL(test_invalid)) return 1;
    if (CALL(test_collection)) return 1;
    if (CALL(test_concurrent_snapshot)) return 1;
    uint64_t value;
    test_assert(CALL(sylvan_statistics_read_counter, MTBDD_LEVEL_STATISTICS, &value));
    test_assert(SYLVAN_STATS ? value > 0 : value == 0);
    test_assert(CALL(sylvan_statistics_read_counter, ZDD_LEVEL_STATISTICS, &value));
    test_assert(SYLVAN_STATS ? value > 0 : value == 0);
    return 0;
}

int main(int argc, char **argv)
{
    workers = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    lace_start(workers, 0);
    sylvan_set_sizes(1<<10, 1<<16, 1<<10, 1<<14);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_zdd();
    sylvan_gc_hook_pregc(count_collection_CALL);
    int result = RUN(runtests);
    sylvan_quit();
    lace_stop();
    return result;
}
