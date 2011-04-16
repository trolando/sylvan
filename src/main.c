#include "runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>

#include <assert.h>

#include "llset.h"
#include "llsched.h"
#include "sylvan.h"
#include "llvector.h"

llsched_t s;
int32_t count[4];
void *testthread(void *data);

void test_sched()
{
    s = llsched_create(3, 4);

    for (int i=0;i<4;i++) count[i] = 0;

    pthread_t thread[4];
    pthread_create(&thread[0], NULL, testthread, (void*)0);
    pthread_create(&thread[1], NULL, testthread, (void*)1);
    pthread_create(&thread[2], NULL, testthread, (void*)2);
    //pthread_create(&thread[3], NULL, testthread, (void*)3);

    void *result;
    pthread_join(thread[0], &result);
    pthread_join(thread[1], &result);
    pthread_join(thread[2], &result);
    //pthread_join(thread[3], &result);

    for (int i=0;i<3;i++) {
        assert(count[i] == 1000*i+100);
    }

    llsched_free(s);
}

void *testthread(void *data) {
    int32_t threadid = (long)data;

    for (int i=0;i<1000*threadid+100;i++) {
        llsched_push(s, threadid, &threadid);
    }

    int32_t mm;

    while (1 == llsched_pop(s, threadid, &mm)) {
        if (mm >= 0) {
            while (1)
            {
                int32_t c=count[mm];
                if (cas(&count[mm], c, c+1)) break;
            }
            mm = -1;
            llsched_push(s, threadid, &mm);
        }
    }

    return NULL;
}

llset_t set;
void *test_set_thread(void*);
#define TEST_SET_NUM 3

void test_set()
{
    set = llset_create(4, 16, NULL, NULL);

    pthread_t thread[TEST_SET_NUM];
    for (int32_t i=0;i<TEST_SET_NUM;i++)
        pthread_create(&thread[i], NULL, test_set_thread, (void*)i);

    void *result;
    for (int32_t i=0;i<TEST_SET_NUM;i++)
        pthread_join(thread[i], &result);

    for (int32_t i=0;i<TEST_SET_NUM*1000;i++) {
        int created;
        llset_get_or_create(set, &i, &created, NULL);
        if (created) printf("created: %d\n", i);
        assert(created == 0);
    }

    llset_free(set);
}

void *test_set_thread(void* data)
{
    int32_t threadid = (int32_t)data;

    for (int i=0;i<1000;i++) {
        int created;
        int32_t value = i + threadid * 1000;
        llset_get_or_create(set, &value, &created, NULL);
        if (!created) printf("exists: %d\n", value);
        assert(created != 0);
    }

    return NULL;
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

	sylvan_print(a);
	sylvan_print(b);
	return 0;

}

void test_sylvan_apply()
{
    BDD a,b,c,d,e,f,g;

    a = sylvan_ithvar(1);
    b = sylvan_ithvar(2);
    c = sylvan_ithvar(3);
    d = sylvan_ithvar(4);
    e = sylvan_ithvar(5);
    f = sylvan_ithvar(6);
    g = sylvan_ithvar(7);

    // a xor b
    BDD axorb = sylvan_makenode(1, b, sylvan_not(b));
    assert(axorb == sylvan_apply(a, b, operator_xor));

    // c or d
    BDD cord = sylvan_makenode(3, d, sylvan_true);
    assert(cord == sylvan_apply(c, d, operator_or));

    BDD t = sylvan_makenode(1, sylvan_false, cord);
    assert(t == sylvan_apply(a, cord, operator_and));

    // (a xor b) and (c or d)
    BDD test = sylvan_makenode(1, sylvan_makenode(2, sylvan_false, cord), sylvan_makenode(2, cord, sylvan_false));
    assert(testEqual(test, sylvan_apply(axorb, cord, operator_and)));
    assert(test == sylvan_apply(cord, axorb, operator_and));

    // not (A and B)  == not A or not B
    test = sylvan_apply(sylvan_not(axorb), sylvan_not(cord), operator_or);
    assert(test == sylvan_not(sylvan_apply(axorb, cord, operator_and)));

    // A and not A == false
    assert(sylvan_false == sylvan_apply(axorb, sylvan_not(axorb), operator_and));

    // A or not A = true
    assert(sylvan_true == sylvan_apply(axorb, sylvan_not(axorb), operator_or));


    //sylvan_print(sylvan_apply(sylvan_not(axorb), sylvan_not(cord), operator_or));
    //sylvan_print(sylvan_not(sylvan_apply(axorb, cord, operator_and)));



    static int cn=1;
    printf("BDD apply test %d successful!\n", cn++);


}

