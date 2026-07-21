#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <inttypes.h>
#include <math.h>

#include <lace.h>
#include <sylvan/internal.h>
#include <sylvan/platform.h>

#include "test_assert.h"

typedef int (*test_bdd_binary_op)(BDD*, BDD, BDD);
typedef int (*test_bdd_unary_set_op)(BDD*, BDD, BDDSET);
typedef int (*test_bdd_binary_set_op)(BDD*, BDD, BDD, BDDSET);

static BDD
test_bdd_var(uint32_t level)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_var_at_level(&result, level);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDDSET
test_bdd_set_from_levels(const uint32_t *levels, size_t count)
{
    BDDSET result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_set_from_array(&result, levels, count);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

TASK(int, test_new_var, BDD*, result)
int
test_new_var_CALL(lace_worker *lace, BDD *result)
{
    (void)lace;
    return bdd_new_var(result);
}

TASK(int, test_variable_set_destinations)
int
test_variable_set_destinations_CALL(lace_worker *lace)
{
    const uint32_t levels[] = {5, 1, 3, 1};
    const uint32_t other_levels[] = {2, 7};
    const uint32_t invalid_level[] = {UINT32_C(0x01000000)};
    BDD variable = mtbdd_invalid;
    BDD same_variable = mtbdd_invalid;
    BDD fresh_variable = mtbdd_invalid;
    BDD other_fresh_variable = mtbdd_invalid;
    BDDSET set = mtbdd_invalid;
    BDDSET empty = mtbdd_invalid;
    BDDSET added = mtbdd_invalid;
    BDDSET removed = mtbdd_invalid;
    BDDSET other = mtbdd_invalid;
    BDDSET united = mtbdd_invalid;
    BDDSET difference = mtbdd_invalid;
    BDD unchanged = bdd_false;

    mtbdd_refs_pushptr(&variable);
    mtbdd_refs_pushptr(&same_variable);
    mtbdd_refs_pushptr(&fresh_variable);
    mtbdd_refs_pushptr(&other_fresh_variable);
    mtbdd_refs_pushptr(&set);
    mtbdd_refs_pushptr(&empty);
    mtbdd_refs_pushptr(&added);
    mtbdd_refs_pushptr(&removed);
    mtbdd_refs_pushptr(&other);
    mtbdd_refs_pushptr(&united);
    mtbdd_refs_pushptr(&difference);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_var_at_level(&variable, 100) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&same_variable, 100) == SYLVAN_OK);
    test_assert(variable == same_variable);
    test_new_var_SPAWN(lace, &fresh_variable);
    int other_fresh_status = bdd_new_var(&other_fresh_variable);
    int fresh_status = test_new_var_SYNC(lace);
    test_assert(fresh_status == SYLVAN_OK);
    test_assert(other_fresh_status == SYLVAN_OK);
    test_assert(mtbdd_node_variable(fresh_variable) > 100);
    test_assert(mtbdd_node_variable(other_fresh_variable) > 100);
    test_assert(fresh_variable != other_fresh_variable);

    test_assert(bdd_set_from_array(&set, levels, 4) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&empty, NULL, 0) == SYLVAN_OK);
    test_assert(empty == bdd_set_empty());
    test_assert(bdd_set_count(set) == 3);
    test_assert(bdd_set_contains(set, 1));
    test_assert(bdd_set_contains(set, 3));
    test_assert(bdd_set_contains(set, 5));

    added = set;
    test_assert(bdd_set_add(&added, added, 2) == SYLVAN_OK);
    removed = added;
    test_assert(bdd_set_remove(&removed, removed, 3) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&other, other_levels, 2) == SYLVAN_OK);
    test_assert(bdd_set_union(&united, set, other) == SYLVAN_OK);
    bdd_set_difference_SPAWN(lace, &difference, united, set);
    int difference_status = bdd_set_difference_SYNC(lace);
    test_assert(difference_status == SYLVAN_OK);

    sylvan_gc_CALL(lace);
    test_assert(variable == same_variable);
    test_assert(bdd_set_count(added) == 4);
    test_assert(!bdd_set_contains(removed, 3));
    test_assert(bdd_set_count(united) == 5);
    test_assert(bdd_set_count(difference) == 2);
    test_assert(bdd_set_contains(difference, 2));
    test_assert(bdd_set_contains(difference, 7));

    test_assert(bdd_var_at_level(&unchanged, UINT32_C(0x01000000)) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_false);
    test_assert(bdd_var_at_level(NULL, 0) == SYLVAN_ERR_INVALID);
    test_assert(bdd_new_var(NULL) == SYLVAN_ERR_INVALID);
    test_assert(bdd_set_from_array(&unchanged, NULL, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_false);
    test_assert(bdd_set_from_array(&unchanged, invalid_level, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_false);
    test_assert(bdd_set_add(&unchanged, mtbdd_invalid, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_false);
    test_assert(bdd_set_remove(&unchanged, set, UINT32_C(0x01000000)) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_false);
    test_assert(bdd_set_union(NULL, set, other) == SYLVAN_ERR_INVALID);
    test_assert(bdd_set_difference_CALL(lace, NULL, set, other) == SYLVAN_ERR_INVALID);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(12);
    return 0;
}

TASK(int, test_mtbdd_construction_destinations)
int
test_mtbdd_construction_destinations_CALL(lace_worker *lace)
{
    const uint32_t levels[] = {0, 1, 2};
    const uint32_t pair_levels[] = {0, 1};
    const uint32_t single_level[] = {0};
    const uint32_t late_level[] = {1};
    const uint8_t first_cube[] = {0, 1, 2};
    const uint8_t second_cube[] = {1, 0, 2};
    const uint8_t equality_cube[] = {3, 0};
    const uint8_t invalid_cube[] = {4, 0, 0};
    const uint8_t zero_cube[] = {0};
    const uint8_t one_cube[] = {1};
    const uint8_t any_cube[] = {2, 2, 2};
    const uint8_t any_single[] = {2};
    MTBDD terminal = mtbdd_int64(42);
    MTBDD other_terminal = mtbdd_invalid;
    BDDSET vars = mtbdd_invalid;
    BDDSET pair_vars = mtbdd_invalid;
    BDDSET single_var = mtbdd_invalid;
    BDDSET late_var = mtbdd_invalid;
    BDD condition = mtbdd_invalid;
    MTBDD ite_result = mtbdd_invalid;
    MTBDD inplace_ite = mtbdd_invalid;
    MTBDD cube_result = mtbdd_invalid;
    MTBDD set_result = mtbdd_invalid;
    MTBDD equality_result = mtbdd_invalid;
    MTBDD leading_result = mtbdd_invalid;
    MTBDD later_cube = mtbdd_invalid;
    MTBDD earlier_result = mtbdd_invalid;
    MTBDD any_result = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&terminal);
    other_terminal = mtbdd_int64(7);
    mtbdd_refs_pushptr(&other_terminal);
    mtbdd_refs_pushptr(&vars);
    mtbdd_refs_pushptr(&pair_vars);
    mtbdd_refs_pushptr(&single_var);
    mtbdd_refs_pushptr(&late_var);
    mtbdd_refs_pushptr(&condition);
    mtbdd_refs_pushptr(&ite_result);
    mtbdd_refs_pushptr(&inplace_ite);
    mtbdd_refs_pushptr(&cube_result);
    mtbdd_refs_pushptr(&set_result);
    mtbdd_refs_pushptr(&equality_result);
    mtbdd_refs_pushptr(&leading_result);
    mtbdd_refs_pushptr(&later_cube);
    mtbdd_refs_pushptr(&earlier_result);
    mtbdd_refs_pushptr(&any_result);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_set_from_array(&vars, levels, 3) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&pair_vars, pair_levels, 2) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&single_var, single_level, 1) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&late_var, late_level, 1) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&condition, 0) == SYLVAN_OK);

    mtbdd_ite_SPAWN(lace, &ite_result, condition, terminal, other_terminal);
    test_assert(mtbdd_cube(&cube_result, vars, first_cube, terminal) == SYLVAN_OK);
    test_assert(mtbdd_ite_SYNC(lace) == SYLVAN_OK);
    inplace_ite = terminal;
    test_assert(mtbdd_ite_CALL(lace, &inplace_ite, condition, inplace_ite, other_terminal) == SYLVAN_OK);
    test_assert(inplace_ite == ite_result);
    test_assert(mtbdd_set_cube_CALL(lace, &set_result, mtbdd_undefined, vars, first_cube, terminal) == SYLVAN_OK);
    test_assert(set_result == cube_result);
    test_assert(mtbdd_set_cube_CALL(lace, &set_result, set_result, vars, second_cube, other_terminal) == SYLVAN_OK);
    test_assert(mtbdd_cube(&equality_result, pair_vars, equality_cube, terminal) == SYLVAN_OK);
    test_assert(mtbdd_cube(&later_cube, late_var, one_cube, terminal) == SYLVAN_OK);
    BDDSET latest_var = bdd_set_next(bdd_set_next(vars));
    test_assert(mtbdd_set_cube_CALL(lace, &leading_result, later_cube, latest_var, zero_cube, terminal) == SYLVAN_OK);
    test_assert(mtbdd_set_cube_CALL(lace, &earlier_result, later_cube, single_var, zero_cube, other_terminal) == SYLVAN_OK);
    test_assert(mtbdd_set_cube_CALL(lace, &any_result, later_cube, single_var, any_single, other_terminal) == SYLVAN_OK);
    test_assert(any_result == other_terminal);
    test_assert(mtbdd_set_cube_CALL(lace, &any_result, set_result, vars, any_cube, other_terminal) == SYLVAN_OK);
    test_assert(any_result == other_terminal);

    sylvan_gc_CALL(lace);

    MTBDD if_false, if_true;
    mtbdd_cofactors(ite_result, &if_false, &if_true);
    test_assert(mtbdd_node_variable(ite_result) == 0);
    test_assert(if_false == other_terminal);
    test_assert(if_true == terminal);

    mtbdd_cofactors(cube_result, &if_false, &if_true);
    test_assert(if_true == mtbdd_undefined);
    mtbdd_cofactors(if_false, &if_false, &if_true);
    test_assert(if_false == mtbdd_undefined);
    test_assert(if_true == terminal);
    test_assert(mtbdd_is_valid(leading_result));
    test_assert(mtbdd_is_valid(earlier_result));

    mtbdd_cofactors(earlier_result, &if_false, &if_true);
    test_assert(if_false == other_terminal);
    test_assert(if_true == later_cube);

    MTBDD first_assignment, second_assignment;
    mtbdd_cofactors(set_result, &first_assignment, &second_assignment);
    mtbdd_cofactors(first_assignment, &if_false, &if_true);
    test_assert(if_true == terminal);
    mtbdd_cofactors(second_assignment, &if_false, &if_true);
    test_assert(if_false == other_terminal);

    mtbdd_cofactors(equality_result, &first_assignment, &second_assignment);
    mtbdd_cofactors(first_assignment, &if_false, &if_true);
    test_assert(if_false == terminal);
    test_assert(if_true == mtbdd_undefined);
    mtbdd_cofactors(second_assignment, &if_false, &if_true);
    test_assert(if_false == mtbdd_undefined);
    test_assert(if_true == terminal);

    test_assert(mtbdd_ite_CALL(lace, NULL, condition, terminal, other_terminal) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_ite_CALL(lace, &unchanged, mtbdd_invalid, terminal, other_terminal) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_cube(NULL, vars, first_cube, terminal) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_cube(&unchanged, vars, NULL, terminal) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_cube(&unchanged, vars, invalid_cube, terminal) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_cube(&unchanged, single_var, equality_cube, terminal) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_set_cube_CALL(lace, &unchanged, set_result, vars, equality_cube, terminal) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_set_cube_CALL(lace, &unchanged, set_result, vars, invalid_cube, terminal) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(17);
    return 0;
}

