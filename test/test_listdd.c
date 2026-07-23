#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <sylvan/internal.h>

#include "test_assert.h"

TASK(int, test_listdd_metadata)

int
test_listdd_metadata_CALL(lace_worker *lace)
{
    const listdd_projection_kind projection_positions[] = {
        LISTDD_KEEP_POSITION,
        LISTDD_PROJECT_POSITION
    };
    const listdd_relation_access relation_positions[] = {
        LISTDD_RELATION_READ_WRITE,
        LISTDD_RELATION_READ_WRITE
    };
    const listdd_relation_access all_relation_positions[] = {
        LISTDD_RELATION_UNUSED,
        LISTDD_RELATION_READ,
        LISTDD_RELATION_WRITE,
        LISTDD_RELATION_READ_WRITE
    };
    listdd_projection *projection = NULL;
    listdd_projection *identity_projection = NULL;
    listdd_projection *raw_projection = NULL;
    listdd_relation_layout *layout = NULL;
    listdd_relation_layout *forms_layout = NULL;
    listdd_relation_layout *raw_layout = NULL;
    listdd_relation_layout *action_layout = NULL;
    listdd_relation_layout *read_layout = NULL;
    listdd_relation_layout *write_layout = NULL;
    listdd_relation_layout *unused_layout = NULL;

    test_assert(listdd_projection_create(&projection, projection_positions, 2) == SYLVAN_OK);
    test_assert(listdd_projection_create(&identity_projection,
        (listdd_projection_kind[]){LISTDD_KEEP_POSITION, LISTDD_KEEP_POSITION}, 2) == SYLVAN_OK);
    test_assert(listdd_contains(listdd_projection_raw(projection),
        (uint32_t[]){1, 0, UINT32_MAX-1}, 3));
    test_assert(listdd_projection_create_raw(&raw_projection,
        listdd_projection_raw(projection)) == SYLVAN_OK);

    test_assert(listdd_relation_layout_create(&layout, relation_positions, 2, 0) == SYLVAN_OK);
    test_assert(listdd_contains(listdd_relation_layout_raw(layout),
        (uint32_t[]){1, 2, 1, 2, UINT32_MAX}, 5));
    test_assert(listdd_relation_layout_create_raw(&raw_layout,
        listdd_relation_layout_raw(layout)) == SYLVAN_OK);
    test_assert(listdd_relation_layout_create(&forms_layout,
        all_relation_positions, 4, 1) == SYLVAN_OK);
    test_assert(listdd_contains(listdd_relation_layout_raw(forms_layout),
        (uint32_t[]){0, 3, 4, 1, 2, 5, UINT32_MAX}, 7));
    test_assert(listdd_relation_layout_create(&action_layout, NULL, 0, 1) == SYLVAN_OK);
    test_assert(listdd_contains(listdd_relation_layout_raw(action_layout),
        (uint32_t[]){5, UINT32_MAX}, 2));
    test_assert(listdd_relation_layout_create(&read_layout,
        (listdd_relation_access[]){LISTDD_RELATION_READ}, 1, 0) == SYLVAN_OK);
    test_assert(listdd_relation_layout_create(&write_layout,
        (listdd_relation_access[]){LISTDD_RELATION_WRITE}, 1, 0) == SYLVAN_OK);
    test_assert(listdd_relation_layout_create(&unused_layout,
        (listdd_relation_access[]){LISTDD_RELATION_UNUSED, LISTDD_RELATION_READ_WRITE},
        2, 0) == SYLVAN_OK);

    LISTDD states = listdd_invalid;
    LISTDD other = listdd_invalid;
    LISTDD projected = listdd_invalid;
    LISTDD matched = listdd_invalid;
    LISTDD relation = listdd_invalid;
    LISTDD source = listdd_invalid;
    LISTDD successors = listdd_invalid;
    LISTDD predecessors = listdd_invalid;
    LISTDD joined = listdd_invalid;
    LISTDD avoid = listdd_invalid;
    LISTDD united = listdd_invalid;
    LISTDD action_relation = listdd_invalid;
    LISTDD action_result = listdd_invalid;
    LISTDD one_source = listdd_invalid;
    LISTDD mixed_relation = listdd_invalid;
    listdd_refs_pushptr(&states);
    listdd_refs_pushptr(&other);
    listdd_refs_pushptr(&projected);
    listdd_refs_pushptr(&matched);
    listdd_refs_pushptr(&relation);
    listdd_refs_pushptr(&source);
    listdd_refs_pushptr(&successors);
    listdd_refs_pushptr(&predecessors);
    listdd_refs_pushptr(&joined);
    listdd_refs_pushptr(&avoid);
    listdd_refs_pushptr(&united);
    listdd_refs_pushptr(&action_relation);
    listdd_refs_pushptr(&action_result);
    listdd_refs_pushptr(&one_source);
    listdd_refs_pushptr(&mixed_relation);

    test_assert(listdd_singleton(&states, (uint32_t[]){1, 2}, 2) == SYLVAN_OK);
    test_assert(listdd_add(&states, states, (uint32_t[]){1, 3}, 2) == SYLVAN_OK);
    test_assert(listdd_add(&states, states, (uint32_t[]){2, 2}, 2) == SYLVAN_OK);
    test_assert(listdd_singleton(&other, (uint32_t[]){1, 9}, 2) == SYLVAN_OK);

    test_assert(listdd_project(&projected, states, projection) == SYLVAN_OK);
    test_assert(listdd_contains(projected, (uint32_t[]){1}, 1));
    test_assert(listdd_contains(projected, (uint32_t[]){2}, 1));
    test_assert(listdd_match(&matched, states, other, projection) == SYLVAN_OK);
    test_assert(listdd_count_double(matched) == 2.0);
    test_assert(listdd_join(&joined, states, states,
        identity_projection, identity_projection) == SYLVAN_OK);
    test_assert(joined == states);
    test_assert(listdd_singleton(&avoid, (uint32_t[]){1}, 1) == SYLVAN_OK);
    test_assert(listdd_project_diff(&projected, states, projection, avoid) == SYLVAN_OK);
    test_assert(listdd_count_double(projected) == 1.0);
    test_assert(listdd_contains(projected, (uint32_t[]){2}, 1));

    /* The advanced wrappers own independent protections for the same roots. */
    listdd_projection_destroy(projection);
    projection = NULL;
    listdd_relation_layout_destroy(layout);
    layout = NULL;
    sylvan_gc_CALL(lace);
    test_assert(listdd_project_raw_CALL(lace, &projected, states,
        listdd_projection_raw(raw_projection)) == SYLVAN_OK);

    const uint32_t read_values[] = {0, 0};
    const uint32_t write_values[] = {0, 1};
    const uint8_t retain_source[] = {1, 0};
    test_assert(listdd_relation_layout_create(&layout, relation_positions, 2, 0) == SYLVAN_OK);
    test_assert(listdd_relation_singleton(&relation, layout, read_values, write_values,
        retain_source, 2, NULL) == SYLVAN_OK);
    test_assert(listdd_is_copy_node(relation));
    test_assert(listdd_is_copy_node(listdd_follow_copy(relation)));
    int contains = -1;
    test_assert(listdd_relation_contains(&contains, relation, layout, read_values, write_values,
        retain_source, 2, NULL) == SYLVAN_OK);
    test_assert(contains == 1);

    test_assert(listdd_singleton(&source, (uint32_t[]){1, 0}, 2) == SYLVAN_OK);
    test_assert(listdd_add(&source, source, (uint32_t[]){2, 0}, 2) == SYLVAN_OK);
    successors = source;
    test_assert(listdd_rel_next(&successors, successors, relation, layout) == SYLVAN_OK);
    test_assert(listdd_contains(successors, (uint32_t[]){1, 1}, 2));
    test_assert(listdd_contains(successors, (uint32_t[]){2, 1}, 2));
    test_assert(listdd_rel_next_union(&united, source, relation, layout, source) == SYLVAN_OK);
    test_assert(listdd_count_double(united) == 4.0);
    test_assert(listdd_rel_prev(&predecessors, successors, relation, layout, source) == SYLVAN_OK);
    test_assert(predecessors == source);
    test_assert(listdd_rel_next_raw_CALL(lace, &action_result, source, relation,
        listdd_relation_layout_raw(raw_layout)) == SYLVAN_OK);
    test_assert(action_result == successors);

    /* READ filters and retains; WRITE replaces. */
    test_assert(listdd_singleton(&one_source, (uint32_t[]){4}, 1) == SYLVAN_OK);
    test_assert(listdd_relation_singleton(&action_relation, read_layout,
        (uint32_t[]){4}, NULL, NULL, 1, NULL) == SYLVAN_OK);
    test_assert(listdd_rel_next(&action_result, one_source,
        action_relation, read_layout) == SYLVAN_OK);
    test_assert(action_result == one_source);
    test_assert(listdd_relation_singleton(&action_relation, write_layout,
        NULL, (uint32_t[]){9}, NULL, 1, NULL) == SYLVAN_OK);
    test_assert(listdd_rel_next(&action_result, one_source,
        action_relation, write_layout) == SYLVAN_OK);
    test_assert(listdd_contains(action_result, (uint32_t[]){9}, 1));

    /* UNUSED preserves the source position before a later read/write field. */
    test_assert(listdd_singleton(&one_source, (uint32_t[]){4, 5}, 2) == SYLVAN_OK);
    test_assert(listdd_relation_singleton(&action_relation, unused_layout,
        (uint32_t[]){0, 5}, (uint32_t[]){0, 9}, NULL, 2, NULL) == SYLVAN_OK);
    test_assert(listdd_rel_next(&action_result, one_source,
        action_relation, unused_layout) == SYLVAN_OK);
    test_assert(listdd_contains(action_result, (uint32_t[]){4, 9}, 2));

    const uint32_t action = 17;
    test_assert(listdd_relation_singleton(&action_relation, action_layout,
        NULL, NULL, NULL, 0, &action) == SYLVAN_OK);
    test_assert(listdd_rel_next(&action_result, listdd_empty_list,
        action_relation, action_layout) == SYLVAN_OK);
    test_assert(action_result == listdd_empty_list);

    LISTDD unchanged = listdd_empty_list;
    listdd_refs_pushptr(&unchanged);
    test_assert(listdd_project(&unchanged, states, NULL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty_list);
    test_assert(listdd_relation_add(&unchanged, relation, layout,
        read_values, write_values, retain_source, 1, NULL) == SYLVAN_ERR_INVALID);
    test_assert(unchanged == listdd_empty_list);
    test_assert(listdd_relation_singleton(&unchanged, layout,
        read_values, write_values, (uint8_t[]){1, 1}, 2, NULL) == SYLVAN_OK);
    test_assert(listdd_is_copy_node(unchanged));

    uint64_t exact = UINT64_MAX;
    test_assert(listdd_count_u64_CALL(lace, &exact, states) == SYLVAN_OK);
    test_assert(exact == 3);
    test_assert(listdd_count_double_CALL(lace, states) == 3.0);
    test_assert(listdd_count_u64_CALL(lace, NULL, states) == SYLVAN_ERR_INVALID);
    test_assert(isnan(listdd_count_double_CALL(lace, listdd_invalid)));

    sylvan_iterator *iterator = NULL;
    test_assert(listdd_iterator_create(&iterator, states) == SYLVAN_OK);
    sylvan_gc_CALL(lace);
    int has_item = 0;
    test_assert(listdd_iterator_next(
        iterator, (uint32_t[1]){0}, 1, &has_item) == SYLVAN_ERR_INVALID);
    const uint32_t expected[][2] = {{1, 2}, {1, 3}, {2, 2}};
    for (size_t i=0; i<3; i++) {
        uint32_t values[2] = {UINT32_MAX, UINT32_MAX};
        has_item = 0;
        test_assert(listdd_iterator_next(iterator, values, 2, &has_item) == SYLVAN_OK);
        test_assert(has_item == 1);
        test_assert(values[0] == expected[i][0] && values[1] == expected[i][1]);
    }
    has_item = 1;
    test_assert(listdd_iterator_next(iterator, (uint32_t[2]){0, 0}, 2, &has_item) == SYLVAN_OK);
    test_assert(has_item == 0);
    sylvan_iterator_destroy(iterator);

    iterator = NULL;
    test_assert(listdd_iterator_create(&iterator, listdd_empty) == SYLVAN_OK);
    has_item = 1;
    test_assert(listdd_iterator_next(iterator, NULL, 0, &has_item) == SYLVAN_OK);
    test_assert(has_item == 0);
    sylvan_iterator_destroy(iterator);

    iterator = NULL;
    test_assert(listdd_iterator_create(&iterator, listdd_empty_list) == SYLVAN_OK);
    has_item = 0;
    test_assert(listdd_iterator_next(iterator, NULL, 0, &has_item) == SYLVAN_OK);
    test_assert(has_item == 1);
    test_assert(listdd_iterator_next(iterator, NULL, 0, &has_item) == SYLVAN_OK);
    test_assert(has_item == 0);
    sylvan_iterator_destroy(iterator);

    iterator = (sylvan_iterator*)(uintptr_t)1;
    test_assert(listdd_iterator_create(&iterator, unchanged) == SYLVAN_ERR_INVALID);
    test_assert(iterator == (sylvan_iterator*)(uintptr_t)1);

    test_assert(listdd_relation_singleton_raw(
        &mixed_relation, (uint32_t[]){0, 0}, (int[]){0, 0}, 2) ==
        SYLVAN_OK);
    test_assert(listdd_relation_add_raw(
        &mixed_relation, mixed_relation, (uint32_t[]){1, 0},
        (int[]){0, 1}, 2) == SYLVAN_OK);
    test_assert(!listdd_is_copy_node(mixed_relation));
    iterator = (sylvan_iterator*)(uintptr_t)1;
    test_assert(listdd_iterator_create(
        &iterator, mixed_relation) == SYLVAN_ERR_INVALID);
    test_assert(iterator == (sylvan_iterator*)(uintptr_t)1);

    contains = 7;
    test_assert(listdd_relation_contains(&contains, listdd_empty_list,
        read_layout, (uint32_t[]){4}, NULL, NULL, 1, NULL) ==
        SYLVAN_ERR_INVALID);
    test_assert(contains == 7);

    listdd_refs_popptr(16);
    listdd_projection_destroy(raw_projection);
    listdd_projection_destroy(identity_projection);
    listdd_relation_layout_destroy(raw_layout);
    listdd_relation_layout_destroy(layout);
    listdd_relation_layout_destroy(forms_layout);
    listdd_relation_layout_destroy(action_layout);
    listdd_relation_layout_destroy(read_layout);
    listdd_relation_layout_destroy(write_layout);
    listdd_relation_layout_destroy(unused_layout);
    return 0;
}

TASK(int, test_listdd_count_overflow)

int
test_listdd_count_overflow_CALL(lace_worker *lace)
{
    LISTDD full = listdd_empty_list;
    LISTDD one = listdd_invalid;
    listdd_refs_pushptr(&full);
    listdd_refs_pushptr(&one);

    for (size_t i=0; i<65; i++) {
        test_assert(_listdd_try_make_node(&one, 1, full, listdd_empty) == SYLVAN_OK);
        test_assert(_listdd_try_make_node(&full, 0, full, one) == SYLVAN_OK);
    }

    uint64_t unchanged = 42;
    test_assert(listdd_count_u64_CALL(lace, &unchanged, full) == SYLVAN_ERR_OVERFLOW);
    test_assert(unchanged == 42);
    test_assert(listdd_count_double_CALL(lace, full) == 0x1p65);

    sylvan_gc_CALL(lace);
    test_assert(listdd_count_u64_CALL(lace, &unchanged, full) == SYLVAN_ERR_OVERFLOW);
    test_assert(unchanged == 42);

    listdd_refs_popptr(2);
    return 0;
}

TASK(int, runtests)

int
runtests_CALL(lace_worker *lace)
{
    if (test_listdd_metadata_CALL(lace)) return 1;
    if (test_listdd_count_overflow_CALL(lace)) return 1;
    return 0;
}

int
main(void)
{
    lace_start(4, 0, 0);
    sylvan_set_sizes(1LL<<20, 1LL<<20, 1LL<<16, 1LL<<16);
    sylvan_init_package();
    listdd_init();

    int result = runtests();

    sylvan_quit();
    lace_stop();
    return result;
}