void test_sylvan_ite()
{
    BDD a,b,c,d,e,f,g;

    a = sylvan_ithvar(1);
    b = sylvan_ithvar(2);
    c = sylvan_ithvar(3);
    d = sylvan_ithvar(4);
    e = sylvan_ithvar(5);
    f = sylvan_ithvar(6);
    g = sylvan_ithvar(7);

    BDD aandb = sylvan_apply(a, b, operator_and);
    assert(aandb == sylvan_ite(a, b, sylvan_false));

    BDD notaandc = sylvan_apply(sylvan_not(a), c, operator_and);

    // a then b else c == (a and b) or (not a and c)
    assert(sylvan_ite(a, b, c) == sylvan_apply(aandb, notaandc, operator_or));

    // not d then (a and b) else (not a and c) ==
    // a then (b and not d) else (c and d)
    assert(sylvan_ite(sylvan_not(d), aandb, notaandc) ==
           sylvan_ite(a, sylvan_apply(b, sylvan_not(d), operator_and), sylvan_apply(c, d, operator_and)));

    BDD etheng = sylvan_apply(e, g, operator_imp);
    BDD test = sylvan_ite(etheng, sylvan_true, b);
    assert(sylvan_ite(b, sylvan_false, etheng) == sylvan_apply(test, sylvan_not(b), operator_and));

    static int cn=1;
    printf("BDD ite test %d successful!\n", cn++);
}

BDD knownresult;

void test_sylvan_ite_ex()
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

    assert(b == sylvan_ite_ex(a, sylvan_true, sylvan_false, (BDDLEVEL[]){1, 2}, 1));
    assert(sylvan_not(b) == sylvan_ite_ex(sylvan_not(a), sylvan_true, sylvan_false, (BDDLEVEL[]){1, 2}, 1));

    BDD aorc = sylvan_apply(a, c, operator_or);
    BDD dorc = sylvan_ite_ex(aorc, sylvan_true, sylvan_false, (BDDLEVEL[]){1,4}, 1);
    assert(testEqual(dorc, sylvan_apply(d, c, operator_or)));

    BDD not_candd = sylvan_not(sylvan_apply(c, d, operator_and));
    BDD note_or_notf = sylvan_apply(sylvan_not(e), sylvan_not(f), operator_or);
    assert(note_or_notf == sylvan_ite_ex(not_candd, sylvan_true, sylvan_false, (BDDLEVEL[]){3,6,4,5}, 2));

    BDD axorc = sylvan_apply(a, c, operator_xor);
    BDD dxorc = sylvan_ite_ex(axorc, sylvan_true, sylvan_false, (BDDLEVEL[]){1,4}, 1);

    //sylvan_print(axorc);
    //sylvan_print(dxorc);

    assert(testEqual(dxorc, sylvan_apply(d, c, operator_xor)));

    // more complex test
    // e imp g then (if a then (b and not d) else (c and d)) else f
    BDD test = sylvan_ite(a, sylvan_apply(b, sylvan_not(d), operator_and), sylvan_apply(c, d, operator_and));
    test = sylvan_ite(sylvan_apply(e, g, operator_imp), test, f);
    // a imp b then (if c then (d and not e) else (f and e)) else g
    BDD cmp = sylvan_ite(c, sylvan_apply(d, sylvan_not(e), operator_and), sylvan_apply(f, e, operator_and));
    cmp = sylvan_ite(sylvan_apply(a, b, operator_imp), cmp, g);

    if (knownresult == sylvan_invalid) knownresult = cmp;
    else assert(cmp == knownresult);

    BDD result = sylvan_ite_ex(test, sylvan_true, sylvan_false, (BDDLEVEL[]){5,1, 7,2, 1,3, 2,4, 4,5, 3,6, 6,7}, 7);
    if (cmp != result) {
        printf("Assertion failed cmp != result: %x != %x\n", cmp, result);
        sylvan_print(cmp);
        sylvan_print(result);
        exit(1);

    }

    static int cn=1;
    printf("BDD ite ex test %d successful!\n", cn++);
}

