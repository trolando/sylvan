#include <sylvan/internal.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <new>
#include <stdexcept>
#include <string>

using namespace sylvan;

namespace {

struct Rational {
    std::int64_t numerator;
    std::int64_t denominator;

    Rational(std::int64_t numerator_, std::int64_t denominator_)
        : numerator(numerator_), denominator(denominator_)
    {
        if (denominator == 0) {
            throw std::domain_error("zero denominator");
        }
        if (denominator < 0) {
            numerator = -numerator;
            denominator = -denominator;
        }
        const std::int64_t divisor = std::gcd(numerator, denominator);
        numerator /= divisor;
        denominator /= divisor;
    }
};

struct RationalContext {
    std::atomic<std::size_t> clones{0};
    std::atomic<std::size_t> destroys{0};
    uint32_t type = UINT32_MAX;
};

RationalContext rational_context;

const Rational&
rational_value(uint64_t value)
{
    return *reinterpret_cast<const Rational*>(
        static_cast<std::uintptr_t>(value));
}

uint64_t
rational_hash(void*, uint64_t value, uint64_t seed)
{
    const Rational& rational = rational_value(value);
    uint64_t hash = seed ^ static_cast<uint64_t>(rational.numerator);
    hash ^= static_cast<uint64_t>(rational.denominator) +
            UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2);
    return hash;
}

int
rational_equal(void*, uint64_t left, uint64_t right)
{
    const Rational& a = rational_value(left);
    const Rational& b = rational_value(right);
    return a.numerator == b.numerator && a.denominator == b.denominator;
}

int
rational_clone(void *context, uint64_t value, uint64_t *result)
{
    try {
        auto *copy = new Rational(rational_value(value));
        *result = static_cast<uint64_t>(
            reinterpret_cast<std::uintptr_t>(copy));
        static_cast<RationalContext*>(context)->clones++;
        return SYLVAN_OK;
    } catch (const std::bad_alloc&) {
        return SYLVAN_ERR_OOM;
    } catch (...) {
        return SYLVAN_ERR_INVALID;
    }
}

void
rational_destroy(void *context, uint64_t value)
{
    delete reinterpret_cast<Rational*>(static_cast<std::uintptr_t>(value));
    static_cast<RationalContext*>(context)->destroys++;
}

int
rational_to_string(
    void*, int complement, uint64_t value, char **result)
{
    try {
        const Rational& rational = rational_value(value);
        std::string text = complement ? "~" : "";
        text += std::to_string(rational.numerator);
        text += "/";
        text += std::to_string(rational.denominator);
        auto *copy = new char[text.size() + 1];
        std::memcpy(copy, text.c_str(), text.size() + 1);
        *result = copy;
        return SYLVAN_OK;
    } catch (const std::bad_alloc&) {
        return SYLVAN_ERR_OOM;
    } catch (...) {
        return SYLVAN_ERR_INVALID;
    }
}

void
rational_string_free(void*, char *string)
{
    delete[] string;
}

int
make_rational(MTBDD *result, const Rational& rational)
{
    const MTBDD leaf = mtbdd_leaf(
        rational_context.type,
        static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(&rational)));
    if (leaf == mtbdd_invalid) return SYLVAN_ERR_OOM;
    *result = leaf;
    return SYLVAN_OK;
}

int
read_rational(MTBDD leaf, Rational *result)
{
    if (!mtbdd_is_leaf(leaf) ||
        mtbdd_leaf_type(leaf) != rational_context.type) {
        return SYLVAN_ERR_INVALID;
    }
    *result = rational_value(mtbdd_leaf_value(leaf));
    return SYLVAN_OK;
}

int
rational_multiply(
    lace_worker*, MTBDD *result, MTBDD *left, MTBDD *right, void*)
{
    if (!mtbdd_is_leaf(*left) || !mtbdd_is_leaf(*right)) {
        return SYLVAN_APPLY_RECURSE;
    }
    try {
        const Rational& a = rational_value(mtbdd_leaf_value(*left));
        const Rational& b = rational_value(mtbdd_leaf_value(*right));
        return make_rational(
            result,
            Rational(
                a.numerator * b.numerator,
                a.denominator * b.denominator));
    } catch (const std::bad_alloc&) {
        return SYLVAN_ERR_OOM;
    } catch (...) {
        return SYLVAN_ERR_INVALID;
    }
}