TASK(int, test_mtbdd_structure_destinations)
int
test_mtbdd_structure_destinations_CALL(lace_worker *lace)
{
    MTBDD terminal = mtbdd_int64(42);
    MTBDD other_terminal = mtbdd_invalid;
    BDD x0 = mtbdd_invalid;
    BDD x1 = mtbdd_invalid;
    BDD x2 = mtbdd_invalid;
    MTBDD source = mtbdd_invalid;
    MTBDD nested = mtbdd_invalid;
    BDDSET support = mtbdd_invalid;
    MTBDD composed = mtbdd_invalid;
    BDDSET composed_support = mtbdd_invalid;
    MTBDD expected_inner = mtbdd_invalid;
    MTBDD expected = mtbdd_invalid;
    MTBDD inplace = mtbdd_invalid;
    MTBDD empty_result = mtbdd_invalid;
    MTBDD support_alias = mtbdd_invalid;
    MTBDDMAP map = mtbdd_invalid;
    MTBDDMAP map2 = mtbdd_invalid;
    MTBDD substituted = mtbdd_invalid;
    MTBDD expected_substituted = mtbdd_invalid;
    MTBDD later_source = mtbdd_invalid;
    MTBDD preserved = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&terminal);
    other_terminal = mtbdd_int64(7);
    mtbdd_refs_pushptr(&other_terminal);
    mtbdd_refs_pushptr(&x0);
    mtbdd_refs_pushptr(&x1);
    mtbdd_refs_pushptr(&x2);
    mtbdd_refs_pushptr(&source);
    mtbdd_refs_pushptr(&nested);
    mtbdd_refs_pushptr(&support);
    mtbdd_refs_pushptr(&composed);
    mtbdd_refs_pushptr(&composed_support);
    mtbdd_refs_pushptr(&expected_inner);
    mtbdd_refs_pushptr(&expected);
    mtbdd_refs_pushptr(&inplace);
    mtbdd_refs_pushptr(&empty_result);
    mtbdd_refs_pushptr(&support_alias);
    mtbdd_refs_pushptr(&map);
    mtbdd_refs_pushptr(&map2);
    mtbdd_refs_pushptr(&substituted);
    mtbdd_refs_pushptr(&expected_substituted);
    mtbdd_refs_pushptr(&later_source);
    mtbdd_refs_pushptr(&preserved);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_var_at_level(&x0, 0) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&x1, 1) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&x2, 2) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &source, x0, terminal, other_terminal) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &nested, x2, source, terminal) == SYLVAN_OK);

    test_assert(mtbdd_map_set(&map, mtbdd_map_empty(), 0, x1) == SYLVAN_OK);
    mtbdd_compose_SPAWN(lace, &composed, nested, map);
    test_assert(mtbdd_support_CALL(lace, &support, nested) == SYLVAN_OK);
    test_assert(mtbdd_compose_SYNC(lace) == SYLVAN_OK);

    mtbdd_support_SPAWN(lace, &composed_support, composed);
    test_assert(mtbdd_ite_CALL(lace, &expected_inner, x1, terminal, other_terminal) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &expected, x2, expected_inner, terminal) == SYLVAN_OK);
    test_assert(mtbdd_support_SYNC(lace) == SYLVAN_OK);
    test_assert(composed == expected);

    inplace = nested;
    test_assert(mtbdd_compose_CALL(lace, &inplace, inplace, map) == SYLVAN_OK);
    test_assert(inplace == composed);

    test_assert(mtbdd_map_set(&map2, mtbdd_map_empty(), 2, x1) == SYLVAN_OK);
    test_assert(mtbdd_compose_CALL(lace, &substituted, nested, map2) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &expected_substituted, x1, source, terminal) == SYLVAN_OK);
    test_assert(substituted == expected_substituted);

    test_assert(mtbdd_ite_CALL(lace, &later_source, x2, terminal, other_terminal) == SYLVAN_OK);
    test_assert(mtbdd_compose_CALL(lace, &preserved, later_source, map) == SYLVAN_OK);
    test_assert(preserved == later_source);
    test_assert(mtbdd_compose_CALL(lace, &empty_result, nested, mtbdd_map_empty()) == SYLVAN_OK);
    test_assert(empty_result == nested);

    support_alias = nested;
    test_assert(mtbdd_support_CALL(lace, &support_alias, support_alias) == SYLVAN_OK);
    test_assert(support_alias == support);
    test_assert(mtbdd_support_CALL(lace, &empty_result, terminal) == SYLVAN_OK);
    test_assert(empty_result == bdd_set_empty());

    sylvan_gc_CALL(lace);
    test_assert(bdd_set_count(support) == 2);
    test_assert(bdd_set_contains(support, 0));
    test_assert(!bdd_set_contains(support, 1));
    test_assert(bdd_set_contains(support, 2));
    test_assert(bdd_set_count(composed_support) == 2);
    test_assert(!bdd_set_contains(composed_support, 0));
    test_assert(bdd_set_contains(composed_support, 1));
    test_assert(bdd_set_contains(composed_support, 2));
    test_assert(mtbdd_is_valid(composed));
    test_assert(mtbdd_is_valid(substituted));

    test_assert(mtbdd_support_CALL(lace, NULL, nested) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_support_CALL(lace, &unchanged, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_compose_CALL(lace, NULL, nested, map) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_compose_CALL(lace, &unchanged, mtbdd_invalid, map) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_compose_CALL(lace, &unchanged, nested, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(22);
    return 0;
}

TASK(int, test_mtbdd_map_destinations)
int
test_mtbdd_map_destinations_CALL(lace_worker *lace)
{
    const uint32_t removed_keys[] = {1, 5};
    MTBDD one = mtbdd_int64(1);
    MTBDD two = mtbdd_invalid;
    MTBDD three = mtbdd_invalid;
    MTBDDMAP map = mtbdd_map_empty();
    MTBDDMAP original = mtbdd_invalid;
    MTBDDMAP other = mtbdd_map_empty();
    MTBDDMAP updated = mtbdd_invalid;
    MTBDDMAP inplace = mtbdd_invalid;
    MTBDDMAP removed = mtbdd_invalid;
    BDDSET remove_set = mtbdd_invalid;
    MTBDDMAP stripped = mtbdd_invalid;
    MTBDDMAP max_key_map = mtbdd_map_empty();
    MTBDDMAP unchanged = bdd_true;

    mtbdd_refs_pushptr(&one);
    two = mtbdd_int64(2);
    mtbdd_refs_pushptr(&two);
    three = mtbdd_int64(3);
    mtbdd_refs_pushptr(&three);
    mtbdd_refs_pushptr(&map);
    mtbdd_refs_pushptr(&original);
    mtbdd_refs_pushptr(&other);
    mtbdd_refs_pushptr(&updated);
    mtbdd_refs_pushptr(&inplace);
    mtbdd_refs_pushptr(&removed);
    mtbdd_refs_pushptr(&remove_set);
    mtbdd_refs_pushptr(&stripped);
    mtbdd_refs_pushptr(&max_key_map);
    mtbdd_refs_pushptr(&unchanged);

    /* Insertion order does not affect the sorted map representation. */
    test_assert(mtbdd_map_set(&map, map, 5, one) == SYLVAN_OK);
    test_assert(mtbdd_map_set(&map, map, 1, two) == SYLVAN_OK);
    test_assert(mtbdd_map_set(&map, map, 3, three) == SYLVAN_OK);
    test_assert(mtbdd_map_count(map) == 3);
    test_assert(mtbdd_map_key(map) == 1);
    test_assert(mtbdd_map_value(map) == two);
    test_assert(mtbdd_map_key(mtbdd_map_next(map)) == 3);
    test_assert(mtbdd_map_value(mtbdd_map_next(map)) == three);
    test_assert(mtbdd_map_key(mtbdd_map_next(mtbdd_map_next(map))) == 5);
    test_assert(mtbdd_map_value(mtbdd_map_next(mtbdd_map_next(map))) == one);
    original = map;

    test_assert(mtbdd_map_set(&map, map, 3, one) == SYLVAN_OK);
    test_assert(mtbdd_map_count(map) == 3);
    test_assert(mtbdd_map_value(mtbdd_map_next(map)) == one);
    test_assert(mtbdd_map_set(&map, map, 3, one) == SYLVAN_OK);

    test_assert(mtbdd_map_set(&other, other, 2, two) == SYLVAN_OK);
    test_assert(mtbdd_map_set(&other, other, 3, three) == SYLVAN_OK);
    test_assert(mtbdd_map_update(&updated, map, other) == SYLVAN_OK);
    test_assert(mtbdd_map_count(updated) == 4);
    test_assert(mtbdd_map_value(mtbdd_map_next(mtbdd_map_next(updated))) == three);
    inplace = map;
    test_assert(mtbdd_map_update(&inplace, inplace, other) == SYLVAN_OK);
    test_assert(inplace == updated);

    test_assert(mtbdd_map_remove(&removed, updated, 0) == SYLVAN_OK);
    test_assert(removed == updated);
    test_assert(mtbdd_map_remove(&removed, updated, UINT32_C(0x00ffffff)) == SYLVAN_OK);
    test_assert(removed == updated);
    test_assert(mtbdd_map_remove(&removed, updated, 2) == SYLVAN_OK);
    test_assert(mtbdd_map_count(removed) == 3);
    test_assert(!mtbdd_map_contains(removed, 2));
    test_assert(mtbdd_map_contains(removed, 3));

    test_assert(bdd_set_from_array(&remove_set, removed_keys, 2) == SYLVAN_OK);
    test_assert(mtbdd_map_remove_all(&stripped, updated, remove_set) == SYLVAN_OK);
    test_assert(mtbdd_map_count(stripped) == 2);
    test_assert(!mtbdd_map_contains(stripped, 1));
    test_assert(mtbdd_map_contains(stripped, 2));
    test_assert(mtbdd_map_contains(stripped, 3));
    test_assert(!mtbdd_map_contains(stripped, 5));
    inplace = updated;
    test_assert(mtbdd_map_remove_all(&inplace, inplace, remove_set) == SYLVAN_OK);
    test_assert(inplace == stripped);

    test_assert(mtbdd_map_set(&max_key_map, max_key_map, UINT32_C(0x00ffffff), one) == SYLVAN_OK);
    test_assert(mtbdd_map_contains(max_key_map, UINT32_C(0x00ffffff)));
    test_assert(mtbdd_map_update(&inplace, mtbdd_map_empty(), updated) == SYLVAN_OK);
    test_assert(inplace == updated);
    test_assert(mtbdd_map_update(&inplace, updated, mtbdd_map_empty()) == SYLVAN_OK);
    test_assert(inplace == updated);
    test_assert(mtbdd_map_remove(&inplace, mtbdd_map_empty(), 1) == SYLVAN_OK);
    test_assert(mtbdd_map_is_empty(inplace));
    test_assert(mtbdd_map_remove_all(&inplace, updated, bdd_set_empty()) == SYLVAN_OK);
    test_assert(inplace == updated);

    sylvan_gc_CALL(lace);
    test_assert(mtbdd_map_count(original) == 3);
    test_assert(mtbdd_map_count(updated) == 4);
    test_assert(mtbdd_map_count(stripped) == 2);
    test_assert(mtbdd_map_value(mtbdd_map_next(mtbdd_map_next(updated))) == three);

    test_assert(mtbdd_map_set(NULL, map, 1, one) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_map_set(&unchanged, mtbdd_invalid, 1, one) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_map_set(&unchanged, map, 1, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_map_set(&unchanged, map, UINT32_C(0x01000000), one) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_map_update(NULL, map, other) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_map_update(&unchanged, mtbdd_invalid, other) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_map_update(&unchanged, map, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_map_remove(NULL, map, 1) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_map_remove(&unchanged, mtbdd_invalid, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_map_remove(&unchanged, map, UINT32_C(0x01000000)) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_map_remove_all(NULL, map, remove_set) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_map_remove_all(&unchanged, mtbdd_invalid, remove_set) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_map_remove_all(&unchanged, map, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(13);
    return 0;
}

TASK(int, test_mtbdd_extrema_destinations)
int
test_mtbdd_extrema_destinations_CALL(lace_worker *lace)
{
    MTBDD int_min = mtbdd_int64(-4);
    MTBDD int_middle = mtbdd_invalid;
    MTBDD int_max = mtbdd_invalid;
    MTBDD double_low = mtbdd_invalid;
    MTBDD double_high = mtbdd_invalid;
    MTBDD fraction_low = mtbdd_invalid;
    MTBDD fraction_high = mtbdd_invalid;
    BDD x0 = mtbdd_invalid;
    BDD x1 = mtbdd_invalid;
    MTBDD int_branch = mtbdd_invalid;
    MTBDD int_dd = mtbdd_invalid;
    MTBDD double_dd = mtbdd_invalid;
    MTBDD fraction_dd = mtbdd_invalid;
    MTBDD mixed_dd = mtbdd_invalid;
    MTBDD partial_dd = mtbdd_invalid;
    MTBDD minimum = mtbdd_invalid;
    MTBDD maximum = mtbdd_invalid;
    MTBDD inplace = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&int_min);
    int_middle = mtbdd_int64(7);
    mtbdd_refs_pushptr(&int_middle);
    int_max = mtbdd_int64(42);
    mtbdd_refs_pushptr(&int_max);
    double_low = mtbdd_double(-1.5);
    mtbdd_refs_pushptr(&double_low);
    double_high = mtbdd_double(2.25);
    mtbdd_refs_pushptr(&double_high);
    fraction_low = mtbdd_fraction(-1, 2);
    mtbdd_refs_pushptr(&fraction_low);
    fraction_high = mtbdd_fraction(1, 3);
    mtbdd_refs_pushptr(&fraction_high);
    mtbdd_refs_pushptr(&x0);
    mtbdd_refs_pushptr(&x1);
    mtbdd_refs_pushptr(&int_branch);
    mtbdd_refs_pushptr(&int_dd);
    mtbdd_refs_pushptr(&double_dd);
    mtbdd_refs_pushptr(&fraction_dd);
    mtbdd_refs_pushptr(&mixed_dd);
    mtbdd_refs_pushptr(&partial_dd);
    mtbdd_refs_pushptr(&minimum);
    mtbdd_refs_pushptr(&maximum);
    mtbdd_refs_pushptr(&inplace);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_var_at_level(&x0, 0) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&x1, 1) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &int_branch, x1, int_min, int_middle) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &int_dd, x0, int_max, int_branch) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &double_dd, x0, double_high, double_low) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &fraction_dd, x1, fraction_high, fraction_low) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &mixed_dd, x0, double_high, int_min) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &partial_dd, x0, int_max, mtbdd_undefined) == SYLVAN_OK);

    mtbdd_find_min_SPAWN(lace, &minimum, int_dd);
    int maximum_status = mtbdd_find_max_CALL(lace, &maximum, int_dd);
    int minimum_status = mtbdd_find_min_SYNC(lace);
    test_assert(minimum_status == SYLVAN_OK);
    test_assert(maximum_status == SYLVAN_OK);
    test_assert(minimum == int_min);
    test_assert(maximum == int_max);

    test_assert(mtbdd_find_min_CALL(lace, &minimum, double_dd) == SYLVAN_OK);
    test_assert(mtbdd_find_max_CALL(lace, &maximum, double_dd) == SYLVAN_OK);
    test_assert(minimum == double_low);
    test_assert(maximum == double_high);
    test_assert(mtbdd_find_min_CALL(lace, &minimum, fraction_dd) == SYLVAN_OK);
    test_assert(mtbdd_find_max_CALL(lace, &maximum, fraction_dd) == SYLVAN_OK);
    test_assert(minimum == fraction_low);
    test_assert(maximum == fraction_high);

    inplace = int_dd;
    test_assert(mtbdd_find_min_CALL(lace, &inplace, inplace) == SYLVAN_OK);
    test_assert(inplace == int_min);
    test_assert(mtbdd_find_max_CALL(lace, &maximum, int_middle) == SYLVAN_OK);
    test_assert(maximum == int_middle);
    test_assert(mtbdd_find_min_CALL(lace, &minimum, mtbdd_undefined) == SYLVAN_OK);
    test_assert(minimum == mtbdd_undefined);
    test_assert(mtbdd_find_min_CALL(lace, &minimum, partial_dd) == SYLVAN_OK);
    test_assert(mtbdd_find_max_CALL(lace, &maximum, partial_dd) == SYLVAN_OK);
    test_assert(minimum == int_max);
    test_assert(maximum == int_max);

    sylvan_gc_CALL(lace);
    test_assert(inplace == int_min);
    test_assert(maximum == int_max);

    test_assert(mtbdd_find_min_CALL(lace, NULL, int_dd) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_find_max_CALL(lace, NULL, int_dd) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_find_min_CALL(lace, &unchanged, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_find_max_CALL(lace, &unchanged, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_find_min_CALL(lace, &unchanged, mixed_dd) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_find_max_CALL(lace, &unchanged, mixed_dd) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(19);
    return 0;
}

static int
test_apply_param_add(lace_worker *lace, MTBDD *destination, MTBDD *a, MTBDD *b, size_t parameter)
{
    (void)lace;
    if (!mtbdd_is_leaf(*a) || !mtbdd_is_leaf(*b)) return SYLVAN_APPLY_RECURSE;
    if (*a == mtbdd_undefined || *b == mtbdd_undefined ||
        mtbdd_leaf_type(*a) != 0 || mtbdd_leaf_type(*b) != 0) {
        return SYLVAN_ERR_INVALID;
    }
    MTBDD result = mtbdd_int64(mtbdd_leaf_int64(*a) + mtbdd_leaf_int64(*b) + (int64_t)parameter);
    if (result == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    *destination = result;
    return SYLVAN_OK;
}

static int
test_apply_unary_scale(lace_worker *lace, MTBDD *destination, MTBDD dd, size_t parameter)
{
    (void)lace;
    if (!mtbdd_is_leaf(dd)) return SYLVAN_APPLY_RECURSE;
    if (dd == mtbdd_undefined || mtbdd_leaf_type(dd) != 0) return SYLVAN_ERR_INVALID;
    MTBDD result = mtbdd_int64(mtbdd_leaf_int64(dd) * (int64_t)parameter);
    if (result == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    *destination = result;
    return SYLVAN_OK;
}

static int
test_apply_fail(lace_worker *lace, MTBDD *destination, MTBDD *a, MTBDD *b)
{
    (void)lace;
    (void)destination;
    (void)a;
    (void)b;
    return SYLVAN_ERR_CALLBACK;
}

static int
test_apply_fail_on_four(lace_worker *lace, MTBDD *destination, MTBDD *a, MTBDD *b)
{
    if (!mtbdd_is_leaf(*a) || !mtbdd_is_leaf(*b)) return SYLVAN_APPLY_RECURSE;
    if (*b != mtbdd_undefined && mtbdd_leaf_type(*b) == 0 && mtbdd_leaf_int64(*b) == 4) {
        return SYLVAN_ERR_CALLBACK;
    }
    return mtbdd_op_plus_CALL(lace, destination, a, b);
}

static int
test_apply_empty_success(lace_worker *lace, MTBDD *destination, MTBDD *a, MTBDD *b)
{
    (void)lace;
    (void)destination;
    (void)a;
    (void)b;
    return SYLVAN_OK;
}

static int
test_apply_always_recurse(lace_worker *lace, MTBDD *destination, MTBDD *a, MTBDD *b)
{
    (void)lace;
    (void)destination;
    (void)a;
    (void)b;
    return SYLVAN_APPLY_RECURSE;
}

TASK(int, test_mtbdd_apply_destinations)
int
test_mtbdd_apply_destinations_CALL(lace_worker *lace)
{
    MTBDD one = mtbdd_int64(1);
    MTBDD two = mtbdd_invalid;
    MTBDD three = mtbdd_invalid;
    MTBDD four = mtbdd_invalid;
    BDD x = mtbdd_invalid;
    MTBDD a = mtbdd_invalid;
    MTBDD b = mtbdd_invalid;
    MTBDD sum = mtbdd_invalid;
    MTBDD difference = mtbdd_invalid;
    MTBDD product = mtbdd_invalid;
    MTBDD minimum = mtbdd_invalid;
    MTBDD maximum = mtbdd_invalid;
    MTBDD negated = mtbdd_invalid;
    MTBDD indicator = mtbdd_invalid;
    MTBDD parameterized = mtbdd_invalid;
    MTBDD scaled = mtbdd_invalid;
    MTBDD inplace = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&one);
    two = mtbdd_int64(2);
    mtbdd_refs_pushptr(&two);
    three = mtbdd_int64(3);
    mtbdd_refs_pushptr(&three);
    four = mtbdd_int64(4);
    mtbdd_refs_pushptr(&four);
    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&a);
    mtbdd_refs_pushptr(&b);
    mtbdd_refs_pushptr(&sum);
    mtbdd_refs_pushptr(&difference);
    mtbdd_refs_pushptr(&product);
    mtbdd_refs_pushptr(&minimum);
    mtbdd_refs_pushptr(&maximum);
    mtbdd_refs_pushptr(&negated);
    mtbdd_refs_pushptr(&indicator);
    mtbdd_refs_pushptr(&parameterized);
    mtbdd_refs_pushptr(&scaled);
    mtbdd_refs_pushptr(&inplace);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_var_at_level(&x, 0) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &a, x, two, one) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &b, x, four, three) == SYLVAN_OK);

    mtbdd_apply_SPAWN(lace, &sum, a, b, mtbdd_op_plus_CALL);
    int product_status = mtbdd_apply_CALL(lace, &product, a, b, mtbdd_op_times_CALL);
    int sum_status = mtbdd_apply_SYNC(lace);
    test_assert(sum_status == SYLVAN_OK);
    test_assert(product_status == SYLVAN_OK);
    test_assert(mtbdd_sub(&difference, a, b) == SYLVAN_OK);
    test_assert(mtbdd_min(&minimum, a, b) == SYLVAN_OK);
    test_assert(mtbdd_max(&maximum, a, b) == SYLVAN_OK);
    test_assert(mtbdd_neg(&negated, a) == SYLVAN_OK);
    test_assert(mtbdd_zero_indicator(&indicator, a) == SYLVAN_OK);
    test_assert(mtbdd_apply_param(&parameterized, a, b, 5, test_apply_param_add, UINT64_C(0x7ffffffe)) == SYLVAN_OK);
    test_assert(mtbdd_apply_unary(&scaled, a, test_apply_unary_scale, 10) == SYLVAN_OK);

    MTBDD low, high;
    mtbdd_cofactors(sum, &low, &high);
    test_assert(mtbdd_leaf_int64(low) == 4 && mtbdd_leaf_int64(high) == 6);
    mtbdd_cofactors(product, &low, &high);
    test_assert(mtbdd_leaf_int64(low) == 3 && mtbdd_leaf_int64(high) == 8);
    test_assert(mtbdd_is_leaf(difference) && mtbdd_leaf_int64(difference) == -2);
    test_assert(minimum == a);
    test_assert(maximum == b);
    mtbdd_cofactors(negated, &low, &high);
    test_assert(mtbdd_leaf_int64(low) == -1 && mtbdd_leaf_int64(high) == -2);
    test_assert(mtbdd_is_leaf(indicator) && mtbdd_leaf_int64(indicator) == 0);
    mtbdd_cofactors(parameterized, &low, &high);
    test_assert(mtbdd_leaf_int64(low) == 9 && mtbdd_leaf_int64(high) == 11);
    mtbdd_cofactors(scaled, &low, &high);
    test_assert(mtbdd_leaf_int64(low) == 10 && mtbdd_leaf_int64(high) == 20);

    inplace = a;
    test_assert(mtbdd_add(&inplace, inplace, b) == SYLVAN_OK);
    test_assert(inplace == sum);
    sylvan_gc_CALL(lace);
    test_assert(inplace == sum);
    test_assert(mtbdd_is_valid(parameterized));
    test_assert(mtbdd_is_valid(scaled));

    test_assert(mtbdd_apply(NULL, a, b, mtbdd_op_plus_CALL) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_apply(&unchanged, mtbdd_invalid, b, mtbdd_op_plus_CALL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_apply(&unchanged, a, mtbdd_invalid, mtbdd_op_plus_CALL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_apply(&unchanged, a, b, NULL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_apply(&unchanged, a, b, test_apply_fail) == SYLVAN_ERR_CALLBACK);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_apply(&unchanged, a, b, test_apply_fail_on_four) == SYLVAN_ERR_CALLBACK);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_apply(&unchanged, one, two, test_apply_empty_success) == SYLVAN_ERR_CALLBACK);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_apply(&unchanged, one, two, test_apply_always_recurse) == SYLVAN_ERR_CALLBACK);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_apply_param(&unchanged, a, b, 0, NULL, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_apply_unary(&unchanged, a, NULL, 0) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(18);
    return 0;
}

