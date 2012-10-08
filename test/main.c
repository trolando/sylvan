#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/time.h>

#include <assert.h>

#include "llgcset.h"
#include "llcache.h"
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

struct llcache
{
    size_t    padded_data_length;
    size_t    key_length;
    size_t    data_length;
    size_t    cache_size;  
    uint32_t *table;        // table with hashes
    uint8_t  *data;         // table with data
    llcache_delete_f  cb_delete;    // delete function (callback pre-delete)
    uint32_t  mask;         // size-1
};

void test_llcache() 
{
    llcache_t c = llcache_create(4, 8, 1<<5, NULL, NULL);
  
    assert(c->padded_data_length == 8);
   
    struct tt {
        uint32_t a, b;
    };

    struct tt n, m;
  
    n.a = 5;
    n.b = 6;
    m.a = 5;

    assert(llcache_put(c, &n));

//    int i;
//    for (i = 0;i<(1<<5); i++) {
//       printf("%d: %X (%d %d)\n", i, c->table[i], *(uint32_t*)&c->data[i*8], *(uint32_t*)&c->data[i*8+4]);
//    }
    assert(llcache_get(c, &m));
    assert(m.b == 6);

    n.b = 7;
    assert(!llcache_put(c, &n));
    assert(n.b == 6);
//    for (i = 0;i<(1<<5); i++) {
//       printf("%d: %X (%d %d)\n", i, c->table[i], *(uint32_t*)&c->data[i*8], *(uint32_t*)&c->data[i*8+4]);
//    }
    assert(llcache_get(c, &m));
    assert(m.b == 7);

    llcache_clear(c);

    assert(llcache_get(c, &m)==0);

    llcache_free(c);
}



extern llgcset_t __sylvan_get_internal_data();
#ifdef CACHE
extern llcache_t __sylvan_get_internal_cache();
#endif

llgcset_t set_under_test;
int set2_test_good=0;

void *llgcset_test_worker(void* arg) {
    uint32_t i=0, j=0, k=0, l;
    uint32_t index;
    int a;
    #define N_TEST_LL 1000
    uint32_t other[N_TEST_LL];
    for(a=0;a<8;a++) {
        printf("%d,", a);
        fflush(stdout);
        for (i=(long)arg;i<50000;i++) {
            //if ((i & 63) == 0) printf("[%d]", i);
            for (l=i;l<i+N_TEST_LL;l++) {
              assert(llgcset_lookup(set_under_test, &l, NULL, &other[l-i]));
            }
            for (j=0;j<5;j++) {
                assert(llgcset_lookup(set_under_test, &i, NULL, &index));
                assert (i == *(uint32_t*)llgcset_index_to_ptr(set_under_test, index, 4));
                for (k=0;k<7;k++) { 
                    llgcset_ref(set_under_test, index);
                    uint32_t i2;
                    assert(llgcset_lookup(set_under_test, &i, NULL, &i2));
                    assert(i2==index);
                    llgcset_deref(set_under_test, index);
                    llgcset_deref(set_under_test, index);
                    llgcset_ref(set_under_test, index);
                    llgcset_deref(set_under_test, index);
                }
                llgcset_deref(set_under_test, index);
            }
            for (l=i;l<i+N_TEST_LL;l++) {
              assert(llgcset_lookup(set_under_test, &l, NULL, &index));
              assert(llgcset_lookup(set_under_test, &l, NULL, &index));
              if (index != other[l-i]) {
                   if ((index & ~15) == (other[l-i] & ~15)) printf(LMAGENTA "\n*** SAME CACHE LINE ***\n" NC);
                   printf("\nIndex %u: %x = %d, Other %u: %x = %d\n", 
                       index, set_under_test->table[index], *(uint32_t*)llgcset_index_to_ptr(set_under_test, index, 4),
                       other[l-i], set_under_test->table[other[l-i]], *(uint32_t*)llgcset_index_to_ptr(set_under_test, other[l-i], 4));
              }
              assert(index == other[l-i]);
              llgcset_deref(set_under_test, index);
              llgcset_ref(set_under_test, index);
              llgcset_deref(set_under_test, index);
              llgcset_deref(set_under_test, other[l-i]);
              llgcset_deref(set_under_test, other[l-i]);
            }
        }
    }
    __sync_fetch_and_add(&set2_test_good, 1);
    return NULL;
}

