#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if defined(__unix__) || defined(__APPLE__)
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <lace.h>

#include <sylvan/common.h>
#include <sylvan/hash.h>
#include <sylvan/platform.h>
#include <sylvan/nodes.h>

#include "test_assert.h"

#define TEST_TABLE_SIZE 512
#define TEST_CACHE_LINE_WORDS (SYLVAN_CACHE_LINE_SIZE / sizeof(uint64_t))
#define TEST_COLLIDING_KEYS (TEST_CACHE_LINE_WORDS + 1)
#define TEST_MAX_WORKERS 64
#define TEST_DEFAULT_WORKERS 4
#define TEST_RACE_KEYS 128
#define TEST_RACE_TABLE_SIZE 4096
#define TEST_DEEP_CHAIN_KEYS 200
#define TEST_CUSTOM_RACE_KEYS 32

/* Number of Lace workers this run was started with. */
static unsigned int test_workers = TEST_DEFAULT_WORKERS;

/* argv[0], so a death test can re-execute this binary. */
static const char *test_self;

/* Nodes are handed out per 512-bucket region, one region per worker at a
   time, so a table used concurrently needs at least that much room per
   worker. Some backends require a power of two, so round up. */
static size_t
race_table_size(void)
{
    size_t size = TEST_RACE_TABLE_SIZE;
    while (size < (size_t)test_workers * 512) size *= 2;
    return size;
}

/**
 * Fill <keys> with <count> leaf keys whose hashes agree modulo <mask> + 1.
 *
 * Agreeing on a power-of-two mask implies agreeing on every smaller one, so
 * passing the table size makes the keys share a bucket in llcht and in the
 * (smaller) llchst head array alike; in list and trie every leaf shares the
 * family anchored at handle 1 regardless.
 */
static void
find_colliding_keys_n(uint64_t *keys, unsigned int count, uint64_t mask)
{
    size_t target = SIZE_MAX;
    unsigned int found = 0;

    for (uint64_t value = 2;; value++) {
        const uint64_t key = UINT64_C(0x4000000000000000) | value;
        const uint64_t hash =
            sylvan_tabhash16(key, 0, UINT64_C(14695981039346656037));
        const size_t bucket = (size_t)(hash & mask);

        if (target == SIZE_MAX) target = bucket;
        if (bucket == target) {
            keys[found++] = key;
            if (found == count) return;
        }
    }
}

static void
find_colliding_keys(uint64_t *keys)
{
    find_colliding_keys_n(keys, TEST_COLLIDING_KEYS, TEST_TABLE_SIZE - 1);
}

struct lookup_race {
    nodes_table *table;
    uint64_t key;
    _Atomic(unsigned int) arrived;
    uint64_t indices[TEST_MAX_WORKERS];
    int created[TEST_MAX_WORKERS];
};

TASK(void, lookup_race_worker, struct lookup_race*, race)