TASK(int, test_mtbdd_threshold_destinations)
int
test_mtbdd_threshold_destinations_CALL(lace_worker *lace)
{
    MTBDD low = mtbdd_double(2.0);
    MTBDD high = mtbdd_invalid;
    MTBDD integer = mtbdd_invalid;
    BDD x = mtbdd_invalid;
    MTBDD dd = mtbdd_invalid;
    MTBDD result = mtbdd_invalid;
    MTBDD parallel_result = mtbdd_invalid;
    MTBDD inplace = mtbdd_invalid;
    MTBDD fraction_low = mtbdd_invalid;
    MTBDD fraction_high = mtbdd_invalid;
    MTBDD fraction_dd = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&low);
    high = mtbdd_double(3.0);
    mtbdd_refs_pushptr(&high);
    integer = mtbdd_int64(2);
    mtbdd_refs_pushptr(&integer);
    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&dd);
    mtbdd_refs_pushptr(&result);
    mtbdd_refs_pushptr(&parallel_result);
    mtbdd_refs_pushptr(&inplace);
    fraction_low = mtbdd_fraction(1, 2);
    mtbdd_refs_pushptr(&fraction_low);
    fraction_high = mtbdd_fraction(3, 2);
    mtbdd_refs_pushptr(&fraction_high);
    mtbdd_refs_pushptr(&fraction_dd);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_var_at_level(&x, 0) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &dd, x, high, low) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &fraction_dd, x, fraction_high, fraction_low) == SYLVAN_OK);

    mtbdd_threshold_double_SPAWN(lace, &parallel_result, dd, 2.5);
    int status = mtbdd_strict_threshold_double_CALL(lace, &result, dd, 2.5);
    int parallel_status = mtbdd_threshold_double_SYNC(lace);
    test_assert(status == SYLVAN_OK && result == x);
    test_assert(parallel_status == SYLVAN_OK && parallel_result == x);

    test_assert(mtbdd_threshold_double(&result, dd, 2.0) == SYLVAN_OK);
    test_assert(result == bdd_true);
    test_assert(mtbdd_strict_threshold_double(&result, dd, 3.0) == SYLVAN_OK);
    test_assert(result == mtbdd_undefined);
    test_assert(mtbdd_threshold_double(&result, fraction_dd, 1.0) == SYLVAN_OK);
    test_assert(result == x);

    inplace = dd;
    test_assert(mtbdd_threshold_double(&inplace, inplace, 2.5) == SYLVAN_OK);
    test_assert(inplace == x);
    sylvan_gc_CALL(lace);
    test_assert(inplace == x);

    test_assert(mtbdd_threshold_double(&result, mtbdd_undefined, 2.5) == SYLVAN_OK);
    test_assert(result == mtbdd_undefined);
    test_assert(mtbdd_threshold_double(NULL, dd, 2.5) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_threshold_double(&unchanged, mtbdd_invalid, 2.5) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_threshold_double(&unchanged, integer, 2.5) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_strict_threshold_double(&unchanged, bdd_true, 2.5) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(12);
    return 0;
}

TASK(int, test_mtbdd_equal_double_destinations)
int
test_mtbdd_equal_double_destinations_CALL(lace_worker *lace)
{
    MTBDD a_low = mtbdd_double(1.0);
    MTBDD a_high = mtbdd_invalid;
    MTBDD b_low = mtbdd_invalid;
    MTBDD b_high = mtbdd_invalid;
    MTBDD int_one = mtbdd_invalid;
    MTBDD int_two = mtbdd_invalid;
    BDD x = mtbdd_invalid;
    MTBDD a = mtbdd_invalid;
    MTBDD b = mtbdd_invalid;
    MTBDD result = mtbdd_invalid;
    MTBDD parallel_result = mtbdd_invalid;
    MTBDD inplace = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&a_low);
    a_high = mtbdd_double(2.0);
    mtbdd_refs_pushptr(&a_high);
    b_low = mtbdd_double(1.05);
    mtbdd_refs_pushptr(&b_low);
    b_high = mtbdd_double(2.05);
    mtbdd_refs_pushptr(&b_high);
    int_one = mtbdd_int64(1);
    mtbdd_refs_pushptr(&int_one);
    int_two = mtbdd_int64(2);
    mtbdd_refs_pushptr(&int_two);
    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&a);
    mtbdd_refs_pushptr(&b);
    mtbdd_refs_pushptr(&result);
    mtbdd_refs_pushptr(&parallel_result);
    mtbdd_refs_pushptr(&inplace);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_var_at_level(&x, 0) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &a, x, a_high, a_low) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &b, x, b_high, b_low) == SYLVAN_OK);

    mtbdd_equal_abs_double_SPAWN(lace, &parallel_result, a, b, 0.1);
    int status = mtbdd_equal_rel_double_CALL(lace, &result, a, b, 0.01);
    int parallel_status = mtbdd_equal_abs_double_SYNC(lace);
    test_assert(status == SYLVAN_OK && result == mtbdd_undefined);
    test_assert(parallel_status == SYLVAN_OK && parallel_result == bdd_true);

    test_assert(mtbdd_equal_abs_double(&result, a, b, 0.01) == SYLVAN_OK);
    test_assert(result == mtbdd_undefined);
    test_assert(mtbdd_equal_rel_double(&result, a, b, 0.1) == SYLVAN_OK);
    test_assert(result == bdd_true);
    test_assert(mtbdd_equal_abs_double(&result, a, a, 0.0) == SYLVAN_OK);
    test_assert(result == bdd_true);
    test_assert(mtbdd_equal_abs_double(&result, a, mtbdd_undefined, 0.1) == SYLVAN_OK);
    test_assert(result == mtbdd_undefined);

    inplace = a;
    test_assert(mtbdd_equal_abs_double(&inplace, inplace, b, 0.1) == SYLVAN_OK);
    test_assert(inplace == bdd_true);
    sylvan_gc_CALL(lace);
    test_assert(inplace == bdd_true);

    test_assert(mtbdd_equal_abs_double(NULL, a, b, 0.1) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_equal_abs_double(&unchanged, mtbdd_invalid, b, 0.1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_equal_rel_double(&unchanged, a, mtbdd_invalid, 0.1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_equal_abs_double(&unchanged, int_one, int_two, 0.1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_equal_rel_double(&unchanged, int_one, int_two, 0.1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(13);
    return 0;
}

TASK(int, test_mtbdd_order_destinations)
int
test_mtbdd_order_destinations_CALL(lace_worker *lace)
{
    MTBDD one = mtbdd_int64(1);
    MTBDD two = mtbdd_invalid;
    MTBDD three = mtbdd_invalid;
    MTBDD four = mtbdd_invalid;
    MTBDD double_one = mtbdd_invalid;
    MTBDD double_two = mtbdd_invalid;
    MTBDD fraction_one_half = mtbdd_invalid;
    MTBDD fraction_two_thirds = mtbdd_invalid;
    BDD x = mtbdd_invalid;
    MTBDD a = mtbdd_invalid;
    MTBDD b = mtbdd_invalid;
    MTBDD result = mtbdd_invalid;
    MTBDD parallel_result = mtbdd_invalid;
    MTBDD inplace = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&one);
    two = mtbdd_int64(2);
    mtbdd_refs_pushptr(&two);
    three = mtbdd_int64(3);
    mtbdd_refs_pushptr(&three);
    four = mtbdd_int64(4);
    mtbdd_refs_pushptr(&four);
    double_one = mtbdd_double(1.0);
    mtbdd_refs_pushptr(&double_one);
    double_two = mtbdd_double(2.0);
    mtbdd_refs_pushptr(&double_two);
    fraction_one_half = mtbdd_fraction(1, 2);
    mtbdd_refs_pushptr(&fraction_one_half);
    fraction_two_thirds = mtbdd_fraction(2, 3);
    mtbdd_refs_pushptr(&fraction_two_thirds);
    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&a);
    mtbdd_refs_pushptr(&b);
    mtbdd_refs_pushptr(&result);
    mtbdd_refs_pushptr(&parallel_result);
    mtbdd_refs_pushptr(&inplace);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_var_at_level(&x, 0) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &a, x, three, one) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &b, x, four, two) == SYLVAN_OK);

    mtbdd_leq_SPAWN(lace, &parallel_result, a, b);
    int status = mtbdd_gt_CALL(lace, &result, b, a);
    int parallel_status = mtbdd_leq_SYNC(lace);
    test_assert(status == SYLVAN_OK && result == bdd_true);
    test_assert(parallel_status == SYLVAN_OK && parallel_result == bdd_true);

    test_assert(mtbdd_lt(&result, a, b) == SYLVAN_OK && result == bdd_true);
    test_assert(mtbdd_geq(&result, a, b) == SYLVAN_OK && result == mtbdd_undefined);
    test_assert(mtbdd_gt(&result, a, b) == SYLVAN_OK && result == mtbdd_undefined);
    test_assert(mtbdd_leq(&result, a, a) == SYLVAN_OK && result == bdd_true);
    test_assert(mtbdd_lt(&result, a, a) == SYLVAN_OK && result == mtbdd_undefined);
    test_assert(mtbdd_geq(&result, a, a) == SYLVAN_OK && result == bdd_true);
    test_assert(mtbdd_gt(&result, a, a) == SYLVAN_OK && result == mtbdd_undefined);
    test_assert(mtbdd_lt(&result, fraction_one_half, fraction_two_thirds) == SYLVAN_OK);
    test_assert(result == bdd_true);
    test_assert(mtbdd_geq(&result, double_two, double_one) == SYLVAN_OK);
    test_assert(result == bdd_true);
    test_assert(mtbdd_leq(&result, mtbdd_undefined, one) == SYLVAN_OK);
    test_assert(result == bdd_true);

    inplace = a;
    test_assert(mtbdd_lt(&inplace, inplace, b) == SYLVAN_OK);
    test_assert(inplace == bdd_true);
    sylvan_gc_CALL(lace);
    test_assert(inplace == bdd_true);

    test_assert(mtbdd_leq(NULL, a, b) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_lt(&unchanged, mtbdd_invalid, b) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_geq(&unchanged, a, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_gt(&unchanged, one, double_one) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(15);
    return 0;
}

static int
test_abstract_fail_on_four(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    if (k == 0 && mtbdd_is_leaf(b) && b != mtbdd_undefined &&
        mtbdd_leaf_type(b) == 0 && mtbdd_leaf_int64(b) == 4) {
        return SYLVAN_ERR_IO;
    }
    return mtbdd_abstract_op_plus_CALL(lace, destination, a, b, k);
}

static int
test_abstract_empty_success(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    (void)lace;
    (void)destination;
    (void)a;
    (void)b;
    (void)k;
    return SYLVAN_OK;
}

static int
test_abstract_positive_status(lace_worker *lace, MTBDD *destination, MTBDD a, MTBDD b, int k)
{
    (void)lace;
    (void)destination;
    (void)a;
    (void)b;
    (void)k;
    return SYLVAN_APPLY_RECURSE;
}

TASK(int, test_mtbdd_abstract_destinations)
int
test_mtbdd_abstract_destinations_CALL(lace_worker *lace)
{
    const uint32_t all_levels[] = {0, 1, 2};
    const uint32_t y_level[] = {2};
    MTBDD one = mtbdd_int64(1);
    MTBDD two = mtbdd_invalid;
    MTBDD three = mtbdd_invalid;
    MTBDD four = mtbdd_invalid;
    BDD x = mtbdd_invalid;
    BDD y = mtbdd_invalid;
    MTBDD low_branch = mtbdd_invalid;
    MTBDD high_branch = mtbdd_invalid;
    MTBDD f = mtbdd_invalid;
    BDDSET all_vars = mtbdd_invalid;
    BDDSET y_vars = mtbdd_invalid;
    MTBDD sum = mtbdd_invalid;
    MTBDD product = mtbdd_invalid;
    MTBDD minimum = mtbdd_invalid;
    MTBDD maximum = mtbdd_invalid;
    MTBDD inplace = mtbdd_invalid;
    MTBDD mixed = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&one);
    two = mtbdd_int64(2);
    mtbdd_refs_pushptr(&two);
    three = mtbdd_int64(3);
    mtbdd_refs_pushptr(&three);
    four = mtbdd_int64(4);
    mtbdd_refs_pushptr(&four);
    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&y);
    mtbdd_refs_pushptr(&low_branch);
    mtbdd_refs_pushptr(&high_branch);
    mtbdd_refs_pushptr(&f);
    mtbdd_refs_pushptr(&all_vars);
    mtbdd_refs_pushptr(&y_vars);
    mtbdd_refs_pushptr(&sum);
    mtbdd_refs_pushptr(&product);
    mtbdd_refs_pushptr(&minimum);
    mtbdd_refs_pushptr(&maximum);
    mtbdd_refs_pushptr(&inplace);
    mtbdd_refs_pushptr(&mixed);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_var_at_level(&x, 0) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&y, 2) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &low_branch, y, two, one) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &high_branch, y, four, three) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &f, x, high_branch, low_branch) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&all_vars, all_levels, 3) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&y_vars, y_level, 1) == SYLVAN_OK);

    mtbdd_abstract_SPAWN(lace, &sum, f, all_vars, mtbdd_abstract_op_plus_CALL);
    int max_status = mtbdd_abstract_max(&maximum, f, all_vars);
    int sum_status = mtbdd_abstract_SYNC(lace);
    test_assert(sum_status == SYLVAN_OK);
    test_assert(max_status == SYLVAN_OK);
    test_assert(mtbdd_abstract_mul(&product, f, all_vars) == SYLVAN_OK);
    test_assert(mtbdd_abstract_min(&minimum, f, all_vars) == SYLVAN_OK);
    test_assert(mtbdd_is_leaf(sum) && mtbdd_leaf_int64(sum) == 20);
    test_assert(mtbdd_is_leaf(product) && mtbdd_leaf_int64(product) == 576);
    test_assert(mtbdd_is_leaf(minimum) && mtbdd_leaf_int64(minimum) == 1);
    test_assert(mtbdd_is_leaf(maximum) && mtbdd_leaf_int64(maximum) == 4);

    test_assert(mtbdd_abstract_add(&sum, f, y_vars) == SYLVAN_OK);
    MTBDD low, high;
    mtbdd_cofactors(sum, &low, &high);
    test_assert(mtbdd_leaf_int64(low) == 3);
    test_assert(mtbdd_leaf_int64(high) == 7);

    inplace = f;
    test_assert(mtbdd_abstract_add(&inplace, inplace, all_vars) == SYLVAN_OK);
    test_assert(mtbdd_is_leaf(inplace) && mtbdd_leaf_int64(inplace) == 20);
    sylvan_gc_CALL(lace);
    test_assert(mtbdd_is_leaf(inplace) && mtbdd_leaf_int64(inplace) == 20);

    test_assert(mtbdd_abstract(NULL, f, all_vars, mtbdd_abstract_op_plus_CALL) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_abstract(&unchanged, mtbdd_invalid, all_vars, mtbdd_abstract_op_plus_CALL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_abstract(&unchanged, f, mtbdd_invalid, mtbdd_abstract_op_plus_CALL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_abstract(&unchanged, f, all_vars, NULL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_abstract(&unchanged, f, y_vars, test_abstract_fail_on_four) == SYLVAN_ERR_IO);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_abstract(&unchanged, one, y_vars, test_abstract_empty_success) == SYLVAN_ERR_CALLBACK);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_abstract(&unchanged, one, y_vars, test_abstract_positive_status) == SYLVAN_ERR_CALLBACK);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_abstract(&unchanged, f, bdd_true, test_abstract_fail_on_four) == SYLVAN_OK);
    test_assert(unchanged == f);

    mtbdd_mul_abstract_add_SPAWN(lace, &sum, f, f, y_vars);
    max_status = mtbdd_mul_abstract_max_CALL(lace, &maximum, f, f, y_vars);
    sum_status = mtbdd_mul_abstract_add_SYNC(lace);
    test_assert(sum_status == SYLVAN_OK);
    test_assert(max_status == SYLVAN_OK);
    mtbdd_cofactors(sum, &low, &high);
    test_assert(mtbdd_leaf_int64(low) == 5 && mtbdd_leaf_int64(high) == 25);
    mtbdd_cofactors(maximum, &low, &high);
    test_assert(mtbdd_leaf_int64(low) == 4 && mtbdd_leaf_int64(high) == 16);

    test_assert(mtbdd_mul_abstract_add(&product, f, f, all_vars) == SYLVAN_OK);
    test_assert(mtbdd_is_leaf(product) && mtbdd_leaf_int64(product) == 60);
    test_assert(mtbdd_mul_abstract_max(&maximum, f, f, all_vars) == SYLVAN_OK);
    test_assert(mtbdd_is_leaf(maximum) && mtbdd_leaf_int64(maximum) == 16);
    test_assert(mtbdd_mul(&minimum, f, f) == SYLVAN_OK);
    inplace = f;
    test_assert(mtbdd_mul_abstract_add(&inplace, inplace, f, bdd_true) == SYLVAN_OK);
    test_assert(inplace == minimum);
    test_assert(mtbdd_mul_abstract_add(&product, two, three, y_vars) == SYLVAN_OK);
    test_assert(mtbdd_is_leaf(product) && mtbdd_leaf_int64(product) == 12);

    test_assert(mtbdd_mul_abstract_add(NULL, f, f, y_vars) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_mul_abstract_add(&unchanged, mtbdd_invalid, f, y_vars) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == f);
    test_assert(mtbdd_mul_abstract_max(&unchanged, f, mtbdd_invalid, y_vars) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == f);
    test_assert(mtbdd_mul_abstract_add(&unchanged, f, f, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == f);
    mixed = mtbdd_double(2.0);
    test_assert(mtbdd_mul_abstract_add(&unchanged, f, mixed, y_vars) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == f);

    sylvan_gc_CALL(lace);
    test_assert(mtbdd_is_leaf(product) && mtbdd_leaf_int64(product) == 12);
    mtbdd_refs_popptr(18);
    return 0;
}