/**
 * Called pre-gc : first, gc the cache to free nodes
 */
void test_pre_gc(const llgcset_t dbs) 
{
    fflush(stdout);
}

int test_llgcset2()
{
    //set_under_test = llgcset_create(sizeof(uint32_t), 20, 10, NULL, NULL, NULL, &test_pre_gc);
    set_under_test = llgcset_create(sizeof(uint32_t), sizeof(uint32_t), 1<<20, NULL, NULL);

    int i;
    pthread_t t[4];
    pthread_create(&t[0], NULL, &llgcset_test_worker, (void*)12);
    pthread_create(&t[1], NULL, &llgcset_test_worker, (void*)89);
    pthread_create(&t[2], NULL, &llgcset_test_worker, (void*)1055);
    pthread_create(&t[3], NULL, &llgcset_test_worker, (void*)5035);
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    pthread_join(t[2], NULL);
    pthread_join(t[3], NULL);

    llgcset_gc(set_under_test);

    int n=0;
    for (i=0;i<set_under_test->table_size;i++) {
        uint32_t key = set_under_test->table[i];
        if (key != 0) {
            if (key != 0x7fffffff) {
                printf("Key=%X\n", key);
            //assert(key == 0x7fffffff);
                n++;
            }
        }
    }
    printf("N=%d", n);
    
    return set2_test_good == 4;
}

int test_llgcset() 
{
    uint32_t entry[] = { 90570123,  43201432,   31007798,  256346587, 
                         543578998, 34534278,   86764826,  572667984, 
                         883562435, 2546247838, 190200937, 918456256, 
                         245892765, 29926542,   862864346, 624500973 };
    
    uint32_t index[16], index2[16];
    int created;
    
    llgcset_t set = llgcset_create(sizeof(uint32_t), sizeof(uint32_t), 1<<5, NULL, NULL); // size: 32

    int i;
    for (i=0;i<16;i++) {
        if (llgcset_lookup(set, &entry[i], &created, &index[i])==0) {
            llgcset_gc(set);
            assert(llgcset_lookup(set, &entry[i], &created, &index[i])!=0);
        }
        //printf("Position: %d\n", index[i]);
        assert(created);
    }
    
    for (i=0;i<16;i++) {
        if (llgcset_lookup(set, &entry[i], &created, &index2[i])==0) {
            llgcset_gc(set);
            assert(llgcset_lookup(set, &entry[i], &created, &index2[i])!=0);
        }
        assert(created == 0);
        assert(index[i] == index2[i]);
    }
    
    int n=0;

    // check all have ref 2
    for (i=0;i<set->table_size;i++) {
        uint32_t key = set->table[i];
        if (key != 0) {
            n++;
            assert((key & 0x0000ffff) == 2);
        }
    }
    
    assert(n == 16);
    
    // deref all twice
    for (i=0;i<16;i++) {
        llgcset_deref(set, index[i]);
        llgcset_deref(set, index[i]);
    }
    
    // check all have ref 0
    n=0;
    for (i=0;i<set->table_size;i++) {
        uint32_t key = set->table[i];
        if (key != 0) {
            n++;
            assert((key & 0x0000ffff) == 0);
        }
    }
    assert(n == 16);
    
/*    // check gc list
    assert(set->gc_head == set->gc_size-1);
    assert(set->gc_tail == 16);
    for (i=0;i<16;i++) {
        assert(set->gc_list[i] == index[i]);
    }
*/    
    for (i=0;i<16;i++) {
        if (llgcset_lookup(set, &entry[i], &created, &index2[i])==0) {
            llgcset_gc(set);
            assert(llgcset_lookup(set, &entry[i], &created, &index2[i])!=0);
        }
        assert(created == 0);
        assert(index[i] == index2[i]);
    }    

    // check all have ref 1
    n=0;
    for (i=0;i<set->table_size;i++) {
        uint32_t key = set->table[i];
        if (key != 0) {
            n++;
            assert((key & 0x0000ffff) == 1);
        }
    }
    assert(n == 16);
 /*   
    assert(set->gc_head == set->gc_size-1);
    assert(set->gc_tail == 16);
 */   
    llgcset_gc(set);
    
    // check all have ref 1
    n=0;
    for (i=0;i<set->table_size;i++) {
        uint32_t key = set->table[i];
        if (key != 0) {
            n++;
            assert((key & 0x0000ffff) == 1);
        }
    }
    assert(n == 16);
    
    //assert(set->gc_head == set->gc_tail);

    // deref all 
    for (i=0;i<16;i++) {
        llgcset_deref(set, index[i]);
    }
    
   // assert((set->gc_tail-set->gc_head == 16+1));

//    for (i=0;i<16;i++) {
//        assert(set->gc_list[set->gc_head+1+i] == index[i]);
//    }
    
    llgcset_gc(set);
    
  //  assert(set->gc_head == set->gc_tail);

    // printf("head=%d tail=%d size=%d\n", set->gc_head, set->gc_tail, set->gc_size);
    
    // check 16 tombstones
    n=0;
    for (i=0;i<set->table_size;i++) {
        uint32_t key = set->table[i];
        if (key != 0) {
            assert(key == 0x7fffffff);
            n++;
        }
    }
    assert(n == 16);

    for (i=0;i<16;i++) {
        if (llgcset_lookup(set, &entry[i], &created, &index[i])==0) {
            llgcset_gc(set);
            assert(llgcset_lookup(set, &entry[i], &created, &index[i])!=0);
        }
        assert(created);
    }
    
    // check all have ref 1
    n=0;
    for (i=0;i<set->table_size;i++) {
        uint32_t key = set->table[i];
        if (key != 0 && key != 0x7fffffff) {
            n++;
            assert((key & 0x0000ffff) == 1);
        }
    }
    assert(n == 16);

    // deref all twice
    for (i=0;i<16;i++) {
        llgcset_deref(set, index[i]);
    }
/*
    int j;
    for (j=0;j<set->table_size;j++) {
        printf("[%d]=%x\n", j, set->table[j]);
    }
*/
    // all now have ref 0
    for (i=0;i<31;i++) {
        if (llgcset_lookup(set, &i, &created, NULL)==0) {
            llgcset_gc(set);
            assert(llgcset_lookup(set, &i, &created, NULL)!=0);
        }
        assert(created);
    }

    llgcset_free(set);    

    return 1;
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

	return 0;
}

