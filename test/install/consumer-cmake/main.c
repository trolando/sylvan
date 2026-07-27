#include <sylvan/platform.h>
#include <sylvan/sylvan.h>

TASK(int, test_consumer)
int test_consumer_CALL(lace_worker* lace)
{
    MTBDD one = mtbdd_int64(1);
    MTBDD two = mtbdd_invalid;
    BDD x = mtbdd_invalid;
    BDD y = mtbdd_invalid;
    BDDSET variables = mtbdd_invalid;
    BDD abstracted = mtbdd_invalid;
    mtbdd_refs_pushptr(&one);
    mtbdd_refs_pushptr(&two);
    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&y);
    mtbdd_refs_pushptr(&variables);
    mtbdd_refs_pushptr(&abstracted);

    int result =
        mtbdd_add(&two, one, one) != SYLVAN_OK ||
        mtbdd_leaf_int64(two) != 2 ||
        bdd_var_at_level(&x, 0) != SYLVAN_OK ||
        bdd_var_at_level(&y, 1) != SYLVAN_OK ||
        bdd_set_from_array(
            &variables, (const uint32_t[]){0}, 1) != SYLVAN_OK ||
        bdd_apply_abstract_CALL(
            lace, &abstracted, x, y, variables,
            BDD_APPLY_AND, BDD_ABSTRACT_EXISTS) != SYLVAN_OK ||
        abstracted != y;

    mtbdd_refs_popptr(6);
    return result;
}

int main(void)
{
    lace_start(1, 0, 0);
    sylvan_set_sizes(1U << 12, 1U << 12, 1U << 10, 1U << 10);
    sylvan_init_package();
    mtbdd_init();

    int result = test_consumer();

    sylvan_quit();
    lace_stop();
    return result;
}

