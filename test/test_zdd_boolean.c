#include <sylvan_int.h>
#include "test_assert.h"

_Static_assert(_Generic(zdd_disjoint(zdd_false, zdd_false), int: 1, default: 0),
    "zdd_disjoint must return int");
_Static_assert(_Generic(zdd_subset(zdd_false, zdd_false), int: 1, default: 0),
    "zdd_subset must return int");

/* Internal constructor, also used by test_table_full. */
ZDD zdd_makemapnode(uint32_t var, ZDD low, ZDD high);

/* Truth-table bit s denotes membership of the set encoded by s. */
static uint32_t
universe(unsigned n)
{
    return n == 5 ? UINT32_MAX : (UINT32_C(1) << (1u << n)) - 1;
}

static ZDD
family(uint32_t table, const uint32_t *vars, unsigned n)
{
    if (n == 0) return table ? zdd_true : zdd_false;
    uint32_t lo = 0, hi = 0;
    for (unsigned i=0; i<(1u << (n-1)); i++) {
        lo |= ((table >> (2*i)) & 1u) << i;
        hi |= ((table >> (2*i+1)) & 1u) << i;
    }
    ZDD low = zdd_refs_push(family(lo, vars+1, n-1));
    ZDD high = family(hi, vars+1, n-1);
    ZDD result = zdd_makenode(*vars, low, high);
    zdd_refs_pop(1);
    return result;
}

static BDD
function(uint32_t table, const uint32_t *vars, unsigned n)
{
    if (n == 0) return table ? sylvan_true : sylvan_false;
    uint32_t lo = 0, hi = 0;
    for (unsigned i=0; i<(1u << (n-1)); i++) {
        lo |= ((table >> (2*i)) & 1u) << i;
        hi |= ((table >> (2*i+1)) & 1u) << i;
    }
    BDD low = mtbdd_refs_push(function(lo, vars+1, n-1));
    BDD high = function(hi, vars+1, n-1);
    BDD result = mtbdd_makenode(*vars, low, high);
    mtbdd_refs_pop(1);
    return result;
}

/* Use the existing enumerator, independently of the new evaluation function. */
static int
check_family(ZDD dd, ZDD dom, unsigned n, uint32_t expected)
{
    test_assert(dd != zdd_invalid);
    uint8_t values[5];
    uint32_t actual = 0;
    for (ZDD leaf=zdd_enum_first(dd, dom, values, NULL); leaf != zdd_false;
         leaf=zdd_enum_next(dd, dom, values, NULL)) {
        test_assert(leaf == zdd_true);
        unsigned assignment = 0;
        for (unsigned i=0; i<n; i++) {
            test_assert(values[i] <= 1);
            assignment |= (unsigned)values[i] << i;
        }
        test_assert(!(actual & (UINT32_C(1) << assignment)));
        actual |= UINT32_C(1) << assignment;
    }
    test_assert(actual == expected);
    return 0;
}

static uint32_t
quantified(uint32_t table, unsigned n, unsigned vars, int unique)
{
    uint32_t result = 0;
    for (unsigned s=0; s<(1u << n); s++) {
        unsigned count = 0, total = 0;
        for (unsigned t=0; t<(1u << n); t++) {
            if ((s & ~vars) != (t & ~vars)) continue;
            count += (table >> t) & 1u;
            total++;
        }
        if (unique ? (count & 1u) : count == total) result |= UINT32_C(1) << s;
    }
    return result;
}

static int
check_binary(ZDD a, ZDD b, ZDD dom, unsigned n, uint32_t at, uint32_t bt)
{
    uint32_t full = universe(n);
    test_assert(check_family(zdd_xor(a, b), dom, n, at ^ bt) == 0);
    test_assert(check_family(zdd_equiv(a, b, dom), dom, n, full ^ (at ^ bt)) == 0);
    test_assert(check_family(zdd_nand(a, b, dom), dom, n, full ^ (at & bt)) == 0);
    test_assert(check_family(zdd_nor(a, b, dom), dom, n, full ^ (at | bt)) == 0);
    test_assert(check_family(zdd_imp(a, b, dom), dom, n, full ^ (at & ~bt)) == 0);
    test_assert(zdd_disjoint(a, b) == ((at & bt) == 0));
    test_assert(zdd_subset(a, b) == ((at & ~bt) == 0));
    return 0;
}