void
lookup_race_worker_CALL(lace_worker *lace, struct lookup_race *race)
{
    const unsigned int slot = (unsigned int)lace->worker;
    atomic_fetch_add_explicit(&race->arrived, 1, memory_order_acq_rel);
    while (atomic_load_explicit(
               &race->arrived, memory_order_acquire) < test_workers) {
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
    uint64_t indices[TEST_MAX_WORKERS][TEST_CUSTOM_RACE_KEYS];
    int created[TEST_MAX_WORKERS][TEST_CUSTOM_RACE_KEYS];
};

TASK(void, custom_lookup_race_worker, struct custom_lookup_race*, race)

void
custom_lookup_race_worker_CALL(
    lace_worker *lace, struct custom_lookup_race *race)
{
    const unsigned int slot = (unsigned int)lace->worker;
    atomic_fetch_add_explicit(&race->arrived, 1, memory_order_acq_rel);
    while (atomic_load_explicit(
               &race->arrived, memory_order_acquire) < test_workers) {
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
    const size_t table_size = race_table_size();
    nodes_table *table = nodes_create(table_size, table_size);
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
        for (size_t worker = 0; worker < test_workers; worker++) {
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

/**
 * Rebuild stress test with adversarial node shapes.
 *
 * The shapes are chosen to maximise structural interference during a
 * (possibly parallel) rebuild:
 *   - many leaves, which all share a single canonical family;
 *   - a long chain of nodes that all share one maximum child, so they all
 *     land in the same family and form a deep chain;
 *   - nodes anchored on members of that deep chain, so a family root and the
 *     interior of another family end up sharing branch slots;
 *   - a wide fan of nodes with pairwise distinct maximum children;
 *   - custom (leaf) nodes interleaved with plain ones, so the rebuild has to
 *     consult the per-bucket custom bitmap.
 *
 * After clear/mark/rebuild every node must still be found at its original
 * index without being recreated, and the node count must be unchanged.
 */
#define TEST_SHAPE_TABLE_SIZE 65536
#define TEST_SHAPE_LEAVES 512
#define TEST_SHAPE_CUSTOM_LEAVES 256
#define TEST_SHAPE_CHAIN 1024
#define TEST_SHAPE_SUBANCHORS 48
#define TEST_SHAPE_SUBFAN 64
#define TEST_SHAPE_TOTAL_LEAVES (TEST_SHAPE_LEAVES + TEST_SHAPE_CUSTOM_LEAVES)
#define TEST_SHAPE_MAX_NODES \
    (2 * TEST_SHAPE_TOTAL_LEAVES + TEST_SHAPE_CHAIN + \
     TEST_SHAPE_SUBANCHORS * TEST_SHAPE_SUBFAN)

struct shape_node {
    uint64_t a;
    uint64_t b;
    uint64_t index;
    int custom;
};

struct shape_set {
    struct shape_node nodes[TEST_SHAPE_MAX_NODES];
    size_t count;
    size_t roots[TEST_SHAPE_MAX_NODES];
    size_t root_count;
};

/* Internal nodes carry a tag above the 40-bit handle so that distinct nodes
   can share the same children; the table only looks at the low 40 bits. */
#define SHAPE_INTERNAL(tag, lo) (((uint64_t)(tag) << 40) | (uint64_t)(lo))

static int
shape_add(struct shape_set *set, nodes_table *table,
          uint64_t a, uint64_t b, int custom, int is_root)
{
    test_assert(set->count < TEST_SHAPE_MAX_NODES);
    int created = 0;
    const uint64_t index = custom
        ? nodes_lookupc(table, a, b, &created)
        : nodes_lookup(table, a, b, &created);
    test_assert(index > 1);
    test_assert(created == 1);
    struct shape_node *node = &set->nodes[set->count];
    node->a = a;
    node->b = b;
    node->index = index;
    node->custom = custom;
    if (is_root) set->roots[set->root_count++] = set->count;
    set->count++;
    return 0;
}

static int
shape_build(struct shape_set *set, nodes_table *table)
{
    set->count = 0;
    set->root_count = 0;

    /* Leaves: every leaf shares the same canonical family. */
    for (uint64_t i = 0; i < TEST_SHAPE_LEAVES; i++) {
        if (shape_add(set, table,
                      UINT64_C(0x4000000000000000) | i, i, 0, 0)) return 1;
    }
    /* Custom leaves, interleaved into the same family. */
    for (uint64_t i = 0; i < TEST_SHAPE_CUSTOM_LEAVES; i++) {
        if (shape_add(set, table,
                      UINT64_C(0x4000000000010000) | i, i, 1, 0)) return 1;
    }

    /* The hub is the leaf with the largest handle, so that it is the maximum
       child of every node in the deep chain below. */
    size_t hub_pos = 0;
    size_t low_pos = 0;
    for (size_t i = 1; i < TEST_SHAPE_TOTAL_LEAVES; i++) {
        if (set->nodes[i].index > set->nodes[hub_pos].index) hub_pos = i;
        if (set->nodes[i].index < set->nodes[low_pos].index) low_pos = i;
    }
    const uint64_t hub = set->nodes[hub_pos].index;
    const uint64_t low = set->nodes[low_pos].index;
    const size_t chain_first = set->count;

    /* Deep chain: every node has `hub` as its maximum child. */
    for (uint64_t i = 0; i < TEST_SHAPE_CHAIN; i++) {
        size_t pos = (size_t)i % TEST_SHAPE_TOTAL_LEAVES;
        if (pos == hub_pos) pos = (pos + 1) % TEST_SHAPE_TOTAL_LEAVES;
        const uint64_t lo = set->nodes[pos].index;
        test_assert(lo < hub);
        if (shape_add(set, table, SHAPE_INTERNAL(i + 1, hub), lo, 0, 1)) {
            return 1;
        }
    }

    /* Families anchored on members of the deep chain: these share branch
       slots with the interior of the chain above. */
    for (uint64_t i = 0; i < TEST_SHAPE_SUBANCHORS; i++) {
        const uint64_t anchor = set->nodes[chain_first + i].index;
        for (uint64_t k = 0; k < TEST_SHAPE_SUBFAN; k++) {
            const uint64_t lo =
                set->nodes[(size_t)k % TEST_SHAPE_TOTAL_LEAVES].index;
            test_assert(lo < anchor);
            if (shape_add(set, table,
                          SHAPE_INTERNAL(k + 1, anchor), lo, 0, 1)) {
                return 1;
            }
        }
    }

    /* Wide fan: pairwise distinct maximum children, one per leaf. This also
       makes every leaf reachable from a root, so marking covers them all. */
    for (size_t i = 0; i < TEST_SHAPE_TOTAL_LEAVES; i++) {
        if (i == low_pos) continue;
        const uint64_t anchor = set->nodes[i].index;
        test_assert(anchor > low);
        if (shape_add(set, table, SHAPE_INTERNAL(0, anchor), low, 0, 1)) {
            return 1;
        }
    }

    return 0;
}

static int
shape_verify(const struct shape_set *set, nodes_table *table)
{
    for (size_t i = 0; i < set->count; i++) {
        const struct shape_node *node = &set->nodes[i];
        int created = 1;
        const uint64_t found = node->custom
            ? nodes_lookupc(table, node->a, node->b, &created)
            : nodes_lookup(table, node->a, node->b, &created);
        test_assert(found == node->index);
        test_assert(created == 0);
    }
    test_assert(nodes_count_nodes(table) == set->count + 2);
    return 0;
}

static int
test_rebuild_shapes(void)
{
    static struct shape_set set;
    nodes_table *table = nodes_create(
        TEST_SHAPE_TABLE_SIZE, TEST_SHAPE_TABLE_SIZE);
    nodes_set_custom(
        table, custom_hash, custom_equals, custom_create, custom_destroy);

    if (shape_build(&set, table)) return 1;
    if (shape_verify(&set, table)) return 1;

    /* Repeat the collect cycle: each rebuild may lay out the structure
       differently, so run it several times over the same node set. */
    for (int round = 0; round < 8; round++) {
        nodes_clear(table);
        for (size_t i = 0; i < set.root_count; i++) {
            nodes_mark_rec(table, set.nodes[set.roots[i]].index);
        }
        nodes_cleanup_custom(table);
        test_assert(nodes_rebuild(table) == 0);
        if (shape_verify(&set, table)) return 1;
    }

    nodes_free(table);
    return 0;
}

#if SYLVAN_NODES_TABLE_LLCHST
static int
test_subtable_routing(void)
{
    nodes_table *table = nodes_create(
        TEST_RACE_TABLE_SIZE, TEST_RACE_TABLE_SIZE);
    const uint64_t a = UINT64_C(0x4000000000000042);
    const uint64_t b = UINT64_C(0x1234);

    int created = 0;
    const uint64_t at_seven = nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(7), a, b, &created);
    test_assert(at_seven > 1 && created == 1);

    created = 1;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(7), a, b,
        &created) == at_seven);
    test_assert(created == 0);

    created = 0;
    const uint64_t at_eight = nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(8), a, b, &created);
    test_assert(at_eight > 1 && at_eight != at_seven && created == 1);

    created = 0;
    const uint64_t at_sparse = nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(1000000), a, b, &created);
    test_assert(
        at_sparse > 1 && at_sparse != at_seven && at_sparse != at_eight &&
        created == 1);

    created = 0;
    const uint64_t at_leaf = nodes_lookup_subtable(
        table, NODES_SUBTABLE_LEAF, a, b, &created);
    test_assert(at_leaf > 1 && at_leaf != at_sparse && created == 1);

    created = 0;
    const uint64_t at_map = nodes_lookup_subtable(
        table, NODES_SUBTABLE_MAP, a, b, &created);
    test_assert(at_map > 1 && at_map != at_leaf && created == 1);

    /* Populate an additional route, then let GC discard it entirely. */
    for (uint64_t i = 0; i < 700; i++) {
        created = 0;
        test_assert(nodes_lookup_subtable(
            table, NODES_SUBTABLE_DECISION(9),
            UINT64_C(0x4000000000010000) | i, i,
            &created) > 1);
        test_assert(created == 1);
    }

    nodes_clear(table);
    nodes_mark_rec(table, at_seven);
    nodes_mark_rec(table, at_eight);
    nodes_mark_rec(table, at_sparse);
    nodes_mark_rec(table, at_leaf);
    nodes_mark_rec(table, at_map);
    test_assert(nodes_rebuild(table) == 0);

    created = 1;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(7), a, b,
        &created) == at_seven);
    test_assert(created == 0);
    created = 1;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(8), a, b,
        &created) == at_eight);
    test_assert(created == 0);
    created = 1;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(1000000), a, b,
        &created) == at_sparse);
    test_assert(created == 0);
    created = 1;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_LEAF, a, b,
        &created) == at_leaf);
    test_assert(created == 0);
    created = 1;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_MAP, a, b,
        &created) == at_map);
    test_assert(created == 0);

    created = 0;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(9),
        UINT64_C(0x4000000000010000), 0,
        &created) > 1);
    test_assert(created == 1);

    nodes_free(table);
    return 0;
}

