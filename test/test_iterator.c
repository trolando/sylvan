#include <sylvan_int.h>
#include "test_assert.h"

/* Internal constructor, used only to check rejection of map records. */
ZDD zdd_makemapnode(uint32_t var, ZDD low, ZDD high);

/* Deliberately nonconsecutive variables, with extra variables before, between
 * and after the support. Truth tables and tuple arrays are independent oracles. */
static uint32_t support[] = {2, 6, 8};
static uint32_t domain_levels[] = {0, 2, 4, 6, 8, 10};
static unsigned collections;

VOID_TASK_0(count_collection)
{
    collections++;
}

static MTBDD
from_table(const MTBDD *table, unsigned depth, unsigned assignment, int zdd)
{
    if (depth == 3) return table[assignment];
    MTBDD low = from_table(table, depth+1, assignment, zdd);
    if (zdd) zdd_refs_push(low); else mtbdd_refs_push(low);
    MTBDD high = from_table(table, depth+1, assignment | (1u << depth), zdd);
    MTBDD result = zdd ? zdd_makenode(support[depth], low, high)
                       : mtbdd_makenode(support[depth], low, high);
    if (zdd) zdd_refs_pop(1); else mtbdd_refs_pop(1);
    return result;
}

static int
numeric_zero(MTBDD leaf)
{
    return leaf != mtbdd_true && mtbdd_gettype(leaf) == 0 && mtbdd_getint64(leaf) == 0;
}

struct filter_context { unsigned calls; int unexpected_false; unsigned call_limit; };

static int
accept_zero(MTBDD leaf, void *context)
{
    struct filter_context *ctx = context;
    ctx->calls++;
    if (ctx->call_limit != 0 && ctx->calls > ctx->call_limit) {
        fprintf(stderr, "Leaf filter exceeded its work bound\n");
        exit(1);
    }
    if (leaf == mtbdd_false) { ctx->unexpected_false = 1; return 0; }
    return numeric_zero(leaf);
}

/* Check exact order against the old enumerators and expand every emitted cube
 * against the truth table. This also detects duplicate/overlapping cubes. */
static int
check_assignments(MTBDD dd, uint64_t domain, const MTBDD *table, int family,
                  sylvan_iterator_mode mode, int filtered, int collect)
{
    sylvan_iterator *it;
    struct filter_context ctx = {0};
    mtbdd_iterator_options options = {mode, filtered ? accept_zero : NULL, &ctx};
    if (family == 0) test_assert(sylvan_iterator_create(dd, domain, mode, &it));
    else if (family == 1) test_assert(mtbdd_iterator_create(dd, domain, &options, &it));
    else test_assert(zdd_iterator_create(dd, domain, &it));
    /* Options are copied, not retained. */
    options.mode = (sylvan_iterator_mode)99;
    options.accept_leaf = NULL;
    options.context = NULL;

    unsigned seen[64] = {0};
    uint8_t values[6], old[6];
    mtbdd_enum_filter_cb filter = filtered ? numeric_zero : NULL;
    MTBDD expected = family == 2 ? zdd_enum_first(dd, domain, old, NULL) :
        mode == SYLVAN_ITERATOR_CUBES ? mtbdd_enum_first(dd, domain, old, filter) :
        mtbdd_enum_all_first(dd, domain, old, filter);
    for (;;) {
        int has_item = -1;
        MTBDD leaf = mtbdd_true;
        if (collect) sylvan_gc();
        if (family == 0) test_assert(sylvan_iterator_next(it, values, 6, &has_item));
        else if (family == 1) test_assert(mtbdd_iterator_next(it, values, 6, &leaf, &has_item));
        else test_assert(zdd_iterator_next(it, values, 6, &has_item));
        if (!has_item) break;
        test_assert(expected != mtbdd_false && leaf == expected);
        test_assert(memcmp(values, old, 6) == 0);
        for (unsigned i=0; i<6; i++) {
            test_assert(values[i] <= (mode == SYLVAN_ITERATOR_CUBES && family != 2 ? 2 : 1));
        }
        for (unsigned a=0; a<64; a++) {
            int matches = 1;
            for (unsigned i=0; i<6; i++) {
                if (values[i] != 2 && values[i] != ((a >> i) & 1)) matches = 0;
            }
            if (!matches) continue;
            unsigned input = ((a >> 1) & 1) | ((a >> 2) & 2) | ((a >> 2) & 4);
            test_assert(!seen[a]++);
            test_assert(table[input] == leaf);
            if (family == 2) test_assert((a & 37u) == 0); /* extra variables */
            if (filtered) test_assert(numeric_zero(leaf));
        }
        expected = family == 2 ? zdd_enum_next(dd, domain, old, NULL) :
            mode == SYLVAN_ITERATOR_CUBES ? mtbdd_enum_next(dd, domain, old, filter) :
            mtbdd_enum_all_next(dd, domain, old, filter);
    }
    test_assert(expected == mtbdd_false);
    for (unsigned a=0; a<64; a++) {
        unsigned input = ((a >> 1) & 1) | ((a >> 2) & 2) | ((a >> 2) & 4);
        int present = table[input] != mtbdd_false && (!filtered || numeric_zero(table[input]));
        if (family == 2 && (a & 37u)) present = 0;
        test_assert(seen[a] == (unsigned)present);
    }
    test_assert(!ctx.unexpected_false);
    memset(values, 0xa5, sizeof(values));
    MTBDD leaf = mtbdd_invalid;
    int has_item = -1;
    if (family == 0) test_assert(sylvan_iterator_next(it, values, 6, &has_item));
    else if (family == 1) test_assert(mtbdd_iterator_next(it, values, 6, &leaf, &has_item));
    else test_assert(zdd_iterator_next(it, values, 6, &has_item));
    test_assert(has_item == 0 && values[0] == 0xa5 && leaf == mtbdd_invalid);
    if (family == 0) sylvan_iterator_destroy(it);
    else if (family == 1) mtbdd_iterator_destroy(it);
    else zdd_iterator_destroy(it);
    return 0;
}