static int
test_boolean_families(void)
{
    uint32_t vars[] = {1,4,7};
    for (unsigned n=0; n<=3; n++) {
        ZDD dom = zdd_refs_push(zdd_set_from_array(vars, n));
        unsigned family_count = 1u << (1u << n);
        ZDD families[256];
        for (unsigned a=0; a<family_count; a++) families[a] = zdd_refs_push(family(a, vars, n));
        for (unsigned a=0; a<family_count; a++) {
            /* Includes empty/full families, {empty set}, and both operand orders. */
            for (unsigned b=0; b<family_count; b++) {
                test_assert(check_binary(families[a], families[b], dom, n, a, b) == 0);
            }
            for (unsigned q=0; q<(1u << n); q++) {
                uint32_t qvars[3];
                size_t count = 0;
                for (unsigned i=0; i<n; i++) if (q & (1u << i)) qvars[count++] = vars[i];
                ZDD quant = zdd_refs_push(zdd_set_from_array(qvars, count));
                test_assert(check_family(zdd_forall(families[a], quant), dom, n, quantified(a, n, q, 0)) == 0);
                test_assert(check_family(zdd_unique(families[a], quant), dom, n, quantified(a, n, q, 1)) == 0);
                zdd_refs_pop(1);
            }
            uint8_t values[3];
            for (unsigned s=0; s<(1u << n); s++) {
                for (unsigned i=0; i<n; i++) values[i] = (uint8_t)((s >> i) & 1u);
                test_assert(zdd_eval_assignment(families[a], dom, values, n) ==
                    (((a >> s) & 1u) ? zdd_true : zdd_false));
            }
        }
        zdd_refs_pop(family_count+1);
    }
    return 0;
}

