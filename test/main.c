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
#include "sylvan.h"

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
	
    // a xor b
    BDD axorb = sylvan_makenode(1, b, sylvan_not(b));
    assert(testEqual(axorb, sylvan_xor(a, b)));

    // c or d
    BDD cord = sylvan_makenode(3, d, sylvan_true);
    assert(cord == sylvan_or(c, d));

    BDD t = sylvan_makenode(1, sylvan_false, cord);
    assert(t == sylvan_and(a, cord));

    // (a xor b) and (c or d)
    BDD test = sylvan_makenode(1, sylvan_makenode(2, sylvan_false, cord), sylvan_makenode(2, cord, sylvan_false));
    assert(testEqual(test, sylvan_and(axorb, cord)));
    assert(test == sylvan_and(cord, axorb));

    // not (A and B)  == not A or not B
    test = sylvan_or(sylvan_not(axorb), sylvan_not(cord));
    assert(test == sylvan_not(sylvan_and(axorb, cord)));

    // A and not A == false
    assert(sylvan_false == sylvan_and(axorb, sylvan_not(axorb)));

    // A or not A = true
    assert(sylvan_true == sylvan_or(axorb, sylvan_not(axorb)));


    //sylvan_print(CALL_APPLY(sylvan_not(axorb), sylvan_not(cord), operator_or));
    //sylvan_print(sylvan_not(CALL_APPLY(axorb, cord, operator_and)));



    static int cn=1;
    printf("BDD apply test %d successful!\n", cn++);


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
    assert(sylvan_ite(a, b, c) == sylvan_or(aandb, notaandc));

    // not d then (a and b) else (not a and c) ==
    // a then (b and not d) else (c and d)
    assert(sylvan_ite(sylvan_not(d), aandb, notaandc) ==
           sylvan_ite(a, sylvan_and(b, sylvan_not(d)), sylvan_and(c, d)));

    BDD etheng = sylvan_imp(e, g);
    BDD test = sylvan_ite(etheng, sylvan_true, b);
    assert(sylvan_ite(b, sylvan_false, etheng) == sylvan_and(test, sylvan_not(b)));

    static int cn=1;
    printf("BDD ite test %d successful!\n", cn++);
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

    BDD a_same = sylvan_biimp(a, aa); // a = a'
    BDD b_same = sylvan_biimp(b, bb); // b = b'
    BDD c_same = sylvan_biimp(c, cc); // c = c'
    BDD d_same = sylvan_biimp(d, dd); // d = d'

    BDD a_diff = sylvan_biimp(sylvan_not(a), aa); // a = ~a'
    BDD b_diff = sylvan_biimp(sylvan_not(b), bb); // b = ~b'
    BDD c_diff = sylvan_biimp(sylvan_not(c), cc); // c = ~c'
    BDD d_diff = sylvan_biimp(sylvan_not(d), dd); // d = ~d'

    // a = ~a' and rest stay same
    BDD change_a = sylvan_and(a_diff, sylvan_and(b_same,sylvan_and(c_same,d_same)));
    // b = ~b' and rest stay same
    BDD change_b = sylvan_and(a_same, sylvan_and(b_diff,sylvan_and(c_same,d_same)));
    // c = ~c' and rest stay same
    BDD change_c = sylvan_and(a_same, sylvan_and(b_same,sylvan_and(c_diff,d_same)));
    // d = ~d' and rest stay same
    BDD change_d = sylvan_and(a_same, sylvan_and(b_same,sylvan_and(c_same,d_diff)));

    BDD r = sylvan_or(change_a, sylvan_or(change_b, sylvan_or(change_c, change_d)));
    // sylvan_print(r);

    // Relation r:
    // (0,x,x,x) <=> (1,x,x,x)
    // (x,0,x,x) <=> (x,1,x,x)
    // (x,x,0,x) <=> (x,x,1,x)
    // (x,x,x,0) <=> (x,x,x,1)

    // start: (0,0,0,0)
    BDD start = sylvan_and(sylvan_not(a),
                sylvan_and(sylvan_not(b),
                sylvan_and(sylvan_not(c),
                           sylvan_not(d))));

    BDD visited = start, next, prev;

    do {
        printf("Visited: \n");
        sylvan_print(visited);

        prev = visited;
        next = sylvan_relprods(visited, r);
        visited = sylvan_or(visited, next);
    } while (visited != prev);

}
/*
void test_CALL_QUANTIFY()
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

    BDD test = CALL_ITE(a, CALL_APPLY(b, d, operator_and), CALL_APPLY(sylvan_not(b), sylvan_not(c), operator_or));

    //sylvan_print(test);
    //sylvan_print(CALL_APPLY(CALL_APPLY(b, d, operator_and), CALL_APPLY(sylvan_not(b), sylvan_not(c), operator_or), operator_and));
    //sylvan_print(CALL_QUANTIFY(test, (BDDLEVEL[]){1, sylvan_forall}, 1));
    //sylvan_print(CALL_APPLY(CALL_APPLY(b, d, operator_and), CALL_APPLY(sylvan_not(b), sylvan_not(c), operator_or), operator_or));
    //sylvan_print(CALL_QUANTIFY(test, (BDDLEVEL[]){1, sylvan_exists}, 1));

    BDD axorb = CALL_APPLY(a, b, operator_xor);
    BDD dthenf = CALL_APPLY(d, f, operator_imp);
    BDD cxorg = CALL_APPLY(c, g, operator_xor);

    assert(testEqual(
           CALL_QUANTIFY(CALL_ITE(dthenf, axorb, cxorg),
                           (BDDLEVEL[]){4, quant_exists}, 1),
           CALL_ITE_EX(dthenf, axorb, cxorg,
                         (BDDLEVEL[]){4, quant_exists}, 1)));

    assert(testEqual(
               CALL_QUANTIFY(CALL_ITE(dthenf, axorb, cxorg),
                               (BDDLEVEL[]){4, quant_forall}, 1),
               CALL_ITE_EX(dthenf, axorb, cxorg, (BDDLEVEL[]){4, quant_forall}, 1)));

    //sylvan_quantify
    static int cn=1;
    printf("BDD quantify test %d successful!\n", cn++);

}

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
        for (int i=0;i<3;i++) test_apply();
        for (int i=0;i<3;i++) test_ite();
        for (int i=0;i<3;i++) test_modelcheck();
        //for (int i=0;i<3;i++) test_CALL_ITE_EX();
        //for (int i=0;i<3;i++) test_CALL_REPLACE();
        //for (int i=0;i<3;i++) test_CALL_QUANTIFY();
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

    sylvan_init(2, 16, 16);
    test_modelcheck();
    sylvan_quit();
    exit(0);

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

    BDD aorb2 = CALL_ITE(a, sylvan_true, b);

    sylvan_print(aorb);
    sylvan_print(aorb2);

    struct timespec begin, end;
    clock_gettime(CLOCK_MONOTONIC, &begin);

    BDD ap = CALL_APPLY(aorb, c, operator_xor);

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

    BDD ap2 = CALL_ITE(aorb, sylvan_not(c), c);
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
