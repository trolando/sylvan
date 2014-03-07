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

int test_llmsset()
{
    uint32_t entry[] = { 90570123,  43201432,   31007798,  256346587,
                         543578998, 34534278,   86764826,  572667984,
                         883562435, 2546247838, 190200937, 918456256,
                         245892765, 29926542,   862864346, 624500973 };

    uint64_t index[16];
    uint64_t insert_index = 0;
    int created;
    unsigned int i;

    llmsset_t set = llmsset_create(sizeof(uint32_t), sizeof(uint32_t), 1<<5); // size: 32

    // Add all entries, but do not ref
    for (i=0;i<16;i++) {
        assert(llmsset_lookup(set, entry + i, &insert_index, &created, index + i) != 0);
        assert(created);
    }

    assert(llmsset_get_filled(set)==16);

    // Clear table
    llmsset_clear(set);

    // Check if table empty
    assert(llmsset_get_filled(set)==0);

    // Check if table really empty
    for (i=0;i<set->table_size;i++) {
        assert(set->table[i] == 0);
    }

    // Cleanup
    llmsset_free(set);

    return 1;
}

llmsset_t msset;

void *llmsset_test_worker(void* arg)
{
    #define N_TEST_LL_MS 1000
    uint64_t stored[N_TEST_LL_MS];

    uint64_t insert_index = (long)arg;

    int loop;
    for(loop=0; loop<8; loop++) {
        printf("%d,", loop);
        fflush(stdout);
        uint32_t value, val2;
        for (value=(size_t)arg;value<50000;value++) {
            // Insert a large bunch of values near "value"
            uint32_t k;
            for (k=0; k<N_TEST_LL_MS; k++) {
                val2 = value + k;
                assert(llmsset_lookup(msset, &val2, &insert_index, NULL, &stored[k]));
                assert(val2 == *(uint32_t*)llmsset_index_to_ptr(msset, stored[k], sizeof(uint32_t)));
            }

            // Multiple times, perform lookup again
            int j;
            for (j=0; j<5; j++) {
                for (k=0; k<N_TEST_LL_MS; k++) {
                    uint64_t index;
                    val2 = value + k;
                    assert(llmsset_lookup(msset, &val2, &insert_index, NULL, &index));

                    if (index != stored[k]) {
                        fprintf(stderr, "Difference! Index %"PRIu64" (%d) vs index %"PRIu64" (%d), expecting %d!\n", index, *(uint32_t*)llmsset_index_to_ptr(msset, index, sizeof(uint32_t)), stored[k], *(uint32_t*)llmsset_index_to_ptr(msset, stored[k], sizeof(uint32_t)), val2);
                    }

                    assert(index == stored[k]);
                    assert(val2 == *(uint32_t*)llmsset_index_to_ptr(msset, index, sizeof(uint32_t)));
                }
            }
        }
    }

    return NULL;
}

int test_llmsset2()
{
    msset = llmsset_create(sizeof(uint32_t), sizeof(uint32_t), 1<<20);

    pthread_t t[4];
    pthread_create(&t[0], NULL, &llmsset_test_worker, (void*)12);
    pthread_create(&t[1], NULL, &llmsset_test_worker, (void*)89);
    pthread_create(&t[2], NULL, &llmsset_test_worker, (void*)1055);
    pthread_create(&t[3], NULL, &llmsset_test_worker, (void*)5035);
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    pthread_join(t[2], NULL);
    pthread_join(t[3], NULL);

    uint64_t i;
    for (i=0;i<msset->table_size;i++) {
        uint64_t key = msset->table[i];
        if (key != 0) {
            printf("Key=%"PRIx64"\n", key);
        }
    }

    llmsset_free(msset);

    return 1;
}


double
uniform_deviate(int seed)
{
    return seed * (1.0 / (RAND_MAX + 1.0));
}

int
rng(int low, int high)
{
    return low + uniform_deviate(rand()) * (high-low);
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

void runtests(int threads)
{
    printf(BOLD "Testing LL MS Set\n" NC);
    printf("Running singlethreaded test... ");
    fflush(stdout);
    test_llmsset();
    printf(LGREEN "success" NC "!\n");
    printf("Running multithreaded test... ");
    fflush(stdout);
    if (1/*skip*/) {
        printf("... " LMAGENTA "skipped" NC ".\n");
    }
    else if (test_llmsset2()) {
        printf("... " LGREEN "success" NC "!\n");
    } else {
        printf(LRED "error" NC "!\n");
        exit(1);
    }

    lace_init(threads, 100000, 0);

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