void tm_test(BDD bdd) {
    for (int a=0;a<2;a++) {
        for (int b=0;b<2;b++) {
            for (int c=0;c<2;c++) {
                for (int d=0;d<2;d++) {
                    BDD r = bdd;
                    if (a) r=sylvan_high(r);
                    else r=sylvan_low(r);
                    if (b) r=sylvan_high(r);
                    else r=sylvan_low(r);
                    if (c) r=sylvan_high(r);
                    else r=sylvan_low(r);
                    if (d) r=sylvan_high(r);
                    else r=sylvan_low(r);
                    if (r == sylvan_true) printf("(%d, %d, %d, %d) => YES\n", a, b, c, d);
                    else printf("(%d, %d, %d, %d) => NO\n", a, b, c, d);
                }
            }
        }
    }
}

void test_modelcheck()
{
    BDD a,b,c,d,aa,bb,cc,dd;

    a = sylvan_ithvar(0);
    b = sylvan_ithvar(1);
    c = sylvan_ithvar(2);
    d = sylvan_ithvar(3);

    aa = sylvan_ithvar(4); // a'
    bb = sylvan_ithvar(5); // b'
    cc = sylvan_ithvar(6); // c'
    dd = sylvan_ithvar(7); // d'

    BDD a_same = sylvan_apply(a, aa, operator_biimp); // a = a'
    BDD b_same = sylvan_apply(b, bb, operator_biimp); // b = b'
    BDD c_same = sylvan_apply(c, cc, operator_biimp); // c = c'
    BDD d_same = sylvan_apply(d, dd, operator_biimp); // d = d'

    BDD a_diff = sylvan_apply(sylvan_not(a), aa, operator_biimp); // a = ~a'
    BDD b_diff = sylvan_apply(sylvan_not(b), bb, operator_biimp); // b = ~b'
    BDD c_diff = sylvan_apply(sylvan_not(c), cc, operator_biimp); // c = ~c'
    BDD d_diff = sylvan_apply(sylvan_not(d), dd, operator_biimp); // d = ~d'

    // a = ~a' and rest stay same
    BDD change_a = sylvan_apply(a_diff, sylvan_apply(b_same,sylvan_apply(c_same,d_same,operator_and),operator_and),operator_and);
    // b = ~b' and rest stay same
    BDD change_b = sylvan_apply(a_same, sylvan_apply(b_diff,sylvan_apply(c_same,d_same,operator_and),operator_and),operator_and);
    // c = ~c' and rest stay same
    BDD change_c = sylvan_apply(a_same, sylvan_apply(b_same,sylvan_apply(c_diff,d_same,operator_and),operator_and),operator_and);
    // d = ~d' and rest stay same
    BDD change_d = sylvan_apply(a_same, sylvan_apply(b_same,sylvan_apply(c_same,d_diff,operator_and),operator_and),operator_and);

    BDD r = sylvan_apply(change_a, sylvan_apply(change_b, sylvan_apply(change_c, change_d, operator_or), operator_or), operator_or);
    // sylvan_print(r);

    // Relation r:
    // (0,x,x,x) <=> (1,x,x,x)
    // (x,0,x,x) <=> (x,1,x,x)
    // (x,x,0,x) <=> (x,x,1,x)
    // (x,x,x,0) <=> (x,x,x,1)

    // start: (0,0,0,0)
    BDD start = sylvan_apply(sylvan_not(a),
                sylvan_apply(sylvan_not(b),
                sylvan_apply(sylvan_not(c),
                          sylvan_not(d),
                                      operator_and),
                                      operator_and),
                                      operator_and);

    BDD visited = start, next, prev;

    BDDLEVEL* pairs = (BDDLEVEL[]){quant_exists,quant_exists,quant_exists,quant_exists,a,b,c,d};

    do {
        printf("Visited: \n");
        // sylvan_print(visited);
        tm_test(visited);

        prev = visited;
        // this is: NEXT := (x'/x) exists(relprod(visited, r), x)
        //          operator AND, quantification and renaming in one operation
        //          possible because quantified and renamed variables don't overlap
        next = sylvan_restructure(visited, r, sylvan_false, pairs, 7);
        // this is: VISITED := VISITED or NEXT
        visited = sylvan_apply(visited, next, operator_or);
    } while (visited != prev);

}