TASK_0(int, test_tables)
{
    BDDSET vars = mtbdd_refs_push(sylvan_set_fromarray(domain_levels, 6));
    ZDD dom = zdd_refs_push(zdd_set_from_mtbdd(vars));
    MTBDD table[8];
    for (unsigned bits=0; bits<256; bits++) {
        for (unsigned i=0; i<8; i++) table[i] = (bits >> i) & 1 ? mtbdd_true : mtbdd_false;
        BDD bdd = mtbdd_refs_push(from_table(table, 0, 0, 0));
        test_assert(check_assignments(bdd, vars, table, 0, SYLVAN_ITERATOR_CUBES, 0, bits == 91) == 0);
        test_assert(check_assignments(bdd, vars, table, 0, SYLVAN_ITERATOR_MINTERMS, 0, bits == 91) == 0);
        /* Shared complemented subgraphs are exercised by both a function and
         * its complement over all Boolean functions of three variables. */
        ZDD zdd = zdd_refs_push(from_table(table, 0, 0, 1));
        test_assert(check_assignments(zdd, dom, table, 2, SYLVAN_ITERATOR_MINTERMS, 0, bits == 91) == 0);
        zdd_refs_pop(1);
        mtbdd_refs_pop(1);
    }
    MTBDD leaves[] = {mtbdd_false, mtbdd_true, mtbdd_refs_push(mtbdd_int64(0)),
        mtbdd_refs_push(mtbdd_int64(-7)), mtbdd_refs_push(mtbdd_double(3.5))};
    unsigned random = 17;
    for (unsigned trial=0; trial<64; trial++) {
        for (unsigned i=0; i<8; i++) {
            random = random * 1664525u + 1013904223u;
            table[i] = leaves[(random >> 16) % 5];
        }
        MTBDD dd = mtbdd_refs_push(from_table(table, 0, 0, 0));
        for (int mode=0; mode<2; mode++) for (int filter=0; filter<2; filter++) {
            test_assert(check_assignments(dd, vars, table, 1, (sylvan_iterator_mode)mode,
                                          filter, trial == 7) == 0);
        }
        mtbdd_refs_pop(1);
    }
    mtbdd_refs_pop(4);
    zdd_refs_pop(1);
    return 0;
}

