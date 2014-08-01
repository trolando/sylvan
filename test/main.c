#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>
#include <inttypes.h>

#include <assert.h>

#include "llmsset.h"
#include "sylvan.h"

#if USE_NUMA
#include "numa_tools.h"
#endif

#define BLACK "\33[22;30m"
#define GRAY "\33[01;30m"
#define RED "\33[22;31m"
#define LRED "\33[01;31m"
#define GREEN "\33[22;32m"
#define LGREEN "\33[01;32m"
#define BLUE "\33[22;34m"
#define LBLUE "\33[01;34m"
#define BROWN "\33[22;33m"
#define YELLOW "\33[01;33m"
#define CYAN "\33[22;36m"
#define LCYAN "\33[22;36m"
#define MAGENTA "\33[22;35m"
#define LMAGENTA "\33[01;35m"
#define NC "\33[0m"
#define BOLD "\33[1m"
#define ULINE "\33[4m" //underline
#define BLINK "\33[5m"
#define INVERT "\33[7m"

__thread uint64_t seed = 1;

uint64_t
xorshift_rand(void)
{
    uint64_t x = seed;
    if (seed == 0) seed = rand();
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    seed = x;
    return x * 2685821657736338717LL;
}

double
uniform_deviate(uint64_t seed)
{
    return seed * (1.0 / (0xffffffffffffffffL + 1.0));
}

int
rng(int low, int high)
{
    return low + uniform_deviate(xorshift_rand()) * (high-low);
}

static inline BDD
make_random(int i, int j)
{
    if (i == j) return rng(0, 2) ? sylvan_true : sylvan_false;

    BDD yes = make_random(i+1, j);
    BDD no = make_random(i+1, j);
    BDD result = sylvan_invalid;

    switch(rng(0, 4)) {
    case 0:
        result = no;
        sylvan_deref(yes);
        break;
    case 1:
        result = yes;
        sylvan_deref(no);
        break;
    case 2:
        result = sylvan_ref(sylvan_makenode(i, yes, no));
        sylvan_deref(no);
        sylvan_deref(yes);
        break;
    case 3:
    default:
        result = sylvan_ref(sylvan_makenode(i, no, yes));
        sylvan_deref(no);
        sylvan_deref(yes);
        break;
    }

    return result;
}


void testFun(BDD p1, BDD p2, BDD r1, BDD r2)
{
    if (r1 == r2) return;

    printf("Parameter 1:\n");
    fflush(stdout);
    sylvan_printdot(p1);
    sylvan_print(p1);printf("\n");

    printf("Parameter 2:\n");
    fflush(stdout);
    sylvan_printdot(p2);
    sylvan_print(p2);printf("\n");

    printf("Result 1:\n");
    fflush(stdout);
    sylvan_printdot(r1);

    printf("Result 2:\n");
    fflush(stdout);
    sylvan_printdot(r2);

    assert(0);
}

int testEqual(BDD a, BDD b)
{
	if (a == b) return 1;

	if (a == sylvan_invalid) {
		printf("a is invalid!\n");
		return 0;
	}

	if (b == sylvan_invalid) {
		printf("b is invalid!\n");
		return 0;
	}

    printf("Not Equal!\n");
    fflush(stdout);

    sylvan_print(a);printf("\n");
    sylvan_print(b);printf("\n");

	return 0;
}

void
test_bdd()
{
    sylvan_gc_disable();

    assert(sylvan_makenode(sylvan_ithvar(1), sylvan_true, sylvan_true) == sylvan_not(sylvan_makenode(sylvan_ithvar(1), sylvan_false, sylvan_false)));
    assert(sylvan_makenode(sylvan_ithvar(1), sylvan_false, sylvan_true) == sylvan_not(sylvan_makenode(sylvan_ithvar(1), sylvan_true, sylvan_false)));
    assert(sylvan_makenode(sylvan_ithvar(1), sylvan_true, sylvan_false) == sylvan_not(sylvan_makenode(sylvan_ithvar(1), sylvan_false, sylvan_true)));
    assert(sylvan_makenode(sylvan_ithvar(1), sylvan_false, sylvan_false) == sylvan_not(sylvan_makenode(sylvan_ithvar(1), sylvan_true, sylvan_true)));

    sylvan_gc_enable();
}

