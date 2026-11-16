#include <sylvan_obj.hpp>
#include <stdexcept>
#include "test_assert.h"

using namespace sylvan;

TASK_2(MTBDD, add, MTBDD, leaf, size_t, param)
{
    if (!mtbdd_isleaf(leaf)) return mtbdd_invalid;
    return mtbdd_double(mtbdd_getdouble(leaf) + param);
}

TASK_4(MTBDD, sum, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    (void)param;
    return CALL(mtbdd_abstract_op_plus, a, b, (int)skipped);
}

TASK_2(MTBDD, bad_map, MTBDD, leaf, size_t, param)
{
    (void)leaf;
    return param ? mtbdd_ithvar(4) : mtbdd_invalid;
}

TASK_4(MTBDD, bad_reduce, MTBDD, a, MTBDD, b, size_t, skipped, size_t, param)
{
    (void)a; (void)b; (void)skipped; (void)param;
    return mtbdd_invalid;
}

template<class Operation>
static bool throws_invalid_argument(Operation operation)
{
    try {
        operation();
    } catch (const std::invalid_argument &) {
        return true;
    }
    return false;
}

static int test_partial_domains()
{
    Mtbdd x = Mtbdd::mtbddVar(0), undefined = Mtbdd::mtbddZero();
    Mtbdd one = Mtbdd::doubleTerminal(1), two = Mtbdd::doubleTerminal(2);
    Mtbdd a = x.Ite(one, undefined), b = x.Ite(two, undefined);
    test_assert(a.AllLeq(b) && a.AllLt(b) && b.AllGeq(a) && b.AllGt(a));
    test_assert(!b.AllLeq(a) && !b.AllLt(a) && !a.AllGeq(b) && !a.AllGt(b));
    test_assert(a.AllLeq(a) && a.AllGeq(a) && !a.AllLt(a) && !a.AllGt(a));
    test_assert(a.AllEqualAbs(b, 1.1) && a.AllEqualRel(b, 1.1));
    test_assert(!a.AllEqualAbs(b, 0.5) && !a.AllEqualRel(b, 0.5));
    Mtbdd disjoint = x.Ite(undefined, two);
    test_assert(a.AllLeq(disjoint) && a.AllLt(disjoint) && a.AllGeq(disjoint) && a.AllGt(disjoint));
    test_assert(a.AllEqualAbs(disjoint, 0) && a.AllEqualRel(disjoint, 0));
    test_assert(undefined.AllLeq(undefined) && undefined.AllLt(undefined));
    test_assert(undefined.AllGeq(undefined) && undefined.AllGt(undefined));
    return 0;
}