static int
test_domains_and_errors(void)
{
    uint32_t vars[] = {1,4,7};
    ZDD dom = zdd_refs_push(zdd_set_from_array(vars, 3));
    ZDD a = zdd_refs_push(family(0x96, vars, 3));
    ZDD b = zdd_refs_push(family(0xe8, vars, 3));
    ZDD empty = zdd_true;
    /* Change the domain without changing either operand, with warm caches. */
    for (unsigned repeat=0; repeat<2; repeat++) {
        test_assert(check_binary(a, b, dom, 3, 0x96, 0xe8) == 0);
        uint32_t wide_vars[] = {1,4,7,9};
        ZDD wide = zdd_refs_push(zdd_set_from_array(wide_vars, 4));
        test_assert(check_binary(a, b, wide, 4, 0x96, 0xe8) == 0);
        ZDD missing = zdd_refs_push(zdd_set_from_array(vars, 2));
        test_assert(zdd_equiv(a, a, missing) == zdd_invalid);
        test_assert(zdd_nand(a, zdd_false, missing) == zdd_invalid);
        test_assert(zdd_nor(zdd_false, a, missing) == zdd_invalid);
        test_assert(zdd_imp(a, a, missing) == zdd_invalid);
        test_assert(zdd_eval_assignment(a, missing, ((uint8_t[]){0,0}), 2) == zdd_invalid);
        zdd_refs_pop(2);
    }
    /* An unvisited branch still makes the domain incomplete. */
    ZDD hidden = zdd_refs_push(family(0x21, vars, 3));
    test_assert(zdd_eval_assignment(hidden, zdd_true, NULL, 0) == zdd_invalid);
    ZDD single = zdd_refs_push(zdd_set_from_array(vars, 1));
    test_assert(zdd_eval_assignment(hidden, single, ((uint8_t[]){0}), 1) == zdd_invalid);
    zdd_refs_pop(2);
    /* Quantify absent variables before, between, and after the support. */
    uint32_t extra[] = {0,3,9};
    for (unsigned i=0; i<3; i++) {
        uint32_t wide_vars[4];
        unsigned j=0, k=0;
        while (j<3 && vars[j]<extra[i]) { wide_vars[k++] = vars[j++]; }
        unsigned extra_bit = k;
        wide_vars[k++] = extra[i];
        while (j<3) wide_vars[k++] = vars[j++];
        uint32_t expanded = 0;
        for (unsigned s=0; s<8; s++) {
            unsigned t = (s & ((1u << extra_bit)-1)) | ((s >> extra_bit) << (extra_bit+1));
            if ((0x96u >> s) & 1u) expanded |= UINT32_C(1) << t;
        }
        ZDD wide = zdd_refs_push(zdd_set_from_array(wide_vars, 4));
        ZDD quant = zdd_refs_push(zdd_set_from_array(&extra[i], 1));
        test_assert(check_family(zdd_forall(a, quant), wide, 4, 0) == 0);
        test_assert(check_family(zdd_unique(a, quant), wide, 4,
            quantified(expanded, 4, 1u << extra_bit, 1)) == 0);
        zdd_refs_pop(2);
    }

    ZDD custom = mtbdd_int64(7);
    mtbdd_refs_push(custom);
    ZDD bad_leaf = zdd_refs_push(zdd_makenode(4, zdd_true, custom));
    ZDD bad[] = {zdd_invalid, custom, bad_leaf};
    for (unsigned i=0; i<3; i++) {
        test_assert(zdd_xor(bad[i], bad[i]) == zdd_invalid);
        test_assert(zdd_equiv(bad[i], bad[i], dom) == zdd_invalid);
        test_assert(zdd_nand(bad[i], zdd_false, dom) == zdd_invalid);
        test_assert(zdd_nor(zdd_false, bad[i], dom) == zdd_invalid);
        test_assert(zdd_imp(bad[i], bad[i], dom) == zdd_invalid);
        test_assert(zdd_disjoint(zdd_false, bad[i]) == -1);
        test_assert(zdd_disjoint(bad[i], zdd_false) == -1);
        test_assert(zdd_subset(zdd_false, bad[i]) == -1);
        test_assert(zdd_subset(bad[i], zdd_false) == -1);
        test_assert(zdd_subset(bad[i], bad[i]) == -1);
        test_assert(zdd_forall(bad[i], empty) == zdd_invalid);
        test_assert(zdd_unique(bad[i], empty) == zdd_invalid);
        test_assert(zdd_eval_assignment(bad[i], dom, ((uint8_t[]){0,0,0}), 3) == zdd_invalid);
    }
    ZDD bad_domains[] = {zdd_invalid, zdd_false, a, custom, zdd_ithvar(4)};
    for (unsigned i=0; i<5; i++) {
        test_assert(zdd_equiv(zdd_true, zdd_true, bad_domains[i]) == zdd_invalid);
        test_assert(zdd_nand(zdd_false, zdd_false, bad_domains[i]) == zdd_invalid);
        test_assert(zdd_nor(zdd_false, zdd_false, bad_domains[i]) == zdd_invalid);
        test_assert(zdd_imp(zdd_true, zdd_true, bad_domains[i]) == zdd_invalid);
        test_assert(zdd_forall(zdd_false, bad_domains[i]) == zdd_invalid);
        test_assert(zdd_unique(zdd_false, bad_domains[i]) == zdd_invalid);
        test_assert(zdd_eval_assignment(zdd_false, bad_domains[i], NULL, 0) == zdd_invalid);
        test_assert(zdd_minimal_sets(sylvan_true, bad_domains[i]) == zdd_invalid);
    }
    test_assert(zdd_eval_assignment(a, dom, NULL, 3) == zdd_invalid);
    test_assert(zdd_eval_assignment(a, dom, ((uint8_t[]){0,0}), 2) == zdd_invalid);
    test_assert(zdd_eval_assignment(zdd_false, dom, ((uint8_t[]){0,2,0}), 3) == zdd_invalid);
    test_assert(zdd_eval_assignment(zdd_true, empty, NULL, 0) == zdd_true);
    test_assert(zdd_eval_assignment(zdd_false, empty, NULL, 0) == zdd_false);
    test_assert(zdd_eval_assignment(zdd_true, dom, ((uint8_t[]){0,0,1}), 3) == zdd_false);
    test_assert(zdd_eval_assignment(zdd_true, dom, ((uint8_t[]){0,0,0}), 3) == zdd_true);
    /* Old single-variable evaluation remains available. */
    test_assert(zdd_eval(a, 1, 0) == zdd_getlow(a));
    test_assert(zdd_eval(a, 1, 1) == zdd_gethigh(a));
    zdd_refs_pop(4);
    mtbdd_refs_pop(1);
    return 0;
}

