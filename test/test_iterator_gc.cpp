#include <vector>

#include <sylvan_int.h>
#include <sylvan_refs.h>

using namespace sylvan;

extern "C" {
extern refs_table_t mtbdd_protected, zdd_protected, lddmc_protected;
void test_iterator_fail_after(long count);
}

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); exit(1); \
} } while (0)

static constexpr size_t width = 80;
static constexpr size_t batch = 256;
static refs_table_t *roots;
static size_t allocation_root_count;
static unsigned family;
static uint64_t diagram, domain, invalid;

extern "C" void test_iterator_allocation(void)
{
    /* Validation and every storage allocation must finish before protection. */
    CHECK(protect_count(roots) == allocation_root_count);
}

static int create_iterator(uint64_t dd, uint64_t vars, sylvan_iterator **result)
{
    allocation_root_count = protect_count(roots);
    mtbdd_iterator_options options = {SYLVAN_ITERATOR_MINTERMS, nullptr, nullptr};
    if (family == 0) return sylvan_iterator_create(dd, vars, SYLVAN_ITERATOR_MINTERMS, result);
    if (family == 1) return mtbdd_iterator_create(dd, vars, &options, result);
    if (family == 2) return zdd_iterator_create(dd, vars, result);
    return lddmc_iterator_create(dd, result);
}

static void destroy_iterator(sylvan_iterator *iterator)
{
    void (*destroy[])(sylvan_iterator*) = {
        sylvan_iterator_destroy, mtbdd_iterator_destroy,
        zdd_iterator_destroy, lddmc_iterator_destroy
    };
    destroy[family](iterator);
}

VOID_TASK_0(build_inputs)
{
    uint32_t levels[width], tuple[width];
    for (size_t i = 0; i < width; i++) {
        levels[i] = static_cast<uint32_t>(i);
        tuple[i] = static_cast<uint32_t>(i + 3);
    }
    if (family < 2) {
        domain = mtbdd_set_fromarray(levels, width);
        diagram = family == 0 ? sylvan_true : mtbdd_int64(42);
        for (size_t i = width; i-- > 0;) diagram = mtbdd_makenode(levels[i], mtbdd_false, diagram);
    } else if (family == 2) {
        domain = zdd_set_from_array(levels, width);
        diagram = domain;
    } else {
        diagram = lddmc_cube(tuple, width);
        invalid = lddmc_make_copynode(lddmc_true, lddmc_false);
        invalid = lddmc_makenode(2, invalid, lddmc_false);
        invalid = lddmc_makenode(1, lddmc_true, invalid);
    }
}

static void check_item(sylvan_iterator *iterator, unsigned item)
{
    uint8_t values[width];
    uint32_t tuple[width];
    MTBDD leaf;
    int has_item;
    if (family == 0) CHECK(sylvan_iterator_next(iterator, values, width, &has_item));
    else if (family == 1) {
        CHECK(mtbdd_iterator_next(iterator, values, width, &leaf, &has_item));
        if (has_item) CHECK(mtbdd_getint64(leaf) == 42);
    } else if (family == 2) CHECK(zdd_iterator_next(iterator, values, width, &has_item));
    else CHECK(lddmc_iterator_next(iterator, tuple, width, &has_item));
    CHECK(has_item == (item == 0 || family == 2));
    if (!has_item) return;
    for (size_t i = 0; i < width; i++) {
        if (family == 3) CHECK(tuple[i] == i + 3);
        else CHECK(values[i] == (family == 2 ? (item == 1 && i == width - 1) : 1));
    }
}

int main(int argc, char **argv)
{
    lace_start(argc > 1 ? static_cast<unsigned>(atoi(argv[1])) : 1, 0);
    sylvan_set_sizes(1<<10, 1<<16, 1<<10, 1<<14);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_zdd();
    sylvan_init_ldd();

    for (family = 0; family < 4; family++) {
        roots = family < 2 ? &mtbdd_protected :
            family == 2 ? &zdd_protected : &lddmc_protected;
        CHECK(protect_count(roots) == 0);
        protect_free(roots);
        protect_create(roots, 32);
        diagram = domain = invalid = 0;
        if (family < 2) { mtbdd_protect(&diagram); mtbdd_protect(&domain); }
        else if (family == 2) { zdd_protect(&diagram); zdd_protect(&domain); }
        else { lddmc_protect(&diagram); lddmc_protect(&invalid); }
        RUN(build_inputs);

        sylvan_iterator *first;
        CHECK(create_iterator(diagram, domain, &first));
        std::vector<sylvan_iterator*> iterators{first};
        const size_t root_count = protect_count(roots);
        sylvan_iterator *untouched = first;
        /* Missing support, or a copy node in a later LDD branch, fails after
         * allocating and walking the graph without publishing any roots. */
        CHECK(!create_iterator(family == 3 ? invalid : diagram,
                               family == 2 ? zdd_true : sylvan_true, &untouched));
        CHECK(untouched == first && protect_count(roots) == root_count);

        /* Fail every iterator allocation, including validation-set growth and
         * stack reallocations. Protection-table allocation keeps its own policy. */
        long failures = 0;
        for (;; failures++) {
            CHECK(failures < 100);
            test_iterator_fail_after(failures);
            sylvan_iterator *iterator = first;
            int ok = create_iterator(diagram, domain, &iterator);
            test_iterator_fail_after(-1);
            if (ok) {
                CHECK(protect_count(roots) == root_count + (family == 3 ? 1 : 2));
                destroy_iterator(iterator);
                CHECK(protect_count(roots) == root_count);
                break;
            }
            CHECK(iterator == first && protect_count(roots) == root_count);
            sylvan_gc();
        }
        CHECK(failures >= 6);

        /* Grow the protection table between collections, under the same
         * threading contract as the ordinary protection APIs. */
        const size_t before_size = roots->refs_size;
        for (size_t i = 0; i < batch; i++) {
            sylvan_iterator *iterator;
            CHECK(create_iterator(diagram, domain, &iterator));
            iterators.push_back(iterator);
        }
        CHECK(roots->refs_size > before_size);
        CHECK(protect_count(roots) == 2 + iterators.size() * (family == 3 ? 1 : 2));
        if (family < 2) { mtbdd_unprotect(&diagram); mtbdd_unprotect(&domain); }
        else if (family == 2) { zdd_unprotect(&diagram); zdd_unprotect(&domain); }
        else { lddmc_unprotect(&diagram); lddmc_unprotect(&invalid); }
        // Only the iterators keep these nonconstant diagrams alive now.
        sylvan_gc();
        for (auto iterator : iterators) check_item(iterator, 0);
        sylvan_gc();
        for (auto iterator : iterators) check_item(iterator, 1);
        for (auto iterator : iterators) destroy_iterator(iterator);
        CHECK(protect_count(roots) == 0);
        sylvan_gc();
    }
    sylvan_quit();
    lace_stop();
    return 0;
}
