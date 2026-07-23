/**
 * Just a small test file to ensure that Sylvan can compile in C++
 */

#include <assert.h>
#include <sylvan/obj.hpp>

#include "test_assert.h"

using namespace sylvan;

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

    BddSet variables;
    variables.add(1);
    variables.add(2);
    test_assert(t.Eval(variables, {0, 1}) == one);
    test_assert(t.Eval(variables, {0}) == Bdd(mtbdd_invalid));
    test_assert(t.UniqueAbstract(variables) == one);
    test_assert(t.UniqueAbstract(BddSet(v1)) == !v2);

    Mtbdd seven = Mtbdd::int64Terminal(7);
    Mtbdd nine = Mtbdd::int64Terminal(9);
    Mtbdd integer_function = Mtbdd::mtbddVar(1).Ite(seven, nine);
    test_assert(integer_function.Eval(variables, {0, 1}) == nine);
    test_assert(integer_function.Eval(variables, {1, 0}) == seven);
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
    sylvan_init_package();
    mtbdd_init();

    test6();

    int res = runtest();

    sylvan_quit();
    lace_stop();

    return res;
}
