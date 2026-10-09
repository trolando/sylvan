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

/* The backend under test, as its diagnostics name it. */
#if SYLVAN_NODES_TABLE_LIST
#define TEST_BACKEND_NAME "list"
#elif SYLVAN_NODES_TABLE_TRIE
#define TEST_BACKEND_NAME "trie"
#elif SYLVAN_NODES_TABLE_LLCHT
#define TEST_BACKEND_NAME "llcht"
#elif SYLVAN_NODES_TABLE_LLCHST
#define TEST_BACKEND_NAME "llchst"
#elif SYLVAN_NODES_TABLE_LLHT
#define TEST_BACKEND_NAME "llht"
#else
#error "no nodes table backend selected"
#endif

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
 * Every word nodes_marked_words gives agrees bit for bit with
 * nodes_is_marked, and every marked index is below nodes_used_bound, which in
 * turn is within the table. Words are fetched seven at a time, so that the
 * blocks straddle every alignment.
 */
static int
check_marked_words(nodes_table *table)
{
    const size_t size = nodes_get_size(table);
    const size_t words = (size + 63) / 64;
    const size_t bound = nodes_used_bound(table);
    test_assert(bound >= 2 && bound <= size);

    uint64_t block[7];
    for (size_t first = 0; first < words; first += 7) {
        const size_t count = words - first < 7 ? words - first : 7;
        nodes_marked_words(table, first, count, block);
        for (size_t w = 0; w < count; w++) {
            for (unsigned int bit = 0; bit < 64; bit++) {
                const uint64_t index = 64 * (uint64_t)(first + w) + bit;
                const int marked = (int)((block[w] >> (63 - bit)) & 1u);
                if (index >= size) {
                    test_assert(!marked);
                    continue;
                }
                test_assert(marked == nodes_is_marked(table, index));
                if (marked) test_assert(index < bound);
            }
        }
    }
    return 0;
}

struct marked_fill {
    nodes_table *table;
    uint64_t salt;
    _Atomic(int) failed;
};

#define TEST_MARKED_FILL_PER_WORKER 700

TASK(void, marked_fill_worker, struct marked_fill*, fill)

void
marked_fill_worker_CALL(lace_worker *lace, struct marked_fill *fill)
{
    const uint64_t worker = (uint64_t)lace->worker;
    for (uint64_t i = 0; i < TEST_MARKED_FILL_PER_WORKER; i++) {
        int created = 0;
        const uint64_t key = UINT64_C(0x4000000000000000) |
                             (fill->salt << 32) | (worker << 16) | i;
        if (nodes_lookup(fill->table, key, i, &created) == 0) {
            atomic_store_explicit(&fill->failed, 1, memory_order_relaxed);
        }
    }
}

/**
 * The two primitives a reorder session's opening passes stand on: the marks
 * a word at a time (nodes_marked_words), and how far into the table they can
 * reach (nodes_used_bound). A pass that trusts the bound skips everything
 * above it, so a node above it would be a node the session never sees.
 *
 * Checked on an empty table; after every worker has filled at once, each from
 * a region of its own; after a collection that keeps only the most recently
 * placed nodes, which must not lower the bound; and after the table has grown
 * and been filled further.
 */
static int
test_marked_words_and_bound(void)
{
    const size_t size = race_table_size() * 4;
    nodes_table *table = nodes_create(size, size * 4);
    if (check_marked_words(table)) return 1;
#if !SYLVAN_NODES_TABLE_LLHT
    /* Nothing claimed yet: only region 0, for the reserved slots. llht has an
       allocator of its own and answers with the whole table. */
    test_assert(nodes_used_bound(table) == 512);
#endif

    struct marked_fill fill = { table, 1, 0 };
    marked_fill_worker_TOGETHER(&fill);
    test_assert(atomic_load(&fill.failed) == 0);
    if (check_marked_words(table)) return 1;

    /* Collect, keeping the highest-indexed tenth of the nodes. */
    const size_t bound = nodes_used_bound(table);
    const size_t words = (nodes_get_size(table) + 63) / 64;
    uint64_t *kept = (uint64_t*)malloc(words * sizeof(uint64_t));
    test_assert(kept != NULL);
    nodes_marked_words(table, 0, words, kept);
    kept[0] &= UINT64_C(0x3fffffffffffffff);
    const size_t live = nodes_count_nodes(table) - 2;
    size_t seen = 0;
    nodes_clear(table);
    for (size_t w = 0; w < words; w++) {
        for (unsigned int bit = 0; bit < 64; bit++) {
            if (((kept[w] >> (63 - bit)) & 1u) == 0) continue;
            if (seen++ >= live - live / 10) {
                nodes_mark_rec(table, 64 * (uint64_t)w + bit);
            }
        }
    }
    free(kept);
    test_assert(nodes_rebuild(table) == 0);
    test_assert(nodes_used_bound(table) == bound);
    if (check_marked_words(table)) return 1;

    nodes_set_size(table, size * 4);
    fill.salt = 2;
    marked_fill_worker_TOGETHER(&fill);
    test_assert(atomic_load(&fill.failed) == 0);
    test_assert(nodes_used_bound(table) >= bound);
    if (check_marked_words(table)) return 1;

    nodes_free(table);
    return 0;
}