static int
test_eval_compose_square(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    return mtbdd_apply_CALL(lace, destination, dd, dd, mtbdd_op_times_CALL);
}

static int
test_eval_compose_fail_on_four(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    (void)lace;
    if (mtbdd_is_leaf(dd) && dd != mtbdd_undefined &&
        mtbdd_leaf_type(dd) == 0 && mtbdd_leaf_int64(dd) == 4) {
        return SYLVAN_ERR_IO;
    }
    *destination = dd;
    return SYLVAN_OK;
}

static int
test_eval_compose_empty_success(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    (void)lace;
    (void)destination;
    (void)dd;
    return SYLVAN_OK;
}

static int
test_eval_compose_positive_status(lace_worker *lace, MTBDD *destination, MTBDD dd)
{
    (void)lace;
    (void)destination;
    (void)dd;
    return SYLVAN_APPLY_RECURSE;
}

TASK(int, test_mtbdd_eval_compose_destinations)
int
test_mtbdd_eval_compose_destinations_CALL(lace_worker *lace)
{
    const uint32_t all_levels[] = {0, 1};
    const uint32_t x0_level[] = {0};
    const uint32_t x1_level[] = {1};
    MTBDD one = mtbdd_int64(1);
    MTBDD two = mtbdd_invalid;
    MTBDD three = mtbdd_invalid;
    MTBDD four = mtbdd_invalid;
    BDD x0 = mtbdd_invalid;
    BDD x1 = mtbdd_invalid;
    MTBDD low_branch = mtbdd_invalid;
    MTBDD high_branch = mtbdd_invalid;
    MTBDD dd = mtbdd_invalid;
    BDDSET all_vars = mtbdd_invalid;
    BDDSET x0_vars = mtbdd_invalid;
    BDDSET x1_vars = mtbdd_invalid;
    MTBDD expected = mtbdd_invalid;
    MTBDD result = mtbdd_invalid;
    MTBDD parallel_result = mtbdd_invalid;
    MTBDD inplace = mtbdd_invalid;
    MTBDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&one);
    two = mtbdd_int64(2);
    mtbdd_refs_pushptr(&two);
    three = mtbdd_int64(3);
    mtbdd_refs_pushptr(&three);
    four = mtbdd_int64(4);
    mtbdd_refs_pushptr(&four);
    mtbdd_refs_pushptr(&x0);
    mtbdd_refs_pushptr(&x1);
    mtbdd_refs_pushptr(&low_branch);
    mtbdd_refs_pushptr(&high_branch);
    mtbdd_refs_pushptr(&dd);
    mtbdd_refs_pushptr(&all_vars);
    mtbdd_refs_pushptr(&x0_vars);
    mtbdd_refs_pushptr(&x1_vars);
    mtbdd_refs_pushptr(&expected);
    mtbdd_refs_pushptr(&result);
    mtbdd_refs_pushptr(&parallel_result);
    mtbdd_refs_pushptr(&inplace);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_var_at_level(&x0, 0) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&x1, 1) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &low_branch, x1, two, one) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &high_branch, x1, four, three) == SYLVAN_OK);
    test_assert(mtbdd_ite_CALL(lace, &dd, x0, high_branch, low_branch) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&all_vars, all_levels, 2) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&x0_vars, x0_level, 1) == SYLVAN_OK);
    test_assert(bdd_set_from_array(&x1_vars, x1_level, 1) == SYLVAN_OK);
    test_assert(mtbdd_mul(&expected, dd, dd) == SYLVAN_OK);

    mtbdd_eval_compose_SPAWN(lace, &parallel_result, dd, all_vars, test_eval_compose_square);
    int status = mtbdd_eval_compose_CALL(lace, &result, dd, x0_vars, test_eval_compose_square);
    int parallel_status = mtbdd_eval_compose_SYNC(lace);
    test_assert(status == SYLVAN_OK && result == expected);
    test_assert(parallel_status == SYLVAN_OK && parallel_result == expected);
    test_assert(mtbdd_eval_compose(&result, dd, bdd_true, test_eval_compose_square) == SYLVAN_OK);
    test_assert(result == expected);

    inplace = dd;
    test_assert(mtbdd_eval_compose(&inplace, inplace, all_vars, test_eval_compose_square) == SYLVAN_OK);
    test_assert(inplace == expected);
    sylvan_gc_CALL(lace);
    test_assert(inplace == expected);

    test_assert(mtbdd_mul(&expected, low_branch, low_branch) == SYLVAN_OK);
    test_assert(mtbdd_eval_compose(&result, low_branch, all_vars, test_eval_compose_square) == SYLVAN_OK);
    test_assert(result == expected);

    test_assert(mtbdd_eval_compose(NULL, dd, all_vars, test_eval_compose_square) == SYLVAN_ERR_INVALID);
    test_assert(mtbdd_eval_compose(&unchanged, mtbdd_invalid, all_vars, test_eval_compose_square) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_eval_compose(&unchanged, dd, mtbdd_invalid, test_eval_compose_square) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_eval_compose(&unchanged, dd, all_vars, NULL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_eval_compose(&unchanged, dd, x1_vars, test_eval_compose_square) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_eval_compose(&unchanged, dd, all_vars, test_eval_compose_fail_on_four) == SYLVAN_ERR_IO);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_eval_compose(&unchanged, one, bdd_true, test_eval_compose_empty_success) == SYLVAN_ERR_CALLBACK);
    test_assert(unchanged == bdd_true);
    test_assert(mtbdd_eval_compose(&unchanged, one, bdd_true, test_eval_compose_positive_status) == SYLVAN_ERR_CALLBACK);
    test_assert(unchanged == bdd_true);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(17);
    return 0;
}

static BDD
test_bdd_binary(test_bdd_binary_op op, BDD a, BDD b)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = op(&result, a, b);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD test_bdd_and(BDD a, BDD b) { return test_bdd_binary(bdd_and, a, b); }
static BDD test_bdd_xor(BDD a, BDD b) { return test_bdd_binary(bdd_xor, a, b); }
static BDD test_bdd_xnor(BDD a, BDD b) { return test_bdd_binary(bdd_xnor, a, b); }
static BDD test_bdd_or(BDD a, BDD b) { return test_bdd_binary(bdd_or, a, b); }
static BDD test_bdd_nand(BDD a, BDD b) { return test_bdd_binary(bdd_nand, a, b); }
static BDD test_bdd_nor(BDD a, BDD b) { return test_bdd_binary(bdd_nor, a, b); }
static BDD test_bdd_imp(BDD a, BDD b) { return test_bdd_binary(bdd_imp, a, b); }
static BDD test_bdd_diff(BDD a, BDD b) { return test_bdd_binary(bdd_diff, a, b); }
static BDD test_bdd_cofactor(BDD f, BDD cube) { return test_bdd_binary(bdd_cofactor, f, cube); }
static BDD test_bdd_restrict(BDD f, BDD c) { return test_bdd_binary(bdd_restrict, f, c); }

static BDD
test_bdd_unary_set(test_bdd_unary_set_op op, BDD dd, BDDSET vars)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = op(&result, dd, vars);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD
test_bdd_binary_set(test_bdd_binary_set_op op, BDD a, BDD b, BDDSET vars)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = op(&result, a, b, vars);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD test_bdd_exists(BDD dd, BDDSET vars) { return test_bdd_unary_set(bdd_exists, dd, vars); }
static BDD test_bdd_forall(BDD dd, BDDSET vars) { return test_bdd_unary_set(bdd_forall, dd, vars); }
static BDD test_bdd_project(BDD dd, BDDSET vars) { return test_bdd_unary_set(bdd_project, dd, vars); }
static BDD test_bdd_and_exists(BDD a, BDD b, BDDSET vars) { return test_bdd_binary_set(bdd_and_exists, a, b, vars); }
static BDD test_bdd_and_project(BDD a, BDD b, BDDSET vars) { return test_bdd_binary_set(bdd_and_project, a, b, vars); }
static BDD test_bdd_rel_prev(BDD a, BDD b, BDDSET vars) { return test_bdd_binary_set(bdd_rel_prev, a, b, vars); }
static BDD test_bdd_rel_next(BDD a, BDD b, BDDSET vars) { return test_bdd_binary_set(bdd_rel_next, a, b, vars); }

static BDD
test_bdd_compose(BDD dd, MTBDDMAP map)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_compose(&result, dd, map);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD
test_bdd_cube(BDDSET vars, const uint8_t *cube)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_cube(&result, vars, cube);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD
test_bdd_or_cube(BDD dd, BDDSET vars, const uint8_t *cube)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    int status = bdd_or_cube(&result, dd, vars, cube);
    mtbdd_unprotect(&result);
    return status == SYLVAN_OK ? result : mtbdd_invalid;
}

static BDD
test_bdd_pick_cube(BDD dd, BDDSET vars)
{
    return test_bdd_unary_set(bdd_pick_cube, dd, vars);
}

static BDD
test_bdd_pick_minterm(BDD dd, BDDSET vars)
{
    return test_bdd_unary_set(bdd_pick_minterm, dd, vars);
}

static BDD
test_bdd_ite(BDD a, BDD b, BDD c)
{
    BDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    test_assert(bdd_ite(&result, a, b, c) == SYLVAN_OK);
    mtbdd_unprotect(&result);
    return result;
}