void
test_cube()
{
    BDDVAR vars[] = {2,4,8,6,1,3};

    char cube[6], check[6];
    int i;
    for (i=0;i<6;i++) cube[i] = rng(0,3);
    BDD bdd = sylvan_cube(vars, 6, cube);

    sylvan_sat_one(bdd, vars, 6, check);
    for (i=0; i<6;i++) assert(cube[i] == check[i]);

    testEqual(sylvan_cube(vars, 6, check), sylvan_sat_one_bdd(bdd));
    assert(sylvan_cube(vars, 6, check) == sylvan_sat_one_bdd(bdd));

    BDD picked = sylvan_pick_cube(bdd);
    assert(testEqual(sylvan_and(picked, bdd), picked));

    bdd = make_random(1, 16);
    for (i=0;i<36;i++) {
        picked = sylvan_pick_cube(bdd);
        assert(testEqual(sylvan_and(picked, bdd), picked));
    }
}

static void
test_operators()
{
    // We need to test: xor, and, or, nand, nor, imp, biimp, invimp, diff, less
    sylvan_gc_disable();

    //int i;
    BDD a = sylvan_ithvar(1);
    BDD b = sylvan_ithvar(2);
    BDD one = make_random(3, 16);
    BDD two = make_random(8, 24);

    // Test or
    assert(testEqual(sylvan_or(a, b), sylvan_makenode(1, b, sylvan_true)));
    assert(testEqual(sylvan_or(a, b), sylvan_or(b, a)));
    assert(testEqual(sylvan_or(one, two), sylvan_or(two, one)));

    // Test and
    assert(testEqual(sylvan_and(a, b), sylvan_makenode(1, sylvan_false, b)));
    assert(testEqual(sylvan_and(a, b), sylvan_and(b, a)));
    assert(testEqual(sylvan_and(one, two), sylvan_and(two, one)));

    // Test xor
    assert(testEqual(sylvan_xor(a, b), sylvan_makenode(1, b, sylvan_not(b))));
    assert(testEqual(sylvan_xor(a, b), sylvan_xor(a, b)));
    assert(testEqual(sylvan_xor(a, b), sylvan_xor(b, a)));
    assert(testEqual(sylvan_xor(one, two), sylvan_xor(two, one)));
    assert(testEqual(sylvan_xor(a, b), sylvan_ite(a, sylvan_not(b), b)));

    // Test diff
    assert(testEqual(sylvan_diff(a, b), sylvan_diff(a, b)));
    assert(testEqual(sylvan_diff(a, b), sylvan_diff(a, sylvan_and(a, b))));
    assert(testEqual(sylvan_diff(a, b), sylvan_and(a, sylvan_not(b))));
    assert(testEqual(sylvan_diff(a, b), sylvan_ite(b, sylvan_false, a)));
    assert(testEqual(sylvan_diff(one, two), sylvan_diff(one, two)));
    assert(testEqual(sylvan_diff(one, two), sylvan_diff(one, sylvan_and(one, two))));
    assert(testEqual(sylvan_diff(one, two), sylvan_and(one, sylvan_not(two))));
    assert(testEqual(sylvan_diff(one, two), sylvan_ite(two, sylvan_false, one)));

    // Test biimp
    assert(testEqual(sylvan_biimp(a, b), sylvan_makenode(1, sylvan_not(b), b)));
    assert(testEqual(sylvan_biimp(a, b), sylvan_biimp(b, a)));
    assert(testEqual(sylvan_biimp(one, two), sylvan_biimp(two, one)));

    // Test nand / and
    assert(testEqual(sylvan_not(sylvan_and(a, b)), sylvan_nand(b, a)));
    assert(testEqual(sylvan_not(sylvan_and(one, two)), sylvan_nand(two, one)));

    // Test nor / or
    assert(testEqual(sylvan_not(sylvan_or(a, b)), sylvan_nor(b, a)));
    assert(testEqual(sylvan_not(sylvan_or(one, two)), sylvan_nor(two, one)));

    // Test xor / biimp
    assert(testEqual(sylvan_xor(a, b), sylvan_not(sylvan_biimp(b, a))));
    assert(testEqual(sylvan_xor(one, two), sylvan_not(sylvan_biimp(two, one))));

    // Test imp
    assert(testEqual(sylvan_imp(a, b), sylvan_ite(a, b, sylvan_true)));
    assert(testEqual(sylvan_imp(one, two), sylvan_ite(one, two, sylvan_true)));
    assert(testEqual(sylvan_imp(one, two), sylvan_not(sylvan_diff(one, two))));
    assert(testEqual(sylvan_invimp(one, two), sylvan_not(sylvan_less(one, two))));
    assert(testEqual(sylvan_imp(a, b), sylvan_invimp(b, a)));
    assert(testEqual(sylvan_imp(one, two), sylvan_invimp(two, one)));

    // Test constrain, exists and forall

    sylvan_gc_enable();
}

