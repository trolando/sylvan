/* Run each exhaustion case in a separate process: the handler must terminate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sylvan.h>

/* These ZDD primitives have no public declarations, but have failure paths too. */
ZDD zdd_makeleaf(uint16_t type, uint64_t value);
ZDD zdd_makemapnode(uint32_t var, ZDD low, ZDD high);

#define TABLE_SIZE 4096

enum node_kind { BDD_NODE, BDD_LEAF, BDD_MAP, LDD_NODE, LDD_COPY, ZDD_NODE, ZDD_LEAF, ZDD_MAP };
static const struct {
    const char *name;
    const char *kind;
} cases[] = {
    { "bdd", "BDD" }, { "bdd_leaf", "BDD" }, { "bdd_map", "BDD" },
    { "ldd", "MDD" }, { "ldd_copy", "MDD" },
    { "zdd", "ZDD" }, { "zdd_leaf", "ZDD" }, { "zdd_map", "ZDD" }
};

static uint64_t roots[TABLE_SIZE];
static size_t created;
static size_t collected_roots;
static unsigned collections;
static const char *expected_kind;
static int handler_returns;

/* With one worker, these counters need no synchronization. */
VOID_TASK_0(after_gc)
{
    collections++;
    collected_roots = created;
}

static void
table_full(const char *kind, size_t filled, size_t total)
{
    /* Do not call Sylvan operations here, even to inspect the table. */
    if (kind == NULL || strcmp(kind, expected_kind) != 0 ||
        lace_get_worker() == NULL || collections == 0 || collected_roots != created ||
        total != TABLE_SIZE || filled <= total / 2 || filled > total ||
        filled != created + 2) { /* The table reserves buckets 0 and 1. */
        fprintf(stderr, "Invalid table-full callback: %s, %zu of %zu, %zu roots, %u collections\n",
                kind ? kind : "NULL", filled, total, created, collections);
        exit(31);
    }

    fprintf(stderr, "handler: %s, %zu of %zu buckets filled\n", kind, filled, total);
    if (!handler_returns) exit(30);
}

VOID_TASK_1(fill_table, int, node_kind)
{
    for (size_t i = 0; i < TABLE_SIZE; i++) {
        if (strcmp(expected_kind, "MDD") == 0) lddmc_protect(&roots[i]);
        else if (strcmp(expected_kind, "ZDD") == 0) zdd_protect(&roots[i]);
        else mtbdd_protect(&roots[i]);

        switch (node_kind) {
        case BDD_NODE:
            roots[i] = mtbdd_makenode((uint32_t)i, mtbdd_false, mtbdd_true);
            break;
        case BDD_LEAF:
            roots[i] = mtbdd_int64((int64_t)i);
            break;
        case BDD_MAP:
            roots[i] = mtbdd_map_add(mtbdd_map_empty(), (uint32_t)i, mtbdd_true);
            break;
        case LDD_NODE:
            roots[i] = lddmc_makenode((uint32_t)i, lddmc_true, lddmc_false);
            break;
        case LDD_COPY:
            roots[i] = lddmc_make_copynode(i == 0 ? lddmc_true : roots[i - 1], lddmc_false);
            break;
        case ZDD_NODE:
            roots[i] = zdd_makenode((uint32_t)i, zdd_false, zdd_true);
            break;
        case ZDD_LEAF:
            roots[i] = zdd_makeleaf(0, i);
            break;
        case ZDD_MAP:
            roots[i] = zdd_makemapnode((uint32_t)i, zdd_false, zdd_true);
            break;
        }
        created++;
    }
}

int
main(int argc, char **argv)
{
    if (argc != 3) return 2;
    size_t node_kind;
    for (node_kind = 0; node_kind < sizeof(cases) / sizeof(cases[0]); node_kind++) {
        if (strcmp(argv[1], cases[node_kind].name) == 0) break;
    }
    if (node_kind == sizeof(cases) / sizeof(cases[0])) return 2;
    expected_kind = cases[node_kind].kind;

    handler_returns = strcmp(argv[2], "return") == 0;
    if (strcmp(argv[2], "handler") == 0 || handler_returns) {
        sylvan_set_table_full_handler(table_full);
    } else if (strcmp(argv[2], "reset") == 0) {
        sylvan_set_table_full_handler(table_full);
        sylvan_set_table_full_handler(NULL);
    } else if (strcmp(argv[2], "default") != 0) {
        return 2;
    }

    lace_start(1, 8192);
    sylvan_set_sizes(TABLE_SIZE, TABLE_SIZE, 1024, 1024);
    sylvan_init_package();
    if (strcmp(expected_kind, "MDD") == 0) sylvan_init_ldd();
    else if (strcmp(expected_kind, "ZDD") == 0) sylvan_init_zdd();
    else sylvan_init_mtbdd();
    sylvan_gc_hook_postgc(after_gc_CALL);

    RUN(fill_table, (int)node_kind);
    fprintf(stderr, "Node creation unexpectedly survived a full table\n");
    sylvan_quit();
    lace_stop();
    return 2;
}