void test_sylvan_quantify()
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

    BDD test = sylvan_ite(a, sylvan_apply(b, d, operator_and), sylvan_apply(sylvan_not(b), sylvan_not(c), operator_or));

    //sylvan_print(test);
    //sylvan_print(sylvan_apply(sylvan_apply(b, d, operator_and), sylvan_apply(sylvan_not(b), sylvan_not(c), operator_or), operator_and));
    //sylvan_print(sylvan_quantify(test, (BDDLEVEL[]){1, sylvan_forall}, 1));
    //sylvan_print(sylvan_apply(sylvan_apply(b, d, operator_and), sylvan_apply(sylvan_not(b), sylvan_not(c), operator_or), operator_or));
    //sylvan_print(sylvan_quantify(test, (BDDLEVEL[]){1, sylvan_exists}, 1));

    BDD axorb = sylvan_apply(a, b, operator_xor);
    BDD dthenf = sylvan_apply(d, f, operator_imp);
    BDD cxorg = sylvan_apply(c, g, operator_xor);

    assert(testEqual(
           sylvan_quantify(sylvan_ite(dthenf, axorb, cxorg),
                           (BDDLEVEL[]){4, quant_exists}, 1),
           sylvan_ite_ex(dthenf, axorb, cxorg,
                         (BDDLEVEL[]){4, quant_exists}, 1)));

    assert(testEqual(
               sylvan_quantify(sylvan_ite(dthenf, axorb, cxorg),
                               (BDDLEVEL[]){4, quant_forall}, 1),
               sylvan_ite_ex(dthenf, axorb, cxorg, (BDDLEVEL[]){4, quant_forall}, 1)));

    //sylvan_quantify
    static int cn=1;
    printf("BDD quantify test %d successful!\n", cn++);

}

void test_sylvan_replace()
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

    assert(b == sylvan_replace(a, (BDDLEVEL[]){1, 2}, 1));
    assert(sylvan_not(b) == sylvan_replace(sylvan_not(a), (BDDLEVEL[]){1, 2}, 1));

    BDD aorc = sylvan_apply(a, c, operator_or);
    BDD dorc = sylvan_replace(aorc, (BDDLEVEL[]){1,4}, 1);
    assert (dorc == sylvan_apply(d, c, operator_or));

    BDD not_candd = sylvan_not(sylvan_apply(c, d, operator_and));
    BDD note_or_notf = sylvan_apply(sylvan_not(e), sylvan_not(f), operator_or);
    assert(note_or_notf == sylvan_replace(not_candd, (BDDLEVEL[]){3,6,4,5}, 2));

    BDD axorc = sylvan_apply(a, c, operator_xor);
    BDD dxorc = sylvan_replace(axorc, (BDDLEVEL[]){1,4}, 1);
    assert( dxorc == sylvan_apply(d, c, operator_xor));

    // more complex test
    // e imp g then (if a then (b and not d) else (c and d)) else f
    BDD test = sylvan_ite(a, sylvan_apply(b, sylvan_not(d), operator_and), sylvan_apply(c, d, operator_and));
    test = sylvan_ite(sylvan_apply(e, g, operator_imp), test, f);
    // a imp b then (if c then (d and not e) else (f and e)) else g
    BDD result = sylvan_replace(test, (BDDLEVEL[]){5,1, 7,2, 1,3, 2,4, 4,5, 3,6, 6,7}, 7);

    BDD cmp = sylvan_ite(c, sylvan_apply(d, sylvan_not(e), operator_and), sylvan_apply(f, e, operator_and));
    cmp = sylvan_ite(sylvan_apply(a, b, operator_imp), cmp, g);

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