/**
 * A table with nothing left to give must say so.
 *
 * The lookup path answers 0 when the arena cannot hand out a slot, and that 0
 * used to be the only evidence there was: a caller that turns it into an error
 * of its own printed nothing about the table, so an exhausted run could end in
 * a bare "build failed". The backend now prints one line per exhaustion
 * episode, and this test exists because the line is the point.
 *
 * Every worker fills at once, which is the shape the defect was reported in:
 * each worker holds a private region, so the one that needs a fresh region can
 * fail while free slots still sit in the regions the others hold. The work
 * runs in a child process because stderr is what is being asserted on, the
 * same reason the reinsert-collision death test re-executes this binary; the
 * child exits 0 only if some lookup really did answer 0.
 */
#define TEST_EXHAUST_ATTEMPTS (1u << 16)

static nodes_table *exhaust_table;
static _Atomic(unsigned int) exhaust_zero_returns;

TASK(void, exhaust_worker, int, unused)

void
exhaust_worker_CALL(lace_worker *lace, int unused)
{
    (void)unused;
    const uint64_t slot = (uint64_t)lace->worker;
    for (unsigned int i = 0; i < TEST_EXHAUST_ATTEMPTS; i++) {
        /* Leaf keys, distinct per worker: every backend accepts them and no
           two workers ever ask for the same node. */
        const uint64_t key =
            UINT64_C(0x4000000000000000) | (slot << 32) | (uint64_t)(i + 2u);
        int created = 0;
        if (nodes_lookup(exhaust_table, key, 0, &created) == 0) {
            atomic_fetch_add_explicit(
                &exhaust_zero_returns, 1, memory_order_relaxed);
            return;
        }
    }
}

static int
run_arena_exhaustion(void)
{
    const size_t size = race_table_size();
    exhaust_table = nodes_create(size, size);
    exhaust_worker_TOGETHER(0);
    const unsigned int zeros = atomic_load_explicit(
        &exhaust_zero_returns, memory_order_relaxed);
    nodes_free(exhaust_table);
    return zeros == 0 ? 1 : 0;
}

#if defined(__unix__) || defined(__APPLE__)
static int
test_arena_exhaustion_reports(const char *self)
{
    int fds[2];
    test_assert(pipe(fds) == 0);

    char workers[16];
    snprintf(workers, sizeof(workers), "%u", test_workers);

    const pid_t child = fork();
    test_assert(child >= 0);
    if (child == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDERR_FILENO) < 0) _exit(127);
        close(fds[1]);
        execl(self, self, "--exhaust", workers, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);

    /* Drain to EOF before waiting, so the child can never block on a full
       pipe; anything past the buffer is counted and dropped. */
    char captured[4096];
    size_t filled = 0;
    for (;;) {
        char chunk[512];
        const ssize_t got = read(fds[0], chunk, sizeof(chunk));
        if (got <= 0) break;
        size_t room = sizeof(captured) - 1 - filled;
        if (room > (size_t)got) room = (size_t)got;
        memcpy(captured + filled, chunk, room);
        filled += room;
    }
    captured[filled] = '\0';
    close(fds[0]);

    int status = 0;
    test_assert(waitpid(child, &status, 0) == child);
    /* Half the assertion: the lookups really did start returning 0. */
    test_assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