static int test_errors()
{
    Mtbdd one = Mtbdd::doubleTerminal(1), two = Mtbdd::doubleTerminal(2);
    Mtbdd x = Mtbdd::mtbddVar(0), a = x.Ite(one, two);
    Mtbdd unsupported[] = {Mtbdd::int64Terminal(1), Mtbdd::mtbddOne(),
        Mtbdd::terminal(sylvan_mt_create_type(), 42), Mtbdd(mtbdd_invalid)};
    bool (Mtbdd::*const predicates[])(const Mtbdd &) const = {
        &Mtbdd::AllLeq, &Mtbdd::AllLt, &Mtbdd::AllGeq, &Mtbdd::AllGt,
        &Mtbdd::AnyLeq, &Mtbdd::AnyLt, &Mtbdd::AnyGeq, &Mtbdd::AnyGt};
    Bdd (Mtbdd::*const comparisons[])(const Mtbdd &) const = {
        &Mtbdd::CompareLeq, &Mtbdd::CompareLt, &Mtbdd::CompareGeq, &Mtbdd::CompareGt};
    bool (Mtbdd::*const tolerance_predicates[])(const Mtbdd &, double) const = {
        &Mtbdd::AllEqualAbs, &Mtbdd::AllEqualRel, &Mtbdd::AnyEqualAbs, &Mtbdd::AnyEqualRel};
    Bdd (Mtbdd::*const tolerance_comparisons[])(const Mtbdd &, double) const = {
        &Mtbdd::CompareEqualAbs, &Mtbdd::CompareEqualRel};
    for (const Mtbdd &bad : unsupported) {
        for (auto method : predicates) {
            test_assert(throws_invalid_argument([&] { (one.*method)(bad); }));
            test_assert(throws_invalid_argument([&] { (bad.*method)(one); }));
        }
        for (auto method : comparisons) {
            test_assert(throws_invalid_argument([&] { (one.*method)(bad); }));
        }
        for (auto method : tolerance_predicates) {
            test_assert(throws_invalid_argument([&] { (one.*method)(bad, 0.1); }));
        }
        for (auto method : tolerance_comparisons) {
            test_assert(throws_invalid_argument([&] { (one.*method)(bad, 0.1); }));
        }
    }
    /* Validate even identical unsupported leaves/subdiagrams. A failing
     * numeric branch must not hide an invalid sibling. */
    for (unsigned i=1; i<3; i++) {
        Mtbdd bad = unsupported[i], partial = x.Ite(bad, Mtbdd::mtbddZero());
        Mtbdd bad_a = x.Ite(bad, two), bad_b = x.Ite(bad, one);
        for (unsigned j=0; j<4; j++) {
            auto method = predicates[j];
            test_assert(throws_invalid_argument([&] { (bad.*method)(bad); }));
            test_assert(throws_invalid_argument([&] { (partial.*method)(partial); }));
            test_assert(throws_invalid_argument([&] { (bad_a.*method)(bad_b); }));
        }
    }
    BddSet variables;
    variables.add(0);
    Mtbdd zero = Mtbdd::doubleTerminal(0), invalid(mtbdd_invalid);
    test_assert(throws_invalid_argument([&] { a.Map(nullptr, 0); }));
    test_assert(throws_invalid_argument([&] { invalid.Map(add_CALL, 0); }));
    for (size_t param=0; param<2; param++) {
        test_assert(throws_invalid_argument([&] { a.Map(bad_map_CALL, param); }));
        test_assert(throws_invalid_argument([&] { a.MapReduce(variables, bad_map_CALL, sum_CALL, zero, param); }));
    }
    test_assert(throws_invalid_argument([&] { a.MapReduce(variables, add_CALL, bad_reduce_CALL, zero, 0); }));
    test_assert(throws_invalid_argument([&] { a.MapReduce(variables, nullptr, sum_CALL, zero, 0); }));
    test_assert(throws_invalid_argument([&] { a.MapReduce(variables, add_CALL, nullptr, zero, 0); }));
    test_assert(throws_invalid_argument([&] { a.MapReduce(variables, add_CALL, sum_CALL, a, 0); }));
    test_assert(throws_invalid_argument([&] { a.MapReduce(variables, add_CALL, sum_CALL, invalid, 0); }));
    test_assert(throws_invalid_argument([&] { invalid.MapReduce(variables, add_CALL, sum_CALL, zero, 0); }));
    BddSet malformed(Bdd::bddZero());
    test_assert(throws_invalid_argument([&] { a.MapReduce(malformed, add_CALL, sum_CALL, zero, 0); }));
    return 0;
}

TASK_0(int, runtests)
{
    test_assert(test_partial_domains() == 0);
    test_assert(test_errors() == 0);
    Mtbdd x = Mtbdd::mtbddVar(0), y = Mtbdd::mtbddVar(2);
    Bdd bx = Bdd::bddVar(0), by = Bdd::bddVar(2);
    Mtbdd one = Mtbdd::doubleTerminal(1), two = Mtbdd::doubleTerminal(2);
    Mtbdd three = Mtbdd::doubleTerminal(3), four = Mtbdd::doubleTerminal(4);
    Mtbdd a = x.Ite(three, one), b = y.Ite(two, four);
    test_assert(a.CompareLeq(b) == (!bx + !by));
    test_assert(a.CompareLt(b) == (!bx + !by));
    test_assert(a.CompareGeq(b) == (bx * by));
    test_assert(a.CompareGt(b) == (bx * by));
    test_assert(a.AnyLeq(b) && a.AnyLt(b) && a.AnyGeq(b) && a.AnyGt(b));
    test_assert(!a.AllLeq(b) && !a.AllLt(b) && !a.AllGeq(b) && !a.AllGt(b));
    test_assert(one.AllLeq(two) && one.AllLt(two) && two.AllGeq(one) && two.AllGt(one));
    test_assert(a.CompareEqualAbs(b, 1.1) == (bx + by));
    test_assert(a.CompareEqualRel(b, 0.4) == bx);
    test_assert(a.AnyEqualAbs(b, 1.1) && a.AnyEqualRel(b, 0.4));
    test_assert(!a.AllEqualAbs(b, 1.1) && !a.AllEqualRel(b, 0.4));
    test_assert(a.AllEqualAbs(a, 0) && a.AllEqualRel(a, 0));
    test_assert(a.Map(add_CALL, 1) == x.Ite(four, two));
    BddSet variables;
    variables.add(0);
    variables.add(1); // absent from a: repeat the reduction twice
    Mtbdd zero = Mtbdd::doubleTerminal(0);
    test_assert(a.MapReduce(variables, add_CALL, sum_CALL, zero, 1) == Mtbdd::doubleTerminal(12));
    test_assert(a.UApply(add_CALL, 1) == a.Map(add_CALL, 1));
    return 0;
}

int main()
{
    lace_start(2, 0);
    sylvan_set_sizes(1<<10, 1<<16, 1<<10, 1<<16);
    sylvan_init_package();
    sylvan_init_mtbdd();
    int result = RUN(runtests);
    sylvan_quit();
    lace_stop();
    return result;
}
