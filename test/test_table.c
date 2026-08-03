#include <stdio.h>
#include <stdint.h>

#include <lace.h>

#include <sylvan/hash.h>
#include <sylvan/platform.h>
#include <sylvan/nodes.h>

#include "test_assert.h"

#define TEST_TABLE_SIZE 512
#define TEST_CACHE_LINE_WORDS (SYLVAN_CACHE_LINE_SIZE / sizeof(uint64_t))
#define TEST_COLLIDING_KEYS (TEST_CACHE_LINE_WORDS + 1)
#define TEST_RACE_WORKERS 4
#define TEST_RACE_KEYS 128
#define TEST_RACE_TABLE_SIZE 4096
#define TEST_DEEP_CHAIN_KEYS 200
#define TEST_CUSTOM_RACE_KEYS 32

static void
find_colliding_keys(uint64_t *keys)
{
    size_t target = SIZE_MAX;
    unsigned int count = 0;

    for (uint64_t value = 2;; value++) {
        const uint64_t key = UINT64_C(0x4000000000000000) | value;
        const uint64_t hash =
            sylvan_tabhash16(key, 0, UINT64_C(14695981039346656037));
        const size_t bucket = (size_t)(hash & (TEST_TABLE_SIZE - 1));

        if (target == SIZE_MAX) target = bucket;
        if (bucket == target) {
            keys[count++] = key;
            if (count == TEST_COLLIDING_KEYS) return;
        }
    }
}

struct lookup_race {
    nodes_table *table;
    uint64_t key;
    _Atomic(unsigned int) arrived;
    uint64_t indices[TEST_RACE_WORKERS];
    int created[TEST_RACE_WORKERS];
};

TASK(void, lookup_race_worker, struct lookup_race*, race)

void
lookup_race_worker_CALL(lace_worker *lace, struct lookup_race *race)
{
    const unsigned int slot = (unsigned int)lace->worker;
    atomic_fetch_add_explicit(&race->arrived, 1, memory_order_acq_rel);
    while (atomic_load_explicit(
               &race->arrived, memory_order_acquire) < TEST_RACE_WORKERS) {
    }

    race->created[slot] = 0;
    race->indices[slot] = nodes_lookup(
        race->table, race->key, 0, &race->created[slot]);
}

static uint64_t
custom_hash(uint64_t a, uint64_t b, uint64_t seed)
{
    return sylvan_tabhash16(a, b, seed);
}

static int
custom_equals(uint64_t lhs_a, uint64_t lhs_b, uint64_t rhs_a, uint64_t rhs_b)
{
    return lhs_a == rhs_a && lhs_b == rhs_b;
}

static int
custom_create_failure(uint64_t *a, uint64_t *b)
{
    (void)a;
    (void)b;
    return -7;
}

static void
custom_destroy(uint64_t a, uint64_t b)
{
    (void)a;
    (void)b;
}

static size_t custom_destroy_count;

static int
custom_create(uint64_t *a, uint64_t *b)
{
    (void)a;
    (void)b;
    return 0;
}

static void
custom_destroy_counted(uint64_t a, uint64_t b)
{
    (void)a;
    (void)b;
    custom_destroy_count++;
}

static uint64_t
custom_hash_degenerate(uint64_t a, uint64_t b, uint64_t seed)
{
    (void)a;
    (void)b;
    (void)seed;
    return UINT64_C(0xabcdef0000000037);
}

static uint64_t
custom_hash_by_a(uint64_t a, uint64_t b, uint64_t seed)
{
    (void)b;
    return sylvan_tabhash16(a, 0, seed);
}

static int
custom_equals_by_a(
    uint64_t lhs_a, uint64_t lhs_b, uint64_t rhs_a, uint64_t rhs_b)
{
    (void)lhs_b;
    (void)rhs_b;
    return lhs_a == rhs_a;
}

static _Atomic(size_t) custom_race_create_count;
static _Atomic(size_t) custom_race_destroy_count;

static int
custom_race_create(uint64_t *a, uint64_t *b)
{
    (void)a;
    (void)b;
    atomic_fetch_add_explicit(
        &custom_race_create_count, 1, memory_order_relaxed);
    return 0;
}

static void
custom_race_destroy(uint64_t a, uint64_t b)
{
    (void)a;
    (void)b;
    atomic_fetch_add_explicit(
        &custom_race_destroy_count, 1, memory_order_relaxed);
}