struct tuples { uint32_t values[27][3]; size_t count; int bad; };

VOID_TASK_3(save_tuple, uint32_t*, values, size_t, count, void*, context)
{
    struct tuples *tuples = context;
    if (count != 3 || tuples->count >= 27) { tuples->bad = 1; return; }
    memcpy(tuples->values[tuples->count++], values, 3*sizeof(*values));
}

TASK_0(int, test_tuples)
{
    uint32_t alphabet[] = {0, 19, UINT32_MAX};
    for (unsigned trial=0; trial<16; trial++) {
        MDD dd = lddmc_false;
        lddmc_refs_pushptr(&dd);
        struct tuples expected = {0}, old = {0};
        for (unsigned a=0; a<3; a++) for (unsigned b=0; b<3; b++) for (unsigned c=0; c<3; c++) {
            if (trial != 0 && ((a*9+b*3+c)*7+trial) % 5 == 0) continue;
            uint32_t values[] = {alphabet[a], alphabet[b], alphabet[c]};
            dd = lddmc_union_cube(dd, values, 3);
            memcpy(expected.values[expected.count++], values, sizeof(values));
        }
        CALL(lddmc_sat_all_nopar, dd, save_tuple_CALL, &old);
        test_assert(!old.bad && old.count == expected.count);
        test_assert(memcmp(old.values, expected.values, expected.count*sizeof(old.values[0])) == 0);
        sylvan_iterator *it;
        test_assert(lddmc_iterator_create(dd, &it));
        lddmc_refs_popptr(1); /* The iterator is now the only root. */
        for (size_t i=0; i<expected.count; i++) {
            if (trial == 0) CALL(sylvan_gc);
            uint32_t values[3];
            int has_item;
            test_assert(lddmc_iterator_next(it, values, 3, &has_item) && has_item);
            test_assert(memcmp(values, expected.values[i], sizeof(values)) == 0);
        }
        uint32_t values[] = {123, 456, 789};
        int has_item = -1;
        for (int i=0; i<2; i++) {
            test_assert(lddmc_iterator_next(it, values, 3, &has_item) && !has_item);
            test_assert(values[0] == 123 && values[1] == 456 && values[2] == 789);
        }
        lddmc_iterator_destroy(it);
    }
    return 0;
}

