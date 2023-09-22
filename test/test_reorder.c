#include <stdio.h>
#include <locale.h>
#include <sys/time.h>

#include <sylvan.h>
#include <sylvan_int.h>
#include "test_assert.h"

/* Obtain current wallclock time */
static double
wctime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

static double t_start;
#define INFO(s, ...) fprintf(stdout, "\r[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(s, ...) { fprintf(stderr, "\r[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__); exit(-1); }

void _sylvan_start();

void _sylvan_quit();

#define create_example_bdd(is_optimal) RUN(create_example_bdd, is_optimal)
TASK_1(BDD, create_example_bdd, size_t, is_optimal)
{
//    BDD is from the paper:
//    Randal E. Bryant Graph-Based Algorithms for Boolean Function Manipulation,
//    IEEE Transactions on Computers, 1986 http://www.cs.cmu.edu/~bryant/pubdir/ieeetc86.pdf

    // the variable indexing is relative to the current level
    BDD v0 = sylvan_ithvar(0);
    BDD v1 = sylvan_ithvar(1);
    BDD v2 = sylvan_ithvar(2);
    BDD v3 = sylvan_ithvar(3);
    BDD v4 = sylvan_ithvar(4);
    BDD v5 = sylvan_ithvar(5);

    if (is_optimal) {
        // optimal order 0, 1, 2, 3, 4, 5
        // minimum 8 nodes including 2 terminal nodes
        return sylvan_or(sylvan_and(v0, v1), sylvan_or(sylvan_and(v2, v3), sylvan_and(v4, v5)));
    } else {
        // not optimal order 0, 3, 1, 4, 2, 5
        // minimum 16 nodes including 2 terminal nodes
        return sylvan_or(sylvan_and(v0, v3), sylvan_or(sylvan_and(v1, v4), sylvan_and(v2, v5)));
    }
}

#define create_example_map(is_optimal) RUN(create_example_map, is_optimal)
TASK_1(BDDMAP, create_example_map, size_t, is_optimal)
{
    BDDMAP map = sylvan_map_empty();
    BDD bdd = create_example_bdd(is_optimal);
    map = sylvan_map_add(map, 0, bdd);
    return map;
}

TASK_0(int, test_varswap)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    /* test ithvar, switch 6 and 7 */
    BDD one = sylvan_ithvar(6);
    BDD two = sylvan_ithvar(7);

    test_assert(levels_level_to_order(&reorder_db->levels, 6) == 6);
    test_assert(sylvan_level_to_order(7) == 7);
    test_assert(sylvan_order_to_level(6) == 6);
    test_assert(sylvan_order_to_level(7) == 7);
    test_assert(one == sylvan_ithvar(6));
    test_assert(two == sylvan_ithvar(7));
    test_assert(mtbdd_getvar(one) == 6);
    test_assert(mtbdd_getvar(two) == 7);

    sylvan_pre_reorder(SYLVAN_REORDER_SIFT);

    test_assert(sylvan_varswap(6) == SYLVAN_REORDER_SUCCESS);

    sylvan_post_reorder();

    test_assert(sylvan_level_to_order(7) == 6);
    test_assert(sylvan_level_to_order(6) == 7);
    test_assert(sylvan_order_to_level(7) == 6);
    test_assert(sylvan_order_to_level(6) == 7);
    test_assert(mtbdd_getvar(one) == 7);
    test_assert(mtbdd_getvar(two) == 6);
    test_assert(one == sylvan_ithvar(7));
    test_assert(two == sylvan_ithvar(6));

    return 0;
}