/**
 * A case-B rewrite that changes the variable identity moves the node from one
 * decision subtable to another, which llchst has to derive from the rewritten
 * record alone. An entry that was explicitly routed to a subtable other than
 * its own identity cannot be derived that way and must be refused.
 */
static int
test_reorder_subtable_migration(void)
{
    nodes_table *table = nodes_create(
        TEST_RACE_TABLE_SIZE, TEST_RACE_TABLE_SIZE);
    if ((nodes_reorder_caps(table) & NODES_CAP_REMOVE) == 0) {
        nodes_free(table);
        return 0;
    }

    uint64_t leaf[2];
    for (uint64_t i = 0; i < 2; i++) {
        int created = 0;
        leaf[i] = nodes_lookup(
            table, UINT64_C(0x4000000000000000) | i, i, &created);
        test_assert(leaf[i] > 1 && created == 1);
    }

    const uint64_t old_b = (UINT64_C(7) << 40) | leaf[1];
    const uint64_t new_b = (UINT64_C(9) << 40) | leaf[1];

    int created = 0;
    const uint64_t index = nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(7), leaf[0], old_b, &created);
    test_assert(index > 1 && created == 1);

    test_assert(nodes_remove(table, index) == SYLVAN_OK);
    uint64_t *record = nodes_get_pointer(table, index);
    record[1] = new_b;
    test_assert(nodes_reinsert(table, index) == SYLVAN_OK);

    created = 1;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(9), leaf[0], new_b,
        &created) == index);
    test_assert(created == 0);

    created = 0;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(7), leaf[0], old_b,
        &created) != index);
    test_assert(created == 1);

    /* Routed to subtable 3 while its identity field says 4: the record does
       not record where it went, so removal must refuse instead of guess. */
    created = 0;
    const uint64_t raw = nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(3), leaf[0],
        (UINT64_C(4) << 40) | leaf[1], &created);
    test_assert(raw > 1 && created == 1);
    test_assert(nodes_remove(table, raw) == SYLVAN_ERR_INVALID);
    created = 1;
    test_assert(nodes_lookup_subtable(
        table, NODES_SUBTABLE_DECISION(3), leaf[0],
        (UINT64_C(4) << 40) | leaf[1], &created) == raw);
    test_assert(created == 0);

    nodes_free(table);
    return 0;
}
#endif