int
rational_sum(
    lace_worker*, MTBDD *result, MTBDD left, MTBDD right,
    size_t skipped, void*)
{
    try {
        const Rational& a = rational_value(mtbdd_leaf_value(left));
        if (skipped != 0) {
            if (skipped >= 62) return SYLVAN_ERR_OVERFLOW;
            return make_rational(
                result,
                Rational(
                    a.numerator * (INT64_C(1) << skipped),
                    a.denominator));
        }
        const Rational& b = rational_value(mtbdd_leaf_value(right));
        return make_rational(
            result,
            Rational(
                a.numerator * b.denominator +
                    b.numerator * a.denominator,
                a.denominator * b.denominator));
    } catch (const std::bad_alloc&) {
        return SYLVAN_ERR_OOM;
    } catch (...) {
        return SYLVAN_ERR_INVALID;
    }
}

int
accept_nonzero(MTBDD leaf, void*)
{
    return rational_value(mtbdd_leaf_value(leaf)).numerator != 0;
}

#define REQUIRE(condition)                                                   \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "requirement failed at line " << __LINE__ << "\n"; \
            return SYLVAN_ERR_INVALID;                                       \
        }                                                                    \
    } while (0)

TASK(int, run_storm_rational_canary)

int
run_storm_rational_canary_CALL(lace_worker *lace)
{
    const sylvan_mt_type_descriptor descriptor = {
        "canary.storm.rational",
        UINT64_C(0x73746f726d726174),
        &rational_context,
        rational_hash,
        rational_equal,
        rational_clone,
        rational_destroy,
        rational_to_string,
        rational_string_free
    };
    REQUIRE(sylvan_mt_register_type(
        &rational_context.type, &descriptor) == SYLVAN_OK);

    MTBDD zero = mtbdd_invalid;
    MTBDD one = mtbdd_invalid;
    MTBDD two = mtbdd_invalid;
    MTBDD three = mtbdd_invalid;
    MTBDD four = mtbdd_invalid;
    MTBDD five = mtbdd_invalid;
    MTBDD six = mtbdd_invalid;
    MTBDD seven = mtbdd_invalid;
    MTBDD eight = mtbdd_invalid;
    BDD x = mtbdd_invalid;
    BDD y = mtbdd_invalid;
    MTBDD f_low = mtbdd_invalid;
    MTBDD f_high = mtbdd_invalid;
    MTBDD f = mtbdd_invalid;
    MTBDD g_low = mtbdd_invalid;
    MTBDD g_high = mtbdd_invalid;
    MTBDD g = mtbdd_invalid;
    MTBDD filtered = mtbdd_invalid;
    MTBDD result = mtbdd_invalid;
    BDDSET all_variables = mtbdd_invalid;
    BDDSET x_variables = mtbdd_invalid;
    BDDSET y_variables = mtbdd_invalid;

    MTBDD *protected_values[] = {
        &zero, &one, &two, &three, &four, &five, &six, &seven, &eight,
        &x, &y, &f_low, &f_high, &f, &g_low, &g_high, &g, &filtered,
        &result, &all_variables, &x_variables, &y_variables
    };
    for (MTBDD *value : protected_values) mtbdd_refs_pushptr(value);

    REQUIRE(make_rational(&zero, Rational(0, 1)) == SYLVAN_OK);
    REQUIRE(make_rational(&one, Rational(1, 1)) == SYLVAN_OK);
    REQUIRE(make_rational(&two, Rational(2, 1)) == SYLVAN_OK);
    REQUIRE(make_rational(&three, Rational(3, 1)) == SYLVAN_OK);
    REQUIRE(make_rational(&four, Rational(4, 1)) == SYLVAN_OK);
    REQUIRE(make_rational(&five, Rational(5, 1)) == SYLVAN_OK);
    REQUIRE(make_rational(&six, Rational(6, 1)) == SYLVAN_OK);
    REQUIRE(make_rational(&seven, Rational(7, 1)) == SYLVAN_OK);
    REQUIRE(make_rational(&eight, Rational(8, 1)) == SYLVAN_OK);
    REQUIRE(bdd_var_at_level(&x, 0) == SYLVAN_OK);
    REQUIRE(bdd_var_at_level(&y, 1) == SYLVAN_OK);
    const uint32_t all_levels[] = {0, 1};
    const uint32_t x_level[] = {0};
    const uint32_t y_level[] = {1};
    REQUIRE(bdd_set_from_array(
        &all_variables, all_levels, 2) == SYLVAN_OK);
    REQUIRE(bdd_set_from_array(
        &x_variables, x_level, 1) == SYLVAN_OK);
    REQUIRE(bdd_set_from_array(
        &y_variables, y_level, 1) == SYLVAN_OK);

    REQUIRE(mtbdd_ite_CALL(lace, &f_low, y, two, one) == SYLVAN_OK);
    REQUIRE(mtbdd_ite_CALL(lace, &f_high, y, four, three) == SYLVAN_OK);
    REQUIRE(mtbdd_ite_CALL(lace, &f, x, f_high, f_low) == SYLVAN_OK);
    REQUIRE(mtbdd_ite_CALL(lace, &g_low, y, six, five) == SYLVAN_OK);
    REQUIRE(mtbdd_ite_CALL(lace, &g_high, y, eight, seven) == SYLVAN_OK);
    REQUIRE(mtbdd_ite_CALL(lace, &g, x, g_high, g_low) == SYLVAN_OK);
    REQUIRE(mtbdd_ite_CALL(lace, &filtered, x, one, zero) == SYLVAN_OK);

    sylvan_gc_CALL(lace);

    mtbdd_iterator_options iterator_options = {
        SYLVAN_ITERATOR_CUBES,
        accept_nonzero,
        nullptr
    };
    sylvan_iterator *iterator = nullptr;
    REQUIRE(mtbdd_iterator_create(
        &iterator, filtered, all_variables, &iterator_options) == SYLVAN_OK);
    uint8_t values[2] = {9, 9};
    MTBDD leaf = mtbdd_invalid;
    int has_item = 0;
    REQUIRE(mtbdd_iterator_next(
        iterator, values, 2, &leaf, &has_item) == SYLVAN_OK);
    REQUIRE(has_item == 1 && leaf == one &&
            values[0] == 1 && values[1] == 2);
    REQUIRE(mtbdd_iterator_next(
        iterator, values, 2, &leaf, &has_item) == SYLVAN_OK);
    REQUIRE(has_item == 0);
    sylvan_iterator_destroy(iterator);

    const mtbdd_combine_reduce_op operation = {
        rational_multiply,
        rational_sum,
        zero,
        nullptr,
        cache_next_opid()
    };
    REQUIRE(mtbdd_combine_reduce_CALL(
        lace, &result, f, g, y_variables, &operation) == SYLVAN_OK);
    sylvan_gc_CALL(lace);

    iterator = nullptr;
    iterator_options.accept_leaf = nullptr;
    REQUIRE(mtbdd_iterator_create(
        &iterator, result, x_variables, &iterator_options) == SYLVAN_OK);

    Rational rational(0, 1);
    REQUIRE(mtbdd_iterator_next(
        iterator, values, 1, &leaf, &has_item) == SYLVAN_OK);
    REQUIRE(has_item == 1 && values[0] == 0);
    REQUIRE(read_rational(leaf, &rational) == SYLVAN_OK);
    REQUIRE(rational.numerator == 17 && rational.denominator == 1);

    REQUIRE(mtbdd_iterator_next(
        iterator, values, 1, &leaf, &has_item) == SYLVAN_OK);
    REQUIRE(has_item == 1 && values[0] == 1);
    REQUIRE(read_rational(leaf, &rational) == SYLVAN_OK);
    REQUIRE(rational.numerator == 53 && rational.denominator == 1);

    REQUIRE(mtbdd_iterator_next(
        iterator, values, 1, &leaf, &has_item) == SYLVAN_OK);
    REQUIRE(has_item == 0);
    sylvan_iterator_destroy(iterator);

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

    int status = run_storm_rational_canary();
    sylvan_gc();
    if (status == SYLVAN_OK &&
        rational_context.clones.load() !=
            rational_context.destroys.load()) {
        std::cerr << "custom rational payloads were not released\n";
        status = SYLVAN_ERR_INVALID;
    }

    sylvan_quit();
    lace_stop();
    return status == SYLVAN_OK ? 0 : 1;
}
