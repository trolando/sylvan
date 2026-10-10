/*
 * Regression tests for the traversal scratch bitmap.
 *
 * Marked traversals -- the node and leaf counters, the .dot writers, the
 * structural hashes -- record "already visited" in the node table's scratch
 * bitmap (see <sylvan/nodes.h>), not in a spare bit of the node's own 16-byte
 * record. Such a bit would sit in the word the unique table hashes and
 * compares, so for the length of a walk the key of every visited node would
 * be wrong and a lookup could fail to find a node that was there. The tests
 * below check the two observable consequences of a mark in the record:
 *
 *   - a traversal must give the same answer twice in a row, and
 *   - after a traversal every node must still be found, by its own key, at
 *     the index it had before.
 *
 * The second one is the sharp one: it is what a corrupted key word breaks.
 *
 * Finally the arena is swept for the record bit each node type leaves unused,
 * to confirm no traversal leaves it set. The sweep is only decisive because
 * this test controls what the arena holds: the ListDD phase runs first, on an
 * arena that holds nothing else, so bit 62 can be attributed; and no MTBDDMAP
 * or ZDDMAP node is ever built here, so bits 60 and 61 can be too. A ListDD
 * record never sets bit 60 or 61, so it may stay alive for the later sweeps.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <lace.h>
#include <sylvan/internal.h>
#include <sylvan/platform.h>

#include "test_assert.h"

/**
 * The bit of the first record word that each node type leaves unused.
 */
#define MARK_BIT_MTBDD  UINT64_C(0x2000000000000000)  /* bit 61 */
#define MARK_BIT_ZDD    UINT64_C(0x1000000000000000)  /* bit 60 */
#define MARK_BIT_LISTDD UINT64_C(0x4000000000000000)  /* bit 62 */

/**
 * Sweep every occupied bucket of the arena and count the records that have
 * any bit of <mask> set in their first (hashed) word.
 */
static size_t
sweep_record_bits(uint64_t mask)
{
    size_t hits = 0;
    const size_t size = nodes_get_size(nodes);
    for (size_t index = 2; index < size; index++) {
        if (!nodes_is_marked(nodes, index)) continue;
        const uint64_t *record = (const uint64_t*)nodes_get_pointer(nodes, index);
        if (record[0] & mask) hits++;
    }
    return hits;
}

/**
 * Collector for the "every node is still findable" check. Walking a diagram
 * needs a visited set, and it deliberately does not use the scratch bitmap:
 * the point is to check the traversals, not to trust them.
 */
static uint8_t  *collect_seen  = NULL;
static uint64_t *collect_index = NULL;
static uint32_t *collect_route = NULL;
static size_t    collect_count = 0;

static int
collect_begin(void)
{
    const size_t size = nodes_get_size(nodes);
    collect_seen = (uint8_t*)calloc(size, 1);
    collect_index = (uint64_t*)malloc(size * sizeof(uint64_t));
    collect_route = (uint32_t*)malloc(size * sizeof(uint32_t));
    collect_count = 0;
    return collect_seen != NULL && collect_index != NULL && collect_route != NULL;
}

static void
collect_end(void)
{
    free(collect_seen);
    free(collect_index);
    free(collect_route);
    collect_seen = NULL;
    collect_index = NULL;
    collect_route = NULL;
    collect_count = 0;
}

static int
collect_visit(uint64_t index, uint32_t route)
{
    if (collect_seen[index]) return 0;
    collect_seen[index] = 1;
    collect_index[collect_count] = index;
    collect_route[collect_count] = route;
    collect_count++;
    return 1;
}

static void
collect_mtbdd(MTBDD dd)
{
    if (dd == bdd_true || dd == mtbdd_undefined) return;
    const uint64_t index = MTBDD_GETINDEX(dd);
    mtbddnode* n = MTBDD_GETNODE(dd);
    uint32_t route;
    if (mtbddnode_isleaf(n)) route = NODES_SUBTABLE_LEAF;
    else if (mtbddnode_ismapnode(n)) route = NODES_SUBTABLE_MAP;
    else route = NODES_SUBTABLE_DECISION(mtbddnode_getvariable(n));
    if (!collect_visit(index, route)) return;
    if (mtbddnode_isleaf(n)) return;
    collect_mtbdd(mtbddnode_getlow(n));
    collect_mtbdd(mtbddnode_gethigh(n));
}

static void
collect_zdd(ZDD dd)
{
    const uint64_t index = ZDD_GETINDEX(dd);
    if (index <= 1) return;
    zddnode* n = ZDD_GETNODE(dd);
    uint32_t route;
    if (zddnode_isleaf(n)) route = NODES_SUBTABLE_LEAF;
    else if (zddnode_ismapnode(n)) route = NODES_SUBTABLE_MAP;
    else route = NODES_SUBTABLE_DECISION(zddnode_getvariable(n));
    if (!collect_visit(index, route)) return;
    if (zddnode_isleaf(n)) return;
    collect_zdd(zddnode_getlow(n));
    collect_zdd(zddnode_gethigh(n));
}

