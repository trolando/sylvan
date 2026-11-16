#include <sylvan.h>
#include "test_assert.h"

using namespace sylvan;

static int accept_true(MTBDD leaf, void *context)
{
    return leaf == *static_cast<MTBDD*>(context);
}

/* Exercise the public wrappers from a non-Lace thread, including C++ names,
 * copied options and all four destruction functions. */
int main(int argc, char **argv)
{
    lace_start(argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 1, 0);
    sylvan_set_sizes(1<<10, 1<<16, 1<<10, 1<<14);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_zdd();
    sylvan_init_ldd();
    sylvan_iterator *bdd, *mtbdd, *zdd, *ldd;
    MTBDD accepted = mtbdd_true;
    mtbdd_iterator_options options = {SYLVAN_ITERATOR_CUBES, accept_true, &accepted};
    test_assert(sylvan_iterator_create(sylvan_true, sylvan_true, SYLVAN_ITERATOR_MINTERMS, &bdd));
    test_assert(mtbdd_iterator_create(mtbdd_true, mtbdd_true, &options, &mtbdd));
    test_assert(zdd_iterator_create(zdd_true, zdd_true, &zdd));
    test_assert(lddmc_iterator_create(lddmc_true, &ldd));
    sylvan_gc();
    int has_item;
    MTBDD leaf;
    test_assert(sylvan_iterator_next(bdd, nullptr, 0, &has_item) && has_item);
    test_assert(mtbdd_iterator_next(mtbdd, nullptr, 0, &leaf, &has_item) && has_item && leaf == accepted);
    test_assert(zdd_iterator_next(zdd, nullptr, 0, &has_item) && has_item);
    test_assert(lddmc_iterator_next(ldd, nullptr, 0, &has_item) && has_item);
    sylvan_gc();
    test_assert(sylvan_iterator_next(bdd, nullptr, 0, &has_item) && !has_item);
    test_assert(mtbdd_iterator_next(mtbdd, nullptr, 0, &leaf, &has_item) && !has_item);
    test_assert(zdd_iterator_next(zdd, nullptr, 0, &has_item) && !has_item);
    test_assert(lddmc_iterator_next(ldd, nullptr, 0, &has_item) && !has_item);
    sylvan_iterator_destroy(bdd);
    mtbdd_iterator_destroy(mtbdd);
    zdd_iterator_destroy(zdd);
    lddmc_iterator_destroy(ldd);
    sylvan_quit();
    lace_stop();
    return 0;
}