struct custom_lookup_race {
    nodes_table *table;
    _Atomic(unsigned int) arrived;
    uint64_t indices[TEST_RACE_WORKERS][TEST_CUSTOM_RACE_KEYS];
    int created[TEST_RACE_WORKERS][TEST_CUSTOM_RACE_KEYS];
};

TASK(void, custom_lookup_race_worker, struct custom_lookup_race*, race)

void
custom_lookup_race_worker_CALL(
    lace_worker *lace, struct custom_lookup_race *race)
{
    const unsigned int slot = (unsigned int)lace->worker;
    atomic_fetch_add_explicit(&race->arrived, 1, memory_order_acq_rel);
    while (atomic_load_explicit(
               &race->arrived, memory_order_acquire) < TEST_RACE_WORKERS) {
    }

    for (uint64_t i = 0; i < TEST_CUSTOM_RACE_KEYS; i++) {
        race->created[slot][i] = 0;
        race->indices[slot][i] = nodes_lookupc(
            race->table, UINT64_C(0x4000000000000000) | i, i,
            &race->created[slot][i]);
    }
}

static int
test_deep_custom_chain(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    nodes_set_custom(
        table, custom_hash_degenerate, custom_equals,
        custom_create, custom_destroy);

    uint64_t indices[TEST_DEEP_CHAIN_KEYS];
    for (uint64_t i = 0; i < TEST_DEEP_CHAIN_KEYS; i++) {
        int created = 0;
        indices[i] = nodes_lookupc(
            table, UINT64_C(0x4000000000000000) | i, i, &created);
        test_assert(indices[i] > 1 && created == 1);
    }

    for (uint64_t i = 0; i < TEST_DEEP_CHAIN_KEYS; i++) {
        int created = 1;
        test_assert(nodes_lookupc(
            table, UINT64_C(0x4000000000000000) | i, i,
            &created) == indices[i]);
        test_assert(created == 0);
    }

    nodes_clear(table);
    for (size_t i = 0; i < TEST_DEEP_CHAIN_KEYS; i++) {
        nodes_mark_rec(table, indices[i]);
    }
    test_assert(nodes_rebuild(table) == 0);
    for (uint64_t i = 0; i < TEST_DEEP_CHAIN_KEYS; i++) {
        int created = 1;
        test_assert(nodes_lookupc(
            table, UINT64_C(0x4000000000000000) | i, i,
            &created) == indices[i]);
        test_assert(created == 0);
    }

    nodes_free(table);
    return 0;
}

static int
test_custom_equality(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    nodes_set_custom(
        table, custom_hash_by_a, custom_equals_by_a,
        custom_create, custom_destroy);

    int created = 0;
    const uint64_t first = nodes_lookupc(
        table, UINT64_C(0x4000000000000007), 111, &created);
    test_assert(first > 1 && created == 1);

    created = 1;
    test_assert(nodes_lookupc(
        table, UINT64_C(0x4000000000000007), 222, &created) == first);
    test_assert(created == 0);

    const uint64_t *data = nodes_get_pointer(table, first);
    test_assert(data[0] == UINT64_C(0x4000000000000007));
    test_assert(data[1] == 111);
    nodes_free(table);
    return 0;
}

static int
test_custom_metadata_cleanup(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    nodes_set_custom(
        table, custom_hash_by_a, custom_equals_by_a,
        custom_create, NULL);

    int created = 0;
    const uint64_t custom_index = nodes_lookupc(
        table, UINT64_C(0x4000000000000001), 111, &created);
    test_assert(custom_index > 1 && created == 1);

    nodes_clear(table);
    nodes_cleanup_custom(table);
    test_assert(nodes_rebuild(table) == 0);

    created = 0;
    const uint64_t plain_index = nodes_lookup(
        table, UINT64_C(0x4000000000000002), 222, &created);
    test_assert(plain_index == custom_index && created == 1);

    nodes_clear(table);
    nodes_mark_rec(table, plain_index);
    nodes_cleanup_custom(table);
    test_assert(nodes_rebuild(table) == 0);

    created = 1;
    test_assert(nodes_lookup(
        table, UINT64_C(0x4000000000000002), 222,
        &created) == plain_index);
    test_assert(created == 0);
    test_assert(nodes_count_nodes(table) == 3);
    nodes_free(table);
    return 0;
}

