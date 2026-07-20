#include <stdio.h>
#include <stdint.h>

#include <lace.h>

#include <sylvan/hash.h>
#include <sylvan/platform.h>
#include <sylvan/nodes.h>

#include "test_assert.h"

#define TEST_TABLE_SIZE 4096
#define TEST_CACHE_LINE_WORDS (SYLVAN_CACHE_LINE_SIZE / sizeof(uint64_t))
#define TEST_COLLIDING_KEYS (TEST_CACHE_LINE_WORDS + 1)

static void
find_colliding_keys(uint64_t *keys)
{
    unsigned int counts[TEST_TABLE_SIZE / TEST_CACHE_LINE_WORDS] = {0};
    uint64_t candidates[TEST_TABLE_SIZE / TEST_CACHE_LINE_WORDS][TEST_COLLIDING_KEYS] = {{0}};

    for (uint64_t value = 2;; value++) {
        const uint64_t key = UINT64_C(0x4000000000000000) | value;
        const uint64_t hash =
            sylvan_tabhash16(key, 0, UINT64_C(14695981039346656037));
        const unsigned int group =
            (unsigned int)((hash & (TEST_TABLE_SIZE - 1)) /
                           TEST_CACHE_LINE_WORDS);
        if (counts[group] < TEST_COLLIDING_KEYS) {
            candidates[group][counts[group]++] = key;
            if (counts[group] == TEST_COLLIDING_KEYS) {
                for (unsigned int i = 0; i < TEST_COLLIDING_KEYS; i++) {
                    keys[i] = candidates[group][i];
                }
                return;
            }
        }
    }
}

int
main(void)
{
    lace_start(1, 0, 0);
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
    lace_stop();

    return 0;
}