void test_xor()
{
    BDD a = sylvan_ithvar(1);
    BDD b = sylvan_ithvar(2);
    BDD test = sylvan_xor(a, b);
    BDD test2 = sylvan_xor(a, b); // same as test...
    BDD test3 = sylvan_makenode(1, sylvan_ref(b), sylvan_not(b)); // same as test...
    if (test != test2 || test != test3) {
        sylvan_print(a); sylvan_print(b);
        sylvan_print(test); sylvan_print(test2); sylvan_print(test3);
    }
    assert(test == test2);
    assert(test2 == test3);
    sylvan_deref(test);
    sylvan_deref(test);
    sylvan_deref(test);
    sylvan_deref(a);
    sylvan_deref(b);
}

void test_diff()
{
    BDD a = sylvan_ithvar(1);
    BDD b = sylvan_ithvar(2);
    BDD test = sylvan_diff(a, b);
    sylvan_diff(a, b); // same as test...
    sylvan_deref(test);
    sylvan_deref(test);
    sylvan_deref(a);
    sylvan_deref(b);
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
        t2 = sylvan_ithvar(i);
        if (i>0) assert (sylvan_count_refs()==2);
        else assert(sylvan_count_refs()==1);
        test = sylvan_or(t1, t2);
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
    BDD axorb = sylvan_makenode(1, sylvan_ref(b), sylvan_not(b));
    assert(testEqual(axorb, sylvan_xor(a, b)));
    sylvan_deref(axorb); // result of sylvan_xor
    
    // c or d
    BDD cord = sylvan_makenode(3, sylvan_ref(d), sylvan_true);
    assert(cord == sylvan_or(c, d));
    sylvan_deref(cord); // result of sylvan_or

    BDD t = sylvan_makenode(1, sylvan_false, sylvan_ref(cord));
    assert(t == sylvan_and(a, cord));
    sylvan_deref(t); // result of cord
    sylvan_deref(t); // t

    // (a xor b) and (c or d)
    BDD test = sylvan_makenode(1, sylvan_makenode(2, sylvan_false, sylvan_ref(cord)), sylvan_makenode(2, sylvan_ref(cord), sylvan_false));
    assert(testEqual(test, sylvan_and(axorb, cord)));
    sylvan_deref(test); // result of sylvan_and
    assert(test == sylvan_and(cord, axorb));
    sylvan_deref(test); // result of sylvan_and
    sylvan_deref(test); // test

    // not (A and B)  == not A or not B
    BDD notaxorb = sylvan_not(axorb);

    BDD notcord = sylvan_not(cord);    
    test = sylvan_or(notaxorb, notcord);
    sylvan_deref(notcord);
    
    BDD tmp = sylvan_and(axorb, cord);
    assert(test == sylvan_not(tmp));
    sylvan_deref(test); // result of sylvan_not
    sylvan_deref(tmp);
    sylvan_deref(test); 
    
    // A and not A == false
    assert(sylvan_false == sylvan_and(axorb, notaxorb));

    // A or not A = true
    assert(sylvan_true == sylvan_or(axorb, notaxorb));

    sylvan_deref(notaxorb);
    sylvan_deref(cord);
    
    sylvan_deref(axorb);
    
    assert((tmp = sylvan_and(a, sylvan_true)) == a);
    sylvan_deref(tmp);
    
    assert((tmp = sylvan_or(a, sylvan_true)) == sylvan_true);
    assert((tmp = sylvan_and(a, sylvan_false)) == sylvan_false);
    
    assert (sylvan_or(sylvan_true, sylvan_false) == sylvan_true);

    sylvan_deref(a);
    sylvan_deref(b);
    sylvan_deref(c);
    sylvan_deref(d);
    sylvan_deref(e);
    sylvan_deref(f);
    sylvan_deref(g);
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
    sylvan_deref(aandb); // result of ite

    BDD notaandc = sylvan_and(sylvan_not(a), c);
    sylvan_deref(a); // not a

    // a then b else c == (a and b) or (not a and c)
    BDD t = sylvan_ite(a,b,c);
    assert(t == sylvan_or(aandb, notaandc));
    sylvan_deref(t);
    sylvan_deref(t);

    // not d then (a and b) else (not a and c) ==
    // a then (b and not d) else (c and d)
    t = sylvan_ite(sylvan_not(d), aandb, notaandc);
    sylvan_deref(d); // not d
    BDD candd = sylvan_and(c, d);
    BDD bandnotd = sylvan_and(b, sylvan_not(d));
    sylvan_deref(d); // not d
    assert(t == sylvan_ite(a, bandnotd, candd));
    sylvan_deref(candd);
    sylvan_deref(bandnotd);
    sylvan_deref(t);
    sylvan_deref(t);

    BDD etheng = sylvan_imp(e, g);
    BDD test = sylvan_ite(etheng, sylvan_true, b);
    t = sylvan_ite(b, sylvan_false, etheng);
    assert(t == sylvan_and(test, sylvan_not(b)));
    sylvan_deref(b); // not b
    sylvan_deref(t);
    sylvan_deref(t);

    sylvan_deref(test);
    sylvan_deref(etheng);
    sylvan_deref(notaandc);
    sylvan_deref(aandb); 
    
    sylvan_deref(a);
    sylvan_deref(b);
    sylvan_deref(c);
    sylvan_deref(d);
    sylvan_deref(e);
    sylvan_deref(f);
    sylvan_deref(g);
}