/**
 * Remove, verify the key misses, reinsert the unchanged record: the node must
 * come back at the same index, and be found rather than recreated.
 */
static int
test_reorder_remove_reinsert(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    if ((nodes_reorder_caps(table) & NODES_CAP_REMOVE) == 0) {
        nodes_free(table);
        return 0;
    }

    const uint64_t a = UINT64_C(0x4000000000000011);
    const uint64_t b = 17;
    int created = 0;
    const uint64_t index = nodes_lookup(table, a, b, &created);
    test_assert(index > 1 && created == 1);

    test_assert(nodes_remove(table, index) == SYLVAN_OK);

    /* The key now misses: the lookup creates a second node for it. */
    created = 0;
    const uint64_t duplicate = nodes_lookup(table, a, b, &created);
    test_assert(duplicate > 1 && duplicate != index && created == 1);
    test_assert(nodes_remove(table, duplicate) == SYLVAN_OK);

    test_assert(nodes_reinsert(table, index) == SYLVAN_OK);
    created = 1;
    test_assert(nodes_lookup(table, a, b, &created) == index);
    test_assert(created == 0);

    nodes_free(table);
    return 0;
}

/**
 * Remove the first, a middle and the last node of one shared chain. The
 * backends order a chain differently (llcht prepends, list appends, the trie
 * shapes a tree), so taking those three positions covers head, interior and
 * tail whichever convention applies.
 */