TASK_0(int, test_edges)
{
    sylvan_iterator *it, *untouched = NULL;
    mtbdd_iterator_options options = {SYLVAN_ITERATOR_MINTERMS, NULL, NULL};
    for (int truth=0; truth<2; truth++) for (int mode=0; mode<2; mode++) {
        MTBDD dd = truth ? mtbdd_true : mtbdd_false;
        options.mode = (sylvan_iterator_mode)mode;
        test_assert(sylvan_iterator_create(dd, sylvan_true, options.mode, &it));
        int has_item = -1;
        test_assert(sylvan_iterator_next(it, NULL, 0, &has_item) && has_item == truth);
        test_assert(sylvan_iterator_next(it, NULL, 0, &has_item) && !has_item);
        sylvan_iterator_destroy(it);
        test_assert(mtbdd_iterator_create(dd, mtbdd_true, &options, &it));
        MTBDD leaf = mtbdd_invalid;
        test_assert(mtbdd_iterator_next(it, NULL, 0, &leaf, &has_item) && has_item == truth);
        test_assert(leaf == (truth ? mtbdd_true : mtbdd_invalid));
        mtbdd_iterator_destroy(it);
        test_assert(zdd_iterator_create(truth ? zdd_true : zdd_false, zdd_true, &it));
        test_assert(zdd_iterator_next(it, NULL, 0, &has_item) && has_item == truth);
        zdd_iterator_destroy(it);
        test_assert(lddmc_iterator_create(truth ? lddmc_true : lddmc_false, &it));
        test_assert(lddmc_iterator_next(it, NULL, 0, &has_item) && has_item == truth);
        test_assert(lddmc_iterator_next(it, NULL, 0, &has_item) && !has_item);
        lddmc_iterator_destroy(it);
    }
    BDDSET vars = mtbdd_refs_push(sylvan_set_fromarray(domain_levels, 6));
    ZDD dom = zdd_refs_push(zdd_set_from_mtbdd(vars));
    uint8_t values[6];
    int has_item;
    test_assert(zdd_iterator_create(dom, dom, &it));
    for (unsigned i=0; i<64; i++) {
        test_assert(zdd_iterator_next(it, values, 6, &has_item) && has_item);
        for (unsigned j=0; j<6; j++) test_assert(values[j] == ((i >> (5-j)) & 1));
    }
    test_assert(zdd_iterator_next(it, values, 6, &has_item) && !has_item);
    zdd_iterator_destroy(it);

    /* Reject a terminal before expanding an astronomically large minterm set. */
    uint32_t levels[81];
    for (unsigned i=0; i<81; i++) levels[i] = i;
    BDDSET wide = mtbdd_refs_push(sylvan_set_fromarray(levels, 81));
    struct filter_context ctx = {0};
    options = (mtbdd_iterator_options){SYLVAN_ITERATOR_MINTERMS, accept_zero, &ctx};
    test_assert(mtbdd_iterator_create(mtbdd_true, wide, &options, &it));
    uint8_t buffer[81];
    MTBDD leaf = mtbdd_invalid;
    test_assert(mtbdd_iterator_next(it, buffer, 81, &leaf, &has_item) && !has_item);
    test_assert(ctx.calls == 1 && leaf == mtbdd_invalid && !ctx.unexpected_false);
    mtbdd_iterator_destroy(it);

    /* A nonconstant rejected suffix must not be replayed 2^80 times for the
     * skipped prefix. Fail immediately on excess work instead of timing out. */
    MTBDD low = mtbdd_refs_push(mtbdd_int64(1));
    MTBDD high = mtbdd_refs_push(mtbdd_int64(2));
    MTBDD rejected = mtbdd_refs_push(mtbdd_makenode(80, low, high));
    ctx = (struct filter_context){0, 0, 2};
    test_assert(mtbdd_iterator_create(rejected, wide, &options, &it));
    memset(buffer, 0xa5, sizeof(buffer));
    test_assert(mtbdd_iterator_next(it, buffer, 81, &leaf, &has_item) && !has_item);
    test_assert(ctx.calls == 2 && leaf == mtbdd_invalid && !ctx.unexpected_false);
    for (size_t i=0; i<sizeof(buffer); i++) test_assert(buffer[i] == 0xa5);
    test_assert(mtbdd_iterator_next(it, buffer, 81, &leaf, &has_item) && !has_item);
    test_assert(ctx.calls == 2);
    mtbdd_iterator_destroy(it);
    mtbdd_refs_pop(3);

    options.accept_leaf = NULL;
    test_assert(!sylvan_iterator_create(sylvan_true, vars, SYLVAN_ITERATOR_CUBES, NULL));
    test_assert(!sylvan_iterator_create(sylvan_true, vars, (sylvan_iterator_mode)2, &untouched));
    test_assert(!mtbdd_iterator_create(mtbdd_true, vars, NULL, &untouched));
    test_assert(!zdd_iterator_create(zdd_true, dom, NULL));
    test_assert(!lddmc_iterator_create(lddmc_true, NULL));
    test_assert(!lddmc_iterator_create(UINT64_MAX, &untouched));
    test_assert(!sylvan_iterator_create(sylvan_invalid, vars, SYLVAN_ITERATOR_CUBES, &untouched));
    test_assert(!mtbdd_iterator_create(mtbdd_invalid, vars, &options, &untouched));
    test_assert(!zdd_iterator_create(zdd_invalid, dom, &untouched));
    test_assert(!sylvan_iterator_create(sylvan_true, sylvan_false, SYLVAN_ITERATOR_CUBES, &untouched));
    test_assert(!mtbdd_iterator_create(mtbdd_true, sylvan_invalid, &options, &untouched));
    test_assert(!zdd_iterator_create(zdd_true, zdd_false, &untouched));
    test_assert(!zdd_iterator_create(zdd_true, vars, &untouched)); /* BDD cube is not a ZDD domain */
    test_assert(!sylvan_iterator_create(sylvan_true, dom, SYLVAN_ITERATOR_CUBES, &untouched));
    BDD x = mtbdd_refs_push(sylvan_ithvar(8));
    MTBDD number = mtbdd_refs_push(mtbdd_int64(0));
    test_assert(!sylvan_iterator_create(number, vars, SYLVAN_ITERATOR_CUBES, &untouched));
    test_assert(!zdd_iterator_create(number, dom, &untouched));
    test_assert(!sylvan_iterator_create(x, sylvan_true, SYLVAN_ITERATOR_CUBES, &untouched));
    test_assert(!mtbdd_iterator_create(x, sylvan_true, &options, &untouched));
    ZDD zx = zdd_refs_push(zdd_makenode(8, zdd_false, zdd_true));
    test_assert(!zdd_iterator_create(zx, zdd_true, &untouched));
    options.accept_leaf = accept_zero;
    test_assert(!mtbdd_iterator_create(x, mtbdd_true, &options, &untouched));
    options.accept_leaf = NULL;
    uint64_t outside = llmsset_get_data_size(nodes);
    test_assert(!sylvan_iterator_create(outside, vars, SYLVAN_ITERATOR_CUBES, &untouched));
    test_assert(!mtbdd_iterator_create(outside, vars, &options, &untouched));
    test_assert(!zdd_iterator_create(outside, dom, &untouched));
    test_assert(!lddmc_iterator_create(outside, &untouched));
    test_assert(mtbdd_iterator_create(number, mtbdd_true, &options, &it));
    test_assert(mtbdd_iterator_next(it, NULL, 0, &leaf, &has_item) && has_item && leaf == number);
    mtbdd_iterator_destroy(it);
    MTBDD map = mtbdd_refs_push(mtbdd_map_add(mtbdd_map_empty(), 2, x));
    test_assert(!sylvan_iterator_create(map, vars, SYLVAN_ITERATOR_CUBES, &untouched));
    test_assert(!mtbdd_iterator_create(map, vars, &options, &untouched));
    test_assert(!zdd_iterator_create(map, dom, &untouched));
    MTBDD embedded = mtbdd_refs_push(mtbdd_makenode(0, mtbdd_false, map));
    test_assert(!mtbdd_iterator_create(embedded, vars, &options, &untouched));
    ZDD zmap = zdd_refs_push(zdd_makemapnode(2, zdd_false, zx));
    test_assert(!zdd_iterator_create(zmap, dom, &untouched));
    test_assert(!mtbdd_iterator_create(zmap, vars, &options, &untouched));
    test_assert(!zdd_iterator_create(zdd_true, zmap, &untouched));
    MDD copy = lddmc_refs_push(lddmc_make_copynode(lddmc_true, lddmc_false));
    test_assert(!lddmc_iterator_create(copy, &untouched));
    MDD hidden_copy = lddmc_refs_push(lddmc_makenode(4, copy, lddmc_false));
    test_assert(!lddmc_iterator_create(hidden_copy, &untouched));
    MDD suffix = lddmc_refs_push(lddmc_makenode(2, lddmc_true, lddmc_false));
    MDD mixed = lddmc_refs_push(lddmc_makenode(0, suffix, suffix));
    test_assert(!lddmc_iterator_create(mixed, &untouched)); /* shared suffix, two depths */
    test_assert(untouched == NULL);

    /* Argument errors must not advance an iterator or overwrite any output. */
    test_assert(mtbdd_iterator_create(x, vars, &options, &it));
    has_item = 71; leaf = mtbdd_invalid;
    memset(values, 0xa5, sizeof(values));
    test_assert(!mtbdd_iterator_next(NULL, values, 6, &leaf, &has_item));
    test_assert(!mtbdd_iterator_next(it, NULL, 6, &leaf, &has_item));
    test_assert(!mtbdd_iterator_next(it, values, 5, &leaf, &has_item));
    test_assert(!mtbdd_iterator_next(it, values, 6, NULL, &has_item));
    test_assert(!mtbdd_iterator_next(it, values, 6, &leaf, NULL));
    test_assert(!sylvan_iterator_next(it, values, 6, &has_item));
    test_assert(!zdd_iterator_next(it, values, 6, &has_item));
    uint32_t tuple = 123;
    test_assert(!lddmc_iterator_next(it, &tuple, 1, &has_item));
    test_assert(has_item == 71 && leaf == mtbdd_invalid && values[0] == 0xa5 && tuple == 123);
    test_assert(mtbdd_iterator_next(it, values, 6, &leaf, &has_item) && has_item);
    test_assert(values[0] == 0 && values[4] == 1 && leaf == mtbdd_true);
    mtbdd_iterator_destroy(it);
    test_assert(lddmc_iterator_create(suffix, &it));
    has_item = 71;
    test_assert(!lddmc_iterator_next(it, &tuple, 0, &has_item));
    test_assert(!lddmc_iterator_next(it, NULL, 1, &has_item));
    test_assert(!lddmc_iterator_next(it, &tuple, 1, NULL));
    test_assert(has_item == 71 && tuple == 123);
    test_assert(lddmc_iterator_next(it, &tuple, 1, &has_item) && has_item && tuple == 2);
    lddmc_iterator_destroy(it);
    sylvan_iterator_destroy(NULL);
    mtbdd_iterator_destroy(NULL);
    zdd_iterator_destroy(NULL);
    lddmc_iterator_destroy(NULL);
    lddmc_refs_pop(4);
    zdd_refs_pop(3);
    mtbdd_refs_pop(6);
    return 0;
}