static void
collect_listdd(LISTDD dd)
{
    if (dd <= listdd_empty_list) return;
    if (!collect_visit(dd, NODES_SUBTABLE_LISTDD)) return;
    mddnode* n = LDD_GETNODE(dd);
    collect_listdd(mddnode_getright(n));
    collect_listdd(mddnode_getdown(n));
}

/**
 * Look every collected node up again by the key stored in its own record. A
 * traversal that rewrote a key word makes this miss and intern a duplicate,
 * which shows up as a different index and as created != 0.
 */
static int
collect_verify_lookups(void)
{
    for (size_t i = 0; i < collect_count; i++) {
        const uint64_t index = collect_index[i];
        const uint64_t *record = (const uint64_t*)nodes_get_pointer(nodes, index);
        const uint64_t a = record[0], b = record[1];
        int created = -1;
        const uint64_t again =
            nodes_lookup_subtable(nodes, collect_route[i], a, b, &created);
        test_assert(again == index);
        test_assert(created == 0);
    }
    return 0;
}

/**
 * ListDD. Runs first, while the arena holds nothing else, so the sweep for
 * bit 62 can be attributed to ListDD records.
 */

TASK(int, test_markbit_listdd)

int
test_markbit_listdd_CALL(lace_worker *lace)
{
    (void)lace;

    LISTDD set = listdd_empty;
    listdd_refs_pushptr(&set);

    /* A set of tuples that share suffixes, so the diagram is a real DAG. */
    for (uint32_t i = 0; i < 12; i++) {
        for (uint32_t j = 0; j < 12; j++) {
            const uint32_t values[4] = {i, j, i + j, (i * j) % 7};
            test_assert(listdd_add(&set, set, values, 4) == SYLVAN_OK);
        }
    }

    const size_t first = listdd_node_count(set);
    const size_t second = listdd_node_count(set);
    test_assert(first == second);
    test_assert(first > 4);

    size_t levels[4] = {0, 0, 0, 0};
    listdd_node_count_per_level(set, levels);
    size_t levels_again[4] = {0, 0, 0, 0};
    listdd_node_count_per_level(set, levels_again);
    for (size_t i = 0; i < 4; i++) test_assert(levels[i] == levels_again[i]);
    test_assert(levels[0] + levels[1] + levels[2] + levels[3] == first);

    char sha[80], sha_again[80];
    listdd_sha256(set, sha);
    listdd_sha256(set, sha_again);
    test_assert(strcmp(sha, sha_again) == 0);

    test_assert(collect_begin());
    collect_listdd(set);
    test_assert(collect_count == first);
    const int status = collect_verify_lookups();
    collect_end();
    test_assert(status == 0);

    /* Nothing but ListDD records is in the arena yet. */
    test_assert(sweep_record_bits(MARK_BIT_LISTDD) == 0);

    listdd_refs_popptr(1);
    return 0;
}

/**
 * MTBDD and BDD.
 */

TASK(int, test_markbit_mtbdd)

int
test_markbit_mtbdd_CALL(lace_worker *lace)
{
    MTBDD roots[3] = {mtbdd_undefined, mtbdd_undefined, mtbdd_undefined};
    MTBDD shared = mtbdd_undefined;
    MTBDD variable = mtbdd_undefined;
    mtbdd_protect(&roots[0]);
    mtbdd_protect(&roots[1]);
    mtbdd_protect(&roots[2]);
    mtbdd_protect(&shared);
    mtbdd_protect(&variable);

    /* A shared spine: every root is built on top of the same subdiagram. */
    shared = bdd_true;
    for (uint32_t level = 6; level < 12; level++) {
        test_assert(bdd_var_at_level(&variable, level) == SYLVAN_OK);
        test_assert(bdd_xor_CALL(lace, &shared, shared, variable) == SYLVAN_OK);
    }

    for (uint32_t r = 0; r < 3; r++) {
        test_assert(bdd_var_at_level(&variable, r) == SYLVAN_OK);
        test_assert(bdd_and_CALL(lace, &roots[r], variable, shared) == SYLVAN_OK);
    }

    /* An MTBDD with real leaves, so the leaf counter has work to do. */
    MTBDD terminals = mtbdd_undefined;
    mtbdd_protect(&terminals);
    test_assert(bdd_var_at_level(&variable, 3) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(
        lace, &terminals, variable, mtbdd_double(1.5), mtbdd_double(2.5)) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&variable, 4) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(
        lace, &terminals, variable, terminals, mtbdd_double(3.5)) == SYLVAN_OK);

    const size_t nodes_first = mtbdd_shared_node_count(roots, 3);
    const size_t nodes_second = mtbdd_shared_node_count(roots, 3);
    test_assert(nodes_first == nodes_second);
    test_assert(nodes_first > 6);

    const size_t single_first = mtbdd_node_count(roots[0]);
    const size_t single_second = mtbdd_node_count(roots[0]);
    test_assert(single_first == single_second);

    /* Sharing is real: the three roots together cost less than three times
       one of them, so the counters do cross previously marked nodes. */
    test_assert(nodes_first < 3 * single_first);

    const size_t leaves_first = mtbdd_leaf_count(terminals);
    const size_t leaves_second = mtbdd_leaf_count(terminals);
    test_assert(leaves_first == leaves_second);
    test_assert(leaves_first == 3);

    char sha[80], sha_again[80];
    mtbdd_sha256(roots[0], sha);
    mtbdd_sha256(roots[0], sha_again);
    test_assert(strcmp(sha, sha_again) == 0);

    test_assert(collect_begin());
    for (int r = 0; r < 3; r++) collect_mtbdd(roots[r]);
    collect_mtbdd(terminals);
    const int status = collect_verify_lookups();
    const size_t collected = collect_count;
    collect_end();
    test_assert(status == 0);
    test_assert(collected >= nodes_first);

    /* Counting again after the lookups still agrees. */
    test_assert(mtbdd_shared_node_count(roots, 3) == nodes_first);
    test_assert(mtbdd_leaf_count(terminals) == leaves_first);

    test_assert(sweep_record_bits(MARK_BIT_MTBDD) == 0);

    /* Positive control: the sweep really is reading live records. Bit 62 is
       the MTBDD leaf flag, and the three double leaves above are the only
       records in the arena that set it -- a ListDD record uses bits 0..55 and
       63 only, and no ZDD node exists yet. */
    test_assert(sweep_record_bits(UINT64_C(0x4000000000000000)) == 3);

    mtbdd_unprotect(&terminals);
    mtbdd_unprotect(&variable);
    mtbdd_unprotect(&shared);
    mtbdd_unprotect(&roots[2]);
    mtbdd_unprotect(&roots[1]);
    mtbdd_unprotect(&roots[0]);
    return 0;
}