TASK_0(int, test_varswap_down)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    MTBDD zero = sylvan_ithvar(0);
    MTBDD one = sylvan_ithvar(1);
    MTBDD two = sylvan_ithvar(2);
    MTBDD three = sylvan_ithvar(3);

    /* swap down manually var 0 to level 3 */
    test_assert(sylvan_level_to_order(0) == 0);
    test_assert(sylvan_level_to_order(1) == 1);
    test_assert(sylvan_level_to_order(2) == 2);
    test_assert(sylvan_level_to_order(3) == 3);

    test_assert(sylvan_order_to_level(0) == 0);
    test_assert(sylvan_order_to_level(1) == 1);
    test_assert(sylvan_order_to_level(2) == 2);
    test_assert(sylvan_order_to_level(3) == 3);

    test_assert(zero == sylvan_ithvar(0));
    test_assert(one == sylvan_ithvar(1));
    test_assert(two == sylvan_ithvar(2));
    test_assert(three == sylvan_ithvar(3));

    sylvan_pre_reorder(SYLVAN_REORDER_SIFT);

    // (0), 1, 2, 3
    test_assert(sylvan_varswap(0) == SYLVAN_REORDER_SUCCESS);
    test_assert(sylvan_varswap(1) == SYLVAN_REORDER_SUCCESS);
    test_assert(sylvan_varswap(2) == SYLVAN_REORDER_SUCCESS);
    // 1, 2, 3, (0)

    sylvan_post_reorder();

    test_assert(sylvan_level_to_order(0) == 1);
    test_assert(sylvan_level_to_order(1) == 2);
    test_assert(sylvan_level_to_order(2) == 3);
    test_assert(sylvan_level_to_order(3) == 0);

    test_assert(sylvan_order_to_level(1) == 0);
    test_assert(sylvan_order_to_level(2) == 1);
    test_assert(sylvan_order_to_level(3) == 2);
    test_assert(sylvan_order_to_level(0) == 3);

    test_assert(zero == sylvan_ithvar(3));
    test_assert(one == sylvan_ithvar(0));
    test_assert(two == sylvan_ithvar(1));
    test_assert(three == sylvan_ithvar(2));

    return 0;
}

TASK_0(int, test_varswap_up)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();


    MTBDD zero = sylvan_ithvar(0);
    MTBDD one = sylvan_ithvar(1);
    MTBDD two = sylvan_ithvar(2);
    MTBDD three = sylvan_ithvar(3);

    /* swap up manually var 3 to level 0 */
    test_assert(zero == sylvan_ithvar(0));
    test_assert(one == sylvan_ithvar(1));
    test_assert(two == sylvan_ithvar(2));
    test_assert(three == sylvan_ithvar(3));

    sylvan_pre_reorder(SYLVAN_REORDER_SIFT);

    // 0, 1, 2, (3)
    test_assert(sylvan_varswap(2) == SYLVAN_REORDER_SUCCESS);
    test_assert(sylvan_varswap(1) == SYLVAN_REORDER_SUCCESS);
    test_assert(sylvan_varswap(0) == SYLVAN_REORDER_SUCCESS);
    // (3), 0, 1, 2

    sylvan_post_reorder();

    test_assert(sylvan_level_to_order(0) == 3);
    test_assert(sylvan_level_to_order(1) == 0);
    test_assert(sylvan_level_to_order(2) == 1);
    test_assert(sylvan_level_to_order(3) == 2);

    test_assert(sylvan_order_to_level(3) == 0);
    test_assert(sylvan_order_to_level(0) == 1);
    test_assert(sylvan_order_to_level(1) == 2);
    test_assert(sylvan_order_to_level(2) == 3);

    test_assert(zero == sylvan_ithvar(1));
    test_assert(one == sylvan_ithvar(2));
    test_assert(two == sylvan_ithvar(3));
    test_assert(three == sylvan_ithvar(0));

    return 0;
}

TASK_0(int, test_sift_down)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    MTBDD zero = sylvan_ithvar(0);
    MTBDD one = sylvan_ithvar(1);
    MTBDD two = sylvan_ithvar(2);
    MTBDD three = sylvan_ithvar(3);

    // we need to make relation between the variables otherwise the lower bounds will make sifting down skip the variables swaps
    MTBDD bdd = sylvan_and(sylvan_and(sylvan_and(zero, one), two), three);
    mtbdd_protect(&bdd);

    /* swap down manually var 0 to level 3 */
    test_assert(sylvan_level_to_order(0) == 0);
    test_assert(sylvan_level_to_order(1) == 1);
    test_assert(sylvan_level_to_order(2) == 2);
    test_assert(sylvan_level_to_order(3) == 3);

    test_assert(sylvan_order_to_level(0) == 0);
    test_assert(sylvan_order_to_level(1) == 1);
    test_assert(sylvan_order_to_level(2) == 2);
    test_assert(sylvan_order_to_level(3) == 3);

    test_assert(zero == sylvan_ithvar(0));
    test_assert(one == sylvan_ithvar(1));
    test_assert(two == sylvan_ithvar(2));
    test_assert(three == sylvan_ithvar(3));

    sifting_state_t state;
    state.low = 0;
    state.high = 3;

    state.size = 0;
    state.pos = 0;

    state.best_size = 770;
    state.best_pos = 3;

    sylvan_pre_reorder(SYLVAN_REORDER_BOUNDED_SIFT);

    // (0), 1, 2, 3
    test_assert(sylvan_siftdown(&state) == SYLVAN_REORDER_SUCCESS);
    // 1, 2, 3, (0)

    sylvan_post_reorder();

    test_assert(sylvan_level_to_order(0) == 1);
    test_assert(sylvan_level_to_order(1) == 2);
    test_assert(sylvan_level_to_order(2) == 3);
    test_assert(sylvan_level_to_order(3) == 0);

    test_assert(sylvan_order_to_level(1) == 0);
    test_assert(sylvan_order_to_level(2) == 1);
    test_assert(sylvan_order_to_level(3) == 2);
    test_assert(sylvan_order_to_level(0) == 3);

    return 0;
}