static int
test_custom_race(void)
{
    nodes_table *table = nodes_create(
        TEST_RACE_TABLE_SIZE, TEST_RACE_TABLE_SIZE);
    nodes_set_custom(
        table, custom_hash_degenerate, custom_equals,
        custom_race_create, custom_race_destroy);
    atomic_store_explicit(
        &custom_race_create_count, 0, memory_order_relaxed);
    atomic_store_explicit(
        &custom_race_destroy_count, 0, memory_order_relaxed);

    struct custom_lookup_race race = {
        .table = table,
        .arrived = 0,
        .indices = {{0}},
        .created = {{0}}
    };
    custom_lookup_race_worker_TOGETHER(&race);

    size_t winners = 0;
    for (size_t key = 0; key < TEST_CUSTOM_RACE_KEYS; key++) {
        size_t key_winners = 0;
        for (size_t worker = 0; worker < TEST_RACE_WORKERS; worker++) {
            test_assert(race.indices[worker][key] > 1);
            test_assert(race.indices[worker][key] == race.indices[0][key]);
            if (race.created[worker][key] == 1) key_winners++;
        }
        test_assert(key_winners == 1);
        winners += key_winners;
    }
    test_assert(winners == TEST_CUSTOM_RACE_KEYS);

    const size_t creates = atomic_load_explicit(
        &custom_race_create_count, memory_order_relaxed);
    test_assert(creates >= winners);
    test_assert(atomic_load_explicit(
        &custom_race_destroy_count, memory_order_relaxed) ==
        creates - winners);

    nodes_clear(table);
    nodes_cleanup_custom(table);
    test_assert(atomic_load_explicit(
        &custom_race_destroy_count, memory_order_relaxed) == creates);
    nodes_free(table);
    return 0;
}

static int
test_exhaustion_and_resize(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    size_t inserted = 0;
    for (uint64_t i = 0; i < TEST_TABLE_SIZE; i++) {
        int created = 0;
        if (nodes_lookup(
                table, UINT64_C(0x4000000000000000) | i, i,
                &created) == 0) {
            break;
        }
        test_assert(created == 1);
        inserted++;
    }
    test_assert(inserted == TEST_TABLE_SIZE - 2);
    test_assert(nodes_count_nodes(table) == TEST_TABLE_SIZE);
    nodes_free(table);

    enum { resize_nodes = 400 };
    uint64_t indices[resize_nodes];
    table = nodes_create(TEST_TABLE_SIZE, TEST_RACE_TABLE_SIZE);
    for (uint64_t i = 0; i < resize_nodes; i++) {
        int created = 0;
        indices[i] = nodes_lookup(
            table, UINT64_C(0x4000000000000000) | i, i, &created);
        test_assert(indices[i] > 1 && created == 1);
    }

    nodes_clear(table);
    for (size_t i = 0; i < resize_nodes; i++) {
        nodes_mark_rec(table, indices[i]);
    }
    nodes_set_size(table, TEST_RACE_TABLE_SIZE);
    test_assert(nodes_get_size(table) == TEST_RACE_TABLE_SIZE);
    test_assert(nodes_rebuild(table) == 0);
    for (uint64_t i = 0; i < resize_nodes; i++) {
        int created = 1;
        test_assert(nodes_lookup(
            table, UINT64_C(0x4000000000000000) | i, i,
            &created) == indices[i]);
        test_assert(created == 0);
    }
    nodes_free(table);
    return 0;
}

TASK(int, run_table_tests)