static int
test_map_errors(void)
{
    uint32_t vars[] = {0,1};
    ZDD dom = zdd_refs_push(zdd_set_from_array(vars, 2));
    for (unsigned kind=0; kind<2; kind++) {
        ZDD map = zdd_refs_push(kind ? zdd_makemapnode(1, zdd_false, zdd_true) :
            mtbdd_map_add(mtbdd_map_empty(), 1, sylvan_true));
        ZDD low_map = zdd_refs_push(zdd_makenode(0, map, zdd_true));
        ZDD high_map = zdd_refs_push(zdd_makenode(0, zdd_true, map));
        ZDD bad[] = {map, low_map, high_map};
        for (size_t i=0; i<sizeof(bad)/sizeof(bad[0]); i++) {
            /* Both map flags are invalid at the root and in either branch. */
            test_assert(zdd_xor(bad[i], zdd_false) == zdd_invalid);
            test_assert(zdd_xor(zdd_false, bad[i]) == zdd_invalid);
            test_assert(zdd_equiv(bad[i], zdd_true, dom) == zdd_invalid);
            test_assert(zdd_equiv(zdd_true, bad[i], dom) == zdd_invalid);
            test_assert(zdd_nand(bad[i], zdd_false, dom) == zdd_invalid);
            test_assert(zdd_nand(zdd_false, bad[i], dom) == zdd_invalid);
            test_assert(zdd_nor(bad[i], zdd_false, dom) == zdd_invalid);
            test_assert(zdd_nor(zdd_false, bad[i], dom) == zdd_invalid);
            test_assert(zdd_imp(bad[i], zdd_true, dom) == zdd_invalid);
            test_assert(zdd_imp(zdd_false, bad[i], dom) == zdd_invalid);
            test_assert(zdd_disjoint(bad[i], zdd_false) == -1);
            test_assert(zdd_disjoint(zdd_false, bad[i]) == -1);
            test_assert(zdd_subset(bad[i], dom) == -1);
            test_assert(zdd_subset(zdd_false, bad[i]) == -1);
            test_assert(zdd_forall(bad[i], zdd_true) == zdd_invalid);
            test_assert(zdd_unique(bad[i], zdd_true) == zdd_invalid);
            test_assert(zdd_eval_assignment(bad[i], dom, ((uint8_t[]){0,0}), 2) == zdd_invalid);
            test_assert(zdd_eval_assignment(bad[i], dom, ((uint8_t[]){1,0}), 2) == zdd_invalid);
            test_assert(zdd_minimal_sets(bad[i], dom) == zdd_invalid);

            /* Domain validation uses the same node validator. */
            test_assert(zdd_equiv(zdd_true, zdd_true, bad[i]) == zdd_invalid);
            test_assert(zdd_nand(zdd_false, zdd_false, bad[i]) == zdd_invalid);
            test_assert(zdd_nor(zdd_false, zdd_false, bad[i]) == zdd_invalid);
            test_assert(zdd_imp(zdd_true, zdd_true, bad[i]) == zdd_invalid);
            test_assert(zdd_forall(zdd_false, bad[i]) == zdd_invalid);
            test_assert(zdd_unique(zdd_false, bad[i]) == zdd_invalid);
            test_assert(zdd_eval_assignment(zdd_false, bad[i], NULL, 0) == zdd_invalid);
            test_assert(zdd_minimal_sets(sylvan_true, bad[i]) == zdd_invalid);
        }
        zdd_refs_pop(3);
    }
    zdd_refs_pop(1);
    return 0;
}

static int
is_monotone(uint32_t table, unsigned n)
{
    for (unsigned s=0; s<(1u << n); s++) {
        if (!((table >> s) & 1u)) continue;
        for (unsigned t=0; t<(1u << n); t++) {
            if ((s & t) == s && !((table >> t) & 1u)) return 0;
        }
    }
    return 1;
}

static uint32_t
minimal_elements(uint32_t table, unsigned n)
{
    uint32_t result = table;
    for (unsigned s=0; s<(1u << n); s++) {
        for (unsigned t=0; t<(1u << n); t++) {
            if (t != s && (t & s) == t && ((table >> t) & 1u)) {
                result &= ~(UINT32_C(1) << s);
            }
        }
    }
    return result;
}

static uint32_t
random_u32(uint32_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 17;
    *state ^= *state << 5;
    return *state;
}