TASK(int, test_protected_destinations)
int
test_protected_destinations_CALL(lace_worker *lace)
{
    BDD a = test_bdd_var(0);
    BDD b = test_bdd_var(1);
    BDD c = test_bdd_var(2);
    BDD and_result = mtbdd_invalid;
    BDD ite_result = mtbdd_invalid;
    BDD xor_result = mtbdd_invalid;
    BDD expected_and = mtbdd_make_node(0, bdd_false, b);
    BDD expected_ite = mtbdd_make_node(0, c, b);
    BDD expected_xor = mtbdd_make_node(0, b, bdd_not(b));

    mtbdd_refs_pushptr(&a);
    mtbdd_refs_pushptr(&b);
    mtbdd_refs_pushptr(&c);
    mtbdd_refs_pushptr(&and_result);
    mtbdd_refs_pushptr(&ite_result);
    mtbdd_refs_pushptr(&xor_result);
    mtbdd_refs_pushptr(&expected_and);
    mtbdd_refs_pushptr(&expected_ite);
    mtbdd_refs_pushptr(&expected_xor);

    BDD pending = mtbdd_invalid;
    mtbdd_refs_pushptr(&pending);
    sylvan_gc_CALL(lace);
    test_assert(pending == mtbdd_invalid);
    test_assert(test_bdd_var(1) == b);

    bdd_and_SPAWN(lace, &and_result, a, b);
    bdd_xor_SPAWN(lace, &xor_result, a, b);
    int ite_status = bdd_ite_CALL(lace, &ite_result, a, b, c);
    int xor_status = bdd_xor_SYNC(lace);
    int and_status = bdd_and_SYNC(lace);

    test_assert(and_status == SYLVAN_OK);
    test_assert(ite_status == SYLVAN_OK);
    test_assert(xor_status == SYLVAN_OK);

    /* The task results must already be rooted when collection starts. */
    sylvan_gc_CALL(lace);
    test_assert(and_result == expected_and);
    test_assert(ite_result == expected_ite);
    test_assert(xor_result == expected_xor);

    BDD unchanged = bdd_true;
    mtbdd_refs_pushptr(&unchanged);
    test_assert(bdd_and_CALL(lace, &unchanged, mtbdd_invalid, b) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_ite_CALL(lace, &unchanged, a, b, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_xor_CALL(lace, &unchanged, mtbdd_invalid, b) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);

    test_bdd_binary_op derived_ops[] = {
        bdd_xnor, bdd_or, bdd_nand, bdd_nor, bdd_imp, bdd_diff
    };
    for (size_t i = 0; i < sizeof(derived_ops) / sizeof(derived_ops[0]); i++) {
        test_assert(derived_ops[i](&unchanged, mtbdd_invalid, b) == SYLVAN_ERR_INVALID);
        test_assert(unchanged == bdd_true);
        test_assert(derived_ops[i](NULL, a, b) == SYLVAN_ERR_INVALID);
    }

    test_assert(bdd_and_CALL(lace, NULL, a, b) == SYLVAN_ERR_INVALID);
    test_assert(bdd_ite_CALL(lace, NULL, a, b, c) == SYLVAN_ERR_INVALID);
    test_assert(bdd_xor_CALL(lace, NULL, a, b) == SYLVAN_ERR_INVALID);

    mtbdd_refs_popptr(11);
    return 0;
}

TASK(int, test_quantification_destinations)
int
test_quantification_destinations_CALL(lace_worker *lace)
{
    BDD a = test_bdd_var(0);
    BDD b = test_bdd_var(1);
    BDD conjunction = mtbdd_invalid;
    BDD disjoint_constraint = mtbdd_invalid;
    BDD exists_result = mtbdd_invalid;
    BDD forall_result = mtbdd_invalid;
    BDD project_result = mtbdd_invalid;
    BDD and_exists_result = mtbdd_invalid;
    BDD and_project_result = mtbdd_invalid;

    mtbdd_refs_pushptr(&a);
    mtbdd_refs_pushptr(&b);
    mtbdd_refs_pushptr(&conjunction);
    mtbdd_refs_pushptr(&disjoint_constraint);
    mtbdd_refs_pushptr(&exists_result);
    mtbdd_refs_pushptr(&forall_result);
    mtbdd_refs_pushptr(&project_result);
    mtbdd_refs_pushptr(&and_exists_result);
    mtbdd_refs_pushptr(&and_project_result);

    test_assert(bdd_and_CALL(lace, &conjunction, a, b) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &disjoint_constraint, bdd_not(a), b) == SYLVAN_OK);

    bdd_exists_SPAWN(lace, &exists_result, conjunction, a);
    bdd_project_SPAWN(lace, &project_result, conjunction, a);
    bdd_and_exists_SPAWN(lace, &and_exists_result, a, b, a);
    int and_project_status = bdd_and_project_CALL(lace, &and_project_result, a, b, a);
    int and_exists_status = bdd_and_exists_SYNC(lace);
    int project_status = bdd_project_SYNC(lace);
    int exists_status = bdd_exists_SYNC(lace);
    int forall_status = bdd_forall(&forall_result, conjunction, a);

    test_assert(exists_status == SYLVAN_OK);
    test_assert(forall_status == SYLVAN_OK);
    test_assert(project_status == SYLVAN_OK);
    test_assert(and_exists_status == SYLVAN_OK);
    test_assert(and_project_status == SYLVAN_OK);

    sylvan_gc_CALL(lace);
    test_assert(exists_result == b);
    test_assert(forall_result == bdd_false);
    test_assert(project_result == a);
    test_assert(and_exists_result == b);
    test_assert(and_project_result == a);

    BDD unchanged = bdd_true;
    mtbdd_refs_pushptr(&unchanged);

    test_bdd_unary_set_op unary_ops[] = {bdd_exists, bdd_forall, bdd_project};
    for (size_t i = 0; i < sizeof(unary_ops) / sizeof(unary_ops[0]); i++) {
        test_assert(unary_ops[i](&unchanged, mtbdd_invalid, a) == SYLVAN_ERR_INVALID);
        test_assert(unchanged == bdd_true);
        test_assert(unary_ops[i](&unchanged, a, mtbdd_invalid) == SYLVAN_ERR_INVALID);
        test_assert(unchanged == bdd_true);
        test_assert(unary_ops[i](NULL, a, a) == SYLVAN_ERR_INVALID);
    }

    test_bdd_binary_set_op binary_ops[] = {bdd_and_exists, bdd_and_project};
    for (size_t i = 0; i < sizeof(binary_ops) / sizeof(binary_ops[0]); i++) {
        test_assert(binary_ops[i](&unchanged, mtbdd_invalid, b, a) == SYLVAN_ERR_INVALID);
        test_assert(unchanged == bdd_true);
        test_assert(binary_ops[i](&unchanged, a, b, mtbdd_invalid) == SYLVAN_ERR_INVALID);
        test_assert(unchanged == bdd_true);
        test_assert(binary_ops[i](NULL, a, b, a) == SYLVAN_ERR_INVALID);
    }

    test_assert(bdd_and_project_CALL(lace, &unchanged, a, b, bdd_set_empty()) == SYLVAN_OK);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_and_project_CALL(lace, &unchanged, a, disjoint_constraint, bdd_set_empty()) == SYLVAN_OK);
    test_assert(unchanged == bdd_false);
    test_assert(bdd_and_project_CALL(lace, &unchanged, bdd_true, a, a) == SYLVAN_OK);
    test_assert(unchanged == a);
    test_assert(bdd_and_project_CALL(lace, &unchanged, a, bdd_true, a) == SYLVAN_OK);
    test_assert(unchanged == a);

    BDD c = test_bdd_var(2);
    BDD d = test_bdd_var(3);
    BDD xor_ab = mtbdd_invalid;
    BDD ite_abc = mtbdd_invalid;
    BDD or_abc = mtbdd_invalid;
    BDD set_bc = mtbdd_invalid;
    BDD set_ad = mtbdd_invalid;
    BDD set_abc = mtbdd_invalid;

    mtbdd_refs_pushptr(&c);
    mtbdd_refs_pushptr(&d);
    mtbdd_refs_pushptr(&xor_ab);
    mtbdd_refs_pushptr(&ite_abc);
    mtbdd_refs_pushptr(&or_abc);
    mtbdd_refs_pushptr(&set_bc);
    mtbdd_refs_pushptr(&set_ad);
    mtbdd_refs_pushptr(&set_abc);

    test_assert(bdd_xor_CALL(lace, &xor_ab, a, b) == SYLVAN_OK);
    test_assert(bdd_ite_CALL(lace, &ite_abc, a, b, c) == SYLVAN_OK);
    test_assert(bdd_ite_CALL(lace, &or_abc, c, bdd_true, conjunction) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &set_bc, b, c) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &set_ad, a, d) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &set_abc, conjunction, c) == SYLVAN_OK);

    BDD samples[] = {
        bdd_false, bdd_true,
        a, bdd_not(a), b, bdd_not(b), c, bdd_not(c),
        conjunction, disjoint_constraint, xor_ab, ite_abc, or_abc
    };
    BDDSET projection_sets[] = {
        bdd_set_empty(), a, b, c, d, conjunction, set_bc, set_ad, set_abc
    };

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        for (size_t j = 0; j < sizeof(samples) / sizeof(samples[0]); j++) {
            for (size_t k = 0; k < sizeof(projection_sets) / sizeof(projection_sets[0]); k++) {
                BDD product = mtbdd_invalid;
                BDD expected = mtbdd_invalid;
                BDD actual = mtbdd_invalid;
                mtbdd_refs_pushptr(&product);
                mtbdd_refs_pushptr(&expected);
                mtbdd_refs_pushptr(&actual);

                test_assert(bdd_and_CALL(lace, &product, samples[i], samples[j]) == SYLVAN_OK);
                test_assert(bdd_project_CALL(lace, &expected, product, projection_sets[k]) == SYLVAN_OK);
                test_assert(bdd_and_project_CALL(lace, &actual, samples[i], samples[j], projection_sets[k]) == SYLVAN_OK);
                test_assert(actual == expected);

                mtbdd_refs_popptr(3);
            }
        }
    }

    /* Leave the cache empty for the cache unit test that follows. */
    sylvan_gc_CALL(lace);

    mtbdd_refs_popptr(18);
    return 0;
}

TASK(int, test_care_destinations)
int
test_care_destinations_CALL(lace_worker *lace)
{
    BDD x = mtbdd_invalid;
    BDD y = mtbdd_invalid;
    BDD z = mtbdd_invalid;
    BDD f = mtbdd_invalid;
    BDD care = mtbdd_invalid;
    BDD cube = mtbdd_invalid;
    BDD constrain_result = mtbdd_invalid;
    BDD restrict_result = mtbdd_invalid;
    BDD cofactor_result = mtbdd_invalid;
    BDD constrain_slice = mtbdd_invalid;
    BDD source_slice = mtbdd_invalid;
    BDD restrict_slice = mtbdd_invalid;
    BDD unchanged = bdd_true;
    BDD inplace = mtbdd_invalid;

    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&y);
    mtbdd_refs_pushptr(&z);
    mtbdd_refs_pushptr(&f);
    mtbdd_refs_pushptr(&care);
    mtbdd_refs_pushptr(&cube);
    mtbdd_refs_pushptr(&constrain_result);
    mtbdd_refs_pushptr(&restrict_result);
    mtbdd_refs_pushptr(&cofactor_result);
    mtbdd_refs_pushptr(&constrain_slice);
    mtbdd_refs_pushptr(&source_slice);
    mtbdd_refs_pushptr(&restrict_slice);
    mtbdd_refs_pushptr(&unchanged);
    mtbdd_refs_pushptr(&inplace);

    x = test_bdd_var(0);
    y = test_bdd_var(1);
    z = test_bdd_var(2);

    test_assert(bdd_xor_CALL(lace, &f, x, y) == SYLVAN_OK);
    test_assert(bdd_xor_CALL(lace, &care, x, z) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &cube, x, bdd_not(z)) == SYLVAN_OK);

    bdd_constrain_SPAWN(lace, &constrain_result, f, care);
    bdd_restrict_SPAWN(lace, &restrict_result, f, care);
    int cofactor_status = bdd_cofactor(&cofactor_result, f, x);
    int restrict_status = bdd_restrict_SYNC(lace);
    int constrain_status = bdd_constrain_SYNC(lace);

    test_assert(constrain_status == SYLVAN_OK);
    test_assert(restrict_status == SYLVAN_OK);
    test_assert(cofactor_status == SYLVAN_OK);

    sylvan_gc_CALL(lace);
    test_assert(cofactor_result == bdd_not(y));
    test_assert(bdd_and_CALL(lace, &constrain_slice, constrain_result, care) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &source_slice, f, care) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &restrict_slice, restrict_result, care) == SYLVAN_OK);
    test_assert(constrain_slice == source_slice);
    test_assert(restrict_slice == source_slice);
    test_assert(mtbdd_node_count(restrict_result) <= mtbdd_node_count(f));

    inplace = f;
    test_assert(bdd_constrain_CALL(lace, &inplace, inplace, care) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &constrain_slice, inplace, care) == SYLVAN_OK);
    test_assert(constrain_slice == source_slice);

    test_bdd_binary_op care_ops[] = {bdd_constrain, bdd_cofactor, bdd_restrict};
    for (size_t i = 0; i < sizeof(care_ops) / sizeof(care_ops[0]); i++) {
        test_assert(care_ops[i](&unchanged, mtbdd_invalid, care) == SYLVAN_ERR_INVALID);
        test_assert(unchanged == bdd_true);
        test_assert(care_ops[i](&unchanged, f, mtbdd_invalid) == SYLVAN_ERR_INVALID);
        test_assert(unchanged == bdd_true);
        test_assert(care_ops[i](NULL, f, care) == SYLVAN_ERR_INVALID);
    }

    test_assert(bdd_cofactor(&unchanged, f, care) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_cofactor(&unchanged, f, bdd_false) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_cofactor(&unchanged, f, bdd_true) == SYLVAN_OK);
    test_assert(unchanged == f);
    test_assert(bdd_cofactor(&unchanged, f, cube) == SYLVAN_OK);
    test_assert(unchanged == bdd_not(y));

    BDD samples[] = {
        bdd_false, bdd_true,
        x, bdd_not(x), y, bdd_not(y), z, bdd_not(z),
        f, care, cube
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        for (size_t j = 0; j < sizeof(samples) / sizeof(samples[0]); j++) {
            BDD constrained = mtbdd_invalid;
            BDD restricted = mtbdd_invalid;
            BDD result_slice = mtbdd_invalid;
            BDD expected_slice = mtbdd_invalid;
            mtbdd_refs_pushptr(&constrained);
            mtbdd_refs_pushptr(&restricted);
            mtbdd_refs_pushptr(&result_slice);
            mtbdd_refs_pushptr(&expected_slice);

            test_assert(bdd_constrain_CALL(lace, &constrained, samples[i], samples[j]) == SYLVAN_OK);
            test_assert(bdd_restrict_CALL(lace, &restricted, samples[i], samples[j]) == SYLVAN_OK);
            test_assert(bdd_and_CALL(lace, &expected_slice, samples[i], samples[j]) == SYLVAN_OK);
            test_assert(bdd_and_CALL(lace, &result_slice, constrained, samples[j]) == SYLVAN_OK);
            test_assert(result_slice == expected_slice);
            test_assert(bdd_and_CALL(lace, &result_slice, restricted, samples[j]) == SYLVAN_OK);
            test_assert(result_slice == expected_slice);
            test_assert(mtbdd_node_count(restricted) <= mtbdd_node_count(samples[i]));

            mtbdd_refs_popptr(4);
        }
    }

    /* Leave the cache empty for the cache unit test that follows. */
    sylvan_gc_CALL(lace);

    mtbdd_refs_popptr(14);
    return 0;
}

TASK(int, test_compose_destinations)
int
test_compose_destinations_CALL(lace_worker *lace)
{
    BDD x = test_bdd_var(0);
    BDD y = test_bdd_var(1);
    BDD z = test_bdd_var(2);
    BDD f = mtbdd_invalid;
    BDD expected = mtbdd_invalid;
    MTBDDMAP map = mtbdd_map_empty();
    MTBDDMAP later_map = mtbdd_map_empty();
    BDD result = mtbdd_invalid;
    BDD identity = mtbdd_invalid;
    BDD inplace = mtbdd_invalid;
    BDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&y);
    mtbdd_refs_pushptr(&z);
    mtbdd_refs_pushptr(&f);
    mtbdd_refs_pushptr(&expected);
    mtbdd_refs_pushptr(&map);
    mtbdd_refs_pushptr(&later_map);
    mtbdd_refs_pushptr(&result);
    mtbdd_refs_pushptr(&identity);
    mtbdd_refs_pushptr(&inplace);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_xor_CALL(lace, &f, x, y) == SYLVAN_OK);
    test_assert(bdd_xor_CALL(lace, &expected, z, y) == SYLVAN_OK);
    test_assert(mtbdd_map_set(&map, map, 0, z) == SYLVAN_OK);

    bdd_compose_SPAWN(lace, &result, f, map);
    int identity_status = bdd_compose_CALL(lace, &identity, f, mtbdd_map_empty());
    int compose_status = bdd_compose_SYNC(lace);
    test_assert(compose_status == SYLVAN_OK);
    test_assert(identity_status == SYLVAN_OK);
    test_assert(identity == f);

    /* A map whose first key comes after the support must rebuild f unchanged. */
    test_assert(mtbdd_map_set(&later_map, later_map, 2, x) == SYLVAN_OK);
    test_assert(bdd_compose_CALL(lace, &identity, f, later_map) == SYLVAN_OK);

    sylvan_gc_CALL(lace);
    test_assert(result == expected);
    test_assert(identity == f);

    inplace = f;
    test_assert(bdd_compose_CALL(lace, &inplace, inplace, map) == SYLVAN_OK);
    test_assert(inplace == expected);

    test_assert(bdd_compose_CALL(lace, &unchanged, mtbdd_invalid, map) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_compose_CALL(lace, &unchanged, f, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_compose_CALL(lace, NULL, f, map) == SYLVAN_ERR_INVALID);

    /* Leave the cache empty for the cache unit test that follows. */
    sylvan_gc_CALL(lace);

    mtbdd_refs_popptr(11);
    return 0;
}