/* Keep multiple independent iterators alive with no caller-owned roots. High
 * arena indices survive shrinking and must remain valid when creating another
 * iterator after the shrink. Automatic collections also run while they live. */
TASK_0(int, test_gc)
{
    for (int policy=0; policy<2; policy++) {
        sylvan_gc_set_policy(policy ? SYLVAN_GC_LOW_MEMORY : SYLVAN_GC_FEW_COLLECTIONS);
        CALL(sylvan_gc);
        const size_t count = 12000;
        MTBDD *garbage = malloc(count*sizeof(*garbage));
        test_assert(garbage != NULL);
        unsigned before = collections;
        for (size_t i=0; i<count; i++) garbage[i] = mtbdd_ref(mtbdd_int64((int64_t)i+100000));
        test_assert(collections > before);
        /* Fresh variable IDs ensure these nodes are allocated after the filler. */
        uint32_t variable = 20000 + (uint32_t)policy;
        BDD vars = mtbdd_refs_push(sylvan_ithvar(variable));
        MTBDD n = mtbdd_refs_push(mtbdd_int64(-98765));
        MTBDD dd = mtbdd_refs_push(mtbdd_makenode(variable, n, mtbdd_true));
        ZDD dom = zdd_refs_push(zdd_makenode(variable, zdd_true, zdd_true));
        uint32_t tuple = variable;
        MDD ldd = lddmc_refs_push(lddmc_cube(&tuple, 1));
        sylvan_iterator *a, *b, *m, *z, *l;
        mtbdd_iterator_options options = {SYLVAN_ITERATOR_MINTERMS, NULL, NULL};
        test_assert(sylvan_iterator_create(vars, vars, SYLVAN_ITERATOR_MINTERMS, &a));
        test_assert(sylvan_iterator_create(vars, vars, SYLVAN_ITERATOR_CUBES, &b));
        test_assert(mtbdd_iterator_create(dd, vars, &options, &m));
        test_assert(zdd_iterator_create(dom, dom, &z));
        test_assert(lddmc_iterator_create(ldd, &l));
        mtbdd_refs_pop(3); zdd_refs_pop(1); lddmc_refs_pop(1);
        for (size_t i=0; i<count; i++) mtbdd_deref(garbage[i]);
        size_t grown = llmsset_get_size(nodes);
        CALL(sylvan_gc);
        test_assert(llmsset_get_size(nodes) < grown);
        test_assert(MTBDD_GETINDEX(vars) >= llmsset_get_size(nodes));
        test_assert(MTBDD_GETINDEX(dd) >= llmsset_get_size(nodes));
        test_assert(ZDD_GETINDEX(dom) >= llmsset_get_size(nodes));
        test_assert(ldd >= llmsset_get_size(nodes));
        test_assert(MTBDD_GETINDEX(vars) < llmsset_get_data_size(nodes));
        sylvan_iterator *after;
        test_assert(mtbdd_iterator_create(dd, vars, &options, &after));
        mtbdd_iterator_destroy(after);
        test_assert(zdd_iterator_create(dom, dom, &after));
        zdd_iterator_destroy(after);
        test_assert(lddmc_iterator_create(ldd, &after));
        lddmc_iterator_destroy(after);
        before = collections;
        for (size_t i=0; i<count; i++) garbage[i] = mtbdd_ref(mtbdd_int64((int64_t)i+200000));
        test_assert(collections > before);
        for (size_t i=0; i<count; i++) mtbdd_deref(garbage[i]);
        free(garbage);
        uint8_t value;
        uint32_t actual;
        int has_item;
        MTBDD leaf;
        uint8_t value_a, value_b;
        int item_a, item_b;
        SPAWN(sylvan_iterator_next, a, &value_a, 1, &item_a);
        SPAWN(sylvan_iterator_next, b, &value_b, 1, &item_b);
        CALL(sylvan_gc);
        int ok_b = SYNC(sylvan_iterator_next);
        int ok_a = SYNC(sylvan_iterator_next);
        test_assert(ok_a && item_a && value_a == 1);
        test_assert(ok_b && item_b && value_b == 1);
        sylvan_iterator_destroy(a);
        CALL(sylvan_gc);
        test_assert(mtbdd_iterator_next(m, &value, 1, &leaf, &has_item) && has_item && value == 0 && leaf == n);
        test_assert(zdd_iterator_next(z, &value, 1, &has_item) && has_item && value == 0);
        test_assert(lddmc_iterator_next(l, &actual, 1, &has_item) && has_item && actual == tuple);
        CALL(sylvan_gc);
        test_assert(sylvan_iterator_next(b, &value, 1, &has_item) && !has_item);
        test_assert(mtbdd_iterator_next(m, &value, 1, &leaf, &has_item) && has_item && value == 1 && leaf == mtbdd_true);
        test_assert(zdd_iterator_next(z, &value, 1, &has_item) && has_item && value == 1);
        test_assert(lddmc_iterator_next(l, &actual, 1, &has_item) && !has_item);
        sylvan_iterator_destroy(b);
        mtbdd_iterator_destroy(m);
        zdd_iterator_destroy(z);
        lddmc_iterator_destroy(l);
    }
    return 0;
}