static int
test_reorder_chain_positions(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    if ((nodes_reorder_caps(table) & NODES_CAP_REMOVE) == 0) {
        nodes_free(table);
        return 0;
    }

    uint64_t keys[TEST_COLLIDING_KEYS];
    uint64_t indices[TEST_COLLIDING_KEYS];
    find_colliding_keys(keys);
    for (unsigned int i = 0; i < TEST_COLLIDING_KEYS; i++) {
        int created = 0;
        indices[i] = nodes_lookup(table, keys[i], 0, &created);
        test_assert(indices[i] > 1 && created == 1);
    }

    const unsigned int victims[3] = {
        0, TEST_COLLIDING_KEYS / 2, TEST_COLLIDING_KEYS - 1
    };
    for (unsigned int k = 0; k < 3; k++) {
        test_assert(nodes_remove(table, indices[victims[k]]) == SYLVAN_OK);
    }

    for (unsigned int i = 0; i < TEST_COLLIDING_KEYS; i++) {
        int is_victim = 0;
        for (unsigned int k = 0; k < 3; k++) {
            if (victims[k] == i) is_victim = 1;
        }
        int created = 1;
        const uint64_t found = nodes_lookup(table, keys[i], 0, &created);
        if (is_victim) {
            /* Gone from the index: recreated at a fresh slot. */
            test_assert(created == 1 && found != indices[i]);
            test_assert(nodes_remove(table, found) == SYLVAN_OK);
        } else {
            test_assert(created == 0 && found == indices[i]);
        }
    }

    for (unsigned int k = 0; k < 3; k++) {
        test_assert(nodes_reinsert(table, indices[victims[k]]) == SYLVAN_OK);
    }
    for (unsigned int i = 0; i < TEST_COLLIDING_KEYS; i++) {
        int created = 1;
        test_assert(nodes_lookup(table, keys[i], 0, &created) == indices[i]);
        test_assert(created == 0);
    }

    nodes_free(table);
    return 0;
}