TASK_0(int, test_sift_up)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    MTBDD zero = sylvan_ithvar(0);
    MTBDD one = sylvan_ithvar(1);
    MTBDD two = sylvan_ithvar(2);
    MTBDD three = sylvan_ithvar(3);

    // we need to make relation between the variables otherwise the lower bounds will make sifting skip the variables swaps
    MTBDD bdd = sylvan_and(sylvan_and(sylvan_and(zero, one), two), three);
    mtbdd_protect(&bdd);

    /* swap up manually var 3 to level 0 */
    test_assert(zero == sylvan_ithvar(0));
    test_assert(one == sylvan_ithvar(1));
    test_assert(two == sylvan_ithvar(2));
    test_assert(three == sylvan_ithvar(3));

    sifting_state_t state;
    state.low = 0;
    state.high = 1;

    state.size = 90;
    state.best_size = 0;

    state.pos = 1;
    state.best_pos = 0;

    sylvan_pre_reorder(SYLVAN_REORDER_BOUNDED_SIFT);

    // 0, (1), 2, 3
    test_assert(sylvan_siftup(&state) == SYLVAN_REORDER_SUCCESS);
    // (1), 0, 2, 3

    sylvan_post_reorder();

    test_assert(sylvan_level_to_order(0) == 1);
    test_assert(sylvan_level_to_order(1) == 0);
    test_assert(sylvan_level_to_order(2) == 2);
    test_assert(sylvan_level_to_order(3) == 3);

    test_assert(sylvan_order_to_level(1) == 0);
    test_assert(sylvan_order_to_level(0) == 1);
    test_assert(sylvan_order_to_level(2) == 2);
    test_assert(sylvan_order_to_level(3) == 3);

    return 0;
}

TASK_0(int, test_sift_back)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    MTBDD zero = sylvan_ithvar(0);
    MTBDD one = sylvan_ithvar(1);
    MTBDD two = sylvan_ithvar(2);
    MTBDD three = sylvan_ithvar(3);

    /* swap up manually var 3 to level 0 */
    test_assert(zero == sylvan_ithvar(0));
    test_assert(one == sylvan_ithvar(1));
    test_assert(two == sylvan_ithvar(2));
    test_assert(three == sylvan_ithvar(3));

    sifting_state_t state;
    state.low = 0;
    state.high = 3;

    state.size = 999;
    state.pos = 3;

    state.best_size = 1;
    state.best_pos = 0;

    sylvan_pre_reorder(SYLVAN_REORDER_BOUNDED_SIFT);

    // 0, 1, 2, (3)
    test_assert(sylvan_siftback(&state) == SYLVAN_REORDER_SUCCESS);
    // (3), 0, 1, 2

    sylvan_post_reorder();

    test_assert(sylvan_level_to_order(0) == 3);
    test_assert(sylvan_level_to_order(1) == 0);
    test_assert(sylvan_level_to_order(2) == 1);
    test_assert(sylvan_level_to_order(3) == 2);

    test_assert(sylvan_order_to_level(3) == 0);
    test_assert(sylvan_order_to_level(0) == 1);
    test_assert(sylvan_order_to_level(1) == 2);
    test_assert(sylvan_order_to_level(2) == 3);

    test_assert(zero == sylvan_ithvar(1));
    test_assert(one == sylvan_ithvar(2));
    test_assert(two == sylvan_ithvar(3));
    test_assert(three == sylvan_ithvar(0));

    state.size = 999;
    state.pos = 0;

    state.best_size = 1;
    state.best_pos = 4;

    sylvan_pre_reorder(SYLVAN_REORDER_BOUNDED_SIFT);

    // (3), 0, 1, 2
    test_assert(sylvan_siftback(&state) == SYLVAN_REORDER_SUCCESS);
    // 0, 1, 2, (3)

    sylvan_post_reorder();

    test_assert(zero == sylvan_ithvar(0));
    test_assert(one == sylvan_ithvar(1));
    test_assert(two == sylvan_ithvar(2));
    test_assert(three == sylvan_ithvar(3));

    return 0;
}

