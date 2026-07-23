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
    ITERATOR_MTBDD
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
    enum iterator_family family;
    int resume;
    int finished;
    uint32_t *levels;
    MTBDD *nodes;
    uint8_t *frame_kinds;
    uint8_t *values;
};

static int
iterator_validate_support(MTBDD dd, BDDSET variables)
{
    BDDSET support = mtbdd_invalid;
    BDDSET missing = mtbdd_invalid;
    mtbdd_protect(&support);
    mtbdd_protect(&missing);

    int status = mtbdd_support(&support, dd);
    if (status == SYLVAN_OK) status = bdd_set_difference(&missing, support, variables);
    if (status == SYLVAN_OK && !bdd_set_is_empty(missing)) status = SYLVAN_ERR_INVALID;

    mtbdd_unprotect(&missing);
    mtbdd_unprotect(&support);
    return status;
}

static int
iterator_create(sylvan_iterator **destination, MTBDD dd, BDDSET variables,
                sylvan_iterator_mode mode, enum iterator_family family)
{
    if (destination == NULL || dd == mtbdd_invalid || variables == mtbdd_invalid ||
        (mode != SYLVAN_ITERATOR_CUBES && mode != SYLVAN_ITERATOR_MINTERMS)) {
        return SYLVAN_ERR_INVALID;
    }

    int status = iterator_validate_support(dd, variables);
    if (status != SYLVAN_OK) return status;

    sylvan_iterator *iterator = calloc(1, sizeof(*iterator));
    if (iterator == NULL) return SYLVAN_ERR_OOM;

    iterator->root = dd;
    iterator->variables = variables;
    iterator->current = dd;
    iterator->count = bdd_set_count(variables);
    iterator->mode = mode;
    iterator->family = family;

    mtbdd_protect(&iterator->root);
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
    return iterator_create(destination, dd, variables, mode, ITERATOR_BDD);
}

int
mtbdd_iterator_create(sylvan_iterator **destination, MTBDD dd, BDDSET variables,
                      sylvan_iterator_mode mode)
{
    return iterator_create(destination, dd, variables, mode, ITERATOR_MTBDD);
}

void
sylvan_iterator_destroy(sylvan_iterator *iterator)
{
    if (iterator == NULL) return;

    mtbdd_unprotect(&iterator->variables);
    mtbdd_unprotect(&iterator->root);
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
            iterator->current = mtbdd_node_high(iterator->nodes[depth]);
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

        const int is_leaf = mtbdd_is_leaf(iterator->current);
        if (is_leaf && iterator->family == ITERATOR_BDD &&
            iterator->current != bdd_true) {
            iterator->finished = 1;
            return SYLVAN_ERR_INVALID;
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
            iterator->frame_kinds[depth] = ITERATOR_FRAME_REPEAT;
            iterator->values[depth] = 0;
        } else {
            const uint32_t node_level = mtbdd_node_variable(iterator->current);
            if (node_level < level) {
                iterator->finished = 1;
                return SYLVAN_ERR_INVALID;
            }
            if (node_level == level) {
                iterator->frame_kinds[depth] = ITERATOR_FRAME_NODE;
                iterator->values[depth] = 0;
                iterator->current = mtbdd_node_low(iterator->current);
            } else if (iterator->mode == SYLVAN_ITERATOR_MINTERMS) {
                iterator->frame_kinds[depth] = ITERATOR_FRAME_REPEAT;
                iterator->values[depth] = 0;
            } else {
                iterator->frame_kinds[depth] = ITERATOR_FRAME_SINGLE;
                iterator->values[depth] = 2;
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