/**
 * The case-B rewrite of a swap: remove the node, replace both children and
 * the variable identity in place, reinsert. The node must be found under its
 * new key at the very same index, and the old key must be gone.
 */
static int
test_reorder_rewrite_in_place(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    if ((nodes_reorder_caps(table) & NODES_CAP_REMOVE) == 0) {
        nodes_free(table);
        return 0;
    }

    /* Four leaves with increasing handles, so that the two records also
       differ in their maximum child, which is what the list and trie
       backends anchor on. */
    uint64_t leaf[4];
    for (uint64_t i = 0; i < 4; i++) {
        int created = 0;
        leaf[i] = nodes_lookup(
            table, UINT64_C(0x4000000000000000) | i, i, &created);
        test_assert(leaf[i] > 1 && created == 1);
    }
    test_assert(leaf[0] < leaf[1] && leaf[1] < leaf[2] && leaf[2] < leaf[3]);

    const uint64_t old_a = leaf[0];
    const uint64_t old_b = (UINT64_C(7) << 40) | leaf[1];
    const uint64_t new_a = leaf[2];
    const uint64_t new_b = (UINT64_C(9) << 40) | leaf[3];

    int created = 0;
    const uint64_t index = nodes_lookup(table, old_a, old_b, &created);
    test_assert(index > 1 && created == 1);

    test_assert(nodes_remove(table, index) == SYLVAN_OK);
    uint64_t *record = nodes_get_pointer(table, index);
    record[0] = new_a;
    record[1] = new_b;
    test_assert(nodes_reinsert(table, index) == SYLVAN_OK);

    created = 1;
    test_assert(nodes_lookup(table, new_a, new_b, &created) == index);
    test_assert(created == 0);

    created = 0;
    const uint64_t revived = nodes_lookup(table, old_a, old_b, &created);
    test_assert(created == 1 && revived != index);

    nodes_free(table);
    return 0;
}

/**
 * Concurrent remove and reinsert by all workers on one shared chain.
 *
 * Every worker owns a disjoint set of keys that all land in the same chain,
 * and each round removes all of them before putting any of them back, so
 * several nodes of one chain are unlinked at the same time and adjacent
 * removals really do overlap. Afterwards the chain must contain exactly the
 * same nodes at the same indices.
 */
#define TEST_REORDER_RACE_KEYS 8
#define TEST_REORDER_RACE_ROUNDS 40

struct reorder_race {
    nodes_table *table;
    _Atomic(unsigned int) arrived;
    _Atomic(unsigned int) failures;
    uint64_t indices[TEST_MAX_WORKERS * TEST_REORDER_RACE_KEYS];
};

TASK(void, reorder_race_worker, struct reorder_race*, race)

void
reorder_race_worker_CALL(lace_worker *lace, struct reorder_race *race)
{
    const unsigned int slot = (unsigned int)lace->worker;
    const uint64_t *mine = race->indices + slot * TEST_REORDER_RACE_KEYS;

    atomic_fetch_add_explicit(&race->arrived, 1, memory_order_acq_rel);
    while (atomic_load_explicit(
               &race->arrived, memory_order_acquire) < test_workers) {
    }

    for (unsigned int round = 0; round < TEST_REORDER_RACE_ROUNDS; round++) {
        for (unsigned int i = 0; i < TEST_REORDER_RACE_KEYS; i++) {
            if (nodes_remove(race->table, mine[i]) != SYLVAN_OK) {
                atomic_fetch_add_explicit(
                    &race->failures, 1, memory_order_relaxed);
            }
        }
        for (unsigned int i = 0; i < TEST_REORDER_RACE_KEYS; i++) {
            if (nodes_reinsert(race->table, mine[i]) != SYLVAN_OK) {
                atomic_fetch_add_explicit(
                    &race->failures, 1, memory_order_relaxed);
            }
        }
    }
}

