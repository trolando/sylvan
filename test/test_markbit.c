/* Traversal regression adapted from sylvan-2 test_markbit.c.
 * Save the keys independently of the traversals and check them both during
 * and after traversal. A leaf printer provides a deterministic observation
 * point while its ancestors are marked; parallel walks also cover all three
 * node representations and all counters. LDD's old mark is bit 0 in 1.x.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sylvan_int.h>
#include "test_assert.h"

static MTBDD roots[3], terminals, callback_root;
static MDD tuples;
static ZDD zdds[2];
static uint8_t *seen;
struct saved_node { uint64_t index, a, b, mark; };
static struct saved_node *saved;
static size_t saved_count;
static size_t bcount, leaves, lcount, zcount, callback_count;
static char bsha[65], lsha[65];
static int callback_failed, callback_calls;

static int remember(uint64_t index, uint64_t mark)
{
    if (index <= 1 || seen[index]) return 0;
    seen[index] = 1;
    const uint64_t *n = llmsset_index_to_ptr(nodes, index);
    saved[saved_count++] = (struct saved_node){index, n[0], n[1], mark};
    return 1;
}

static void collect_mtbdd(MTBDD dd)
{
    if (!remember(dd & UINT64_C(0xffffffffff), UINT64_C(1)<<61)) return;
    mtbddnode_t n = MTBDD_GETNODE(dd);
    if (mtbddnode_isleaf(n)) return;
    collect_mtbdd(mtbddnode_getlow(n));
    collect_mtbdd(mtbddnode_gethigh(n));
}

static void collect_ldd(MDD dd)
{
    if (!remember(dd, 1)) return;
    collect_ldd(lddmc_getright(dd));
    collect_ldd(lddmc_getdown(dd));
}

static void collect_zdd(ZDD dd)
{
    if (!remember(ZDD_GETINDEX(dd), UINT64_C(1)<<60)) return;
    zddnode_t n = ZDD_GETNODE(dd);
    collect_zdd(zddnode_getlow(n));
    collect_zdd(zddnode_gethigh(n));
}

static int verify_lookups(void)
{
    for (size_t i = 0; i < saved_count; i++) {
        const struct saved_node *s = &saved[i];
        int created = -1;
        uint64_t again = llmsset_lookup(nodes, s->a, s->b, &created);
        test_assert(again == s->index);
        test_assert(created == 0);
    }
    return 0;
}

static int verify_records(void)
{
    for (size_t i = 0; i < saved_count; i++) {
        const struct saved_node *s = &saved[i];
        const uint64_t *n = llmsset_index_to_ptr(nodes, s->index);
        test_assert((n[0] & s->mark) == 0);
        test_assert(n[0] == s->a && n[1] == s->b);
    }
    return 0;
}

/* Called with the leaf and all its ancestors still marked. Nested counting
 * must see an independent visited set. GC must not consume traversal marks,
 * and a table resize must not invalidate the suspended walk's bitmap. */
static char *print_leaf(int complement, uint64_t value, char *buf, size_t size)
{
    (void)complement;
    (void)value;
    callback_calls++;
    callback_failed |= verify_records();
    callback_failed |= verify_lookups();
    if (!callback_failed) {
        if (mtbdd_nodecount(callback_root) != callback_count) callback_failed = 1;
        size_t before = llmsset_get_size(nodes);
        sylvan_gc();
        if (llmsset_get_size(nodes) <= before) callback_failed = 1;
        if (mtbdd_nodecount(callback_root) != callback_count) callback_failed = 1;
        callback_failed |= verify_lookups();
    }
    snprintf(buf, size, "leaf");
    return buf;
}

static int check_traversals(void)
{
    test_assert(mtbdd_nodecount_more(roots, 3) == bcount);
    test_assert(mtbdd_leafcount(terminals) == leaves);
    MTBDD repeated[] = {terminals, terminals};
    test_assert(mtbdd_leafcount_more(repeated, 2) == leaves);
    test_assert(lddmc_nodecount(tuples) == lcount);
    size_t levels[4] = {0};
    lddmc_nodecount_levels(tuples, levels);
    test_assert(levels[0] + levels[1] + levels[2] + levels[3] == lcount);
    test_assert(zdd_nodecount(zdds, 2) == zcount);
    char sha[65];
    mtbdd_getsha(roots[0], sha);
    test_assert(strcmp(sha, bsha) == 0);
    lddmc_getsha(tuples, sha);
    test_assert(strcmp(sha, lsha) == 0);
    return 0;
}

TASK_1(int, walk_many, int, depth)
{
    if (depth) {
        SPAWN(walk_many, depth-1);
        int result = CALL(walk_many, depth-1);
        return SYNC(walk_many) | result;
    }
    for (int i = 0; i < 100; i++) {
        if (check_traversals()) return 1;
    }
    return 0;
}