BDD knownresult;

#define REFSTACK(size) BDD refstack[size]; int refs=0;
#define REF(a) ({register BDD res = (a);refstack[refs++] = res;res;})
#define UNREF while (refs) { sylvan_deref(refstack[--refs]); }

void test_modelcheck()
{
    REFSTACK(32)
    
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
                         REF(sylvan_or(b, 
                         REF(sylvan_or(c, d)))));
    BDD xx = sylvan_or(aa,
                         REF(sylvan_or(bb, 
                         REF(sylvan_or(cc, dd)))));

    BDD universe = sylvan_or(x, xx);

    BDD a_same = sylvan_biimp(a, aa); // a = a'
    BDD b_same = sylvan_biimp(b, bb); // b = b'
    BDD c_same = sylvan_biimp(c, cc); // c = c'
    BDD d_same = sylvan_biimp(d, dd); // d = d'

    BDD a_diff = sylvan_biimp(REF(sylvan_not(a)), aa); // a = ~a'
    BDD b_diff = sylvan_biimp(REF(sylvan_not(b)), bb); // b = ~b'
    BDD c_diff = sylvan_biimp(REF(sylvan_not(c)), cc); // c = ~c'
    BDD d_diff = sylvan_biimp(REF(sylvan_not(d)), dd); // d = ~d'
    
    UNREF

    // a = ~a' and rest stay same
    BDD change_a = sylvan_and(a_diff, REF(sylvan_and(b_same,REF(sylvan_and(c_same,d_same)))));
    // b = ~b' and rest stay same
    BDD change_b = sylvan_and(a_same, REF(sylvan_and(b_diff,REF(sylvan_and(c_same,d_same)))));
    // c = ~c' and rest stay same
    BDD change_c = sylvan_and(a_same, REF(sylvan_and(b_same,REF(sylvan_and(c_diff,d_same)))));
    // d = ~d' and rest stay same
    BDD change_d = sylvan_and(a_same, REF(sylvan_and(b_same,REF(sylvan_and(c_same,d_diff)))));
    
    UNREF

    sylvan_deref(a_same);
    sylvan_deref(b_same);
    sylvan_deref(c_same);
    sylvan_deref(d_same);
    
    sylvan_deref(a_diff);
    sylvan_deref(b_diff);
    sylvan_deref(c_diff);
    sylvan_deref(d_diff);

    BDD r = sylvan_or(change_a, REF(sylvan_or(change_b, REF(sylvan_or(change_c, change_d)))));
    UNREF
    
    sylvan_deref(change_a);
    sylvan_deref(change_b);
    sylvan_deref(change_c);
    sylvan_deref(change_d);
    
    // sylvan_print(r);

    // Relation r:
    // (0,x,x,x) <=> (1,x,x,x)
    // (x,0,x,x) <=> (x,1,x,x)
    // (x,x,0,x) <=> (x,x,1,x)
    // (x,x,x,0) <=> (x,x,x,1)

    // start: (0,0,0,0)
    BDD start = sylvan_and(REF(sylvan_not(a)), 
              REF(sylvan_and(REF(sylvan_not(b)), REF(sylvan_and(REF(sylvan_not(c)), REF(sylvan_not(d)))))));

    UNREF

    sylvan_deref(a);
    sylvan_deref(b);
    sylvan_deref(c);
    sylvan_deref(d);
    
    sylvan_deref(aa);
    sylvan_deref(bb);
    sylvan_deref(cc);
    sylvan_deref(dd);

    BDD visited = start, prev = sylvan_invalid;

    /* Check if RelProdS gives the same result as RelProd and Substitute */
    assert(REF(sylvan_relprods(visited, r, sylvan_true)) ==
           REF(sylvan_substitute(REF(sylvan_relprod(visited, r, x)), xx)));
    UNREF

    /* Expected first: (0,0,0,0), (1,0,0,0), (0,1,0,0), (0,0,1,0), (0,0,0,0) */

    do {
        //printf("Visited: \n");
        //sylvan_print(visited);
        if (prev != sylvan_invalid) sylvan_deref(prev);
        prev = visited;
        BDD next = sylvan_relprods(visited, r, sylvan_true);
        visited = sylvan_or(visited, next);
        // check that the "visited" set is a subset of all parents of next.
        BDD check = sylvan_relprods_reversed(next, r, sylvan_true);
        assert (sylvan_diff(prev, check) == sylvan_false); // prev \ check = 0
        sylvan_deref(check);
        sylvan_deref(next);
    } while (visited != prev);
    sylvan_deref(x);
    sylvan_deref(xx);
    sylvan_deref(universe);

    sylvan_deref(visited);
    sylvan_deref(prev);
    sylvan_deref(r);
}