static int
test_minimal_sets(void)
{
    uint32_t vars[] = {1,4,7,10,13};
    ZDD dom = zdd_refs_push(zdd_set_from_array(vars, 3));
    unsigned monotone_count = 0;
    /* Port of sylvan-2's exhaustive test: exactly 20 functions are monotone. */
    for (uint32_t table=0; table<256; table++) {
        BDD f = mtbdd_refs_push(function(table, vars, 3));
        ZDD result = zdd_refs_push(zdd_minimal_sets(f, dom));
        if (is_monotone(table, 3)) {
            monotone_count++;
            test_assert(check_family(result, dom, 3, minimal_elements(table, 3)) == 0);
            test_assert(zdd_minimal_sets(f, dom) == result);
        } else test_assert(result == zdd_invalid);
        zdd_refs_pop(1);
        mtbdd_refs_pop(1);
    }
    test_assert(monotone_count == 20);
    zdd_refs_pop(1);

    dom = zdd_refs_push(zdd_set_from_array(vars, 5));
    uint32_t state = 123;
    for (unsigned trial=0; trial<300; trial++) {
        /* Upward closure of a random family gives a random monotone function. */
        uint32_t generators = random_u32(&state) & ~UINT32_C(1), table = 0;
        for (unsigned s=0; s<32; s++) {
            for (unsigned t=0; t<32; t++) {
                if ((s & t) == t && ((generators >> t) & 1u)) table |= UINT32_C(1) << s;
            }
        }
        BDD f = mtbdd_refs_push(function(table, vars, 5));
        ZDD result = zdd_refs_push(zdd_minimal_sets(f, dom));
        if (trial % 10 == 0) sylvan_gc();
        test_assert(check_family(result, dom, 5, minimal_elements(table, 5)) == 0);
        test_assert(zdd_minimal_sets(f, dom) == result);
        zdd_refs_pop(1);
        mtbdd_refs_pop(1);
    }
    /* Source example: x1 OR (x4 AND x7), plus unused domain variables. */
    BDD f = mtbdd_refs_push(function(0xea, vars, 3));
    ZDD result = zdd_refs_push(zdd_minimal_sets(f, dom));
    test_assert(check_family(result, dom, 5, (1u << 1) | (1u << 6)) == 0);
    ZDD missing = zdd_refs_push(zdd_set_from_array(vars, 2));
    test_assert(zdd_minimal_sets(f, missing) == zdd_invalid);
    test_assert(zdd_minimal_sets(sylvan_invalid, dom) == zdd_invalid);
    BDD map = mtbdd_refs_push(mtbdd_map_add(mtbdd_map_empty(), 1, sylvan_true));
    test_assert(zdd_minimal_sets(map, dom) == zdd_invalid);
    mtbdd_refs_pop(1);
    test_assert(zdd_minimal_sets(mtbdd_int64(1), dom) == zdd_invalid);
    test_assert(zdd_minimal_sets(sylvan_false, zdd_true) == zdd_false);
    test_assert(zdd_minimal_sets(sylvan_true, zdd_true) == zdd_true);
    zdd_refs_pop(3);
    mtbdd_refs_pop(1);
    return 0;
}

/* Collect concurrently with recursive operations to exercise internal roots. */
TASK_1(int, boolean_work, uint32_t, state)
{
    uint32_t vars[] = {1,4,7,10,13};
    ZDD dom = zdd_refs_push(zdd_set_from_array(vars, 5));
    ZDD quant = zdd_refs_push(zdd_set_from_array(vars, 3));
    for (unsigned trial=0; trial<100; trial++) {
        uint32_t at = random_u32(&state), bt = random_u32(&state);
        ZDD a = zdd_refs_push(family(at, vars, 5));
        ZDD b = zdd_refs_push(family(bt, vars, 5));
        test_assert(check_binary(a, b, dom, 5, at, bt) == 0);
        test_assert(check_family(zdd_forall(a, quant), dom, 5, quantified(at, 5, 7, 0)) == 0);
        test_assert(check_family(zdd_unique(a, quant), dom, 5, quantified(at, 5, 7, 1)) == 0);
        uint32_t table = 0;
        for (unsigned t=0; t<32; t++) {
            for (unsigned u=0; u<32; u++) {
                if ((u & t) == u && ((at >> u) & 1u)) table |= UINT32_C(1) << t;
            }
        }
        BDD f = mtbdd_refs_push(function(table, vars, 5));
        test_assert(check_family(zdd_minimal_sets(f, dom), dom, 5, minimal_elements(table, 5)) == 0);
        mtbdd_refs_pop(1);
        zdd_refs_pop(2);
    }
    zdd_refs_pop(2);
    return 0;
}