TASK_0(int, test_reorder_perm)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    MTBDD zero = sylvan_ithvar(0);
    MTBDD one = sylvan_ithvar(1);
    MTBDD two = sylvan_ithvar(2);
    MTBDD three = sylvan_ithvar(3);

    /* reorder the variables according to the variable permutation*/
    test_assert(zero == sylvan_ithvar(0));
    test_assert(one == sylvan_ithvar(1));
    test_assert(two == sylvan_ithvar(2));
    test_assert(three == sylvan_ithvar(3));

    uint32_t perm[4] = {3, 0, 2, 1};

    sylvan_pre_reorder(SYLVAN_REORDER_BOUNDED_SIFT);

    test_assert(sylvan_reorder_perm(perm) == SYLVAN_REORDER_SUCCESS);

    sylvan_post_reorder();

    test_assert(sylvan_level_to_order(0) == perm[0]);
    test_assert(sylvan_level_to_order(1) == perm[1]);
    test_assert(sylvan_level_to_order(2) == perm[2]);
    test_assert(sylvan_level_to_order(3) == perm[3]);

    test_assert(sylvan_order_to_level(perm[0]) == 0);
    test_assert(sylvan_order_to_level(perm[1]) == 1);
    test_assert(sylvan_order_to_level(perm[2]) == 2);
    test_assert(sylvan_order_to_level(perm[3]) == 3);

    test_assert(zero == sylvan_ithvar(1));
    test_assert(one == sylvan_ithvar(3));
    test_assert(two == sylvan_ithvar(2));
    test_assert(three == sylvan_ithvar(0));

    return 0;
}

TASK_0(int, test_reorder)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    BDD bdd = create_example_bdd(0);
    sylvan_protect(&bdd);

    size_t not_optimal_order_size = sylvan_nodecount(bdd);

    sylvan_reduce_heap(SYLVAN_REORDER_SIFT);

    size_t not_optimal_order_reordered_size = sylvan_nodecount(bdd);

    test_assert(not_optimal_order_reordered_size < not_optimal_order_size);

    uint32_t perm[6] = {0, 1, 2, 3, 4, 5};
    int identity = 1;
    // check if the new order is identity with the old order
    for (size_t i = 0; i < reorder_db->levels.count; i++) {
        if (sylvan_order_to_level(i) != perm[i]) {
            identity = 0;
            break;
        }
    }

//     if we gave it not optimal ordering then the new ordering should not be identity
    test_assert(identity == 0);

    test_assert(sylvan_reorder_perm(perm) == SYLVAN_REORDER_SUCCESS);

    size_t not_optimal_size_again = sylvan_nodecount(bdd);
    test_assert(not_optimal_order_size == not_optimal_size_again);

    for (size_t i = 0; i < reorder_db->levels.count; i++) {
        test_assert(sylvan_order_to_level(i) == perm[i]);
    }

    sylvan_unprotect(&bdd);

    return 0;
}

TASK_0(int, test_map_reorder)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    BDDMAP map = create_example_map(0);
    sylvan_protect(&map);

    size_t size_before = sylvan_nodecount(map);
    sylvan_reduce_heap(SYLVAN_REORDER_SIFT);
    size_t size_after = sylvan_nodecount(map);

    test_assert(size_after < size_before);
    sylvan_unprotect(&map);

    return 0;
}

TASK_0(int, test_interact)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    MTBDD bdd2 = create_example_bdd(0);
    sylvan_ref(bdd2);

    BDD bdd1 = sylvan_or(sylvan_ithvar(6), sylvan_ithvar(7));
    sylvan_ref(bdd1);

    sylvan_pre_reorder(SYLVAN_REORDER_BOUNDED_SIFT);

    assert(interact_test(&reorder_db->matrix, 6, 7));
    assert(interact_test(&reorder_db->matrix, 7, 6));

    for (size_t i = 0; i < reorder_db->levels.count; ++i) {
        for (size_t j = i + 1; j < reorder_db->levels.count - 2; ++j) {
            // test interaction of variables belonging to bdd2
            assert(interact_test(&reorder_db->matrix, i, j));
            assert(interact_test(&reorder_db->matrix, j, i));
            // test interaction of variables not belonging to bdd2
            assert(!interact_test(&reorder_db->matrix, 6, j));
            assert(!interact_test(&reorder_db->matrix, 6, i));
            assert(!interact_test(&reorder_db->matrix, 7, j));
            assert(!interact_test(&reorder_db->matrix, 7, i));
        }
    }

    sylvan_post_reorder();
    interact_deinit(&reorder_db->matrix);

    sylvan_deref(bdd1);
    sylvan_deref(bdd2);
    return 0;
}

