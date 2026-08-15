/**
 * Just a small test file to ensure that Sylvan can compile in C++
 */

#include <assert.h>
#include <string.h>

#include <string>

#include <sylvan/obj.hpp>

#include "test_assert.h"

using namespace sylvan;

/**
 * A custom leaf type whose clone callback always fails. Creating a leaf of
 * this type is therefore a deterministic failure inside mtbdd_leaf, which
 * reports it by returning mtbdd_invalid rather than through a status.
 */
static uint64_t
failing_hash(void *context, uint64_t value, uint64_t seed)
{
    (void)context;
    return value ^ seed;
}

static int
failing_equal(void *context, uint64_t left, uint64_t right)
{
    (void)context;
    return left == right;
}

static int
failing_clone(void *context, uint64_t value, uint64_t *result)
{
    (void)context;
    (void)value;
    (void)result;
    return SYLVAN_ERR_OOM;
}

static void
failing_destroy(void *context, uint64_t value)
{
    (void)context;
    (void)value;
}

TASK(int, runtest)
int runtest_CALL(lace_worker* lace)
{
    (void)lace;

    Bdd one = Bdd::bddOne();
    Bdd zero = Bdd::bddZero();

    test_assert(one != zero);
    test_assert(one == !zero);

    Bdd v1 = Bdd::bddVar(1);
    Bdd v2 = Bdd::bddVar(2);

    Bdd t = v1 + v2;
    Bdd u = v1;
    u += v2;

    test_assert(t == u);

    BddMap map;
    map.put(2, t);

    test_assert(v2.Compose(map) == (v1 + v2));
    test_assert((t * v2) == v2);
    test_assert(t.IntersectionWitness(v2) != zero);
    test_assert(t.IntersectionWitness(!t) == zero);

    BddSet variables;
    variables.add(1);
    variables.add(2);
    test_assert(t.Eval(variables, {0, 1}) == one);
    test_assert(t.UniqueAbstract(variables) == one);
    test_assert(t.UniqueAbstract(BddSet(v1)) == !v2);
    uint64_t count;
    test_assert(t.SatCount(variables) == 3.0);
    test_assert(t.SatCountU64(count, variables) && count == 3);

    Mtbdd seven = Mtbdd::int64Terminal(7);
    Mtbdd nine = Mtbdd::int64Terminal(9);
    Mtbdd two = Mtbdd::int64Terminal(2);
    test_assert(mtbdd_leaf_int64((seven / two).GetMTBDD()) == 3);
    test_assert(mtbdd_leaf_int64(seven.Divide(two).GetMTBDD()) == 3);
    Mtbdd integer_function = Mtbdd::mtbddVar(1).Ite(seven, nine);
    test_assert(integer_function.Eval(variables, {0, 1}) == nine);
    test_assert(integer_function.Eval(variables, {1, 0}) == seven);
    test_assert(integer_function.SatCount(variables) == 4.0);
    test_assert(integer_function.SatCountU64(count, variables) && count == 4);
    test_assert(seven.AllLt(nine));
    test_assert(seven.AnyLeq(nine));
    test_assert(!seven.AnyGt(nine));
    test_assert(seven.CompareLt(nine) == one);
    test_assert(seven.CompareGeq(nine) == zero);

    Mtbdd one_double = Mtbdd::doubleTerminal(1.0);
    Mtbdd close_double = Mtbdd::doubleTerminal(1.05);
    test_assert(one_double.AllEqualAbs(close_double, 0.1));
    test_assert(one_double.AnyEqualRel(close_double, 0.1));
    test_assert(one_double.CompareEqualAbs(close_double, 0.01) == zero);

    return 0;
}

/**
 * A failing operation throws sylvan_error carrying the C status, and leaves
 * the wrapper object it was invoked on unchanged.
 */