void runtests(int threads, int iterations)
{
    /*
    for (int i=0;i<1000;i++) test_sched();
    printf("test sched done");
    for (int i=0;i<1000;i++) {
        test_set();
        printf("test set done\n");
    }
    */
    knownresult = sylvan_invalid;

    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);

    for (int j=0;j<iterations;j++){
        sylvan_init(threads, 16, 16);
        for (int i=0;i<3;i++) test_sylvan_apply();
        for (int i=0;i<3;i++) test_sylvan_ite();
        for (int i=0;i<3;i++) test_sylvan_ite_ex();
        for (int i=0;i<3;i++) test_sylvan_replace();
        for (int i=0;i<3;i++) test_sylvan_quantify();
        sylvan_quit();
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    //calculate diff
    if (end.tv_nsec < begin.tv_nsec) {
        end.tv_sec--;
        end.tv_nsec += 1000000000L;
    }
    end.tv_nsec -= begin.tv_nsec;
    end.tv_sec -= begin.tv_sec;

    long ms = end.tv_sec * 1000 + end.tv_nsec / 1000000;
    long us = (end.tv_nsec % 1000000) / 1000;
    printf("Time: %ld.%03ld ms\n", ms, us);
}

int main(int argc, char **argv)
{
/*
    sylvan_init(2, 16, 16);
    test_modelcheck();
    sylvan_quit();
    exit(0);
*/
    int threads = 2;
    int iterations = 5000;
    if (argc > 1) sscanf(argv[1], "%d", &threads);
    if (argc > 2) sscanf(argv[2], "%d", &iterations);

    runtests(threads, iterations);
    exit(0);

/*
    int threads = 2;
    if (argc > 1) sscanf(argv[1], "%d", &threads);

    sylvan_init(threads, 26, 23);

    BDD a,b,c;

    // A or B       and     C

    // which is A then 1 else B      and C

    a = sylvan_ithvar(1); // A
    b = sylvan_ithvar(2); // B
    c = sylvan_ithvar(3); // C

    BDD aorb = sylvan_makenode(1, b, sylvan_true);

    BDD aorb2 = sylvan_ite(a, sylvan_true, b);

    sylvan_print(aorb);
    sylvan_print(aorb2);

    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);

    BDD ap = sylvan_apply(aorb, c, operator_xor);

    clock_gettime(CLOCK_MONOTONIC, &end);

    //calculate diff
    if (end.tv_nsec < begin.tv_nsec) {
        end.tv_sec--;
        end.tv_nsec += 1000000000L;
    }
    end.tv_nsec -= begin.tv_nsec;
    end.tv_sec -= begin.tv_sec;

    long ms = end.tv_sec * 1000 + end.tv_nsec / 1000000;
    long us = (end.tv_nsec % 1000000) / 1000;
    printf("Time: %ld.%03ld ms\n", ms, us);

    sylvan_print(ap);

    BDD ap2 = sylvan_ite(aorb, sylvan_not(c), c);
    sylvan_print(ap2);

    sylvan_quit();
*/
/*
    llset_t t = llset_create_size(8, 26);

    printf("size, %d\n", (1<<26)*8);

    char* data = (char*)llset_getOrCreate(t, "Hello\r\n\0", NULL);
    char* data2 = (char*)llset_getOrCreate(t, "Hello\r\n\0", NULL);
    printf("%s", data);
    if (data != data2) printf("error\n");
    else printf("good\n");

    llset_free(t);
*/

/*

    llvector_t v = llvector_create(8);

    llvector_add(v, "ASDFQWER");
    llvector_add(v, "12344321");
    llvector_add(v, "qwerrewq");
    llvector_add(v, "freddref");

    llvector_delete(v, 1);

    for (int i=0; i<llvector_count(v); i++) {
        char buf[9];
        buf[8] = 0;
        llvector_get(v, i, buf);
        printf("%d: %s\n", i, buf);
    }


    return 0;*/
}