TASK_0(int, runtests)
{
    size_t bdd_roots = mtbdd_count_protected(), zdd_roots = zdd_count_protected();
    size_t ldd_roots = lddmc_count_protected();
    test_assert(CALL(test_tables) == 0);
    test_assert(CALL(test_tuples) == 0);
    test_assert(CALL(test_edges) == 0);
    test_assert(CALL(test_gc) == 0);
    test_assert(mtbdd_count_protected() == bdd_roots);
    test_assert(zdd_count_protected() == zdd_roots);
    test_assert(lddmc_count_protected() == ldd_roots);
#if SYLVAN_STATS
    sylvan_stats_t stats;
    sylvan_stats_snapshot(&stats);
    test_assert(stats.counters[SYLVAN_ITERATOR_CREATED] > 0);
    test_assert(stats.counters[SYLVAN_ITERATOR_ITEMS] > 0);
#endif
    return 0;
}

int main(int argc, char **argv)
{
    lace_start(argc > 1 ? (unsigned)atoi(argv[1]) : 1, 0);
    sylvan_set_sizes(1<<10, 1<<16, 1<<10, 1<<14);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_zdd();
    sylvan_init_ldd();
    sylvan_gc_hook_pregc(count_collection_CALL);
    int result = RUN(runtests);
    sylvan_quit();
    lace_stop();
    return result;
}