static void
test_relprod()
{
    sylvan_gc_disable();

    BDDVAR vars[] = {0,2,4};
    BDDVAR all_vars[] = {0,1,2,3,4,5};

    BDDSET all_vars_set = sylvan_set_fromarray(all_vars, 6);

    BDD s, t, next, prev;
    BDD zeroes, ones;

    // transition relation: 000 --> 111 and !000 --> 000
    t = sylvan_false;
    t = sylvan_or(t, sylvan_cube(all_vars, 6, (char[]){0,1,0,1,0,1}));
    t = sylvan_or(t, sylvan_cube(all_vars, 6, (char[]){1,0,2,0,2,0}));
    t = sylvan_or(t, sylvan_cube(all_vars, 6, (char[]){2,0,1,0,2,0}));
    t = sylvan_or(t, sylvan_cube(all_vars, 6, (char[]){2,0,2,0,1,0}));

    s = sylvan_cube(vars, 3, (char[]){0,0,1});
    zeroes = sylvan_cube(vars, 3, (char[]){0,0,0});
    ones = sylvan_cube(vars, 3, (char[]){1,1,1});

    next = sylvan_relprod_paired(s, t, all_vars_set);
    prev = sylvan_relprod_paired_prev(next, t, all_vars_set);
    assert(next == zeroes);
    assert(prev == sylvan_not(zeroes));

    next = sylvan_relprod_paired(next, t, all_vars_set);
    prev = sylvan_relprod_paired_prev(next, t, all_vars_set);
    assert(next == ones);
    assert(prev == zeroes);

    t = sylvan_cube(all_vars, 6, (char[]){0,0,0,0,0,1});
    assert(sylvan_relprod_paired_prev(s, t, all_vars_set) == zeroes);
    assert(sylvan_relprod_paired_prev(sylvan_not(s), t, all_vars_set) == sylvan_false);
    assert(sylvan_relprod_paired(s, t, all_vars_set) == sylvan_false);
    assert(sylvan_relprod_paired(zeroes, t, all_vars_set) == s);

    t = sylvan_cube(all_vars, 6, (char[]){0,0,0,0,0,2});
    assert(sylvan_relprod_paired_prev(s, t, all_vars_set) == zeroes);
    assert(sylvan_relprod_paired_prev(zeroes, t, all_vars_set) == zeroes);
    assert(sylvan_relprod_paired(sylvan_not(zeroes), t, all_vars_set) == sylvan_false);

    sylvan_gc_enable();
}

