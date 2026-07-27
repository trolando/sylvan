/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
 * Copyright 2019-2026 Tom van Dijk, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sylvan/internal.h>

#include <stdlib.h>
#include <string.h>

enum iterator_family {
    ITERATOR_BDD,
    ITERATOR_MTBDD,
    ITERATOR_ZDD,
    ITERATOR_LISTDD
};

enum iterator_frame_kind {
    ITERATOR_FRAME_SINGLE,
    ITERATOR_FRAME_NODE,
    ITERATOR_FRAME_REPEAT
};

struct sylvan_iterator {
    MTBDD root;
    BDDSET variables;
    MTBDD current;
    size_t count;
    size_t depth;
    sylvan_iterator_mode mode;
    mtbdd_iterator_leaf_filter_cb accept_leaf;
    void *filter_context;
    enum iterator_family family;
    int resume;
    int finished;
    uint32_t *levels;
    MTBDD *nodes;
    uint8_t *frame_kinds;
    uint8_t *values;
    uint32_t *listdd_values;
};

struct iterator_visit {
    uint64_t node;
    size_t depth;
};

struct iterator_visit_set {
    struct iterator_visit *entries;
    uint8_t *occupied;
    size_t capacity;
    size_t size;
};

static uint64_t
iterator_visit_hash(uint64_t node, size_t depth)
{
    uint64_t value = node ^ ((uint64_t)depth + UINT64_C(0x9e3779b97f4a7c15));
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static int
iterator_visit_set_grow(struct iterator_visit_set *set)
{
    const size_t old_capacity = set->capacity;
    const size_t new_capacity = old_capacity == 0 ? 64 : old_capacity * 2;
    if (new_capacity < old_capacity ||
        new_capacity > SIZE_MAX / sizeof(*set->entries)) {
        return SYLVAN_ERR_OOM;
    }

    struct iterator_visit *entries =
        calloc(new_capacity, sizeof(*entries));
    uint8_t *occupied = calloc(new_capacity, sizeof(*occupied));
    if (entries == NULL || occupied == NULL) {
        free(entries);
        free(occupied);
        return SYLVAN_ERR_OOM;
    }

    for (size_t i = 0; i < old_capacity; i++) {
        if (!set->occupied[i]) continue;
        const struct iterator_visit entry = set->entries[i];
        size_t slot =
            (size_t)iterator_visit_hash(entry.node, entry.depth) &
            (new_capacity - 1);
        while (occupied[slot]) slot = (slot + 1) & (new_capacity - 1);
        entries[slot] = entry;
        occupied[slot] = 1;
    }

    free(set->entries);
    free(set->occupied);
    set->entries = entries;
    set->occupied = occupied;
    set->capacity = new_capacity;
    return SYLVAN_OK;
}

static int
iterator_visit_set_insert(struct iterator_visit_set *set,
                          uint64_t node, size_t depth)
{
    if (set->capacity == 0 || set->size >= set->capacity / 2) {
        const int status = iterator_visit_set_grow(set);
        if (status != SYLVAN_OK) return status;
    }

    size_t slot =
        (size_t)iterator_visit_hash(node, depth) & (set->capacity - 1);
    while (set->occupied[slot]) {
        if (set->entries[slot].node == node &&
            set->entries[slot].depth == depth) {
            return 0;
        }
        slot = (slot + 1) & (set->capacity - 1);
    }

    set->entries[slot].node = node;
    set->entries[slot].depth = depth;
    set->occupied[slot] = 1;
    set->size++;
    return 1;
}

static int
iterator_validate_graph(uint64_t root, size_t width,
                        enum iterator_family family)
{
    struct iterator_visit_set seen = {0};
    struct iterator_visit *stack = NULL;
    size_t stack_size = 0;
    size_t stack_capacity = 0;
    int status = SYLVAN_OK;

    stack_capacity = 64;
    stack = malloc(stack_capacity * sizeof(*stack));
    if (stack == NULL) return SYLVAN_ERR_OOM;
    stack[stack_size++] = (struct iterator_visit){root, 0};

    while (stack_size != 0) {
        const struct iterator_visit current = stack[--stack_size];
        const int inserted = iterator_visit_set_insert(
            &seen, current.node, current.depth);
        if (inserted < 0) {
            status = inserted;
            break;
        }
        if (inserted == 0) continue;

        uint64_t children[2];
        size_t child_depth[2];
        size_t child_count = 0;

        if (family == ITERATOR_ZDD) {
            const ZDD dd = (ZDD)current.node;
            if (dd == zdd_false || dd == zdd_base) continue;
            if (zdd_is_leaf(dd)) {
                status = SYLVAN_ERR_INVALID;
                break;
            }
            children[0] = zdd_node_low(dd);
            children[1] = zdd_node_high(dd);
            child_depth[0] = child_depth[1] = 0;
            child_count = 2;
        } else {
            const LISTDD dd = (LISTDD)current.node;
            if (dd == listdd_empty) continue;
            if (dd == listdd_empty_list) {
                if (current.depth != width) status = SYLVAN_ERR_INVALID;
                if (status != SYLVAN_OK) break;
                continue;
            }
            if (current.depth >= width || listdd_is_copy_node(dd)) {
                status = SYLVAN_ERR_INVALID;
                break;
            }
            children[0] = listdd_node_right(dd);
            children[1] = listdd_node_down(dd);
            child_depth[0] = current.depth;
            child_depth[1] = current.depth + 1;
            child_count = 2;
        }

        if (stack_size > SIZE_MAX - child_count) {
            status = SYLVAN_ERR_OOM;
            break;
        }
        if (stack_size + child_count > stack_capacity) {
            size_t new_capacity = stack_capacity * 2;
            if (new_capacity < stack_capacity ||
                new_capacity > SIZE_MAX / sizeof(*stack)) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            struct iterator_visit *grown =
                realloc(stack, new_capacity * sizeof(*stack));
            if (grown == NULL) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            stack = grown;
            stack_capacity = new_capacity;
        }
        for (size_t i = 0; i < child_count; i++) {
            stack[stack_size++] =
                (struct iterator_visit){children[i], child_depth[i]};
        }
    }

    free(stack);
    free(seen.entries);
    free(seen.occupied);
    return status;
}

static int
iterator_validate_support(MTBDD dd, BDDSET variables, enum iterator_family family)
{
    BDDSET support = mtbdd_invalid;
    BDDSET missing = mtbdd_invalid;
    mtbdd_protect(&support);
    mtbdd_protect(&missing);

    int status = family == ITERATOR_ZDD
        ? zdd_support(&support, (ZDD)dd)
        : mtbdd_support(&support, dd);
    if (status == SYLVAN_OK) status = bdd_set_difference(&missing, support, variables);
    if (status == SYLVAN_OK && !bdd_set_is_empty(missing)) status = SYLVAN_ERR_INVALID;

    mtbdd_unprotect(&missing);
    mtbdd_unprotect(&support);
    return status;
}

static int
iterator_create(sylvan_iterator **destination, MTBDD dd, BDDSET variables,
                sylvan_iterator_mode mode, enum iterator_family family,
                mtbdd_iterator_leaf_filter_cb accept_leaf,
                void *filter_context)
{
    if (destination == NULL || dd == mtbdd_invalid || variables == mtbdd_invalid ||
        (mode != SYLVAN_ITERATOR_CUBES && mode != SYLVAN_ITERATOR_MINTERMS)) {
        return SYLVAN_ERR_INVALID;
    }

    int status = iterator_validate_support(dd, variables, family);
    if (status != SYLVAN_OK) return status;
    if (family == ITERATOR_ZDD) {
        status = iterator_validate_graph(
            (uint64_t)dd, bdd_set_count(variables), family);
        if (status != SYLVAN_OK) return status;
    }

    sylvan_iterator *iterator = calloc(1, sizeof(*iterator));
    if (iterator == NULL) return SYLVAN_ERR_OOM;

    iterator->root = dd;
    iterator->variables = variables;
    iterator->current = dd;
    iterator->count = bdd_set_count(variables);
    iterator->mode = mode;
    iterator->accept_leaf = accept_leaf;
    iterator->filter_context = filter_context;
    iterator->family = family;

    if (family == ITERATOR_ZDD) zdd_protect((ZDD*)&iterator->root);
    else mtbdd_protect(&iterator->root);
    mtbdd_protect(&iterator->variables);

    if (iterator->count != 0) {
        iterator->levels = malloc(iterator->count * sizeof(*iterator->levels));
        iterator->nodes = malloc(iterator->count * sizeof(*iterator->nodes));
        iterator->frame_kinds = malloc(iterator->count * sizeof(*iterator->frame_kinds));
        iterator->values = malloc(iterator->count * sizeof(*iterator->values));
        if (iterator->levels == NULL || iterator->nodes == NULL ||
            iterator->frame_kinds == NULL || iterator->values == NULL) {
            sylvan_iterator_destroy(iterator);
            return SYLVAN_ERR_OOM;
        }

        BDDSET remaining = variables;
        for (size_t i = 0; i < iterator->count; i++) {
            iterator->levels[i] = bdd_set_first(remaining);
            remaining = bdd_set_next(remaining);
        }
    }

    *destination = iterator;
    sylvan_stats_count(SYLVAN_ITERATOR_CREATED);
    return SYLVAN_OK;
}

int
bdd_iterator_create(sylvan_iterator **destination, BDD dd, BDDSET variables,
                    sylvan_iterator_mode mode)
{
    return iterator_create(
        destination, dd, variables, mode, ITERATOR_BDD, NULL, NULL);
}

int
mtbdd_iterator_create(sylvan_iterator **destination, MTBDD dd, BDDSET variables,
                      const mtbdd_iterator_options *options)
{
    if (options == NULL) return SYLVAN_ERR_INVALID;
    return iterator_create(
        destination, dd, variables, options->mode, ITERATOR_MTBDD,
        options->accept_leaf, options->context);
}

int
zdd_iterator_create(sylvan_iterator **destination, ZDD dd, BDDSET domain)
{
    return iterator_create(
        destination, (MTBDD)dd, domain, SYLVAN_ITERATOR_MINTERMS, ITERATOR_ZDD,
        NULL, NULL);
}

int
listdd_iterator_create(sylvan_iterator **destination, LISTDD dd)
{
    if (destination == NULL || dd == listdd_invalid) return SYLVAN_ERR_INVALID;

    size_t count = 0;
    LISTDD cursor = dd;
    if (dd != listdd_empty) {
        while (cursor > listdd_empty_list) {
            if (listdd_is_copy_node(cursor) || count == SIZE_MAX) {
                return SYLVAN_ERR_INVALID;
            }
            count++;
            cursor = listdd_node_down(cursor);
        }
        if (cursor != listdd_empty_list) return SYLVAN_ERR_INVALID;
    }
    const int validation_status = iterator_validate_graph(
        (uint64_t)dd, count, ITERATOR_LISTDD);
    if (validation_status != SYLVAN_OK) return validation_status;

    sylvan_iterator *iterator = calloc(1, sizeof(*iterator));
    if (iterator == NULL) return SYLVAN_ERR_OOM;

    iterator->root = (MTBDD)dd;
    iterator->current = (MTBDD)dd;
    iterator->count = count;
    iterator->family = ITERATOR_LISTDD;
    listdd_protect((LISTDD*)&iterator->root);

    if (count != 0) {
        iterator->nodes = malloc(count * sizeof(*iterator->nodes));
        iterator->listdd_values =
            malloc(count * sizeof(*iterator->listdd_values));
        if (iterator->nodes == NULL || iterator->listdd_values == NULL) {
            sylvan_iterator_destroy(iterator);
            return SYLVAN_ERR_OOM;
        }
    }

    *destination = iterator;
    sylvan_stats_count(SYLVAN_ITERATOR_CREATED);
    return SYLVAN_OK;
}

void
sylvan_iterator_destroy(sylvan_iterator *iterator)
{
    if (iterator == NULL) return;

    if (iterator->family == ITERATOR_LISTDD) {
        listdd_unprotect((LISTDD*)&iterator->root);
    } else {
        mtbdd_unprotect(&iterator->variables);
        if (iterator->family == ITERATOR_ZDD) zdd_unprotect((ZDD*)&iterator->root);
        else mtbdd_unprotect(&iterator->root);
    }
    free(iterator->listdd_values);
    free(iterator->values);
    free(iterator->frame_kinds);
    free(iterator->nodes);
    free(iterator->levels);
    free(iterator);
}

static int
iterator_backtrack(sylvan_iterator *iterator)
{
    while (iterator->depth != 0) {
        const size_t depth = --iterator->depth;
        if (iterator->frame_kinds[depth] == ITERATOR_FRAME_NODE &&
            iterator->values[depth] == 0) {
            iterator->values[depth] = 1;
            iterator->current = iterator->family == ITERATOR_ZDD
                ? (MTBDD)zdd_node_high((ZDD)iterator->nodes[depth])
                : mtbdd_node_high(iterator->nodes[depth]);
            iterator->depth++;
            return 1;
        }
        if (iterator->frame_kinds[depth] == ITERATOR_FRAME_REPEAT &&
            iterator->values[depth] == 0) {
            iterator->values[depth] = 1;
            iterator->current = iterator->nodes[depth];
            iterator->depth++;
            return 1;
        }
    }

    iterator->finished = 1;
    return 0;
}

static int
iterator_next(sylvan_iterator *iterator, uint8_t *values, size_t count,
              MTBDD *leaf, int *has_item)
{
    if (iterator == NULL || leaf == NULL || has_item == NULL ||
        count != iterator->count || (count != 0 && values == NULL)) {
        return SYLVAN_ERR_INVALID;
    }
    if (iterator->finished) {
        *has_item = 0;
        return SYLVAN_OK;
    }

    if (iterator->resume) {
        iterator->resume = 0;
        if (!iterator_backtrack(iterator)) {
            *has_item = 0;
            return SYLVAN_OK;
        }
    }

    for (;;) {
        if (iterator->current == mtbdd_undefined) {
            if (!iterator_backtrack(iterator)) {
                *has_item = 0;
                return SYLVAN_OK;
            }
            continue;
        }

        const int is_leaf = iterator->family == ITERATOR_ZDD
            ? zdd_is_leaf((ZDD)iterator->current)
            : mtbdd_is_leaf(iterator->current);
        if (is_leaf && iterator->family == ITERATOR_BDD &&
            iterator->current != bdd_true) {
            iterator->finished = 1;
            return SYLVAN_ERR_INVALID;
        }
        if (is_leaf && iterator->family == ITERATOR_ZDD &&
            iterator->current != zdd_base) {
            iterator->finished = 1;
            return SYLVAN_ERR_INVALID;
        }
        if (is_leaf && iterator->family == ITERATOR_MTBDD &&
            iterator->accept_leaf != NULL &&
            !iterator->accept_leaf(
                iterator->current, iterator->filter_context)) {
            if (!iterator_backtrack(iterator)) {
                *has_item = 0;
                return SYLVAN_OK;
            }
            continue;
        }

        if ((iterator->mode == SYLVAN_ITERATOR_CUBES && is_leaf) ||
            iterator->depth == iterator->count) {
            if (!is_leaf) {
                iterator->finished = 1;
                return SYLVAN_ERR_INVALID;
            }
            if (iterator->mode == SYLVAN_ITERATOR_CUBES) {
                const size_t remaining = iterator->count - iterator->depth;
                if (remaining != 0) {
                    memset(iterator->values + iterator->depth, 2, remaining);
                }
            }
            if (iterator->count != 0) {
                memcpy(values, iterator->values, iterator->count);
            }
            *leaf = iterator->current;
            *has_item = 1;
            iterator->resume = 1;
            sylvan_stats_count(SYLVAN_ITERATOR_ITEMS);
            return SYLVAN_OK;
        }

        const size_t depth = iterator->depth;
        const uint32_t level = iterator->levels[depth];
        iterator->nodes[depth] = iterator->current;

        if (is_leaf) {
            iterator->frame_kinds[depth] = iterator->family == ITERATOR_ZDD
                ? ITERATOR_FRAME_SINGLE
                : ITERATOR_FRAME_REPEAT;
            iterator->values[depth] = 0;
        } else {
            const uint32_t node_level = iterator->family == ITERATOR_ZDD
                ? zdd_top_var((ZDD)iterator->current)
                : mtbdd_node_variable(iterator->current);
            if (node_level < level) {
                iterator->finished = 1;
                return SYLVAN_ERR_INVALID;
            }
            if (node_level == level) {
                iterator->frame_kinds[depth] = ITERATOR_FRAME_NODE;
                iterator->values[depth] = 0;
                iterator->current = iterator->family == ITERATOR_ZDD
                    ? (MTBDD)zdd_node_low((ZDD)iterator->current)
                    : mtbdd_node_low(iterator->current);
            } else if (iterator->mode == SYLVAN_ITERATOR_MINTERMS &&
                       iterator->family != ITERATOR_ZDD) {
                iterator->frame_kinds[depth] = ITERATOR_FRAME_REPEAT;
                iterator->values[depth] = 0;
            } else {
                iterator->frame_kinds[depth] = ITERATOR_FRAME_SINGLE;
                iterator->values[depth] = iterator->family == ITERATOR_ZDD ? 0 : 2;
            }
        }
        iterator->depth++;
    }
}

int
bdd_iterator_next(sylvan_iterator *iterator, uint8_t *values, size_t count,
                  int *has_item)
{
    if (iterator == NULL || iterator->family != ITERATOR_BDD) return SYLVAN_ERR_INVALID;
    MTBDD leaf;
    return iterator_next(iterator, values, count, &leaf, has_item);
}

int
mtbdd_iterator_next(sylvan_iterator *iterator, uint8_t *values, size_t count,
                    MTBDD *leaf, int *has_item)
{
    if (iterator == NULL || iterator->family != ITERATOR_MTBDD) return SYLVAN_ERR_INVALID;
    return iterator_next(iterator, values, count, leaf, has_item);
}

int
zdd_iterator_next(sylvan_iterator *iterator, uint8_t *values, size_t count,
                  int *has_item)
{
    if (iterator == NULL || iterator->family != ITERATOR_ZDD) return SYLVAN_ERR_INVALID;
    MTBDD leaf;
    return iterator_next(iterator, values, count, &leaf, has_item);
}

static int
listdd_iterator_backtrack(sylvan_iterator *iterator)
{
    while (iterator->depth != 0) {
        const size_t depth = --iterator->depth;
        const LISTDD right =
            listdd_node_right((LISTDD)iterator->nodes[depth]);
        if (right != listdd_empty) {
            iterator->current = (MTBDD)right;
            return 1;
        }
    }

    iterator->finished = 1;
    return 0;
}

int
listdd_iterator_next(sylvan_iterator *iterator, uint32_t *values, size_t count,
                     int *has_item)
{
    if (iterator == NULL || iterator->family != ITERATOR_LISTDD ||
        has_item == NULL || count != iterator->count ||
        (count != 0 && values == NULL)) {
        return SYLVAN_ERR_INVALID;
    }
    if (iterator->finished) {
        *has_item = 0;
        return SYLVAN_OK;
    }

    if (iterator->resume) {
        iterator->resume = 0;
        if (!listdd_iterator_backtrack(iterator)) {
            *has_item = 0;
            return SYLVAN_OK;
        }
    }

    for (;;) {
        const LISTDD current = (LISTDD)iterator->current;
        if (current == listdd_empty) {
            if (!listdd_iterator_backtrack(iterator)) {
                *has_item = 0;
                return SYLVAN_OK;
            }
            continue;
        }
        if (current == listdd_empty_list) {
            if (iterator->depth != iterator->count) {
                iterator->finished = 1;
                return SYLVAN_ERR_INVALID;
            }
            if (count != 0) {
                memcpy(values, iterator->listdd_values,
                       count * sizeof(*values));
            }
            *has_item = 1;
            iterator->resume = 1;
            sylvan_stats_count(SYLVAN_ITERATOR_ITEMS);
            return SYLVAN_OK;
        }
        if (iterator->depth == iterator->count ||
            listdd_is_copy_node(current)) {
            iterator->finished = 1;
            return SYLVAN_ERR_INVALID;
        }

        const size_t depth = iterator->depth++;
        iterator->nodes[depth] = (MTBDD)current;
        iterator->listdd_values[depth] = listdd_node_value(current);
        iterator->current = (MTBDD)listdd_node_down(current);
    }
}