/**
 * ZDD.
 */

TASK(int, test_markbit_zdd)

int
test_markbit_zdd_CALL(lace_worker *lace)
{
    BDDSET domain = mtbdd_invalid;
    BDD bdd = bdd_true;
    BDD variable = mtbdd_invalid;
    ZDD zdds[2] = {zdd_false, zdd_false};
    mtbdd_protect(&domain);
    mtbdd_protect(&bdd);
    mtbdd_protect(&variable);
    zdd_protect(&zdds[0]);
    zdd_protect(&zdds[1]);

    const uint32_t levels[6] = {0, 1, 2, 3, 4, 5};
    test_assert(bdd_set_from_array(&domain, levels, 6) == SYLVAN_OK);

    for (size_t i = 0; i < 6; i += 2) {
        test_assert(bdd_var_at_level(&variable, levels[i]) == SYLVAN_OK);
        test_assert(bdd_or_CALL(lace, &bdd, bdd, variable) == SYLVAN_OK);
    }
    test_assert(zdd_from_bdd_CALL(lace, &zdds[0], bdd, domain) == SYLVAN_OK);
    test_assert(zdd_from_bdd_CALL(lace, &zdds[1], bdd_not(bdd), domain) == SYLVAN_OK);

    const size_t first = zdd_shared_node_count(zdds, 2);
    const size_t second = zdd_shared_node_count(zdds, 2);
    test_assert(first == second);
    test_assert(first > 2);

    const size_t single_first = zdd_node_count(zdds[0]);
    test_assert(zdd_node_count(zdds[0]) == single_first);

    test_assert(collect_begin());
    collect_zdd(zdds[0]);
    collect_zdd(zdds[1]);
    const int status = collect_verify_lookups();
    collect_end();
    test_assert(status == 0);

    test_assert(zdd_shared_node_count(zdds, 2) == first);

    test_assert(sweep_record_bits(MARK_BIT_ZDD) == 0);

    zdd_unprotect(&zdds[1]);
    zdd_unprotect(&zdds[0]);
    mtbdd_unprotect(&variable);
    mtbdd_unprotect(&bdd);
    mtbdd_unprotect(&domain);
    return 0;
}

TASK(int, runtests)

int
runtests_CALL(lace_worker *lace)
{
    printf("test_markbit_listdd...\n");
    if (test_markbit_listdd_CALL(lace)) return 1;
    printf("test_markbit_mtbdd...\n");
    if (test_markbit_mtbdd_CALL(lace)) return 1;
    printf("test_markbit_zdd...\n");
    if (test_markbit_zdd_CALL(lace)) return 1;

    /* One last sweep over everything the whole run left behind. */
    printf("test_markbit_sweep...\n");
    if (sweep_record_bits(MARK_BIT_MTBDD | MARK_BIT_ZDD) != 0) {
        fprintf(stderr, "a record still carries a traversal mark bit\n");
        return 1;
    }
    return 0;
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    lace_start(4, 0, 0);
    sylvan_set_sizes(1LL<<20, 1LL<<20, 1LL<<16, 1LL<<16);
    if (sylvan_init_package() != SYLVAN_OK ||
        listdd_init() != SYLVAN_OK ||
        mtbdd_init() != SYLVAN_OK ||
        zdd_init() != SYLVAN_OK) {
        fprintf(stderr, "Sylvan initialization failed.\n");
        return 1;
    }

    const int result = runtests();

    sylvan_quit();
    lace_stop();
    return result;
}