static void
test_compose()
{
    sylvan_gc_disable();

    BDD a = sylvan_ithvar(1);
    BDD b = sylvan_ithvar(2);

    BDD a_or_b = sylvan_or(a, b);

    BDD one = make_random(3, 16);
    BDD two = make_random(8, 24);

    BDDMAP map = sylvan_map_empty();

    map = sylvan_map_add(map, 1, one);
    map = sylvan_map_add(map, 2, two);

    assert(sylvan_map_key(map) == 1);
    assert(sylvan_map_value(map) == one);
    assert(sylvan_map_key(sylvan_map_next(map)) == 2);
    assert(sylvan_map_value(sylvan_map_next(map)) == two);

    assert(testEqual(one, sylvan_compose(a, map)));
    assert(testEqual(two, sylvan_compose(b, map)));

    assert(testEqual(sylvan_or(one, two), sylvan_compose(a_or_b, map)));

    map = sylvan_map_add(map, 2, one);
    assert(testEqual(sylvan_compose(a_or_b, map), one));

    map = sylvan_map_add(map, 1, two);
    assert(testEqual(sylvan_or(one, two), sylvan_compose(a_or_b, map)));

    assert(testEqual(sylvan_and(one, two), sylvan_compose(sylvan_and(a, b), map)));

    sylvan_gc_enable();
}

/** GC testing */
VOID_TASK_2(gctest_fill, int, levels, int, width)
{
    if (levels > 1) {
        int i;
        for (i=0; i<width; i++) { SPAWN(gctest_fill, levels-1, width); }
        for (i=0; i<width; i++) { SYNC(gctest_fill); }
    } else {
        sylvan_deref(make_random(0, 10));
    }
}

void report_table()
{
    llmsset_t __sylvan_get_internal_data();
    llmsset_t tbl = __sylvan_get_internal_data();
    size_t filled = llmsset_get_filled(tbl);
    size_t total = llmsset_get_size(tbl);
    printf("done, table: %0.1f%% full (%zu nodes).\n", 100.0*(double)filled/total, filled);
}

void test_gc(int threads)
{
    int N_canaries = 16;
    BDD canaries[N_canaries];
    char* hashes[N_canaries];
    char* hashes2[N_canaries];
    int i,j;
    for (i=0;i<N_canaries;i++) {
        canaries[i] = make_random(0, 10);
        hashes[i] = (char*)malloc(80);
        hashes2[i] = (char*)malloc(80);
        sylvan_getsha(canaries[i], hashes[i]);
        sylvan_test_isbdd(canaries[i]);
    }
    assert(sylvan_count_refs() == (size_t)N_canaries);
    for (j=0;j<10*threads;j++) {
        CALL(gctest_fill, 6, 5);
        for (i=0;i<N_canaries;i++) {
            sylvan_test_isbdd(canaries[i]);
            sylvan_getsha(canaries[i], hashes2[i]);
            assert(strcmp(hashes[i], hashes2[i]) == 0);
        }
    }
    assert(sylvan_count_refs() == (size_t)N_canaries);
}

void runtests(int threads)
{
#if USE_NUMA
    numa_distribute(threads);
#endif

    lace_init(threads, 100000);
    lace_startup(0, NULL, NULL);

    printf(BOLD "Testing Sylvan\n");

    printf(NC "Testing basic bdd functionality... ");
    fflush(stdout);
    sylvan_init(16, 16, 1);
    test_bdd();
    sylvan_quit();
    printf(LGREEN "success" NC "!\n");

    // what happens if we make a cube
    printf(NC "Testing cube function... ");
    fflush(stdout);
    int j;
    for (j=0;j<20;j++) {
        sylvan_init(16, 16, 1);
        test_cube();
        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Testing relational products... ");
    fflush(stdout);
    for (j=0;j<20;j++) {
        sylvan_init(16, 16, 1);
        test_relprod();
        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Testing function composition... ");
    fflush(stdout);
    for (j=0;j<20;j++) {
        sylvan_init(16, 16, 1);
        test_compose();
        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Testing garbage collection... ");
    fflush(stdout);
    sylvan_init(14, 10, 1);
    test_gc(threads);
    sylvan_quit();
    printf(LGREEN "success" NC "!\n");

    printf(NC "Testing operators... ");
    fflush(stdout);
    for (j=0;j<50;j++) {
        sylvan_init(16, 16, 1);
        test_operators();
        test_operators();
        test_operators();
        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    lace_exit();
}

int main(int argc, char **argv)
{
    int threads = 2;
    if (argc > 1) sscanf(argv[1], "%d", &threads);

    runtests(threads);
    printf(NC);
    exit(0);
}