TASK(int, test_cube_destinations)
int
test_cube_destinations_CALL(lace_worker *lace)
{
    BDD x = test_bdd_var(0);
    BDD y = test_bdd_var(1);
    BDD z = test_bdd_var(2);
    BDD xy = mtbdd_invalid;
    BDDSET vars = mtbdd_invalid;
    BDD cube_result = mtbdd_invalid;
    BDD union_result = mtbdd_invalid;
    BDD expected_union = mtbdd_invalid;
    BDD picked = mtbdd_invalid;
    BDD minterm = mtbdd_invalid;
    BDD expected_minterm = mtbdd_invalid;
    BDD unchanged = bdd_true;
    const uint8_t path_values[] = {0, 1, 2};
    const uint8_t minterm_values[] = {0, 1, 0};

    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&y);
    mtbdd_refs_pushptr(&z);
    mtbdd_refs_pushptr(&xy);
    mtbdd_refs_pushptr(&vars);
    mtbdd_refs_pushptr(&cube_result);
    mtbdd_refs_pushptr(&union_result);
    mtbdd_refs_pushptr(&expected_union);
    mtbdd_refs_pushptr(&picked);
    mtbdd_refs_pushptr(&minterm);
    mtbdd_refs_pushptr(&expected_minterm);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_and_CALL(lace, &xy, x, y) == SYLVAN_OK);
    test_assert(bdd_and_CALL(lace, &vars, xy, z) == SYLVAN_OK);

    bdd_cube_SPAWN(lace, &cube_result, vars, path_values);
    int minterm_cube_status = bdd_cube_CALL(lace, &expected_minterm, vars, minterm_values);
    int cube_status = bdd_cube_SYNC(lace);
    test_assert(cube_status == SYLVAN_OK);
    test_assert(minterm_cube_status == SYLVAN_OK);

    bdd_or_cube_SPAWN(lace, &union_result, z, vars, path_values);
    int pick_status = bdd_pick_cube_CALL(lace, &picked, cube_result, vars);
    int minterm_status = bdd_pick_minterm_CALL(lace, &minterm, cube_result, vars);
    int union_status = bdd_or_cube_SYNC(lace);
    test_assert(union_status == SYLVAN_OK);
    test_assert(pick_status == SYLVAN_OK);
    test_assert(minterm_status == SYLVAN_OK);
    test_assert(bdd_or(&expected_union, z, cube_result) == SYLVAN_OK);

    sylvan_gc_CALL(lace);
    test_assert(union_result == expected_union);
    test_assert(picked == cube_result);
    test_assert(minterm == expected_minterm);

    union_result = z;
    test_assert(bdd_or_cube_CALL(lace, &union_result, union_result, vars, path_values) == SYLVAN_OK);
    test_assert(union_result == expected_union);

    const uint8_t invalid_values[] = {0, 3, 2};
    test_assert(bdd_cube_CALL(lace, &unchanged, vars, invalid_values) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_cube_CALL(lace, &unchanged, mtbdd_invalid, path_values) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_cube_CALL(lace, &unchanged, vars, NULL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_cube_CALL(lace, NULL, vars, path_values) == SYLVAN_ERR_INVALID);

    test_assert(bdd_or_cube_CALL(lace, &unchanged, mtbdd_invalid, vars, path_values) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_or_cube_CALL(lace, &unchanged, z, mtbdd_invalid, path_values) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_or_cube_CALL(lace, &unchanged, z, vars, invalid_values) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_or_cube_CALL(lace, NULL, z, vars, path_values) == SYLVAN_ERR_INVALID);

    test_assert(bdd_pick_cube_CALL(lace, &unchanged, mtbdd_invalid, vars) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_pick_minterm_CALL(lace, &unchanged, cube_result, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_pick_cube_CALL(lace, NULL, cube_result, vars) == SYLVAN_ERR_INVALID);
    test_assert(bdd_pick_minterm_CALL(lace, &unchanged, bdd_false, vars) == SYLVAN_OK);
    test_assert(unchanged == bdd_false);

    sylvan_gc_CALL(lace);

    mtbdd_refs_popptr(12);
    return 0;
}

TASK(int, test_relational_destinations)
int
test_relational_destinations_CALL(lace_worker *lace)
{
    uint32_t state_vars[] = {0, 2, 4};
    uint32_t all_vars[] = {0, 1, 2, 3, 4, 5};
    const uint8_t transition_1[] = {0, 1, 0, 1, 0, 1};
    const uint8_t transition_2[] = {1, 0, 2, 0, 2, 0};
    const uint8_t transition_3[] = {2, 0, 1, 0, 2, 0};
    const uint8_t transition_4[] = {2, 0, 2, 0, 1, 0};
    const uint8_t state_values[] = {0, 0, 1};
    const uint8_t zero_values[] = {0, 0, 0};

    BDDSET state_set = test_bdd_set_from_levels(state_vars, 3);
    BDDSET all_set = test_bdd_set_from_levels(all_vars, 6);
    BDD transition = bdd_false;
    BDD state = mtbdd_invalid;
    BDD zeroes = mtbdd_invalid;
    BDD next = mtbdd_invalid;
    BDD prev = mtbdd_invalid;
    BDD closure = mtbdd_invalid;
    BDD closure_again = mtbdd_invalid;
    BDD in_place = mtbdd_invalid;
    BDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&state_set);
    mtbdd_refs_pushptr(&all_set);
    mtbdd_refs_pushptr(&transition);
    mtbdd_refs_pushptr(&state);
    mtbdd_refs_pushptr(&zeroes);
    mtbdd_refs_pushptr(&next);
    mtbdd_refs_pushptr(&prev);
    mtbdd_refs_pushptr(&closure);
    mtbdd_refs_pushptr(&closure_again);
    mtbdd_refs_pushptr(&in_place);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_or_cube_CALL(lace, &transition, transition, all_set, transition_1) == SYLVAN_OK);
    test_assert(bdd_or_cube_CALL(lace, &transition, transition, all_set, transition_2) == SYLVAN_OK);
    test_assert(bdd_or_cube_CALL(lace, &transition, transition, all_set, transition_3) == SYLVAN_OK);
    test_assert(bdd_or_cube_CALL(lace, &transition, transition, all_set, transition_4) == SYLVAN_OK);
    test_assert(bdd_cube_CALL(lace, &state, state_set, state_values) == SYLVAN_OK);
    test_assert(bdd_cube_CALL(lace, &zeroes, state_set, zero_values) == SYLVAN_OK);

    bdd_rel_next_SPAWN(lace, &next, state, transition, all_set);
    int terminal_status = bdd_rel_prev_CALL(lace, &prev, bdd_true, bdd_true, all_set);
    int next_status = bdd_rel_next_SYNC(lace);
    test_assert(terminal_status == SYLVAN_OK);
    test_assert(next_status == SYLVAN_OK);
    test_assert(prev == bdd_true);
    test_assert(next == zeroes);

    test_assert(bdd_rel_prev_CALL(lace, &prev, transition, next, all_set) == SYLVAN_OK);
    test_assert(prev == bdd_not(zeroes));
    test_assert(bdd_transitive_closure_CALL(lace, &closure, transition) == SYLVAN_OK);
    test_assert(bdd_transitive_closure_CALL(lace, &closure_again, closure) == SYLVAN_OK);
    test_assert(closure_again == closure);

    sylvan_gc_CALL(lace);
    test_assert(next == zeroes);
    test_assert(prev == bdd_not(zeroes));
    test_assert(closure_again == closure);

    in_place = state;
    test_assert(bdd_rel_next_CALL(lace, &in_place, in_place, transition, all_set) == SYLVAN_OK);
    test_assert(in_place == zeroes);

    test_assert(bdd_rel_next_CALL(lace, &unchanged, mtbdd_invalid, transition, all_set) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_rel_prev_CALL(lace, &unchanged, transition, next, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_transitive_closure_CALL(lace, &unchanged, mtbdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_rel_next_CALL(lace, NULL, state, transition, all_set) == SYLVAN_ERR_INVALID);
    test_assert(bdd_rel_prev_CALL(lace, NULL, transition, state, all_set) == SYLVAN_ERR_INVALID);
    test_assert(bdd_transitive_closure_CALL(lace, NULL, transition) == SYLVAN_ERR_INVALID);

    /* Leave the cache empty for the cache unit test that follows. */
    sylvan_gc_CALL(lace);

    mtbdd_refs_popptr(11);
    return 0;
}

static BDD
test_map_reduce_select(void *context, uint8_t *cube)
{
    return cube[0] ? *(BDD*)context : bdd_false;
}

static BDD
test_map_reduce_fail(void *context, uint8_t *cube)
{
    (void)context;
    (void)cube;
    return mtbdd_invalid;
}

TASK(int, test_map_reduce_destinations)
int
test_map_reduce_destinations_CALL(lace_worker *lace)
{
    BDD x = test_bdd_var(0);
    BDD y = test_bdd_var(1);
    BDD value = test_bdd_var(2);
    BDDSET vars = mtbdd_invalid;
    BDD result = mtbdd_invalid;
    BDD terminal = mtbdd_invalid;
    BDD in_place = bdd_true;
    BDD unchanged = bdd_true;

    mtbdd_refs_pushptr(&x);
    mtbdd_refs_pushptr(&y);
    mtbdd_refs_pushptr(&value);
    mtbdd_refs_pushptr(&vars);
    mtbdd_refs_pushptr(&result);
    mtbdd_refs_pushptr(&terminal);
    mtbdd_refs_pushptr(&in_place);
    mtbdd_refs_pushptr(&unchanged);

    test_assert(bdd_and_CALL(lace, &vars, x, y) == SYLVAN_OK);

    bdd_map_reduce_or_SPAWN(lace, &result, bdd_true, vars, test_map_reduce_select, &value);
    int terminal_status = bdd_map_reduce_or_CALL(lace, &terminal, bdd_false, vars, test_map_reduce_select, &value);
    int result_status = bdd_map_reduce_or_SYNC(lace);
    test_assert(terminal_status == SYLVAN_OK);
    test_assert(result_status == SYLVAN_OK);
    test_assert(terminal == bdd_false);
    test_assert(result == value);

    sylvan_gc_CALL(lace);
    test_assert(result == value);

    test_assert(bdd_map_reduce_or(&in_place, in_place, vars, test_map_reduce_select, &value) == SYLVAN_OK);
    test_assert(in_place == value);

    test_assert(bdd_map_reduce_or_CALL(lace, &unchanged, bdd_true, vars, test_map_reduce_fail, NULL) == SYLVAN_ERR_CALLBACK);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_map_reduce_or_CALL(lace, &unchanged, mtbdd_invalid, vars, test_map_reduce_select, &value) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_map_reduce_or_CALL(lace, &unchanged, bdd_true, mtbdd_invalid, test_map_reduce_select, &value) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_map_reduce_or_CALL(lace, &unchanged, bdd_true, vars, NULL, &value) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == bdd_true);
    test_assert(bdd_map_reduce_or_CALL(lace, NULL, bdd_true, vars, test_map_reduce_select, &value) == SYLVAN_ERR_INVALID);

    sylvan_gc_CALL(lace);
    mtbdd_refs_popptr(8);
    return 0;
}