TASK(int, runtest_errors)
int runtest_errors_CALL(lace_worker* lace)
{
    (void)lace;

    BddSet variables;
    variables.add(1);
    variables.add(2);

    Bdd t = Bdd::bddVar(1) + Bdd::bddVar(2);

    /* Evaluating a two-variable cube under one value is SYLVAN_ERR_INVALID.
     * This used to silently produce a Bdd holding mtbdd_invalid. */
    const Bdd t_before = t;
    int caught = 0;
    try {
        t.Eval(variables, {0});
    } catch (const sylvan_error &e) {
        caught = 1;
        test_assert(e.status() == SYLVAN_ERR_INVALID);
        test_assert(std::string(e.what()) ==
                    std::string(sylvan_status_string(SYLVAN_ERR_INVALID)));
    }
    test_assert(caught);
    test_assert(t == t_before);

    /* The exception is catchable as its standard base class too. */
    caught = 0;
    try {
        t.Eval(variables, {0});
    } catch (const std::runtime_error &) {
        caught = 1;
    }
    test_assert(caught);

    /* An in-place operator that throws leaves its operand unchanged: adding
     * leaves of two different types is SYLVAN_ERR_INVALID. */
    Mtbdd number = Mtbdd::int64Terminal(7);
    Mtbdd real = Mtbdd::doubleTerminal(1.0);
    const Mtbdd number_before = number;
    caught = 0;
    try {
        number += real;
    } catch (const sylvan_error &e) {
        caught = 1;
        test_assert(e.status() == SYLVAN_ERR_INVALID);
    }
    test_assert(caught);
    test_assert(number == number_before);
    test_assert(mtbdd_leaf_int64(number.GetMTBDD()) == 7);

    /* The same holds for the out-of-place form. */
    caught = 0;
    try {
        Mtbdd sum = number + real;
        (void)sum;
    } catch (const sylvan_error &e) {
        caught = 1;
        test_assert(e.status() == SYLVAN_ERR_INVALID);
    }
    test_assert(caught);
    test_assert(mtbdd_leaf_int64(number.GetMTBDD()) == 7);

    /* A custom leaf whose clone callback fails is reported by mtbdd_leaf as
     * mtbdd_invalid; that sentinel must become an exception as well, rather
     * than an Mtbdd holding an invalid handle. */
    sylvan_mt_type_descriptor descriptor;
    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.name = "test_cxx.failing_leaf";
    descriptor.cache_id = UINT64_C(0x5f3a1c94b20de761);
    descriptor.hash = failing_hash;
    descriptor.equal = failing_equal;
    descriptor.clone = failing_clone;
    descriptor.destroy = failing_destroy;

    uint32_t failing_type = 0;
    test_assert(sylvan_mt_register_type(&failing_type, &descriptor) == SYLVAN_OK);

    caught = 0;
    try {
        Mtbdd leaf = Mtbdd::terminal(failing_type, 42);
        (void)leaf;
    } catch (const sylvan_error &e) {
        caught = 1;
        test_assert(e.status() == SYLVAN_ERR_INVALID);
    }
    test_assert(caught);

    /* Successful operations are unaffected by any of the above. */
    test_assert((t * Bdd::bddVar(2)) == Bdd::bddVar(2));
    test_assert(t.NodeCount() == (Bdd::bddVar(1) + Bdd::bddVar(2)).NodeCount());
    test_assert(t.NodeCount() > 1);

    return 0;
}

void test6()
{
    BddMap m1;
    BddMap m2(m1);  // this triggers an assertion
}

int main()
{
    // Standard Lace initialization with 1 worker
    lace_start(1, 0, 0);

    // Simple Sylvan initialization, also initialize BDD support
    sylvan_set_sizes(1LL<<16, 1LL<<16, 1LL<<16, 1LL<<16);
    if (sylvan_init_package() != SYLVAN_OK ||
        mtbdd_init() != SYLVAN_OK) {
        fprintf(stderr, "Sylvan initialization failed.\n");
        return 1;
    }

    test6();

    int res = runtest();
    if (res == 0) res = runtest_errors();

    sylvan_quit();
    lace_stop();

    return res;
}