TASK_0(int, run_test)
{
    for (int i = 0; i < 3; i++) mtbdd_protect(&roots[i]);
    mtbdd_protect(&terminals);
    mtbdd_protect(&callback_root);
    lddmc_protect(&tuples);
    for (int i = 0; i < 2; i++) zdd_protect(&zdds[i]);

    /* A shared DAG of tuples, as in the upstream test. */
    for (uint32_t i = 0; i < 12; i++) {
        for (uint32_t j = 0; j < 12; j++) {
            uint32_t values[4] = {i, j, i+j, (i*j)%7};
            tuples = lddmc_union_cube(tuples, values, 4);
        }
    }
    MTBDD shared = mtbdd_true;
    mtbdd_protect(&shared);
    for (uint32_t v = 6; v < 16; v++) shared = sylvan_xor(shared, sylvan_ithvar(v));
    for (uint32_t r = 0; r < 3; r++) roots[r] = sylvan_and(sylvan_ithvar(r), shared);
    mtbdd_unprotect(&shared);
    terminals = mtbdd_makenode(4, mtbdd_double(1.5), mtbdd_double(2.5));
    terminals = mtbdd_makenode(3, terminals, mtbdd_double(3.5));
    uint32_t vars[16];
    for (uint32_t i = 0; i < 16; i++) vars[i] = i;
    BDD domain = sylvan_set_fromarray(vars, 16);
    mtbdd_protect(&domain);
    zdds[0] = zdd_from_mtbdd(roots[0], domain);
    zdds[1] = zdd_from_mtbdd(sylvan_not(roots[0]), domain);
    mtbdd_unprotect(&domain);

    uint32_t type = sylvan_mt_create_type();
    sylvan_mt_set_to_str(type, print_leaf);
    callback_root = mtbdd_makeleaf(type, 42);
    for (int v = 15; v >= 0; v--) callback_root = mtbdd_makenode((uint32_t)v, mtbdd_false, callback_root);
    callback_count = mtbdd_nodecount(callback_root);
    bcount = mtbdd_nodecount_more(roots, 3);
    leaves = mtbdd_leafcount(terminals);
    lcount = lddmc_nodecount(tuples);
    zcount = zdd_nodecount(zdds, 2);
    test_assert(leaves == 3 && lcount > 4 && zcount > 2);
    test_assert(bcount < 3 * mtbdd_nodecount(roots[0]));
    mtbdd_getsha(roots[0], bsha);
    lddmc_getsha(tuples, lsha);

    size_t size = llmsset_get_size(nodes);
    seen = calloc(size, 1);
    saved = malloc(size * sizeof(*saved));
    test_assert(seen != NULL && saved != NULL);
    collect_ldd(tuples);
    for (int i = 0; i < 3; i++) collect_mtbdd(roots[i]);
    collect_mtbdd(terminals);
    collect_mtbdd(callback_root);
    for (int i = 0; i < 2; i++) collect_zdd(zdds[i]);

    FILE *out = fopen("test-markbit.dot", "w");
    test_assert(out != NULL);
    mtbdd_fprintdot(out, callback_root);
    mtbdd_fprintdot_nc(out, callback_root);
    lddmc_fprintdot(out, tuples);
    zdd_fprintdot(out, zdds[0]);
    test_assert(fclose(out) == 0);
    test_assert(remove("test-markbit.dot") == 0);
    test_assert(callback_calls == 2 && !callback_failed);
    test_assert(verify_records() == 0);
    test_assert(check_traversals() == 0);

    /* Several simultaneous traversals, with lookups and collections on the
     * calling worker. Collections wait at Lace's normal safepoints. */
    SPAWN(walk_many, 2);
    int failed = 0;
    for (int i = 0; i < 100; i++) {
        failed |= verify_lookups();
        if (i % 10 == 0) CALL(sylvan_gc);
    }
    failed |= SYNC(walk_many);
    test_assert(failed == 0);
    test_assert(verify_records() == 0);
    test_assert(check_traversals() == 0);
    free(saved);
    free(seen);
    for (int i = 0; i < 3; i++) mtbdd_unprotect(&roots[i]);
    mtbdd_unprotect(&terminals);
    mtbdd_unprotect(&callback_root);
    lddmc_unprotect(&tuples);
    for (int i = 0; i < 2; i++) zdd_unprotect(&zdds[i]);
    return 0;
}

int main(void)
{
    lace_start(4, 0);
    sylvan_set_sizes(1LL<<12, 1LL<<18, 1LL<<12, 1LL<<16);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_ldd();
    sylvan_init_zdd();
    sylvan_gc_hook_main(sylvan_gc_aggressive_resize_CALL);
    int result = RUN(run_test);
    sylvan_quit();
    lace_stop();
    return result;
}