static int
test_reorder_race(void)
{
    const size_t table_size = race_table_size();
    nodes_table *table = nodes_create(table_size, table_size);
    if ((nodes_reorder_caps(table) & NODES_CAP_REMOVE) == 0) {
        nodes_free(table);
        return 0;
    }

    const unsigned int count = test_workers * TEST_REORDER_RACE_KEYS;
    static uint64_t keys[TEST_MAX_WORKERS * TEST_REORDER_RACE_KEYS];
    static struct reorder_race race;
    find_colliding_keys_n(keys, count, table_size - 1);

    race.table = table;
    atomic_store_explicit(&race.arrived, 0, memory_order_relaxed);
    atomic_store_explicit(&race.failures, 0, memory_order_relaxed);
    for (unsigned int i = 0; i < count; i++) {
        int created = 0;
        race.indices[i] = nodes_lookup(table, keys[i], 0, &created);
        test_assert(race.indices[i] > 1 && created == 1);
    }

    reorder_race_worker_TOGETHER(&race);

    test_assert(atomic_load_explicit(
        &race.failures, memory_order_relaxed) == 0);
    for (unsigned int i = 0; i < count; i++) {
        int created = 1;
        test_assert(nodes_lookup(table, keys[i], 0, &created) ==
                    race.indices[i]);
        test_assert(created == 0);
    }
    test_assert(nodes_count_nodes(table) == count + 2);

    nodes_free(table);
    return 0;
}

/**
 * Child process for the reinsert-collision death test: two equal records at
 * different indices, one of them indexed. Reinserting the other one is an
 * invariant violation and must abort rather than return.
 */
TASK(int, run_reinsert_collision)

int
run_reinsert_collision_CALL(lace_worker *lace)
{
    (void)lace;
    sylvan_init_hash();
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    const uint64_t a = UINT64_C(0x4000000000000021);
    const uint64_t b = 33;

    int created = 0;
    const uint64_t first = nodes_lookup(table, a, b, &created);
    if (first <= 1 || created != 1) return 1;
    if (nodes_remove(table, first) != SYLVAN_OK) return 1;

    created = 0;
    const uint64_t second = nodes_lookup(table, a, b, &created);
    if (second <= 1 || second == first || created != 1) return 1;

    /* <second> is indexed and equal to the record at <first>. */
    nodes_reinsert(table, first);
    return 1;  /* must not be reached */
}

#if defined(__unix__) || defined(__APPLE__)
static int
test_reinsert_collision_aborts(const char *self)
{
    nodes_table *probe = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    const uint32_t caps = nodes_reorder_caps(probe);
    nodes_free(probe);
    if ((caps & NODES_CAP_REMOVE) == 0) return 0;

    const pid_t child = fork();
    test_assert(child >= 0);
    if (child == 0) {
        /* The diagnostic is the point of the test, but it would only be
           noise in the log of a passing run. */
        if (freopen("/dev/null", "w", stderr) == NULL) _exit(127);
        execl(self, self, "--reinsert-collision", (char *)NULL);
        _exit(127);
    }

    int status = 0;
    test_assert(waitpid(child, &status, 0) == child);
    test_assert(WIFSIGNALED(status));
    test_assert(WTERMSIG(status) == SIGABRT);
    return 0;
}
#endif

/**
 * The capability mask is the contract: a primitive whose bit is missing must
 * change nothing and report SYLVAN_ERR_INVALID, and a backend with no
 * capabilities at all also refuses to reserve.
 */