TASK_0(int, test_gc_operations)
{
    SPAWN(boolean_work, 731);
    SPAWN(boolean_work, 1729);
    for (unsigned i=0; i<100; i++) CALL(sylvan_gc);
    int second = SYNC(boolean_work);
    int first = SYNC(boolean_work);
    test_assert(first == 0 && second == 0);

    /* Keep inputs whose indices exceed the shrunken hash-table size. */
    sylvan_gc();
    size_t initial_size = llmsset_get_size(nodes);
    const size_t count = 18000;
    MTBDD *garbage = malloc(count * sizeof(*garbage));
    test_assert(garbage != NULL);
    for (size_t i=0; i<count; i++) garbage[i] = mtbdd_ref(mtbdd_int64((int64_t)i + 100000));
    test_assert(llmsset_get_size(nodes) > initial_size);
    uint32_t vars[] = {101,104,107,110,113};
    ZDD dom = zdd_refs_push(zdd_set_from_array(vars, 5));
    ZDD a = zdd_refs_push(family(0x96969696, vars, 5));
    ZDD b = zdd_refs_push(family(0xe8e8e8e8, vars, 5));
    BDD f = mtbdd_refs_push(function(0xeaeaeaea, vars, 5));
    for (size_t i=0; i<count; i++) mtbdd_deref(garbage[i]);
    free(garbage);
    sylvan_gc();
    test_assert(llmsset_get_size(nodes) == initial_size);
    test_assert(ZDD_GETINDEX(a) >= llmsset_get_size(nodes));
    test_assert(ZDD_GETINDEX(a) < llmsset_get_data_size(nodes));
    test_assert(check_binary(a, b, dom, 5, 0x96969696, 0xe8e8e8e8) == 0);
    test_assert(check_family(zdd_forall(a, dom), dom, 5, 0) == 0);
    test_assert(check_family(zdd_unique(a, dom), dom, 5, 0) == 0);
    test_assert(zdd_eval_assignment(a, dom, ((uint8_t[]){1,0,0,0,0}), 5) == zdd_true);
    test_assert(check_family(zdd_minimal_sets(f, dom), dom, 5, (1u << 1) | (1u << 6)) == 0);
    mtbdd_refs_pop(1);
    zdd_refs_pop(3);
    return 0;
}

TASK_0(int, runtests)
{
    if (test_boolean_families()) return 1;
    if (test_domains_and_errors()) return 1;
    if (test_map_errors()) return 1;
    if (test_minimal_sets()) return 1;
    if (CALL(test_gc_operations)) return 1;
#if SYLVAN_STATS
    sylvan_stats_t stats;
    sylvan_stats_snapshot(&stats);
    const Sylvan_Counters counters[] = {
        ZDD_XOR, ZDD_EQUIV, ZDD_NAND, ZDD_NOR, ZDD_IMP, ZDD_DISJOINT,
        ZDD_SUBSET, ZDD_FORALL, ZDD_UNIQUE, ZDD_MINIMAL_SETS,
        ZDD_WITHOUT_SUPERSETS, ZDD_BOOLEAN_CHECK, ZDD_EVAL_ASSIGNMENT
    };
    for (size_t i=0; i<sizeof(counters)/sizeof(counters[0]); i++) {
        test_assert(stats.counters[counters[i]] > 0);
        if (counters[i] != ZDD_EVAL_ASSIGNMENT) {
            test_assert(stats.counters[counters[i]+1] > 0);
            test_assert(stats.counters[counters[i]+2] > 0);
        }
    }
#endif
    return 0;
}

int main(int argc, char **argv)
{
    unsigned workers = argc > 1 ? (unsigned)atoi(argv[1]) : 1;
    lace_start(workers, 0);
    sylvan_set_sizes(1<<12, 1<<20, 1<<10, 1<<18);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_zdd();
    int result = RUN(runtests);
    sylvan_quit();
    lace_stop();
    return result;
}
