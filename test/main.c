#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>

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

    uint64_t index[16], index2[16];
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

    // Add all entries, and do ref
    for (i=0;i<16;i++) {
        assert(llmsset_lookup(set, entry + i, &insert_index, &created, index + i) != 0);
        assert(created);
        llmsset_ref(set, index[i]);
    }

    // Check that GC does not remove ref'd
    assert(llmsset_get_filled(set)==16);
    llmsset_gc(set);
    assert(llmsset_get_filled(set)==16);

    // Add all entries again, then deref
    for (i=0;i<16;i++) {
        assert(llmsset_lookup(set, entry + i, &insert_index, &created, index2 + i) != 0);
        assert(!created);
        assert(index[i] == index2[i]);
        llmsset_deref(set, index[i]);
    }

    // Check that GC removes deref'd
    assert(llmsset_get_filled(set)==16);
    llmsset_gc(set);
    assert(llmsset_get_filled(set)==0);

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
                        fprintf(stderr, "Difference! Index %Ld (%d) vs index %Ld (%d), expecting %d!\n", index, *(uint32_t*)llmsset_index_to_ptr(msset, index, sizeof(uint32_t)), stored[k], *(uint32_t*)llmsset_index_to_ptr(msset, stored[k], sizeof(uint32_t)), val2);
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

    llmsset_gc(msset);

    uint64_t i;
    for (i=0;i<msset->table_size;i++) {
        uint64_t key = msset->table[i];
        if (key != 0) {
            printf("Key=%llX\n", key);
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
    sylvan_print(p1);

    printf("Parameter 2:\n");
    fflush(stdout);
    sylvan_printdot(p2);
    sylvan_print(p2);

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

    sylvan_print(a);
    sylvan_print(b);

	return 0;
}

void
test_xor()
{
    BDD a = sylvan_ithvar(1);
    BDD b = sylvan_ithvar(2);
    testFun(a, b, sylvan_xor(a, b), sylvan_xor(a, b));
    testFun(a, b, sylvan_xor(a, b), sylvan_makenode(1, b, sylvan_not(b)));
}

static inline void
test_diff2(BDD a, BDD b)
{
    sylvan_ref(sylvan_diff(a, b));
    testFun(a, b, sylvan_diff(a, b), sylvan_diff(a, b));
    testFun(a, b, sylvan_diff(a, b), sylvan_diff(a, sylvan_and(a, b)));
    testFun(a, b, sylvan_diff(a, b), sylvan_and(a, sylvan_not(b)));
    testFun(a, b, sylvan_diff(a, b), sylvan_ite(b, sylvan_false, a));
    sylvan_deref(sylvan_diff(a, b));
}

static void
test_diff()
{
    test_diff2(sylvan_ithvar(1), sylvan_ithvar(2));
    int i;
    for (i = 0; i<10; i++) {
        test_diff2(make_random(2, 8), make_random(5, 10));
        test_diff2(make_random(18, 28), make_random(25, 35));
        test_diff2(make_random(3, 11), make_random(5, 10));
        test_diff2(make_random(2, 15), make_random(7, 10));
    }
}

void test_or()
{
    BDD test = sylvan_false;

    // int values[16] = {0,1,1,0,1,0,0,1,1,0,1,0,1,1,1,0};

    BDD t1, t2;

    int i;
    for (i=0;i<16;i++) {
        if (i>0) assert (sylvan_count_refs()==1);
        else assert(sylvan_count_refs()==0);
        t1 = test;
        t2 = sylvan_ref(sylvan_ithvar(i));
        if (i>0) assert (sylvan_count_refs()==2);
        else assert(sylvan_count_refs()==1);
        test = sylvan_ref(sylvan_or(t1, t2));
        if (i>0) assert (sylvan_count_refs()==3);
        else assert(sylvan_count_refs()==2);
        sylvan_deref(t1);
        assert(sylvan_count_refs()==2);
        sylvan_deref(t2);
        assert (sylvan_count_refs()==1);
    }

    sylvan_deref(test);
    assert(sylvan_count_refs()==0);
}

void test_apply()
{
    BDD a,b,c,d,e,f,g;

    a = sylvan_ithvar(1);
    b = sylvan_ithvar(2);
    c = sylvan_ithvar(3);
    d = sylvan_ithvar(4);
    e = sylvan_ithvar(5);
    f = sylvan_ithvar(6);
    g = sylvan_ithvar(7);

    // REF: a,b,c,d,e,f,g

    // a xor b
    BDD axorb = sylvan_makenode(1, (b), sylvan_not(b));
    assert(testEqual(axorb, (sylvan_xor(a, b))));

    // c or d
    BDD cord = sylvan_makenode(3, (d), sylvan_true);
    assert(cord == (sylvan_or(c, d)));

    BDD t = sylvan_makenode(1, sylvan_false, (cord));
    assert(t == (sylvan_and(a, cord)));

    // (a xor b) and (c or d)
    BDD test = sylvan_makenode(1, sylvan_makenode(2, sylvan_false, (cord)), sylvan_makenode(2, (cord), sylvan_false));
    assert(testEqual(test, sylvan_and(axorb, cord)));
    assert(test == sylvan_and(cord, axorb));

    // not (A and B)  == not A or not B
    BDD notaxorb = sylvan_not(axorb);

    BDD notcord = sylvan_not(cord);
    test = sylvan_or(notaxorb, notcord);

    BDD tmp = sylvan_and(axorb, cord);
    assert(test == sylvan_not(tmp));

    // A and not A == false
    assert(sylvan_false == sylvan_and(axorb, notaxorb));

    // A or not A = true
    assert(sylvan_true == sylvan_or(axorb, notaxorb));

    assert((tmp = sylvan_and(a, sylvan_true)) == a);

    assert((tmp = sylvan_or(a, sylvan_true)) == sylvan_true);
    assert((tmp = sylvan_and(a, sylvan_false)) == sylvan_false);

    assert (sylvan_or(sylvan_true, sylvan_false) == sylvan_true);

    return;
    (void)g; (void)f; (void)e;
}

void test_ite()
{
    BDD a,b,c,d,e,f,g;

    a = sylvan_ithvar(1);
    b = sylvan_ithvar(2);
    c = sylvan_ithvar(3);
    d = sylvan_ithvar(4);
    e = sylvan_ithvar(5);
    f = sylvan_ithvar(6);
    g = sylvan_ithvar(7);

    BDD aandb = sylvan_and(a, b);
    assert(aandb == sylvan_ite(a, b, sylvan_false));

    BDD notaandc = sylvan_and(sylvan_not(a), c);

    // a then b else c == (a and b) or (not a and c)
    BDD t = sylvan_ite(a,b,c);
    assert(t == sylvan_or(aandb, notaandc));

    // not d then (a and b) else (not a and c) ==
    // a then (b and not d) else (c and d)
    t = sylvan_ite(sylvan_not(d), aandb, notaandc);
    BDD candd = sylvan_and(c, d);
    BDD bandnotd = sylvan_and(b, sylvan_not(d));
    assert(t == sylvan_ite(a, bandnotd, candd));

    BDD etheng = sylvan_imp(e, g);
    BDD test = sylvan_ite(etheng, sylvan_true, b);
    t = sylvan_ite(b, sylvan_false, etheng);
    assert(t == sylvan_and(test, sylvan_not(b)));

    return;
    (void)f;
}

BDD knownresult;

void test_modelcheck()
{
    BDD a,b,c,d,aa,bb,cc,dd;

    a = sylvan_ithvar(0);
    b = sylvan_ithvar(2);
    c = sylvan_ithvar(4);
    d = sylvan_ithvar(6);

    aa = sylvan_ithvar(1); // a'
    bb = sylvan_ithvar(3); // b'
    cc = sylvan_ithvar(5); // c'
    dd = sylvan_ithvar(7); // d'

    BDD x = sylvan_or(a,
                         (sylvan_or(b,
                         (sylvan_or(c, d)))));
    BDD xx = sylvan_or(aa,
                         (sylvan_or(bb,
                         (sylvan_or(cc, dd)))));

    //BDD universe = sylvan_or(x, xx);

    BDD a_same = sylvan_biimp(a, aa); // a = a'
    BDD b_same = sylvan_biimp(b, bb); // b = b'
    BDD c_same = sylvan_biimp(c, cc); // c = c'
    BDD d_same = sylvan_biimp(d, dd); // d = d'

    BDD a_diff = sylvan_biimp((sylvan_not(a)), aa); // a = ~a'
    BDD b_diff = sylvan_biimp((sylvan_not(b)), bb); // b = ~b'
    BDD c_diff = sylvan_biimp((sylvan_not(c)), cc); // c = ~c'
    BDD d_diff = sylvan_biimp((sylvan_not(d)), dd); // d = ~d'

    // a = ~a' and rest stay same
    BDD change_a = sylvan_and(a_diff, (sylvan_and(b_same,(sylvan_and(c_same,d_same)))));
    // b = ~b' and rest stay same
    BDD change_b = sylvan_and(a_same, (sylvan_and(b_diff,(sylvan_and(c_same,d_same)))));
    // c = ~c' and rest stay same
    BDD change_c = sylvan_and(a_same, (sylvan_and(b_same,(sylvan_and(c_diff,d_same)))));
    // d = ~d' and rest stay same
    BDD change_d = sylvan_and(a_same, (sylvan_and(b_same,(sylvan_and(c_same,d_diff)))));

    BDD r = sylvan_or(change_a, (sylvan_or(change_b, (sylvan_or(change_c, change_d)))));

    // Relation r:
    // (0,x,x,x) <=> (1,x,x,x)
    // (x,0,x,x) <=> (x,1,x,x)
    // (x,x,0,x) <=> (x,x,1,x)
    // (x,x,x,0) <=> (x,x,x,1)

    // start: (0,0,0,0)
    BDD start = sylvan_and((sylvan_not(a)),
              (sylvan_and((sylvan_not(b)), (sylvan_and((sylvan_not(c)), (sylvan_not(d)))))));

    BDD visited = start, prev = sylvan_invalid;

    /* Check if RelProdS gives the same result as RelProd and Substitute */
    assert((sylvan_relprods(visited, r, sylvan_true)) ==
           (sylvan_substitute((sylvan_relprod(visited, r, x)), xx)));

    /* Expected first: (0,0,0,0), (1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,0) */

    do {
        //printf("Visited: \n");
        //sylvan_print(visited);
        prev = visited;
        BDD next = sylvan_relprods(visited, r, sylvan_true);
        visited = sylvan_or(visited, next);
        // check that the "visited" set is a subset of all parents of next.
        BDD check = sylvan_relprods_reversed(next, r, sylvan_true);
        assert (sylvan_diff(prev, check) == sylvan_false); // prev \ check = 0
    } while (visited != prev);
}

void test_exists_forall()
{
    BDD a,b,c,d,e,f,g,h;

    a = sylvan_ithvar(1);
    b = sylvan_ithvar(2);
    c = sylvan_ithvar(3);
    d = sylvan_ithvar(4);
    e = sylvan_ithvar(5);
    f = sylvan_ithvar(6);
    g = sylvan_ithvar(7);
    h = sylvan_ithvar(8);

    (sylvan_or((sylvan_not(b)), (sylvan_not(c))));

    sylvan_ite(a, (sylvan_and(b, d)), (sylvan_or((sylvan_not(b)), (sylvan_not(c)))));

    BDD axorb = sylvan_xor(a, b);
    BDD dthenf = sylvan_imp(d, f);
    BDD cxorg = sylvan_xor(c, g);

    (sylvan_exists((sylvan_ite(dthenf, axorb, cxorg)), d));
    (sylvan_forall((sylvan_ite(dthenf, axorb, cxorg)), d));
    (sylvan_exists(axorb, sylvan_false));
    (sylvan_exists(axorb, sylvan_false));
    (sylvan_exists(dthenf, a));
    (sylvan_exists(dthenf, d));
    (sylvan_exists(dthenf, f));
    (sylvan_exists(sylvan_true, sylvan_false));

    return;
    (void)h; (void)e;
}
/*
void test_CALL_REPLACE()
{
    BDD a,b,c,d,e,f,g,h;

    a = sylvan_ithvar(1);
    b = sylvan_ithvar(2);
    c = sylvan_ithvar(3);
    d = sylvan_ithvar(4);
    e = sylvan_ithvar(5);
    f = sylvan_ithvar(6);
    g = sylvan_ithvar(7);
    h = sylvan_ithvar(8);

    assert(b == CALL_REPLACE(a, (BDDLEVEL[]){1, 2}, 1));
    assert(sylvan_not(b) == CALL_REPLACE(sylvan_not(a), (BDDLEVEL[]){1, 2}, 1));

    BDD aorc = CALL_APPLY(a, c, operator_or);
    BDD dorc = CALL_REPLACE(aorc, (BDDLEVEL[]){1,4}, 1);
    assert (dorc == CALL_APPLY(d, c, operator_or));

    BDD not_candd = sylvan_not(CALL_APPLY(c, d, operator_and));
    BDD note_or_notf = CALL_APPLY(sylvan_not(e), sylvan_not(f), operator_or);
    assert(note_or_notf == CALL_REPLACE(not_candd, (BDDLEVEL[]){3,6,4,5}, 2));

    BDD axorc = CALL_APPLY(a, c, operator_xor);
    BDD dxorc = CALL_REPLACE(axorc, (BDDLEVEL[]){1,4}, 1);
    assert( dxorc == CALL_APPLY(d, c, operator_xor));

    // more complex test
    // e imp g then (if a then (b and not d) else (c and d)) else f
    BDD test = CALL_ITE(a, CALL_APPLY(b, sylvan_not(d), operator_and), CALL_APPLY(c, d, operator_and));
    test = CALL_ITE(CALL_APPLY(e, g, operator_imp), test, f);
    // a imp b then (if c then (d and not e) else (f and e)) else g
    BDD result = CALL_REPLACE(test, (BDDLEVEL[]){5,1, 7,2, 1,3, 2,4, 4,5, 3,6, 6,7}, 7);

    BDD cmp = CALL_ITE(c, CALL_APPLY(d, sylvan_not(e), operator_and), CALL_APPLY(f, e, operator_and));
    cmp = CALL_ITE(CALL_APPLY(a, b, operator_imp), cmp, g);

    if (knownresult == sylvan_invalid) knownresult = cmp;
    else assert(cmp == knownresult);

    if (cmp != result) {
        printf("Assertion failed cmp != result: %x != %x\n", cmp, result);
        sylvan_print(cmp);
        sylvan_print(result);
        exit(1);
    }

    static int cn=1;
    printf("BDD replace test %d successful!\n", cn++);


}
*/

void __is_sylvan_clean()
{
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

    sylvan_package_init(threads, 100000);

    printf(BOLD "Testing Sylvan\n");

    printf(NC "Running test 'Xor'... ");
    fflush(stdout);
    int j;
    for (j=0;j<16;j++) {
        sylvan_init(6, 6, 1);

        test_xor();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();

        int i;
        for (i=0;i<3;i++) test_xor();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();

        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Running test 'Diff'... ");
    fflush(stdout);
    for (j=0;j<16;j++) {
        sylvan_init(20, 14, 1);

        test_diff();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();

        int i;
        for (i=0;i<3;i++) test_diff();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();

        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Running test 'Or'... ");
    fflush(stdout);
    for (j=0;j<16;j++) {
        sylvan_init(9, 9, 1);

        test_or();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();

        int i;
        for (i=0;i<3;i++) test_or();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();

        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Running test 'Apply'... ");
    fflush(stdout);
    for (j=0;j<16;j++) {
        sylvan_init(6, 6, 1);
        test_apply();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();

        int i;
        for (i=0;i<3;i++) test_apply();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();

        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Running test 'ITE'... ");
    fflush(stdout);
    for (j=0;j<16;j++) {
        sylvan_init(5, 5, 1);
        int i;
        for (i=0;i<3;i++) test_ite();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();
        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Running test 'ExistsForall'... ");
    fflush(stdout);
    for (j=0;j<16;j++) {
        sylvan_init(16, 16, 1);
        int i;
        for (i=0;i<3;i++) test_exists_forall();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();
        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");


    printf(NC "Running test 'ModelCheck'... ");
    fflush(stdout);
    for (j=0;j<16;j++) {
        sylvan_init(7, 10, 3); // 7, 4, 2, 2 but slow on 2
        int i;
        for (i=0;i<3;i++) test_modelcheck();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();
        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Running test 'Mixed'... ");
    fflush(stdout);
    for (j=0;j<16;j++) {
        sylvan_init(7, 10, 3);
        int i;
        for (i=0;i<3;i++) test_apply();
        for (i=0;i<3;i++) test_ite();
        for (i=0;i<3;i++) test_modelcheck();
        // verify gc
        sylvan_gc();
        __is_sylvan_clean();
        sylvan_quit();
    }
    printf(LGREEN "success" NC "!\n");

    printf(NC "Running two-threaded stresstest 'Mixed'... ");
    fflush(stdout);

    struct timeval begin, end;
    gettimeofday(&begin, NULL);

    sylvan_init(20, 10, 1); // minumum: X, 7, 4, 4 ??
    for (j=0;j<10000;j++){
        int i;
        for (i=0;i<3;i++) test_apply();
        for (i=0;i<3;i++) test_ite();
        for (i=0;i<3;i++) test_modelcheck();
        for (i=0;i<3;i++) test_apply();
        for (i=0;i<3;i++) test_ite();
        for (i=0;i<3;i++) test_modelcheck();
    }
    sylvan_quit();

    gettimeofday(&end, NULL);
    printf(LGREEN "success" NC);

    //calculate diff
    if (end.tv_usec < begin.tv_usec) {
        end.tv_sec--;
        end.tv_usec += 1000000L;
    }
    end.tv_usec -= begin.tv_usec;
    end.tv_sec -= begin.tv_sec;

    long ms = end.tv_sec * 1000 + end.tv_usec / 1000;
    long us = (end.tv_usec % 1000) / 1000;
    printf(NC " (%ld.%03ld ms)!\n", ms, us);

    sylvan_report_stats();
    sylvan_package_exit();
}

int main(int argc, char **argv)
{
    int threads = 2;
    if (argc > 1) sscanf(argv[1], "%d", &threads);

    /*
    sylvan_package_init(1, 100000);
    sylvan_init(20, 10, 1);
    BDD bdd = make_random(0, 15);
    sylvan_serialize_add(bdd);
    sylvan_serialize_totext(stdout);

    FILE *tmp = fopen("test.bdd", "w");
    sylvan_serialize_tofile(tmp);
    fclose(tmp);
    tmp = fopen("test.bdd", "r");
    size_t v = sylvan_serialize_get(bdd);
    sylvan_serialize_fromfile(tmp);
    assert (bdd == sylvan_serialize_get_reversed(v));

    printf("\n");
    sylvan_printdot(bdd);
    sylvan_quit();
    sylvan_package_exit();
    exit(0);
    */

    runtests(threads);
    printf(NC);
    exit(0);
}
