/* Deterministic exhaustion of worker-owned regions after an automatic GC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sylvan.h>
#include <sylvan_int.h>
#include "test_assert.h"

/* These primitives also have allocation failure paths. */
ZDD zdd_makeleaf(uint16_t type, uint64_t value);
ZDD zdd_makemapnode(uint32_t var, ZDD low, ZDD high);

enum node_kind {
    BDD_NODE, BDD_LEAF, BDD_CUSTOM, BDD_MAP, LDD_NODE, LDD_COPY,
    ZDD_NODE, ZDD_LEAF, ZDD_CUSTOM, ZDD_MAP
};

static uint64_t roots[1024];
static size_t root_count;
static unsigned requester, collections, hook_calls;
static int claim_errors[4];
static uint32_t custom_type;
static unsigned custom_created, custom_destroyed;
static int shrink_hook;
static size_t expected_full_size;
static unsigned expected_collections;

VOID_TASK_0(mark_roots)
{
    for (size_t i = 0; i < root_count; i++) llmsset_mark(nodes, roots[i]);
}

VOID_TASK_0(custom_main)
{
    /* Deliberately leave growth entirely to allocation recovery. */
    hook_calls++;
    if (shrink_hook) llmsset_set_size(nodes, 1024);
}

static uint64_t
custom_hash(uint64_t value, uint64_t seed)
{
    return sylvan_tabhash16(value, 0, seed);
}

static int
custom_equals(uint64_t a, uint64_t b)
{
    return a == b;
}

static void
custom_create(uint64_t *value)
{
    (void)value;
    custom_created++;
}

static void
custom_destroy(uint64_t value)
{
    (void)value;
    custom_destroyed++;
}

VOID_TASK_0(claim_free_regions)
{
    unsigned worker = lace_get_worker()->worker;
    if (worker == (requester + 1) % 4 || worker == (requester + 2) % 4) {
        size_t size = llmsset_get_size(nodes);
        /* Split the free regions between these workers, leaving each one's
         * last region almost empty. At 2048 buckets this takes one node each;
         * the maximum-size case also reserves all six free regions at 4096. */
        for (size_t i = 0; i < size / 2 - 1023; i++) {
            int created = 0;
            uint64_t key = UINT64_C(0xf000000000000000) + 2048 * worker + i;
            uint64_t index = llmsset_lookup(nodes, key, 0, &created);
            claim_errors[worker] |= index < 1024 || index >= size || !created;
        }
    }
}

VOID_TASK_0(after_gc)
{
    collections++;
    size_t size = llmsset_get_size(nodes);
    if (size == 2048 || (expected_full_size && size == expected_full_size)) {
        /* Regions 0 and 1 contain 1024 live buckets. Two other workers each
         * claim one of regions 2 and 3 before the allocating worker resumes.
         * Its retry must fail although 1022 buckets are still free. These
         * temporary entries are not marked in the following collection, so
         * the low-memory hook sees exactly 50% live and will not grow. */
        TOGETHER(claim_free_regions);
    }
}

static void
unexpected_full(const char *kind, size_t filled, size_t total)
{
    if (expected_full_size != 0 && total == expected_full_size &&
        collections == expected_collections && strcmp(kind, "BDD") == 0 &&
        filled == (total == 1024 ? 1024u : total - 1022)) exit(0);
    fprintf(stderr, "Premature table full: %s, %zu of %zu after %u collections\n",
            kind, filled, total, collections);
    exit(30);
}