void test_exists_forall()
{
    REFSTACK(32)

    BDD a,b,c,d,e,f,g,h;

    a = sylvan_ithvar(1);
    b = sylvan_ithvar(2);
    c = sylvan_ithvar(3);
    d = sylvan_ithvar(4);
    e = sylvan_ithvar(5);
    f = sylvan_ithvar(6);
    g = sylvan_ithvar(7);
    h = sylvan_ithvar(8);

    REF(sylvan_or(REF(sylvan_not(b)), REF(sylvan_not(c))));

    BDD test = sylvan_ite(a, REF(sylvan_and(b, d)), REF(sylvan_or(REF(sylvan_not(b)), REF(sylvan_not(c)))));

    BDD axorb = sylvan_xor(a, b);
    BDD dthenf = sylvan_imp(d, f);
    BDD cxorg = sylvan_xor(c, g);

    REF(sylvan_exists(REF(sylvan_ite(dthenf, axorb, cxorg)), d));
    REF(sylvan_forall(REF(sylvan_ite(dthenf, axorb, cxorg)), d));
    REF(sylvan_exists(axorb, sylvan_false));
    REF(sylvan_exists(axorb, sylvan_false));
    REF(sylvan_exists(dthenf, a));
    REF(sylvan_exists(dthenf, d));
    REF(sylvan_exists(dthenf, f));
    REF(sylvan_exists(sylvan_true, sylvan_false));

    UNREF
    
    sylvan_deref(axorb);
    sylvan_deref(test);
    sylvan_deref(dthenf);
    sylvan_deref(cxorg);
    
    sylvan_deref(a);
    sylvan_deref(b);
    sylvan_deref(c);
    sylvan_deref(d);
    sylvan_deref(e);
    sylvan_deref(f);
    sylvan_deref(g);
    sylvan_deref(h);
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
    int n=0;
    int failure=0;
    int k;
/*#if CACHE    
    llgcset_t cache = __sylvan_get_internal_cache();
    for (k=0;k<cache->size;k++) {
        if (cache->table[k] == 0) continue;
        if (cache->table[k] == 0x7fffffff) continue;
        // if ((cache->table[k] & 0x0000ffff) == 0x0000fffe) continue; // !
        if (!failure) printf(LRED "\nFailure!\n");
        failure++;
        // Find value...
        BDDOP op = *(BDDOP *)(&cache->data[k*32]);
        printf(NC "Cache entry still being referenced: %08X (%u) (%d)\n", cache->table[k], k, op);
        n++;
    }
    if (n>0) {
        printf(LRED "%d ref'd cache entries" NC "!\n", n);
        fflush(stdout);
        assert(0);
    }
    
    n=0; // superfluous
#endif    */
    llgcset_t set = __sylvan_get_internal_data();

    // check empty gc queue
    // assert(set->gc_head == set->gc_tail);
    
    for (k=0;k<set->table_size;k++) {
        if (set->table[k] == 0) continue;
        if (set->table[k] == 0x7fffffff) continue;
        //if ((set->table[k] & 0x0000ffff) == 0x0000fffe) continue; // If we allow this, we need to modify more!
        if (!failure) printf(LRED "\nFailure!\n" NC "Cache is clean, but BDD table is still in use!\n");
        failure++;
        printf("BDD key being referenced: %08X\n", set->table[k]);
        sylvan_print(k);
        n++;
    }
    
    if (n>0) {
        printf(LRED "%d dangling ref's" NC "!\n", n);
        fflush(stdout);
        assert(0);
    }
}

void runtests(int threads)
{
    printf(BOLD "Testing LL Cache\n" NC);
    printf("Running singlethreaded test... ");
    fflush(stdout);
    test_llcache();
    printf(LGREEN "success" NC "!\n");


    printf(BOLD "Testing LL GC Set\n" NC);
    printf("Running singlethreaded test... ");
    fflush(stdout);
    test_llgcset();
    printf(LGREEN "success" NC "!\n");
    printf("Running multithreaded test... ");
    fflush(stdout);
    if (1/*skip*/) {
        printf("... " LMAGENTA "skipped" NC ".\n");
    }
    else if (test_llgcset2()) {
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
        sylvan_init(12, 12, 1);
        
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

    sylvan_init(16, 10, 3); // minumum: X, 7, 4, 4 ??
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

    runtests(threads);
    printf(NC);
    exit(0);
}