#if SYLVAN_NODES_TABLE_LLHT
    /* llht is open-addressed, and a table with no empty slot left in a key's
       probe sequence refuses on the probe count -- returning 0 without ever
       asking the arena for a record. Which of the two refusals a filling table
       reaches first is not a race but a consequence of how the slots ran out.
       One worker walks the table down to its last few and always meets the
       probe limit first; several workers hold a private region each, and the
       one that needs a fresh region finds the arena empty first, so only a
       multi-worker run is guaranteed to print the diagnostic. Measured on
       this backend: 0 of 20 runs at one worker, 20 of 20 at eight. So the
       diagnostic is demanded here only where it is reachable. */
    const int expect_diagnostic = test_workers > 1;
#else
    const int expect_diagnostic = 1;
#endif
    if (expect_diagnostic) {
        /* Exactly one line, from however many workers ran out: the episode
           reports itself, not each lookup that failed inside it. */
        unsigned int lines = 0;
        for (const char *at = captured;
             (at = strstr(at, "out of node slots")) != NULL; at++) {
            lines++;
        }
        test_assert(lines == 1);
        test_assert(strstr(captured, TEST_BACKEND_NAME) != NULL);
    }
    return 0;
}
#endif

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

/* shape_build's answer when the table cannot hold these shapes. */
#define TEST_SHAPE_UNORDERED 2

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

    /* The families below need every chain node they anchor on to out-index
       the leaves they pair it with, which holds whenever the table hands out
       indices in creation order. One that does not -- the striped allocator
       of a SYLVAN_REGION_STRIPED_START build, with several workers -- cannot
       build these shapes at all, so say so before adding any of them, and let
       the caller skip rather than fail. */
    for (uint64_t i = 0; i < TEST_SHAPE_SUBANCHORS; i++) {
        const uint64_t anchor = set->nodes[chain_first + i].index;
        for (uint64_t k = 0; k < TEST_SHAPE_SUBFAN; k++) {
            const uint64_t lo =
                set->nodes[(size_t)k % TEST_SHAPE_TOTAL_LEAVES].index;
            if (lo >= anchor) return TEST_SHAPE_UNORDERED;
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

/** Skip a shape test on a table that cannot hold the shapes; see shape_build. */
static int
shape_skip(nodes_table *table, const char *test)
{
    fprintf(stderr, "test_table: skipping %s: this table does not hand out "
            "node indices in creation order\n", test);
    nodes_free(table);
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

    const int built = shape_build(&set, table);
    if (built == TEST_SHAPE_UNORDERED) {
        return shape_skip(table, "the rebuild shapes");
    }
    if (built) return 1;
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
 * The batched primitives against what the single-node ones would have done.
 *
 * All the keys are leaves, so on the anchored backends they are one family and
 * the removals really do repair overlapping and nested regions rather than
 * independent ones. The batch is collective, so it runs in its own Lace frame,
 * which is also how the reorder session calls it.
 */
#define TEST_BATCH_KEYS 512
#define TEST_BATCH_STRIDE 8

struct batch_case {
    nodes_table *table;
    uint64_t *indices;
    size_t count;
};

TASK(int, batch_remove_task, struct batch_case*, work)

int
batch_remove_task_CALL(lace_worker *lace, struct batch_case *work)
{
    return nodes_remove_batch_CALL(
        lace, work->table, work->indices, work->count);
}

TASK(int, batch_reinsert_task, struct batch_case*, work)

int
batch_reinsert_task_CALL(lace_worker *lace, struct batch_case *work)
{
    return nodes_reinsert_batch_CALL(
        lace, work->table, work->indices, work->count);
}

static int
test_reorder_batch(void)
{
    const size_t table_size = race_table_size() * 2;
    nodes_table *table = nodes_create(table_size, table_size);
    if ((nodes_reorder_caps(table) & NODES_CAP_BATCH) == 0) {
        nodes_free(table);
        return 0;
    }

    static uint64_t keys[TEST_BATCH_KEYS];
    static uint64_t indices[TEST_BATCH_KEYS];
    static uint64_t victims[TEST_BATCH_KEYS / TEST_BATCH_STRIDE];

    for (unsigned int i = 0; i < TEST_BATCH_KEYS; i++) {
        keys[i] = UINT64_C(0x4000000000000000) | (i + 2u);
        int created = 0;
        indices[i] = nodes_lookup(table, keys[i], 0, &created);
        test_assert(indices[i] > 1 && created == 1);
    }

    const size_t victim_count = TEST_BATCH_KEYS / TEST_BATCH_STRIDE;
    for (size_t k = 0; k < victim_count; k++) {
        victims[k] = indices[k * TEST_BATCH_STRIDE];
    }

    struct batch_case work = { table, victims, victim_count };
    test_assert(batch_remove_task_NEWFRAME(&work) == SYLVAN_OK);

    for (unsigned int i = 0; i < TEST_BATCH_KEYS; i++) {
        int created = 1;
        const uint64_t found = nodes_lookup(table, keys[i], 0, &created);
        if (i % TEST_BATCH_STRIDE == 0) {
            /* Gone from the index: recreated at a fresh slot. */
            test_assert(created == 1 && found != indices[i]);
            test_assert(nodes_remove(table, found) == SYLVAN_OK);
        } else {
            test_assert(created == 0 && found == indices[i]);
        }
    }

    test_assert(batch_reinsert_task_NEWFRAME(&work) == SYLVAN_OK);

    for (unsigned int i = 0; i < TEST_BATCH_KEYS; i++) {
        int created = 1;
        test_assert(nodes_lookup(table, keys[i], 0, &created) == indices[i]);
        test_assert(created == 0);
    }

    /* A batch that names a node it cannot remove refuses the whole batch and
       leaves the index alone, because the phase that discovers it only reads.
       The bad index is out of range rather than merely absent: an index that
       is in range but not in its own family is a caller bug the debug builds
       assert on, exactly as the single-node nodes_remove does. */
    static uint64_t spoiled[TEST_BATCH_KEYS / TEST_BATCH_STRIDE];
    memcpy(spoiled, victims, victim_count * sizeof(uint64_t));
    spoiled[victim_count / 2] = table_size + 1u;
    struct batch_case bad = { table, spoiled, victim_count };
    test_assert(batch_remove_task_NEWFRAME(&bad) == SYLVAN_ERR_INVALID);

    for (unsigned int i = 0; i < TEST_BATCH_KEYS; i++) {
        int created = 1;
        test_assert(nodes_lookup(table, keys[i], 0, &created) == indices[i]);
        test_assert(created == 0);
    }

    nodes_free(table);
    return 0;
}

#ifdef NDEBUG
/**
 * A refused removal changes nothing.
 *
 * Release builds only: a debug build asserts on a node that is not where its
 * record says before it refuses (see CALLER ERRORS in nodes.h). The chained
 * backends claim a node's link before they look for the node, so a refusal
 * has to give the link back. Two ways it used to stay behind:
 *
 *  - A node whose record was rewritten while it was still indexed. It stays
 *    in its chain, and a link left saying "removal in progress" cut the chain
 *    short at it -- every key behind it missed and was interned again -- and
 *    made the node itself impossible to remove.
 *  - A slot nothing was ever inserted into. The list backend never writes
 *    the link of a node it appends, so the next node to claim the slot
 *    inherited the claimed link and could not be removed either.
 */
static int
test_reorder_refusal_changes_nothing(void)
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

    /* A record none of those chains or families can hold: not a leaf,
       anchored (list, trie) where no node of this table has a family, and
       hashing to the other parity, so to another head of any power-of-two
       head array (llcht, llchst). */
    const uint64_t seed = UINT64_C(14695981039346656037);
    const uint64_t parity = sylvan_tabhash16(keys[0], 0, seed) & 1u;
    uint64_t foreign = 0;
    while (foreign == 1 ||
           (sylvan_tabhash16(0, foreign, seed) & 1u) == parity) {
        foreign++;
    }

    const unsigned int middle = TEST_COLLIDING_KEYS / 2;
    uint64_t *record = nodes_get_pointer(table, indices[middle]);
    const uint64_t saved_a = record[0];
    const uint64_t saved_b = record[1];
    record[0] = 0;
    record[1] = foreign;
    test_assert(nodes_remove(table, indices[middle]) == SYLVAN_ERR_INVALID);
    record[0] = saved_a;
    record[1] = saved_b;

    /* Every key is still found where it was, behind the refused node as much
       as in front of it, and the refused node can still be removed. */
    for (unsigned int i = 0; i < TEST_COLLIDING_KEYS; i++) {
        int created = 1;
        test_assert(nodes_lookup(table, keys[i], 0, &created) == indices[i]);
        test_assert(created == 0);
    }
    test_assert(nodes_remove(table, indices[middle]) == SYLVAN_OK);
    test_assert(nodes_reinsert(table, indices[middle]) == SYLVAN_OK);

    /* The slot the allocator hands out next has never held a node; see
       test_reorder_release for the allocation order this relies on. */
    const uint64_t unused = indices[TEST_COLLIDING_KEYS - 1] + 1;
    test_assert(!nodes_is_marked(table, unused));
    test_assert(nodes_remove(table, unused) == SYLVAN_ERR_INVALID);

    int created = 0;
    const uint64_t fresh = nodes_lookup(
        table, UINT64_C(0x4000000000054321), 0, &created);
    test_assert(created == 1 && fresh == unused);
    test_assert(nodes_remove(table, fresh) == SYLVAN_OK);
    test_assert(nodes_reinsert(table, fresh) == SYLVAN_OK);

    nodes_free(table);
    return 0;
}
#endif

/**
 * Remove, release, and (after a simulated barrier) claim the slot again: the
 * recycled index must come back for a different key, and the old key must be
 * gone. Releasing a node that is still in the index must be refused.
 */
static int
test_reorder_release(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    if ((nodes_reorder_caps(table) & NODES_CAP_RELEASE) == 0) {
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

    const size_t before = nodes_count_nodes(table);
    test_assert(nodes_release(table, indices[1]) == SYLVAN_ERR_INVALID);
    test_assert(nodes_count_nodes(table) == before);

    test_assert(nodes_remove(table, indices[1]) == SYLVAN_OK);
    test_assert(nodes_release(table, indices[1]) == SYLVAN_OK);
    test_assert(nodes_count_nodes(table) == before - 1);

    /* Simulated barrier: every removal of this phase is complete, so the slot
       may be handed out again. */
    int created = 0;
    const uint64_t fresh = nodes_lookup(
        table, UINT64_C(0x4000000000098765), 5, &created);
    test_assert(created == 1 && fresh == indices[1]);

    /* The old key is gone, and its neighbours in the chain survived. */
    created = 0;
    test_assert(nodes_lookup(table, keys[1], 0, &created) != indices[1]);
    test_assert(created == 1);
    for (unsigned int i = 0; i < TEST_COLLIDING_KEYS; i++) {
        if (i == 1) continue;
        created = 1;
        test_assert(nodes_lookup(table, keys[i], 0, &created) == indices[i]);
        test_assert(created == 0);
    }

    nodes_free(table);
    return 0;
}

/**
 * Releasing a custom node destroys its payload once and clears its custom
 * bit, so that the recycled slot is an ordinary node afterwards: with a stale
 * bit the rebuild would hash it through the custom callback and lose it, and
 * the next cleanup would destroy a payload that is not there.
 */
static int
test_reorder_release_custom(void)
{
    nodes_table *table = nodes_create(TEST_TABLE_SIZE, TEST_TABLE_SIZE);
    if ((nodes_reorder_caps(table) & NODES_CAP_RELEASE) == 0) {
        nodes_free(table);
        return 0;
    }
    nodes_set_custom(
        table, custom_hash, custom_equals,
        custom_create, custom_destroy_counted);

    int created = 0;
    const uint64_t index = nodes_lookupc(
        table, UINT64_C(0x4000000000000031), 49, &created);
    test_assert(index > 1 && created == 1);

    custom_destroy_count = 0;
    test_assert(nodes_remove(table, index) == SYLVAN_OK);
    test_assert(nodes_release(table, index) == SYLVAN_OK);
    test_assert(custom_destroy_count == 1);

    created = 0;
    const uint64_t plain = nodes_lookup(
        table, UINT64_C(0x4000000000000032), 50, &created);
    test_assert(plain == index && created == 1);

    custom_destroy_count = 0;
    nodes_clear(table);
    nodes_mark_rec(table, plain);
    nodes_cleanup_custom(table);
    test_assert(custom_destroy_count == 0);
    test_assert(nodes_rebuild(table) == 0);

    created = 1;
    test_assert(nodes_lookup(
        table, UINT64_C(0x4000000000000032), 50, &created) == plain);
    test_assert(created == 0);

    nodes_free(table);
    return 0;
}

/**
 * Native per-variable enumeration must agree with a ground-truth scan of the
 * arena: every allocated non-leaf record whose identity field names the
 * variable, and nothing else.
 */
#define TEST_ENUM_NODES 96

struct enum_collect {
    uint64_t seen[TEST_ENUM_NODES * 2];
    size_t count;
    size_t limit;
};

static int
enum_collect_cb(uint64_t index, void *context)
{
    struct enum_collect *collect = (struct enum_collect *)context;
    if (collect->count == collect->limit) return 42;
    if (collect->count < TEST_ENUM_NODES * 2) {
        collect->seen[collect->count] = index;
    }
    collect->count++;
    return 0;
}

static int
compare_u64(const void *left, const void *right)
{
    const uint64_t a = *(const uint64_t *)left;
    const uint64_t b = *(const uint64_t *)right;
    if (a < b) return -1;
    return a > b ? 1 : 0;
}

static int
test_reorder_enumerate(void)
{
    nodes_table *table = nodes_create(
        TEST_RACE_TABLE_SIZE, TEST_RACE_TABLE_SIZE);
    if ((nodes_reorder_caps(table) & NODES_CAP_ENUMERATE) == 0) {
        nodes_free(table);
        return 0;
    }

    /* Leaves keep their identity field at zero, so they can never be mistaken
       for a node of variable 3 or 4 by the ground-truth scan. */
    uint64_t leaf[4];
    for (uint64_t i = 0; i < 4; i++) {
        int created = 0;
        leaf[i] = nodes_lookup(
            table, UINT64_C(0x4000000000000000) | i, i, &created);
        test_assert(leaf[i] > 1 && created == 1);
    }

    for (uint64_t i = 0; i < TEST_ENUM_NODES; i++) {
        const uint32_t variable = (i % 2) ? 4u : 3u;
        int created = 0;
        const uint64_t index = nodes_lookup_subtable(
            table, NODES_SUBTABLE_DECISION(variable),
            leaf[i % 4] | (i << 40),
            ((uint64_t)variable << 40) | leaf[(i + 1) % 4], &created);
        test_assert(index > 1 && created == 1);
    }

    for (uint32_t variable = 3; variable <= 4; variable++) {
        static struct enum_collect collect;
        collect.count = 0;
        collect.limit = SIZE_MAX;
        test_assert(nodes_enumerate_var(
            table, variable, enum_collect_cb, &collect) == SYLVAN_OK);

        static uint64_t truth[TEST_ENUM_NODES * 2];
        size_t truth_count = 0;
        for (uint64_t index = 2; index < nodes_get_size(table); index++) {
            if (!nodes_is_marked(table, index)) continue;
            const uint64_t *record = nodes_get_pointer(table, index);
            if (record[0] & UINT64_C(0x4000000000000000)) continue;
            if ((uint32_t)(record[1] >> 40) != variable) continue;
            test_assert(truth_count < TEST_ENUM_NODES * 2);
            truth[truth_count++] = index;
        }

        test_assert(collect.count == truth_count);
        test_assert(truth_count == TEST_ENUM_NODES / 2);
        qsort(collect.seen, collect.count, sizeof(uint64_t), compare_u64);
        for (size_t i = 0; i < truth_count; i++) {
            test_assert(collect.seen[i] == truth[i]);
        }
    }

    /* A variable that never had a subtable enumerates nothing, and a callback
       that returns non-zero stops the walk and yields its value. */
    static struct enum_collect empty;
    empty.count = 0;
    empty.limit = SIZE_MAX;
    test_assert(nodes_enumerate_var(
        table, 11u, enum_collect_cb, &empty) == SYLVAN_OK);
    test_assert(empty.count == 0);

    static struct enum_collect stopped;
    stopped.count = 0;
    stopped.limit = 5;
    test_assert(nodes_enumerate_var(
        table, 3u, enum_collect_cb, &stopped) == 42);
    test_assert(stopped.count == 5);

    test_assert(nodes_enumerate_var(table, 3u, NULL, NULL) ==
                SYLVAN_ERR_INVALID);

    nodes_free(table);
    return 0;
}

/**
 * Reserve, then let every worker allocate its share: all of them must
 * succeed. A reservation beyond the table's capacity must be refused without
 * changing anything.
 */
#define TEST_RESERVE_PER_WORKER 64

struct reserve_race {
    nodes_table *table;
    _Atomic(unsigned int) arrived;
    _Atomic(unsigned int) failures;
};

TASK(void, reserve_race_worker, struct reserve_race*, race)

void
reserve_race_worker_CALL(lace_worker *lace, struct reserve_race *race)
{
    const unsigned int slot = (unsigned int)lace->worker;
    atomic_fetch_add_explicit(&race->arrived, 1, memory_order_acq_rel);
    while (atomic_load_explicit(
               &race->arrived, memory_order_acquire) < test_workers) {
    }

    for (unsigned int i = 0; i < TEST_RESERVE_PER_WORKER; i++) {
        int created = 0;
        const uint64_t key = UINT64_C(0x4000000000000000) |
            ((uint64_t)(slot + 1) << 24) | i;
        if (nodes_lookup(race->table, key, 0, &created) <= 1) {
            atomic_fetch_add_explicit(
                &race->failures, 1, memory_order_relaxed);
        }
    }
}

static int
test_reorder_reserve(void)
{
    /* A reservation of <count> asks the table for <count> free slots beyond
       a margin of one region per worker (see nodes_reserve), and the race
       table is only guaranteed to hold the margin. Size for both, or the
       reservation below is refused for the honest reason at eight workers. */
    const uint64_t wanted =
        (uint64_t)test_workers * TEST_RESERVE_PER_WORKER;
    size_t table_size = race_table_size();
    while (table_size < (size_t)test_workers * 512 + wanted + 2) {
        table_size *= 2;
    }
    nodes_table *table = nodes_create(table_size, table_size);
    if (nodes_reorder_caps(table) == 0) {
        test_assert(nodes_reserve(table, 1) == SYLVAN_ERR_INVALID);
        nodes_free(table);
        return 0;
    }

    /* More than the table can ever hold: refused, nothing changed. */
    const size_t before = nodes_count_nodes(table);
    test_assert(nodes_reserve(table, table_size + 1) == SYLVAN_ERR_OOM);
    test_assert(nodes_count_nodes(table) == before);
    test_assert(nodes_reserve(table, 0) == SYLVAN_OK);

    test_assert(nodes_reserve(table, wanted) == SYLVAN_OK);

    static struct reserve_race race;
    race.table = table;
    atomic_store_explicit(&race.arrived, 0, memory_order_relaxed);
    atomic_store_explicit(&race.failures, 0, memory_order_relaxed);
    reserve_race_worker_TOGETHER(&race);
    test_assert(atomic_load_explicit(
        &race.failures, memory_order_relaxed) == 0);
    test_assert(nodes_count_nodes(table) == before + wanted);

    /* Fill the table, then no reservation can be honoured any more. */
    for (uint64_t i = 0; i < table_size; i++) {
        int created = 0;
        if (nodes_lookup(
                table, UINT64_C(0x4000000000000000) |
                (UINT64_C(1) << 40) | i, i, &created) == 0) {
            break;
        }
    }
    test_assert(nodes_reserve(table, 1) == SYLVAN_ERR_OOM);

    nodes_free(table);
    return 0;
}

/**
 * After nodes_prepare_reorder, removal allocates nothing.
 *
 * The adversarial shapes are what make this a test rather than a formality.
 * The deep chain is one family of a thousand nodes, all sharing a maximum
 * child, so a backend that repairs its index by rebuilding the region that
 * depended on the vanishing slot has to collect and reinsert hundreds of nodes
 * for a single removal -- far past any buffer a removal could sensibly start
 * with, and therefore a heap allocation in the middle of an index change.
 *
 * Every removal here is also undone, so the whole set has to come back at its
 * original indices: an allocation-free removal still has to be a correct one.
 */
static int
test_prepare_removals(void)
{
    static struct shape_set set;
    nodes_table *table = nodes_create(
        TEST_SHAPE_TABLE_SIZE, TEST_SHAPE_TABLE_SIZE);
    nodes_set_custom(
        table, custom_hash, custom_equals, custom_create, custom_destroy);

    const int built = shape_build(&set, table);
    if (built == TEST_SHAPE_UNORDERED) {
        return shape_skip(table, "the prepared removals");
    }
    if (built) return 1;
    if (shape_verify(&set, table)) return 1;

    if ((nodes_reorder_caps(table) & NODES_CAP_REMOVE) == 0) {
        test_assert(
            nodes_prepare_reorder(table, NULL, 0) == SYLVAN_ERR_INVALID);
        nodes_free(table);
        return 0;
    }

    /* shape_build lays the leaves down first, so the deep chain starts here. */
    const size_t chain = TEST_SHAPE_TOTAL_LEAVES;

#if SYLVAN_NODES_TABLE_TRIE
    /* Unprepared, this backend's removal grows a buffer of its own, which is
       precisely the allocation the preflight exists to remove. Checking that
       it does keeps the assertion below from passing for the wrong reason. */
    const uint64_t unprepared = nodes_late_allocations();
    for (size_t i = 0; i < 64; i++) {
        test_assert(
            nodes_remove(table, set.nodes[chain + i].index) == SYLVAN_OK);
    }
    for (size_t i = 64; i-- > 0; ) {
        test_assert(
            nodes_reinsert(table, set.nodes[chain + i].index) == SYLVAN_OK);
    }
    test_assert(nodes_late_allocations() > unprepared);
    if (shape_verify(&set, table)) return 1;
#endif

    test_assert(nodes_prepare_reorder(table, NULL, 0) == SYLVAN_OK);

    const uint64_t prepared = nodes_late_allocations();
    for (size_t i = 0; i < TEST_SHAPE_CHAIN; i++) {
        test_assert(
            nodes_remove(table, set.nodes[chain + i].index) == SYLVAN_OK);
    }
    for (size_t i = TEST_SHAPE_CHAIN; i-- > 0; ) {
        test_assert(
            nodes_reinsert(table, set.nodes[chain + i].index) == SYLVAN_OK);
    }
    test_assert(nodes_late_allocations() == prepared);
    if (shape_verify(&set, table)) return 1;

    /* Idempotent: a second call has nothing left to do and says so. */
    test_assert(nodes_prepare_reorder(table, NULL, 0) == SYLVAN_OK);
    test_assert(nodes_late_allocations() == prepared);

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
    if (sylvan_init_hash() != SYLVAN_OK) return 1;
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
                                    NODES_CAP_ENUMERATE |
                                    NODES_CAP_BATCH)) == 0);

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
    if ((caps & NODES_CAP_BATCH) == 0) {
        test_assert(nodes_remove_batch(table, &index, 1) == SYLVAN_ERR_INVALID);
        test_assert(
            nodes_reinsert_batch(table, &index, 1) == SYLVAN_ERR_INVALID);
    }
    if (caps == 0) {
        test_assert(nodes_reserve(table, 1) == SYLVAN_ERR_INVALID);
        test_assert(
            nodes_prepare_reorder(table, NULL, 0) == SYLVAN_ERR_INVALID);
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
    test_assert(sylvan_init_hash() == SYLVAN_OK);

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
    if (test_reorder_enumerate()) return 1;
    if (test_reorder_reserve()) return 1;
    if (test_prepare_removals()) return 1;
    if (test_reorder_release()) return 1;
#ifdef NDEBUG
    if (test_reorder_refusal_changes_nothing()) return 1;
#endif
    if (test_reorder_release_custom()) return 1;
    if (test_reorder_race()) return 1;
    if (test_reorder_batch()) return 1;
#if defined(__unix__) || defined(__APPLE__)
    if (test_reinsert_collision_aborts(test_self)) return 1;
#endif
    if (test_deep_custom_chain()) return 1;
    if (test_custom_equality()) return 1;
    if (test_custom_metadata_cleanup()) return 1;
    if (test_custom_race()) return 1;
    if (test_exhaustion_and_resize()) return 1;
    if (test_marked_words_and_bound()) return 1;
#if defined(__unix__) || defined(__APPLE__)
    if (test_arena_exhaustion_reports(test_self)) return 1;
#endif
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

    /* Internal entry point: the exhaustion test re-executes this binary
       with stderr on a pipe, so that it can read the diagnostic. */
    if (argc > 2 && strcmp(argv[1], "--exhaust") == 0) {
        const long requested = strtol(argv[2], NULL, 10);
        if (requested < 1 || requested > TEST_MAX_WORKERS) return 1;
        test_workers = (unsigned int)requested;
        lace_start(test_workers, 0, 0);
        const int status = run_arena_exhaustion();
        lace_stop();
        return status;
    }

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
