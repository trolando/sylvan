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
    _Atomic(unsigned int) go;
    uint64_t indices[TEST_RACE_WORKERS];
    int created[TEST_RACE_WORKERS];
};

TASK(void, lookup_race_worker, struct lookup_race*, race, unsigned int, slot)

void
lookup_race_worker_CALL(
    lace_worker *lace, struct lookup_race *race, unsigned int slot)
{
    (void)lace;
    while (!atomic_load_explicit(&race->go, memory_order_acquire)) {
    }

    race->created[slot] = 0;
    race->indices[slot] = nodes_lookup(
        race->table, race->key, 0, &race->created[slot]);
}

TASK(int, run_lookup_race, struct lookup_race*, race)

int
run_lookup_race_CALL(lace_worker *lace, struct lookup_race *race)
{
    lookup_race_worker_SPAWN(lace, race, 0);
    lookup_race_worker_SPAWN(lace, race, 1);
    lookup_race_worker_SPAWN(lace, race, 2);

    atomic_store_explicit(&race->go, 1, memory_order_release);

    lookup_race_worker_CALL(lace, race, 3);
    lookup_race_worker_SYNC(lace);
    lookup_race_worker_SYNC(lace);
    lookup_race_worker_SYNC(lace);

    unsigned int created = 0;
    for (unsigned int i = 0; i < TEST_RACE_WORKERS; i++) {
        if (race->indices[i] <= 1 || race->indices[i] != race->indices[0]) {
            return 1;
        }
        if (race->created[i] == 1) created++;
    }

    return created == 1 ? 0 : 1;
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

int
main(void)
{
    lace_start(TEST_RACE_WORKERS, 0, 0);
    sylvan_init_hash();

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
            .go = 0,
            .indices = {0},
            .created = {0}
        };
        test_assert(run_lookup_race(&race) == 0);
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

    lace_stop();

    return 0;
}