static int
test_reorder_contract(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    const uint32_t caps = nodes_reorder_caps(table);
    /* One line of evidence in the log about which half of this file the
       backend under test actually exercises. */
    fprintf(stderr, "test_table: reorder caps 0x%x, %u workers\n",
            (unsigned int)caps, test_workers);
    test_assert((caps & ~(uint32_t)(NODES_CAP_REMOVE | NODES_CAP_RELEASE |
                                    NODES_CAP_ENUMERATE)) == 0);

    const uint64_t key = UINT64_C(0x4000000000000005);
    int created = 0;
    const uint64_t index = nodes_lookup(table, key, 5, &created);
    test_assert(index > 1 && created == 1);

    if ((caps & NODES_CAP_REMOVE) == 0) {
        test_assert(nodes_remove(table, index) == SYLVAN_ERR_INVALID);
        test_assert(nodes_reinsert(table, index) == SYLVAN_ERR_INVALID);
    }
    if ((caps & NODES_CAP_RELEASE) == 0) {
        test_assert(nodes_release(table, index) == SYLVAN_ERR_INVALID);
    }
    if ((caps & NODES_CAP_ENUMERATE) == 0) {
        test_assert(
            nodes_enumerate_var(table, 0, NULL, NULL) == SYLVAN_ERR_INVALID);
    }
    if (caps == 0) {
        test_assert(nodes_reserve(table, 1) == SYLVAN_ERR_INVALID);
    }

    /* Nothing above may have touched the index. */
    created = 1;
    test_assert(nodes_lookup(table, key, 5, &created) == index);
    test_assert(created == 0);
    test_assert(nodes_count_nodes(table) == 3);
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
#if SYLVAN_NODES_TABLE_LLCHST
    test_assert(
        capacity_bytes >=
        TEST_TABLE_SIZE * 24 + bitmap_bytes + region_bytes);
#else
    test_assert(
        capacity_bytes == TEST_TABLE_SIZE * 24 + bitmap_bytes + region_bytes ||
        capacity_bytes == TEST_TABLE_SIZE * 32 + bitmap_bytes + region_bytes ||
        capacity_bytes == TEST_TABLE_SIZE * 40 + bitmap_bytes + region_bytes);
#endif

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

    const size_t race_size = race_table_size();
    table = nodes_create(race_size, race_size);
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
             worker < test_workers; worker++) {
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

    if (test_reorder_contract()) return 1;
    if (test_reorder_remove_reinsert()) return 1;
    if (test_reorder_chain_positions()) return 1;
    if (test_reorder_rewrite_in_place()) return 1;
    if (test_reorder_race()) return 1;
#if defined(__unix__) || defined(__APPLE__)
    if (test_reinsert_collision_aborts(test_self)) return 1;
#endif
    if (test_deep_custom_chain()) return 1;
    if (test_custom_equality()) return 1;
    if (test_custom_metadata_cleanup()) return 1;
    if (test_custom_race()) return 1;
    if (test_exhaustion_and_resize()) return 1;
    if (test_rebuild_shapes()) return 1;
#if SYLVAN_NODES_TABLE_LLCHST
    if (test_subtable_routing()) return 1;
    if (test_reorder_subtable_migration()) return 1;
#endif

    return 0;
}

int
main(int argc, char **argv)
{
    test_self = argv[0];

    /* Internal entry point: the reinsert-collision death test re-executes
       this binary and expects the child to abort. */
    if (argc > 1 && strcmp(argv[1], "--reinsert-collision") == 0) {
        lace_start(1, 0, 0);
        const int status = run_reinsert_collision();
        lace_stop();
        return status;
    }

    /* Optional argument: number of Lace workers (default TEST_DEFAULT_WORKERS).
       The stress harness uses this to replay the tests at other worker
       counts. */
    if (argc > 1) {
        const long requested = strtol(argv[1], NULL, 10);
        if (requested < 1 || requested > TEST_MAX_WORKERS) {
            fprintf(stderr, "test_table: workers must be 1..%d\n",
                    TEST_MAX_WORKERS);
            return 1;
        }
        test_workers = (unsigned int)requested;
    }

    lace_start(test_workers, 0, 0);
    const int result = run_table_tests();

    lace_stop();
    return result;
}