TASK(int, test_listdd_set_destinations)
int
test_listdd_set_destinations_CALL(lace_worker *lace)
{
    LISTDD a = listdd_invalid;
    LISTDD b = listdd_invalid;
    LISTDD projection = listdd_invalid;
    LISTDD united = listdd_invalid;
    LISTDD difference = listdd_invalid;
    LISTDD intersection = listdd_invalid;
    LISTDD matched = listdd_invalid;
    LISTDD spawned_union = listdd_invalid;
    LISTDD spawned_difference = listdd_invalid;
    LISTDD in_place = listdd_invalid;
    LISTDD empty_vector = listdd_invalid;
    LISTDD relation = listdd_invalid;
    LISTDD unchanged = listdd_empty;
    LISTDD other_unchanged = listdd_empty_list;

    listdd_refs_pushptr(&a);
    listdd_refs_pushptr(&b);
    listdd_refs_pushptr(&projection);
    listdd_refs_pushptr(&united);
    listdd_refs_pushptr(&difference);
    listdd_refs_pushptr(&intersection);
    listdd_refs_pushptr(&matched);
    listdd_refs_pushptr(&spawned_union);
    listdd_refs_pushptr(&spawned_difference);
    listdd_refs_pushptr(&in_place);
    listdd_refs_pushptr(&empty_vector);
    listdd_refs_pushptr(&relation);
    listdd_refs_pushptr(&unchanged);
    listdd_refs_pushptr(&other_unchanged);

    test_assert(listdd_singleton(&a, (uint32_t[]){1, 2}, 2) == SYLVAN_OK);
    test_assert(listdd_add(&a, a, (uint32_t[]){2, 3}, 2) == SYLVAN_OK);
    test_assert(listdd_singleton(&b, (uint32_t[]){2, 3}, 2) == SYLVAN_OK);
    test_assert(listdd_add(&b, b, (uint32_t[]){4, 5}, 2) == SYLVAN_OK);
    test_assert(listdd_singleton(&projection, (uint32_t[]){1, UINT32_MAX}, 2) == SYLVAN_OK);
    test_assert(listdd_singleton(&empty_vector, NULL, 0) == SYLVAN_OK);
    test_assert(empty_vector == listdd_empty_list);
    LISTDD original_a = a;
    test_assert(listdd_add(&a, a, (uint32_t[]){1, 2}, 2) == SYLVAN_OK);
    test_assert(a == original_a);

    test_assert(listdd_relation_singleton(&relation, (uint32_t[]){3, 0, 7}, (int[]){0, 1, 0}, 3) == SYLVAN_OK);
    test_assert(listdd_relation_contains(relation, (uint32_t[]){3, 0, 7}, (int[]){0, 1, 0}, 3));
    LISTDD original_relation = relation;
    test_assert(listdd_relation_add(&relation, relation, (uint32_t[]){3, 0, 7}, (int[]){0, 1, 0}, 3) == SYLVAN_OK);
    test_assert(relation == original_relation);
    test_assert(listdd_relation_add(&relation, relation, (uint32_t[]){4, 0, 8}, (int[]){0, 1, 0}, 3) == SYLVAN_OK);
    test_assert(listdd_relation_contains(relation, (uint32_t[]){4, 0, 8}, (int[]){0, 1, 0}, 3));

    listdd_union_SPAWN(lace, &spawned_union, a, b);
    test_assert(listdd_intersection_CALL(lace, &intersection, a, b) == SYLVAN_OK);
    test_assert(listdd_union_SYNC(lace) == SYLVAN_OK);
    test_assert(listdd_count(spawned_union) == 3);
    test_assert(listdd_count(intersection) == 1);
    test_assert(listdd_contains(intersection, (uint32_t[]){2, 3}, 2));

    test_assert(listdd_diff(&difference, a, b) == SYLVAN_OK);
    test_assert(listdd_count(difference) == 1);
    test_assert(listdd_contains(difference, (uint32_t[]){1, 2}, 2));

    test_assert(listdd_union_diff(&united, &spawned_difference, a, b) == SYLVAN_OK);
    test_assert(united == spawned_union);
    test_assert(listdd_count(spawned_difference) == 1);
    test_assert(listdd_contains(spawned_difference, (uint32_t[]){4, 5}, 2));

    test_assert(listdd_match(&matched, a, b, projection) == SYLVAN_OK);
    test_assert(listdd_count(matched) == 1);
    test_assert(listdd_contains(matched, (uint32_t[]){2, 3}, 2));

    in_place = a;
    test_assert(listdd_union(&in_place, in_place, b) == SYLVAN_OK);
    test_assert(in_place == spawned_union);

    sylvan_gc_CALL(lace);
    test_assert(listdd_count(united) == 3);
    test_assert(listdd_count(difference) == 1);
    test_assert(listdd_count(intersection) == 1);
    test_assert(listdd_count(matched) == 1);
    test_assert(listdd_relation_contains(relation, (uint32_t[]){3, 0, 7}, (int[]){0, 1, 0}, 3));

    test_assert(listdd_union(&unchanged, listdd_invalid, b) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_diff(&unchanged, a, listdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_union_diff(&unchanged, &other_unchanged, listdd_invalid, b) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(other_unchanged == listdd_empty_list);
    test_assert(listdd_intersection(&unchanged, listdd_empty_list, b) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_match(&unchanged, a, b, listdd_invalid) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_union(NULL, a, b) == SYLVAN_ERR_INVALID);
    test_assert(listdd_union_diff(&unchanged, NULL, a, b) == SYLVAN_ERR_INVALID);
    test_assert(listdd_singleton(&unchanged, NULL, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_add(&unchanged, listdd_invalid, (uint32_t[]){1}, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_add(&unchanged, a, NULL, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_add(&unchanged, listdd_empty_list, (uint32_t[]){1}, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_relation_singleton(&unchanged, (uint32_t[]){1}, NULL, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_relation_add(&unchanged, relation, NULL, (int[]){0}, 1) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty);
    test_assert(listdd_relation_add(NULL, relation, (uint32_t[]){1}, (int[]){0}, 1) == SYLVAN_ERR_INVALID);

    sylvan_gc_CALL(lace);
    listdd_refs_popptr(14);
    return 0;
}

SYLVAN_TLS uint64_t seed = 1;

uint64_t
xorshift_rand(void)
{
    uint64_t x = seed;
    if (seed == 0) seed = rand();
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    seed = x;
    return x * 2685821657736338717LL;
}

double
uniform_deviate(uint64_t seed)
{
    return seed * (1.0 / ((double)(UINT64_MAX) + 1.0));
}

int
rng(int low, int high)
{
    return low + (int)(uniform_deviate(xorshift_rand()) * (high-low));
}

static int
test_cache()
{
    test_assert(cache_getused() == 0);

    /**
     * Test cache for large number of random entries
     */

    size_t number_add = 4000000;
    uint64_t *arr = (uint64_t*)malloc(sizeof(uint64_t)*4*number_add);
    for (size_t i=0; i<number_add*4; i++) arr[i] = xorshift_rand();
    for (size_t i=0; i<number_add; i++) {
        test_assert(cache_put(arr[4*i], arr[4*i+1], arr[4*i+2], arr[4*i+3]));
        uint64_t val;
        int res = cache_get(arr[4*i], arr[4*i+1], arr[4*i+2], &val);
        test_assert(res == 1);
        test_assert(val == arr[4*i+3]);
    }
    size_t count = 0;
    for (size_t i=0; i<number_add; i++) {
        uint64_t val;
        int res = cache_get(arr[4*i], arr[4*i+1], arr[4*i+2], &val);
        test_assert(res == 0 || val == arr[4*i+3]);
        if (res) count++;
    }
    test_assert(count == cache_getused());

    /**
     * Now also test for double entries
     */

    for (size_t i=0; i<number_add/2; i++) {
        test_assert(cache_put6(arr[8*i], arr[8*i+1], arr[8*i+2], arr[8*i+3], arr[8*i+4], arr[8*i+5], arr[8*i+6], arr[8*i+7]));
        uint64_t val1, val2;
        int res = cache_get6(arr[8*i], arr[8*i+1], arr[8*i+2], arr[8*i+3], arr[8*i+4], arr[8*i+5], &val1, &val2);
        test_assert(res == 1);
        test_assert(val1 == arr[8*i+6]);
        test_assert(val2 == arr[8*i+7]);
    }
    for (size_t i=0; i<number_add/2; i++) {
        uint64_t val1, val2;
        int res = cache_get6(arr[8*i], arr[8*i+1], arr[8*i+2], arr[8*i+3], arr[8*i+4], arr[8*i+5], &val1, &val2);
        test_assert(res == 0 || (val1 == arr[8*i+6] && val2 == arr[8*i+7]));
    }

    /**
     * And test that single entries are not corrupted
     */
    for (size_t i=0; i<number_add; i++) {
        uint64_t val;
        int res = cache_get(arr[4*i], arr[4*i+1], arr[4*i+2], &val);
        test_assert(res == 0 || val == arr[4*i+3]);
    }

    /**
     * TODO: multithreaded test
     */

    free(arr);
    return 0;
}

static inline BDD
make_random(int i, int j)
{
    if (i == j) return rng(0, 2) ? bdd_true : bdd_false;

    BDD yes = make_random(i+1, j);
    BDD no = make_random(i+1, j);
    BDD result = mtbdd_invalid;

    switch(rng(0, 4)) {
    case 0:
        result = no;
        mtbdd_deref(yes);
        break;
    case 1:
        result = yes;
        mtbdd_deref(no);
        break;
    case 2:
        result = mtbdd_ref(mtbdd_make_node(i, yes, no));
        mtbdd_deref(no);
        mtbdd_deref(yes);
        break;
    case 3:
    default:
        result = mtbdd_ref(mtbdd_make_node(i, no, yes));
        mtbdd_deref(no);
        mtbdd_deref(yes);
        break;
    }

    return result;
}

static LISTDD
make_random_ldd_set(int depth, int maxvalue, int elements)
{
    uint32_t *values = (uint32_t*)malloc((size_t)depth * sizeof(*values));
    LISTDD result = listdd_empty; // empty set
    listdd_refs_pushptr(&result);
    for (int i=0; i<elements; i++) {
        for (int j=0; j<depth; j++) {
            values[j] = rng(0, maxvalue);
        }
        test_assert(listdd_add(&result, result, values, depth) == SYLVAN_OK);
    }
    listdd_refs_popptr(1);
    free(values);
    return result;
}

static int
test_mtbdd()
{
    MTBDD fraction = mtbdd_fraction(-6, 8);
    test_assert(fraction != mtbdd_invalid);
    int32_t numerator;
    uint32_t denominator;
    test_assert(mtbdd_leaf_fraction(fraction, &numerator, &denominator) == 0);
    test_assert(numerator == -3);
    test_assert(denominator == 4);

    fraction = mtbdd_fraction(INT64_MIN, UINT64_C(1) << 33);
    test_assert(fraction != mtbdd_invalid);
    test_assert(mtbdd_leaf_fraction(fraction, &numerator, &denominator) == 0);
    test_assert(numerator == -1073741824);
    test_assert(denominator == 1);

    test_assert(mtbdd_fraction(INT64_MIN, 1) == mtbdd_invalid);
    test_assert(mtbdd_fraction(1, 0) == mtbdd_invalid);
    test_assert(mtbdd_leaf_fraction(mtbdd_int64(1), &numerator, &denominator) == -1);

    MTBDD if_false, if_true;
    MTBDD root = mtbdd_make_node(3, mtbdd_int64(1), mtbdd_int64(2));
    mtbdd_cofactors(root, &if_false, &if_true);
    test_assert(if_false == mtbdd_int64(1));
    test_assert(if_true == mtbdd_int64(2));
    mtbdd_cofactors(mtbdd_int64(3), &if_false, &if_true);
    test_assert(if_false == mtbdd_int64(3) && if_true == mtbdd_int64(3));

    uint32_t variables[64];
    for (uint32_t i=0; i<64; i++) variables[i] = i;
    MTBDD variable_set = test_bdd_set_from_levels(variables, 64);

    MTBDD result = mtbdd_invalid;
    mtbdd_protect(&result);
    test_assert(mtbdd_abstract_add(&result, mtbdd_double(1.0), variable_set) == SYLVAN_OK);
    test_assert(mtbdd_leaf_double(result) == ldexp(1.0, 64));

    test_assert(mtbdd_abstract_mul(&result, mtbdd_double(0.5), variable_set) == SYLVAN_OK);
    test_assert(mtbdd_leaf_double(result) == 0.0);
    mtbdd_unprotect(&result);

    return 0;
}

int testEqual(BDD a, BDD b)
{
    if (a == b) return 1;

    if (a == mtbdd_invalid) {
        fprintf(stderr, "a is invalid!\n");
        return 0;
    }

    if (b == mtbdd_invalid) {
        fprintf(stderr, "b is invalid!\n");
        return 0;
    }

    fprintf(stderr, "a and b are not equal!\n");

    bdd_fprint(stderr, a);fprintf(stderr, "\n");
    bdd_fprint(stderr, b);fprintf(stderr, "\n");

    return 0;
}

int
test_bdd()
{
    test_assert(test_bdd_ite(test_bdd_var(1), bdd_true, bdd_true) == bdd_not(test_bdd_ite(test_bdd_var(1), bdd_false, bdd_false)));
    test_assert(test_bdd_ite(test_bdd_var(1), bdd_false, bdd_true) == bdd_not(test_bdd_ite(test_bdd_var(1), bdd_true, bdd_false)));
    test_assert(test_bdd_ite(test_bdd_var(1), bdd_true, bdd_false) == bdd_not(test_bdd_ite(test_bdd_var(1), bdd_false, bdd_true)));
    test_assert(test_bdd_ite(test_bdd_var(1), bdd_false, bdd_false) == bdd_not(test_bdd_ite(test_bdd_var(1), bdd_true, bdd_true)));

    BDD a = test_bdd_var(0);
    BDD b = test_bdd_var(1);
    BDD conjunction = test_bdd_and(a, b);
    test_assert(test_bdd_exists(conjunction, a) == b);
    test_assert(test_bdd_forall(conjunction, a) == bdd_false);
    test_assert(test_bdd_project(conjunction, a) == a);
    test_assert(test_bdd_and_exists(a, b, a) == b);
    test_assert(test_bdd_and_project(a, b, a) == a);

    return 0;
}

int
test_cube()
{
    const BDDSET vars = test_bdd_set_from_levels(((uint32_t[]){1,2,3,4,6,8}), 6);

    uint8_t cube[6], check[6];
    int i, j;
    for (i=0;i<6;i++) cube[i] = rng(0,3);
    BDD bdd = test_bdd_cube(vars, cube);

    bdd_pick_cube_values(bdd, vars, check);
    for (i=0; i<6;i++) test_assert(cube[i] == check[i]);

    BDD picked_single = test_bdd_pick_minterm(bdd, vars);
    test_assert(testEqual(test_bdd_and(picked_single, bdd), picked_single));
    assert(bdd_sat_count(picked_single, vars)==1);

    BDD picked = test_bdd_pick_cube(bdd, vars);
    test_assert(testEqual(test_bdd_and(picked, bdd), picked));

    BDD t1 = test_bdd_cube(vars, ((uint8_t[]){1,1,2,2,0,0}));
    BDD t2 = test_bdd_cube(vars, ((uint8_t[]){1,1,1,0,0,2}));
    test_assert(testEqual(test_bdd_or_cube(t1, vars, ((uint8_t[]){1,1,1,0,0,2})), test_bdd_or(t1, t2)));
    t2 = test_bdd_cube(vars, ((uint8_t[]){2,2,2,1,1,0}));
    test_assert(testEqual(test_bdd_or_cube(t1, vars, ((uint8_t[]){2,2,2,1,1,0})), test_bdd_or(t1, t2)));
    t2 = test_bdd_cube(vars, ((uint8_t[]){1,1,1,0,0,0}));
    test_assert(testEqual(test_bdd_or_cube(t1, vars, ((uint8_t[]){1,1,1,0,0,0})), test_bdd_or(t1, t2)));

    bdd = make_random(1, 16);
    const BDDSET all_vars = test_bdd_set_from_levels(
        ((uint32_t[]){1,2,3,4,5,6,7,8,9,10,11,12,13,14,15}), 15);
    for (j=0;j<10;j++) {
        for (i=0;i<6;i++) cube[i] = rng(0,3);
        BDD c = test_bdd_cube(vars, cube);
        test_assert(test_bdd_or_cube(bdd, vars, cube) == test_bdd_or(bdd, c));
    }

    for (i=0;i<10;i++) {
        picked = test_bdd_pick_cube(bdd, all_vars);
        test_assert(testEqual(test_bdd_and(picked, bdd), picked));
    }

    const BDDSET limited_vars = test_bdd_set_from_levels(((uint32_t[]){1,3,8}), 3);
    picked = test_bdd_pick_cube(bdd, limited_vars);
    test_assert(bdd == bdd_false || test_bdd_and(picked, bdd) != bdd_false);

    BDD x = test_bdd_var(1);
    BDD y = test_bdd_var(2);
    test_assert(test_bdd_cofactor(test_bdd_xor(x, y), x) == bdd_not(y));
    test_assert(test_bdd_cofactor(test_bdd_xor(x, y), bdd_not(x)) == y);
    test_assert(test_bdd_cofactor(test_bdd_xor(x, y), test_bdd_or(x, y)) == mtbdd_invalid);

    BDD restricted = test_bdd_restrict(bdd, picked);
    test_assert(mtbdd_node_count(restricted) <= mtbdd_node_count(bdd));
    test_assert(test_bdd_and(restricted, picked) == test_bdd_and(bdd, picked));

    // simple test for mtbdd_enum_all
    uint8_t arr[6];
    MTBDD leaf = mtbdd_first_minterm(bdd_true, vars, arr, NULL);
    test_assert(leaf == bdd_true);
    test_assert(mtbdd_first_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 0 && arr[5] == 0);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 0 && arr[5] == 1);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 1 && arr[5] == 0);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 0 && arr[4] == 1 && arr[5] == 1);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 1 && arr[4] == 0 && arr[5] == 0);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 1 && arr[4] == 0 && arr[5] == 1);
    test_assert(mtbdd_next_minterm(bdd_true, vars, arr, NULL) == bdd_true);
    test_assert(arr[0] == 0 && arr[1] == 0 && arr[2] == 0 && arr[3] == 1 && arr[4] == 1 && arr[5] == 0);

    mtbdd_first_minterm(bdd_true, vars, arr, NULL);
    size_t count = 1;
    while (mtbdd_next_minterm(bdd_true, vars, arr, NULL) != mtbdd_undefined) {
        test_assert(count < 64);
        count++;
    }
    test_assert(count == 64);

    return 0;
}

static int
test_operators()
{
    // We need to test: xor, and, or, nand, nor, imp, biimp, invimp, diff, less

    //int i;
    BDD a = test_bdd_var(1);
    BDD b = test_bdd_var(2);
    BDD one = make_random(1, 12);
    BDD two = make_random(6, 24);

    // Test or
    test_assert(testEqual(test_bdd_or(a, b), mtbdd_make_node(1, b, bdd_true)));
    test_assert(testEqual(test_bdd_or(a, b), test_bdd_or(b, a)));
    test_assert(testEqual(test_bdd_or(one, two), test_bdd_or(two, one)));

    // Test and
    test_assert(testEqual(test_bdd_and(a, b), mtbdd_make_node(1, bdd_false, b)));
    test_assert(testEqual(test_bdd_and(a, b), test_bdd_and(b, a)));
    test_assert(testEqual(test_bdd_and(one, two), test_bdd_and(two, one)));

    // Test xor
    test_assert(testEqual(test_bdd_xor(a, b), mtbdd_make_node(1, b, bdd_not(b))));
    test_assert(testEqual(test_bdd_xor(a, b), test_bdd_xor(a, b)));
    test_assert(testEqual(test_bdd_xor(a, b), test_bdd_xor(b, a)));
    test_assert(testEqual(test_bdd_xor(one, two), test_bdd_xor(two, one)));
    test_assert(testEqual(test_bdd_xor(a, b), test_bdd_ite(a, bdd_not(b), b)));

    // Test diff
    test_assert(testEqual(test_bdd_diff(a, b), test_bdd_diff(a, b)));
    test_assert(testEqual(test_bdd_diff(a, b), test_bdd_diff(a, test_bdd_and(a, b))));
    test_assert(testEqual(test_bdd_diff(a, b), test_bdd_and(a, bdd_not(b))));
    test_assert(testEqual(test_bdd_diff(a, b), test_bdd_ite(b, bdd_false, a)));
    test_assert(testEqual(test_bdd_diff(one, two), test_bdd_diff(one, two)));
    test_assert(testEqual(test_bdd_diff(one, two), test_bdd_diff(one, test_bdd_and(one, two))));
    test_assert(testEqual(test_bdd_diff(one, two), test_bdd_and(one, bdd_not(two))));
    test_assert(testEqual(test_bdd_diff(one, two), test_bdd_ite(two, bdd_false, one)));

    // Test biimp
    test_assert(testEqual(test_bdd_xnor(a, b), mtbdd_make_node(1, bdd_not(b), b)));
    test_assert(testEqual(test_bdd_xnor(a, b), test_bdd_xnor(b, a)));
    test_assert(testEqual(test_bdd_xnor(one, two), test_bdd_xnor(two, one)));

    // Test nand / and
    test_assert(testEqual(bdd_not(test_bdd_and(a, b)), test_bdd_nand(b, a)));
    test_assert(testEqual(bdd_not(test_bdd_and(one, two)), test_bdd_nand(two, one)));

    // Test nor / or
    test_assert(testEqual(bdd_not(test_bdd_or(a, b)), test_bdd_nor(b, a)));
    test_assert(testEqual(bdd_not(test_bdd_or(one, two)), test_bdd_nor(two, one)));

    // Test xor / biimp
    test_assert(testEqual(test_bdd_xor(a, b), bdd_not(test_bdd_xnor(b, a))));
    test_assert(testEqual(test_bdd_xor(one, two), bdd_not(test_bdd_xnor(two, one))));

    // Test imp
    test_assert(testEqual(test_bdd_imp(a, b), test_bdd_ite(a, b, bdd_true)));
    test_assert(testEqual(test_bdd_imp(one, two), test_bdd_ite(one, two, bdd_true)));
    test_assert(testEqual(test_bdd_imp(one, two), bdd_not(test_bdd_diff(one, two))));
    test_assert(testEqual(test_bdd_imp(two, one), bdd_not(test_bdd_diff(two, one))));
    test_assert(testEqual(test_bdd_imp(a, b), bdd_not(test_bdd_diff(a, b))));
    test_assert(testEqual(test_bdd_imp(one, two), bdd_not(test_bdd_diff(one, two))));

    return 0;
}

static int
test_disjoint_subset()
{
    // We need to test: disjoint, subset
#define VARS 3    
    BDD v[VARS];
    for (int i=0; i<VARS; i++) v[i] = bdd_not(test_bdd_var(i));
#undef VARS

    BDD test_input[] = {
        bdd_true, bdd_false,
        bdd_false, bdd_true,
        v[0], v[1],
        v[1], v[1],
        v[0], bdd_not(v[0]),
        test_bdd_and(v[0],v[1]), v[2],
        test_bdd_and(v[0],v[1]), test_bdd_and(bdd_not(v[0]),v[1]),
        test_bdd_and(v[0],v[1]), test_bdd_or(bdd_not(v[0]),v[2]),
        test_bdd_and(v[0],v[1]), test_bdd_and(v[0],bdd_not(v[1])),
        test_bdd_or(v[0],v[1]), test_bdd_and(v[0],bdd_not(v[1])),
        test_bdd_and(v[1],test_bdd_or(v[0],v[2])), test_bdd_or(v[1],test_bdd_and(v[0],bdd_not(v[2])))
    };

    for (int i=0; i<11; i++) {
        BDD t1 = test_input[2*i];
        BDD t2 = test_input[2*i+1];
        test_assert(bdd_disjoint(t1,t2) == (test_bdd_and(t1,t2)==bdd_false));
        test_assert(bdd_subseteq(t1,t2) == (test_bdd_or(bdd_not(t1),t2) == bdd_true));
    }

    return 0;
}

int
test_relprod()
{
    uint32_t vars[] = {0,2,4};
    uint32_t all_vars[] = {0,1,2,3,4,5};

    BDDSET vars_set = test_bdd_set_from_levels(vars, 3);
    BDDSET all_vars_set = test_bdd_set_from_levels(all_vars, 6);

    BDD s, t, next, prev;
    BDD zeroes, ones;

    // transition relation: 000 --> 111 and !000 --> 000
    t = bdd_false;
    t = test_bdd_or_cube(t, all_vars_set, ((uint8_t[]){0,1,0,1,0,1}));
    t = test_bdd_or_cube(t, all_vars_set, ((uint8_t[]){1,0,2,0,2,0}));
    t = test_bdd_or_cube(t, all_vars_set, ((uint8_t[]){2,0,1,0,2,0}));
    t = test_bdd_or_cube(t, all_vars_set, ((uint8_t[]){2,0,2,0,1,0}));

    s = test_bdd_cube(vars_set, (uint8_t[]){0,0,1});
    zeroes = test_bdd_cube(vars_set, (uint8_t[]){0,0,0});
    ones = test_bdd_cube(vars_set, (uint8_t[]){1,1,1});

    next = test_bdd_rel_next(s, t, all_vars_set);
    prev = test_bdd_rel_prev(t, next, all_vars_set);
    test_assert(next == zeroes);
    test_assert(prev == bdd_not(zeroes));

    next = test_bdd_rel_next(next, t, all_vars_set);
    prev = test_bdd_rel_prev(t, next, all_vars_set);
    test_assert(next == ones);
    test_assert(prev == zeroes);

    t = test_bdd_cube(all_vars_set, (uint8_t[]){0,0,0,0,0,1});
    test_assert(test_bdd_rel_prev(t, s, all_vars_set) == zeroes);
    test_assert(test_bdd_rel_prev(t, bdd_not(s), all_vars_set) == bdd_false);
    test_assert(test_bdd_rel_next(s, t, all_vars_set) == bdd_false);
    test_assert(test_bdd_rel_next(zeroes, t, all_vars_set) == s);

    t = test_bdd_cube(all_vars_set, (uint8_t[]){0,0,0,0,0,2});
    test_assert(test_bdd_rel_prev(t, s, all_vars_set) == zeroes);
    test_assert(test_bdd_rel_prev(t, zeroes, all_vars_set) == zeroes);
    test_assert(test_bdd_rel_next(bdd_not(zeroes), t, all_vars_set) == bdd_false);

    return 0;
}

int
test_compose()
{
    BDD a = test_bdd_var(1);
    BDD b = test_bdd_var(2);

    BDD a_or_b = test_bdd_or(a, b);

    BDD one = make_random(3, 16);
    BDD two = make_random(8, 24);

    MTBDDMAP map = mtbdd_map_empty();
    mtbdd_protect(&map);

    test_assert(mtbdd_map_set(&map, map, 1, one) == SYLVAN_OK);
    test_assert(mtbdd_map_set(&map, map, 2, two) == SYLVAN_OK);

    test_assert(mtbdd_map_key(map) == 1);
    test_assert(mtbdd_map_value(map) == one);
    test_assert(mtbdd_map_key(mtbdd_map_next(map)) == 2);
    test_assert(mtbdd_map_value(mtbdd_map_next(map)) == two);

    test_assert(testEqual(one, test_bdd_compose(a, map)));
    test_assert(testEqual(two, test_bdd_compose(b, map)));

    test_assert(testEqual(test_bdd_or(one, two), test_bdd_compose(a_or_b, map)));

    test_assert(mtbdd_map_set(&map, map, 2, one) == SYLVAN_OK);
    test_assert(testEqual(test_bdd_compose(a_or_b, map), one));

    test_assert(mtbdd_map_set(&map, map, 1, two) == SYLVAN_OK);
    test_assert(testEqual(test_bdd_or(one, two), test_bdd_compose(a_or_b, map)));

    test_assert(testEqual(test_bdd_and(one, two), test_bdd_compose(test_bdd_and(a, b), map)));

    // test that composing [0:=true] on "0" yields true
    test_assert(mtbdd_map_set(&map, mtbdd_map_empty(), 1, bdd_true) == SYLVAN_OK);
    test_assert(testEqual(test_bdd_compose(a, map), bdd_true));

    // test that composing [0:=false] on "0" yields false
    test_assert(mtbdd_map_set(&map, mtbdd_map_empty(), 1, bdd_false) == SYLVAN_OK);
    test_assert(testEqual(test_bdd_compose(a, map), bdd_false));

    mtbdd_unprotect(&map);
    return 0;
}

int
test_ldd()
{
    // very basic testing of makenode
    for (int i=0; i<10; i++) {
        uint32_t value = rng(0, 100);
        LISTDD m = listdd_make_node(value, listdd_empty_list, listdd_empty);
        test_assert(listdd_node_value(m) == value);
        test_assert(listdd_node_down(m) == listdd_empty_list);
        test_assert(listdd_node_right(m) == listdd_empty);
        test_assert(listdd_is_copy_node(m) == 0);
        test_assert(listdd_follow(m, value) == listdd_empty_list);
        for (int j=0; j<100; j++) {
            uint32_t other_value = rng(0, 100);
            if (value != other_value) test_assert(listdd_follow(m, other_value) == listdd_empty);
        }
    }

    // test handling of the copy node by primitives
    LISTDD m = listdd_make_copy_node(listdd_empty_list, listdd_empty);
    test_assert(listdd_is_copy_node(m) == 1);
    test_assert(listdd_node_value(m) == 0);
    test_assert(listdd_node_down(m) == listdd_empty_list);
    test_assert(listdd_node_right(m) == listdd_empty);
    m = listdd_extend_node(m, 0, listdd_empty_list);
    test_assert(listdd_is_copy_node(m) == 1);
    test_assert(listdd_node_value(m) == 0);
    test_assert(listdd_node_down(m) == listdd_empty_list);
    test_assert(listdd_node_right(m) != listdd_empty);
    test_assert(listdd_follow(m, 0) == listdd_empty_list);
    test_assert(listdd_node_value(listdd_node_right(m)) == 0);
    test_assert(listdd_is_copy_node(listdd_node_right(m)) == 0);
    test_assert(listdd_make_node(0, listdd_empty_list, listdd_empty) == listdd_node_right(m));

    // test union_cube
    for (int i=0; i<100; i++) {
        int depth = rng(1, 6);
        int elements = rng(1, 30);
        m = make_random_ldd_set(depth, 10, elements);
        assert(m != listdd_empty_list);
        assert(m != listdd_empty);
        assert(listdd_count(m) <= elements);
        assert(listdd_count(m) >= 1);
    }

    // test simply transition relation
    {
        LISTDD states = listdd_invalid;
        LISTDD rel = listdd_invalid;
        LISTDD meta = listdd_invalid;
        LISTDD expected = listdd_invalid;
        LISTDD combined = listdd_invalid;
        LISTDD statezero = listdd_invalid;
        LISTDD states2 = listdd_invalid;
        listdd_refs_pushptr(&states);
        listdd_refs_pushptr(&rel);
        listdd_refs_pushptr(&meta);
        listdd_refs_pushptr(&expected);
        listdd_refs_pushptr(&combined);
        listdd_refs_pushptr(&statezero);
        listdd_refs_pushptr(&states2);

        // relation: (0,0) to (1,1)
        test_assert(listdd_singleton(&rel, (uint32_t[]){0,1,0,1}, 4) == SYLVAN_OK);
        test_assert(listdd_count(rel) == 1);
        // relation: (0,0) to (2,2)
        test_assert(listdd_add(&rel, rel, (uint32_t[]){0,2,0,2}, 4) == SYLVAN_OK);
        test_assert(listdd_count(rel) == 2);
        // meta: read write read write
        test_assert(listdd_singleton(&meta, (uint32_t[]){1,2,1,2}, 4) == SYLVAN_OK);
        test_assert(listdd_count(meta) == 1);
        // initial state: (0,0)
        test_assert(listdd_singleton(&states, (uint32_t[]){0,0}, 2) == SYLVAN_OK);
        test_assert(listdd_count(states) == 1);
        // relprod should give two states
        states = listdd_rel_next(states, rel, meta);
        test_assert(listdd_count(states) == 2);
        // relprod should give states (1,1) and (2,2)
        test_assert(listdd_singleton(&expected, (uint32_t[]){1,1}, 2) == SYLVAN_OK);
        test_assert(listdd_add(&expected, expected, (uint32_t[]){2,2}, 2) == SYLVAN_OK);
        test_assert(states == expected);

        // now test relprod union on the simple example
        test_assert(listdd_singleton(&states, (uint32_t[]){0,0}, 2) == SYLVAN_OK);
        states = listdd_rel_next_union(states, rel, meta, states);
        test_assert(listdd_count(states) == 3);
        test_assert(listdd_union(&combined, states, expected) == SYLVAN_OK);
        test_assert(states == combined);

        // now create transition (1,1) --> (1,1) (using copy nodes)
        test_assert(listdd_relation_singleton(&rel, (uint32_t[]){1,0,1,0}, (int[]){0,1,0,1}, 4) == SYLVAN_OK);
        states = listdd_rel_next(states, rel, meta);
        // the result should be just state (1,1)
        test_assert(listdd_singleton(&combined, (uint32_t[]){1,1}, 2) == SYLVAN_OK);
        test_assert(states == combined);

        test_assert(listdd_singleton(&statezero, (uint32_t[]){0,0}, 2) == SYLVAN_OK);
        test_assert(listdd_add(&states, statezero, (uint32_t[]){1,1}, 2) == SYLVAN_OK);
        test_assert(listdd_rel_next_union(states, rel, meta, statezero) == states);

        // now create transition (*,*) --> (*,*) (copy nodes)
        test_assert(listdd_relation_singleton(&rel, (uint32_t[]){0,0}, (int[]){1,1}, 2) == SYLVAN_OK);
        test_assert(listdd_singleton(&meta, (uint32_t[]){4,4}, 2) == SYLVAN_OK);
        states = make_random_ldd_set(2, 10, 10);
        states2 = make_random_ldd_set(2, 10, 10);
        test_assert(listdd_union(&combined, states, states2) == SYLVAN_OK);
        test_assert(combined == listdd_rel_next_union(states, rel, meta, states2));

        listdd_refs_popptr(7);
    }

    return 0;
}

TASK(int, runtests)
int runtests_CALL(lace_worker* lace)
{
    printf("Testing protected destinations.\n");
    for (int j = 0; j < 10; j++) {
        if (test_variable_set_destinations_CALL(lace)) return 1;
        if (test_mtbdd_construction_destinations_CALL(lace)) return 1;
        if (test_mtbdd_structure_destinations_CALL(lace)) return 1;
        if (test_mtbdd_map_destinations_CALL(lace)) return 1;
        if (test_mtbdd_extrema_destinations_CALL(lace)) return 1;
        if (test_mtbdd_apply_destinations_CALL(lace)) return 1;
        if (test_mtbdd_threshold_destinations_CALL(lace)) return 1;
        if (test_mtbdd_equal_double_destinations_CALL(lace)) return 1;
        if (test_mtbdd_order_destinations_CALL(lace)) return 1;
        if (test_mtbdd_abstract_destinations_CALL(lace)) return 1;
        if (test_mtbdd_eval_compose_destinations_CALL(lace)) return 1;
        if (test_protected_destinations_CALL(lace)) return 1;
        if (test_quantification_destinations_CALL(lace)) return 1;
        if (test_care_destinations_CALL(lace)) return 1;
        if (test_compose_destinations_CALL(lace)) return 1;
        if (test_cube_destinations_CALL(lace)) return 1;
        if (test_relational_destinations_CALL(lace)) return 1;
        if (test_map_reduce_destinations_CALL(lace)) return 1;
        if (test_listdd_set_destinations_CALL(lace)) return 1;
    }

    // we are not testing garbage collection
    sylvan_gc_disable();

    printf("Testing cache.\n");
    if (test_cache()) return 1;
    printf("Testing bdd.\n");
    if (test_bdd()) return 1;
    printf("Testing mtbdd.\n");
    if (test_mtbdd()) return 1;
    printf("Testing cube.\n");
    for (int j=0;j<10;j++) if (test_cube()) return 1;
    printf("Testing relprod.\n");
    for (int j=0;j<10;j++) if (test_relprod()) return 1;
    printf("Testing compose.\n");
    for (int j=0;j<10;j++) if (test_compose()) return 1;
    printf("Testing operators.\n");
    for (int j=0;j<10;j++) if (test_operators()) return 1;
    printf("Testing disjoint and subset.\n");
    for (int j=0;j<10;j++) if (test_disjoint_subset()) return 1;

    printf("Testing ldd.\n");
    if (test_ldd()) return 1;

    return 0;
    (void)lace; // suppress unused parameter error
}

int main()
{
    // Use multiple workers to exercise the protected-destination SPAWN/SYNC path.
    lace_start(4, 0, 0);

    // Simple Sylvan initialization, also initialize BDD, MTBDD and LDD support
    sylvan_set_sizes(1LL<<20, 1LL<<20, 1LL<<16, 1LL<<16);
    sylvan_init_package();
    mtbdd_init();
    listdd_init();

    printf("Sylvan initialization complete.\n");

    int res = runtests();

    sylvan_quit();
    lace_stop();

    return res;
}