TASK_1(int, regression, int, kind)
{
    requester = lace_get_worker()->worker;
    uint64_t child = 0;
    if (kind == BDD_NODE || kind == BDD_MAP) child = mtbdd_makenode(5, mtbdd_false, mtbdd_true);
    if (kind == LDD_NODE || kind == LDD_COPY) child = lddmc_makenode(5, lddmc_true, lddmc_false);
    if (kind == ZDD_NODE || kind == ZDD_MAP) child = zdd_makenode(5, zdd_false, zdd_true);
    uint64_t child_data[2] = {0, 0};
    if (child) memcpy(child_data, llmsset_index_to_ptr(nodes, child), sizeof(child_data));

    /* Fill every bucket, skipping keys whose bounded hash probe fails near
     * capacity. Only the pending node constructor will protect its child. */
    size_t count = 2 + (child != 0);
    for (uint64_t key = 1; count < 1024 && key < 10000; key++) {
        int created;
        uint64_t index = llmsset_lookup(nodes, UINT64_C(0xe000000000000000) + key, 0, &created);
        if (index != 0) {
            test_assert(created);
            roots[root_count++] = index;
            count++;
        }
    }
    test_assert(count == 1024);
    test_assert(llmsset_get_size(nodes) == 1024);
    test_assert(llmsset_count_marked(nodes) == 1024);

    uint64_t result = 0;
    switch (kind) {
    case BDD_NODE: result = mtbdd_makenode(4, child, mtbdd_true); break;
    case BDD_LEAF: result = mtbdd_int64(42); break;
    case BDD_CUSTOM: result = mtbdd_makeleaf(custom_type, 42); break;
    case BDD_MAP: result = mtbdd_map_add(mtbdd_map_empty(), 4, child); break;
    case LDD_NODE: result = lddmc_makenode(4, child, lddmc_false); break;
    case LDD_COPY: result = lddmc_make_copynode(child, lddmc_false); break;
    case ZDD_NODE: result = zdd_makenode(4, child, zdd_true); break;
    case ZDD_LEAF: result = zdd_makeleaf(0, 42); break;
    case ZDD_CUSTOM: result = zdd_makeleaf((uint16_t)custom_type, 42); break;
    case ZDD_MAP: result = zdd_makemapnode(4, zdd_false, child); break;
    }
    test_assert(result > 1);
    test_assert(llmsset_get_size(nodes) == 4096);
    test_assert(collections == (hook_calls ? 3u : 2u));
    test_assert(hook_calls == 0 || hook_calls == collections);
    for (unsigned i = 0; i < 4; i++) test_assert(!claim_errors[i]);
    if (child) {
        test_assert(llmsset_is_marked(nodes, child));
        test_assert(memcmp(child_data, llmsset_index_to_ptr(nodes, child), sizeof(child_data)) == 0);
    }
    if (kind == BDD_CUSTOM || kind == ZDD_CUSTOM) {
        test_assert(custom_created == 1 && custom_destroyed == 0);
        uint64_t again = kind == BDD_CUSTOM ? mtbdd_makeleaf(custom_type, 42) : zdd_makeleaf((uint16_t)custom_type, 42);
        test_assert(again == result && custom_created == 1);
        /* Neither handle is rooted: verify the custom bit survives recovery
         * and dispatches the destructor in the next collection. */
        CALL(sylvan_gc_collect);
        test_assert(custom_destroyed == 1);
    }
    return 0;
}

int
main(int argc, char **argv)
{
    int disabled = 0;
    if (argc > 1) {
        if (strcmp(argv[1], "max") == 0) {
            expected_full_size = 4096;
            expected_collections = 3; /* no-op hook, then two forced growths */
        } else if (strcmp(argv[1], "disabled") == 0) {
            expected_full_size = 1024;
            disabled = 1;
        } else return 2;
    }
    lace_start(4, 8192);
    sylvan_set_table_full_handler(unexpected_full);
    for (int policy = expected_full_size ? 2 : 0; policy < 4; policy++) {
        for (int kind = BDD_NODE; kind <= ZDD_MAP; kind++) {
            root_count = collections = hook_calls = 0;
            custom_created = custom_destroyed = 0;
            shrink_hook = policy == 3;
            memset(claim_errors, 0, sizeof(claim_errors));
            sylvan_set_sizes(1024, expected_full_size && !disabled ? expected_full_size : 8192, 1024, 8192);
            sylvan_init_package();
            sylvan_init_mtbdd();
            sylvan_init_ldd();
            sylvan_init_zdd();
            custom_type = sylvan_mt_create_type();
            sylvan_mt_set_hash(custom_type, custom_hash);
            sylvan_mt_set_equals(custom_type, custom_equals);
            sylvan_mt_set_create(custom_type, custom_create);
            sylvan_mt_set_destroy(custom_type, custom_destroy);
            sylvan_gc_set_policy(policy == 0 ? SYLVAN_GC_FEW_COLLECTIONS : SYLVAN_GC_LOW_MEMORY);
            if (policy >= 2) sylvan_gc_hook_main(custom_main_CALL);
            sylvan_gc_add_mark(mark_roots_CALL);
            sylvan_gc_hook_postgc(after_gc_CALL);
            if (disabled) sylvan_gc_disable();
            test_assert(RUN(regression, kind) == 0);
            test_assert(expected_full_size == 0); /* handler must have exited */
            sylvan_quit();
        }
    }
    sylvan_set_table_full_handler(NULL);
    lace_stop();
    return 0;
}
