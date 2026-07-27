#include <sylvan/internal.h>

#include <cstdint>
#include <iostream>
#include <string>

using namespace sylvan;

namespace {

#define REQUIRE(condition)                                                   \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "requirement failed at line " << __LINE__ << "\n"; \
            return SYLVAN_ERR_INVALID;                                       \
        }                                                                    \
    } while (0)

TASK(int, storm_shift_signature,
     BDD*, result, BDD, dd, BDDSET, variables, uint32_t, offset)

int
storm_shift_signature_CALL(
    lace_worker *lace,
    BDD *destination,
    BDD dd,
    BDDSET variables,
    uint32_t offset)
{
    if (destination == nullptr || dd == mtbdd_invalid ||
        variables == mtbdd_invalid) {
        return SYLVAN_ERR_INVALID;
    }
    if (bdd_set_is_empty(variables)) {
        *destination = dd;
        return SYLVAN_OK;
    }

    sylvan_gc_test(lace);

    const uint32_t level = bdd_set_first(variables);
    const BDDSET next = bdd_set_next(variables);
    BDD low = dd;
    BDD high = dd;
    if (!bdd_is_leaf(dd)) {
        const uint32_t dd_level = mtbdd_node_variable(dd);
        if (dd_level < level) return SYLVAN_ERR_INVALID;
        if (dd_level == level) mtbdd_cofactors(dd, &low, &high);
    }

    BDD shifted_low = mtbdd_invalid;
    BDD shifted_high = mtbdd_invalid;
    BDD computed = mtbdd_invalid;
    mtbdd_refs_pushptr(&shifted_low);
    mtbdd_refs_pushptr(&shifted_high);
    mtbdd_refs_pushptr(&computed);

    storm_shift_signature_SPAWN(
        lace, &shifted_high, high, next, offset);
    int status = storm_shift_signature_CALL(
        lace, &shifted_low, low, next, offset);
    const int high_status = storm_shift_signature_SYNC(lace);
    if (status == SYLVAN_OK) status = high_status;
    if (status == SYLVAN_OK) {
        status = _mtbdd_try_make_node(
            &computed, level + offset, shifted_low, shifted_high);
    }
    if (status == SYLVAN_OK) *destination = computed;

    mtbdd_refs_popptr(3);
    return status;
}

TASK(int, run_storm_recursion_canary)

int
run_storm_recursion_canary_CALL(lace_worker *lace)
{
    BDD x0 = mtbdd_invalid;
    BDD x1 = mtbdd_invalid;
    BDD x2 = mtbdd_invalid;
    BDD x4 = mtbdd_invalid;
    BDD x5 = mtbdd_invalid;
    BDD x6 = mtbdd_invalid;
    BDD left = mtbdd_invalid;
    BDD input = mtbdd_invalid;
    BDD expected_left = mtbdd_invalid;
    BDD expected = mtbdd_invalid;
    BDD actual = mtbdd_invalid;
    BDDSET variables = mtbdd_invalid;

    BDD *protected_values[] = {
        &x0, &x1, &x2, &x4, &x5, &x6, &left, &input,
        &expected_left, &expected, &actual, &variables
    };
    for (BDD *value : protected_values) mtbdd_refs_pushptr(value);

    REQUIRE(bdd_var_at_level(&x0, 0) == SYLVAN_OK);
    REQUIRE(bdd_var_at_level(&x1, 1) == SYLVAN_OK);
    REQUIRE(bdd_var_at_level(&x2, 2) == SYLVAN_OK);
    REQUIRE(bdd_var_at_level(&x4, 4) == SYLVAN_OK);
    REQUIRE(bdd_var_at_level(&x5, 5) == SYLVAN_OK);
    REQUIRE(bdd_var_at_level(&x6, 6) == SYLVAN_OK);
    REQUIRE(bdd_and_CALL(lace, &left, x0, x1) == SYLVAN_OK);
    REQUIRE(bdd_or_CALL(lace, &input, left, x2) == SYLVAN_OK);
    REQUIRE(bdd_and_CALL(
        lace, &expected_left, x4, x5) == SYLVAN_OK);
    REQUIRE(bdd_or_CALL(
        lace, &expected, expected_left, x6) == SYLVAN_OK);
    const uint32_t levels[] = {0, 1, 2};
    REQUIRE(bdd_set_from_array(
        &variables, levels, 3) == SYLVAN_OK);

    sylvan_gc_CALL(lace);
    REQUIRE(storm_shift_signature_CALL(
        lace, &actual, input, variables, 4) == SYLVAN_OK);
    REQUIRE(actual == expected);

    sylvan_gc_CALL(lace);
    REQUIRE(actual == expected);

    mtbdd_refs_popptr(
        sizeof(protected_values) / sizeof(protected_values[0]));
    return SYLVAN_OK;
}

} // namespace

int
main(int argc, char **argv)
{
    const int workers = argc == 2 ? std::stoi(argv[1]) : 1;
    lace_start(static_cast<unsigned int>(workers), 0, 0);
    sylvan_set_sizes(
        static_cast<size_t>(1) << 14,
        static_cast<size_t>(1) << 14,
        static_cast<size_t>(1) << 12,
        static_cast<size_t>(1) << 12);
    sylvan_init_package();
    mtbdd_init();

    const int status = run_storm_recursion_canary();

    sylvan_quit();
    lace_stop();
    return status == SYLVAN_OK ? 0 : 1;
}