TASK_0(int, test_ref_nodes)
{
    // we need to delete all data so we reset sylvan
    _sylvan_quit();
    _sylvan_start();

    MTBDD bdd = create_example_bdd(1);
    sylvan_ref(bdd);

    MTBDD zero = bdd;
    MTBDD one = mtbdd_gethigh(zero);
    MTBDD two = mtbdd_getlow(zero);
    MTBDD three = mtbdd_gethigh(two);
    MTBDD four = mtbdd_getlow(two);
    MTBDD five = mtbdd_gethigh(four);

    sylvan_pre_reorder(SYLVAN_REORDER_BOUNDED_SIFT);

    test_assert(0 == mrc_ref_nodes_get(&reorder_db->mrc, zero));
    test_assert(1 == mrc_ref_nodes_get(&reorder_db->mrc, one));
    test_assert(2 == mrc_ref_nodes_get(&reorder_db->mrc, two));
    test_assert(1 == mrc_ref_nodes_get(&reorder_db->mrc, three));
    test_assert(2 == mrc_ref_nodes_get(&reorder_db->mrc, four));
    test_assert(1 == mrc_ref_nodes_get(&reorder_db->mrc, five));

    sylvan_post_reorder();

    sylvan_deref(bdd);

    return 0;
}

TASK_1(int, runtests, size_t, ntests)
{
    printf("testing varswap...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_varswap)) return 1;
    printf("testing varswap_down...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_varswap_down)) return 1;
    printf("testing varswap_up...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_varswap_up)) return 1;
    printf("testing sift_down...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_sift_down)) return 1;
    printf("testing sift_up...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_sift_up)) return 1;
    printf("testing sift_back...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_sift_back)) return 1;
    printf("testing reorder_perm...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_reorder_perm)) return 1;
    printf("testing reorder...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_reorder)) return 1;
    printf("testing map_reorder...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_map_reorder)) return 1;
    printf("testing interact...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_interact)) return 1;
    printf("testing ref_nodes...\n");
    for (size_t j = 0; j < ntests; j++) if (RUN(test_ref_nodes)) return 1;
    return 0;
}

static int terminate_reordering = 0;

VOID_TASK_0(reordering_start)
{
#ifndef NDEBUG
    size_t size = llmsset_count_marked(nodes);
    printf("RE: start: %zu size\n", size);
#endif
}

VOID_TASK_0(reordering_progress)
{
#ifndef NDEBUG
    size_t size = llmsset_count_marked(nodes);
    printf("RE: progress: %zu size\n", size);
#endif
}

VOID_TASK_0(reordering_end)
{
#ifndef NDEBUG
    size_t size = llmsset_count_marked(nodes);
    printf("RE: end: %zu size\n", size);
#endif
}

int should_reordering_terminate()
{
    return terminate_reordering;
}

void _sylvan_start()
{
    sylvan_set_limits(1LL << 23, 1, 0);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_reorder();
    sylvan_gc_enable();
    sylvan_set_reorder_print(false);
    sylvan_set_reorder_nodes_threshold(1); // keep it 1, otherwise we skip levels which will fail the test expectations
}

void _sylvan_quit()
{
    sylvan_quit();
}

int main()
{
    setlocale(LC_NUMERIC, "en_US.utf-8");
    t_start = wctime();

    lace_start(1, 0);

    _sylvan_start();

    sylvan_re_hook_prere(TASK(reordering_start));
    sylvan_re_hook_postre(TASK(reordering_end));
    sylvan_re_hook_progre(TASK(reordering_progress));
    sylvan_re_hook_termre(should_reordering_terminate);

    size_t ntests = 1;

    int res = RUN(runtests, ntests);

    sylvan_stats_report(stdout);

    _sylvan_quit();
    lace_stop();

    return res;
}