int
run_table_tests_CALL(lace_worker *lace)
{
    (void)lace;
    sylvan_init_hash();

    const size_t bitmap_bytes =
        2 * ((TEST_TABLE_SIZE + 63) / 64) * sizeof(uint64_t);
    const size_t region_bytes = sizeof(uint64_t);
    const size_t capacity_bytes = nodes_get_capacity_bytes(TEST_TABLE_SIZE);
    test_assert(
        capacity_bytes == TEST_TABLE_SIZE * 24 + bitmap_bytes + region_bytes ||
        capacity_bytes == TEST_TABLE_SIZE * 32 + bitmap_bytes + region_bytes ||
        capacity_bytes == TEST_TABLE_SIZE * 40 + bitmap_bytes + region_bytes);

    uint64_t *memory = sylvan_alloc_aligned(4096);
    test_assert(memory != NULL);
    for (size_t i=0; i<512; i++) memory[i] = UINT64_MAX;
    sylvan_clear_aligned(memory, 4096);
    for (size_t i=0; i<512; i++) test_assert(memory[i] == 0);
    sylvan_free_aligned(memory, 4096);

    uint64_t keys[TEST_COLLIDING_KEYS];
    uint64_t indices[TEST_COLLIDING_KEYS];
    find_colliding_keys(keys);

    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    for (unsigned int i = 0; i < TEST_COLLIDING_KEYS; i++) {
        int created = 0;
        indices[i] = nodes_lookup(table, keys[i], 0, &created);
        test_assert(indices[i] > 1 && created);
    }

    nodes_clear(table);
    for (unsigned int i = 0; i < TEST_COLLIDING_KEYS; i++) {
        test_assert(!nodes_is_marked(table, indices[i]));
        nodes_mark_rec(table, indices[i]);
        test_assert(nodes_is_marked(table, indices[i]));
    }
    test_assert(nodes_rebuild(table) == 0);
    for (unsigned int i = 0; i < TEST_COLLIDING_KEYS; i++) {
        int created = 0;
        test_assert(nodes_lookup(table, keys[i], 0, &created) == indices[i]);
        test_assert(!created);
    }

    nodes_free(table);

    table = nodes_create(TEST_RACE_TABLE_SIZE, TEST_RACE_TABLE_SIZE);
    uint64_t race_indices[TEST_RACE_KEYS];
    for (uint64_t i = 0; i < TEST_RACE_KEYS; i++) {
        struct lookup_race race = {
            .table = table,
            .key = UINT64_C(0x4000000000000000) | (UINT64_C(1) << 32) | i,
            .arrived = 0,
            .indices = {0},
            .created = {0}
        };
        lookup_race_worker_TOGETHER(&race);
        unsigned int created = 0;
        for (unsigned int worker = 0;
             worker < TEST_RACE_WORKERS; worker++) {
            test_assert(race.indices[worker] > 1);
            test_assert(race.indices[worker] == race.indices[0]);
            const uint64_t *data = nodes_get_pointer(
                table, race.indices[worker]);
            test_assert(data[0] == race.key && data[1] == 0);
            if (race.created[worker] == 1) created++;
        }
        test_assert(created == 1);
        race_indices[i] = race.indices[0];
    }
    test_assert(nodes_count_nodes(table) == TEST_RACE_KEYS + 2);

    nodes_clear(table);
    for (size_t i = 0; i < TEST_RACE_KEYS; i++) {
        nodes_mark_rec(table, race_indices[i]);
    }
    test_assert(nodes_rebuild(table) == 0);
    for (uint64_t i = 0; i < TEST_RACE_KEYS; i++) {
        int race_created = 0;
        const uint64_t key =
            UINT64_C(0x4000000000000000) | (UINT64_C(1) << 32) | i;
        test_assert(nodes_lookup(table, key, 0, &race_created) == race_indices[i]);
        test_assert(!race_created);
    }
    nodes_free(table);

    table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    nodes_set_custom(
        table, custom_hash, custom_equals,
        custom_create_failure, custom_destroy);
    const size_t nodes_before_failure = nodes_count_nodes(table);
    int created = 0;
    test_assert(nodes_lookupc(table, 42, 24, &created) == 0);
    test_assert(created == -7);
    test_assert(nodes_count_nodes(table) == nodes_before_failure);
    nodes_free(table);

    enum { custom_node_count = 128 };
    uint64_t custom_indices[custom_node_count];
    table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    nodes_set_custom(
        table, custom_hash, custom_equals,
        custom_create, custom_destroy_counted);
    for (uint64_t i = 0; i < custom_node_count; i++) {
        created = 0;
        custom_indices[i] = nodes_lookupc(
            table, UINT64_C(0x4000000000000000) | i, i, &created);
        test_assert(custom_indices[i] > 1 && created == 1);
    }

    nodes_clear(table);
    for (size_t i = 0; i < custom_node_count; i += 2) {
        nodes_mark_rec(table, custom_indices[i]);
    }
    custom_destroy_count = 0;
    nodes_cleanup_custom(table);
    test_assert(custom_destroy_count == custom_node_count / 2);

    nodes_clear(table);
    nodes_cleanup_custom(table);
    test_assert(custom_destroy_count == custom_node_count);
    nodes_free(table);

    if (test_deep_custom_chain()) return 1;
    if (test_custom_equality()) return 1;
    if (test_custom_metadata_cleanup()) return 1;
    if (test_custom_race()) return 1;
    if (test_exhaustion_and_resize()) return 1;

    return 0;
}

int
main(void)
{
    lace_start(TEST_RACE_WORKERS, 0, 0);
    const int result = run_table_tests();

    lace_stop();
    return result;
}
