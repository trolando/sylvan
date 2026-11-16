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

#include <sylvan_int.h>

#include <stdlib.h>
#include <string.h>

/* Private validation results; the public API returns 1 on success, 0 on error. */
enum { ITERATOR_OK = 0, ITERATOR_INVALID = -1, ITERATOR_OOM = -2 };

enum iterator_family {
    ITERATOR_BDD,
    ITERATOR_MTBDD,
    ITERATOR_ZDD,
    ITERATOR_LDD
};

enum iterator_frame_kind {
    ITERATOR_FRAME_SINGLE,
    ITERATOR_FRAME_NODE,
    ITERATOR_FRAME_REPEAT,
    ITERATOR_FRAME_REPEAT_NONEMPTY
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
    uint32_t *lddmc_values;
};

static void iterator_free(sylvan_iterator *iterator);

static void
iterator_protect(sylvan_iterator *iterator)
{
    if (iterator->family == ITERATOR_LDD) {
        lddmc_protect(&iterator->root);
    } else if (iterator->family == ITERATOR_ZDD) {
        zdd_protect(&iterator->root);
        zdd_protect(&iterator->variables);
    } else {
        mtbdd_protect(&iterator->root);
        mtbdd_protect(&iterator->variables);
    }
}

static void
iterator_unprotect(sylvan_iterator *iterator)
{
    if (iterator->family == ITERATOR_LDD) {
        lddmc_unprotect(&iterator->root);
    } else if (iterator->family == ITERATOR_ZDD) {
        zdd_unprotect(&iterator->root);
        zdd_unprotect(&iterator->variables);
    } else {
        mtbdd_unprotect(&iterator->root);
        mtbdd_unprotect(&iterator->variables);
    }
}

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
        return ITERATOR_OOM;
    }

    struct iterator_visit *entries =
        calloc(new_capacity, sizeof(*entries));
    uint8_t *occupied = calloc(new_capacity, sizeof(*occupied));
    if (entries == NULL || occupied == NULL) {
        free(entries);
        free(occupied);
        return ITERATOR_OOM;
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
    return ITERATOR_OK;
}

static int
iterator_visit_set_insert(struct iterator_visit_set *set,
                          uint64_t node, size_t depth)
{
    if (set->capacity == 0 || set->size >= set->capacity / 2) {
        const int status = iterator_visit_set_grow(set);
        if (status != ITERATOR_OK) return status;
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

/* Check the arena bound, not the hash-table size: GC can shrink the latter
 * below a live node index. Handles must otherwise refer to live nodes of the
 * requested family; node records do not carry a family tag. */
static int
iterator_valid_handle(uint64_t dd, enum iterator_family family)
{
    if (family == ITERATOR_LDD) return dd < llmsset_get_data_size(nodes);
    if (family == ITERATOR_ZDD && (dd == zdd_false || dd == zdd_true)) return 1;
    uint64_t index = MTBDD_GETINDEX(dd);
    return (dd & ~(mtbdd_complement | UINT64_C(0x000000ffffffffff))) == 0 &&
        (index == 0 || (index >= 2 && index < llmsset_get_data_size(nodes)));
}

static int
iterator_internal_node(uint64_t dd)
{
    return MTBDD_GETINDEX(dd) >= 2 && !mtbdd_isleaf(dd) &&
        !mtbddnode_ismapnode(MTBDD_GETNODE(dd)) &&
        !zddnode_ismapnode(ZDD_GETNODE(dd));
}

/* A BDD variable set is a positive cube; a ZDD domain is its power set. */
static int
iterator_read_domain(sylvan_iterator *iterator)
{
    uint64_t cursor = iterator->variables;
    uint32_t previous = 0;
    const int zdd = iterator->family == ITERATOR_ZDD;
    const uint64_t terminal = zdd ? zdd_true : sylvan_true;
    while (cursor != terminal) {
        if (!iterator_valid_handle(cursor, iterator->family) ||
            !iterator_internal_node(cursor)) return ITERATOR_INVALID;
        uint32_t level = zdd ? zdd_getvar(cursor) : mtbdd_getvar(cursor);
        uint64_t low = zdd ? zdd_getlow(cursor) : mtbdd_getlow(cursor);
        uint64_t high = zdd ? zdd_gethigh(cursor) : mtbdd_gethigh(cursor);
        if ((iterator->count != 0 && level <= previous) ||
            (zdd ? low != high : low != mtbdd_false)) return ITERATOR_INVALID;
        previous = level;
        iterator->count++;
        cursor = high;
    }
    if (iterator->count > SIZE_MAX / sizeof(*iterator->nodes)) return ITERATOR_OOM;
    if (iterator->count == 0) return ITERATOR_OK;
    iterator->levels = malloc(iterator->count * sizeof(*iterator->levels));
    if (iterator->levels == NULL) return ITERATOR_OOM;
    cursor = iterator->variables;
    for (size_t i=0; i<iterator->count; i++) {
        iterator->levels[i] = zdd ? zdd_getvar(cursor) : mtbdd_getvar(cursor);
        cursor = zdd ? zdd_gethigh(cursor) : mtbdd_gethigh(cursor);
    }
    return ITERATOR_OK;
}

/* Validate without constructing support diagrams or using the operation cache.
 * The private visited set bounds work on shared subgraphs and keeps independent
 * iterators independent. For LDDs, depth is part of the key: a shared suffix
 * reached at different tuple positions must still have the required width. */
static int
iterator_validate_graph(const sylvan_iterator *iterator)
{
    struct iterator_visit_set seen = {0};
    size_t stack_size = 0, stack_capacity = 64;
    struct iterator_visit *stack = malloc(stack_capacity * sizeof(*stack));
    if (stack == NULL) return ITERATOR_OOM;
    stack[stack_size++] = (struct iterator_visit){iterator->root, 0};
    int status = ITERATOR_OK;

    while (stack_size != 0) {
        const struct iterator_visit current = stack[--stack_size];
        const uint64_t dd = current.node;
        if (!iterator_valid_handle(dd, iterator->family)) {
            status = ITERATOR_INVALID;
            break;
        }
        int inserted = iterator_visit_set_insert(&seen, dd, current.depth);
        if (inserted < 0) { status = inserted; break; }
        if (inserted == 0) continue;

        uint64_t children[2];
        size_t child_depth[2];
        if (iterator->family == ITERATOR_LDD) {
            if (dd == lddmc_false) continue;
            if (dd == lddmc_true) {
                if (current.depth != iterator->count) status = ITERATOR_INVALID;
                if (status != ITERATOR_OK) break;
                continue;
            }
            if (current.depth >= iterator->count || lddmc_iscopy(dd)) {
                status = ITERATOR_INVALID;
                break;
            }
            children[0] = lddmc_getright(dd);
            children[1] = lddmc_getdown(dd);
            child_depth[0] = current.depth;
            child_depth[1] = current.depth + 1;
        } else {
            const int zdd = iterator->family == ITERATOR_ZDD;
            if (dd == (zdd ? zdd_false : mtbdd_false) ||
                dd == (zdd ? zdd_true : mtbdd_true)) continue;
            if (mtbdd_isleaf(dd)) {
                if (iterator->family != ITERATOR_MTBDD) status = ITERATOR_INVALID;
                if (status != ITERATOR_OK) break;
                continue;
            }
            if (!iterator_internal_node(dd)) { status = ITERATOR_INVALID; break; }
            uint32_t level = zdd ? zdd_getvar(dd) : mtbdd_getvar(dd);
            size_t first = current.depth, last = iterator->count;
            while (first < last) {
                size_t mid = first + (last-first)/2;
                if (iterator->levels[mid] < level) first = mid+1;
                else last = mid;
            }
            if (first == iterator->count || iterator->levels[first] != level) {
                status = ITERATOR_INVALID;
                break;
            }
            children[0] = zdd ? zdd_getlow(dd) : mtbdd_getlow(dd);
            children[1] = zdd ? zdd_gethigh(dd) : mtbdd_gethigh(dd);
            child_depth[0] = child_depth[1] = first+1;
        }

        if (stack_size > stack_capacity-2) {
            size_t new_capacity = stack_capacity * 2;
            if (new_capacity < stack_capacity || new_capacity > SIZE_MAX / sizeof(*stack)) {
                status = ITERATOR_OOM;
                break;
            }
            struct iterator_visit *grown = realloc(stack, new_capacity * sizeof(*stack));
            if (grown == NULL) { status = ITERATOR_OOM; break; }
            stack = grown;
            stack_capacity = new_capacity;
        }
        for (size_t i=0; i<2; i++) {
            stack[stack_size++] = (struct iterator_visit){children[i], child_depth[i]};
        }
    }
    free(stack);
    free(seen.entries);
    free(seen.occupied);
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
        return ITERATOR_INVALID;
    }

    sylvan_iterator *iterator = calloc(1, sizeof(*iterator));
    if (iterator == NULL) return ITERATOR_OOM;

    iterator->root = dd;
    iterator->variables = variables;
    iterator->current = dd;
    iterator->mode = mode;
    iterator->accept_leaf = accept_leaf;
    iterator->filter_context = filter_context;
    iterator->family = family;

    int status = iterator_read_domain(iterator);
    if (status == ITERATOR_OK) status = iterator_validate_graph(iterator);
    if (status != ITERATOR_OK) {
        iterator_free(iterator);
        return status;
    }

    if (iterator->count != 0) {
        iterator->nodes = malloc(iterator->count * sizeof(*iterator->nodes));
        iterator->frame_kinds = malloc(iterator->count * sizeof(*iterator->frame_kinds));
        iterator->values = malloc(iterator->count * sizeof(*iterator->values));
        if (iterator->levels == NULL || iterator->nodes == NULL ||
            iterator->frame_kinds == NULL || iterator->values == NULL) {
            iterator_free(iterator);
            return ITERATOR_OOM;
        }
    }

    iterator_protect(iterator);
    *destination = iterator;
    sylvan_stats_count(SYLVAN_ITERATOR_CREATED);
    return ITERATOR_OK;
}

TASK_IMPL_4(int, sylvan_iterator_create, BDD, dd, BDDSET, variables,
            sylvan_iterator_mode, mode, sylvan_iterator**, result)
{
    return iterator_create(result, dd, variables, mode, ITERATOR_BDD, NULL, NULL) == ITERATOR_OK;
}

TASK_IMPL_4(int, mtbdd_iterator_create, MTBDD, dd, BDDSET, variables,
            const mtbdd_iterator_options*, options, sylvan_iterator**, result)
{
    if (options == NULL) return 0;
    return iterator_create(result, dd, variables, options->mode, ITERATOR_MTBDD,
                           options->accept_leaf, options->context) == ITERATOR_OK;
}

TASK_IMPL_3(int, zdd_iterator_create, ZDD, dd, ZDD, domain, sylvan_iterator**, result)
{
    return iterator_create(result, dd, domain, SYLVAN_ITERATOR_MINTERMS,
                           ITERATOR_ZDD, NULL, NULL) == ITERATOR_OK;
}

TASK_IMPL_2(int, lddmc_iterator_create, MDD, dd, sylvan_iterator**, result)
{
    if (result == NULL || !iterator_valid_handle(dd, ITERATOR_LDD)) return 0;
    size_t count = 0;
    MDD cursor = dd;
    if (dd != lddmc_false) {
        while (cursor > lddmc_true) {
            if (!iterator_valid_handle(cursor, ITERATOR_LDD) ||
                lddmc_iscopy(cursor) || count >= SIZE_MAX / sizeof(MDD)) return 0;
            count++;
            cursor = lddmc_getdown(cursor);
        }
        if (cursor != lddmc_true) return 0;
    }
    sylvan_iterator *iterator = calloc(1, sizeof(*iterator));
    if (iterator == NULL) return 0;
    iterator->root = iterator->current = dd;
    iterator->count = count;
    iterator->family = ITERATOR_LDD;
    if (iterator_validate_graph(iterator) != ITERATOR_OK) {
        iterator_free(iterator);
        return 0;
    }
    if (count != 0) {
        iterator->nodes = malloc(count * sizeof(*iterator->nodes));
        iterator->lddmc_values = malloc(count * sizeof(*iterator->lddmc_values));
        if (iterator->nodes == NULL || iterator->lddmc_values == NULL) {
            iterator_free(iterator);
            return 0;
        }
    }
    iterator_protect(iterator);
    *result = iterator;
    sylvan_stats_count(SYLVAN_ITERATOR_CREATED);
    return 1;
}

/* Failed creation has never protected roots. Successful iterators reach here
 * after destruction has unprotected their roots. */
static void
iterator_free(sylvan_iterator *iterator)
{
    free(iterator->lddmc_values);
    free(iterator->values);
    free(iterator->frame_kinds);
    free(iterator->nodes);
    free(iterator->levels);
    free(iterator);
}

void
sylvan_iterator_destroy(sylvan_iterator *iterator)
{
    if (iterator == NULL) return;
    iterator_unprotect(iterator);
    iterator_free(iterator);
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
                ? (MTBDD)zdd_gethigh((ZDD)iterator->nodes[depth])
                : mtbdd_gethigh(iterator->nodes[depth]);
            iterator->depth++;
            return 1;
        }
        if (iterator->frame_kinds[depth] == ITERATOR_FRAME_REPEAT_NONEMPTY &&
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
        return ITERATOR_INVALID;
    }
    if (iterator->finished) {
        *has_item = 0;
        return ITERATOR_OK;
    }

    if (iterator->resume) {
        iterator->resume = 0;
        if (!iterator_backtrack(iterator)) {
            *has_item = 0;
            return ITERATOR_OK;
        }
    }

    for (;;) {
        if (iterator->current == mtbdd_false) {
            if (!iterator_backtrack(iterator)) {
                *has_item = 0;
                return ITERATOR_OK;
            }
            continue;
        }

        const int is_leaf = iterator->family == ITERATOR_ZDD
            ? zdd_isleaf((ZDD)iterator->current)
            : mtbdd_isleaf(iterator->current);
        if (is_leaf && iterator->family == ITERATOR_BDD &&
            iterator->current != sylvan_true) {
            iterator->finished = 1;
            return ITERATOR_INVALID;
        }
        if (is_leaf && iterator->family == ITERATOR_ZDD &&
            iterator->current != zdd_true) {
            iterator->finished = 1;
            return ITERATOR_INVALID;
        }
        if (is_leaf && iterator->family == ITERATOR_MTBDD &&
            iterator->accept_leaf != NULL &&
            !iterator->accept_leaf(
                iterator->current, iterator->filter_context)) {
            if (!iterator_backtrack(iterator)) {
                *has_item = 0;
                return ITERATOR_OK;
            }
            continue;
        }

        if ((iterator->mode == SYLVAN_ITERATOR_CUBES && is_leaf) ||
            iterator->depth == iterator->count) {
            if (!is_leaf) {
                iterator->finished = 1;
                return ITERATOR_INVALID;
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
            /* Replaying a skipped variable is useful only if its first branch
             * emitted an item. Otherwise an entirely rejected subgraph would
             * be retried for every assignment to the skipped prefix. */
            for (size_t i = 0; i < iterator->depth; i++) {
                if (iterator->frame_kinds[i] == ITERATOR_FRAME_REPEAT) {
                    iterator->frame_kinds[i] = ITERATOR_FRAME_REPEAT_NONEMPTY;
                }
            }
            *leaf = iterator->current;
            *has_item = 1;
            iterator->resume = 1;
            sylvan_stats_count(SYLVAN_ITERATOR_ITEMS);
            return ITERATOR_OK;
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
                ? zdd_getvar((ZDD)iterator->current)
                : mtbdd_getvar(iterator->current);
            if (node_level < level) {
                iterator->finished = 1;
                return ITERATOR_INVALID;
            }
            if (node_level == level) {
                iterator->frame_kinds[depth] = ITERATOR_FRAME_NODE;
                iterator->values[depth] = 0;
                iterator->current = iterator->family == ITERATOR_ZDD
                    ? (MTBDD)zdd_getlow((ZDD)iterator->current)
                    : mtbdd_getlow(iterator->current);
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

TASK_IMPL_4(int, sylvan_iterator_next, sylvan_iterator*, iterator,
            uint8_t*, values, size_t, count, int*, has_item)
{
    if (iterator == NULL || iterator->family != ITERATOR_BDD) return 0;
    MTBDD leaf;
    return iterator_next(iterator, values, count, &leaf, has_item) == ITERATOR_OK;
}

TASK_IMPL_5(int, mtbdd_iterator_next, sylvan_iterator*, iterator,
            uint8_t*, values, size_t, count, MTBDD*, leaf, int*, has_item)
{
    if (iterator == NULL || iterator->family != ITERATOR_MTBDD) return 0;
    return iterator_next(iterator, values, count, leaf, has_item) == ITERATOR_OK;
}

TASK_IMPL_4(int, zdd_iterator_next, sylvan_iterator*, iterator,
            uint8_t*, values, size_t, count, int*, has_item)
{
    if (iterator == NULL || iterator->family != ITERATOR_ZDD) return 0;
    MTBDD leaf;
    return iterator_next(iterator, values, count, &leaf, has_item) == ITERATOR_OK;
}

static int
lddmc_iterator_backtrack(sylvan_iterator *iterator)
{
    while (iterator->depth != 0) {
        const size_t depth = --iterator->depth;
        const MDD right =
            lddmc_getright((MDD)iterator->nodes[depth]);
        if (right != lddmc_false) {
            iterator->current = (MTBDD)right;
            return 1;
        }
    }

    iterator->finished = 1;
    return 0;
}

TASK_IMPL_4(int, lddmc_iterator_next, sylvan_iterator*, iterator,
            uint32_t*, values, size_t, count, int*, has_item)
{
    if (iterator == NULL || iterator->family != ITERATOR_LDD ||
        has_item == NULL || count != iterator->count ||
        (count != 0 && values == NULL)) {
        return 0;
    }
    if (iterator->finished) {
        *has_item = 0;
        return 1;
    }

    if (iterator->resume) {
        iterator->resume = 0;
        if (!lddmc_iterator_backtrack(iterator)) {
            *has_item = 0;
            return 1;
        }
    }

    for (;;) {
        const MDD current = (MDD)iterator->current;
        if (current == lddmc_false) {
            if (!lddmc_iterator_backtrack(iterator)) {
                *has_item = 0;
                return 1;
            }
            continue;
        }
        if (current == lddmc_true) {
            if (iterator->depth != iterator->count) {
                iterator->finished = 1;
                return 0;
            }
            if (count != 0) {
                memcpy(values, iterator->lddmc_values,
                       count * sizeof(*values));
            }
            *has_item = 1;
            iterator->resume = 1;
            sylvan_stats_count(SYLVAN_ITERATOR_ITEMS);
            return 1;
        }
        if (iterator->depth == iterator->count ||
            lddmc_iscopy(current)) {
            iterator->finished = 1;
            return 0;
        }

        const size_t depth = iterator->depth++;
        iterator->nodes[depth] = (MTBDD)current;
        iterator->lddmc_values[depth] = lddmc_getvalue(current);
        iterator->current = (MTBDD)lddmc_getdown(current);
    }
}

void
mtbdd_iterator_destroy(sylvan_iterator *iterator)
{
    sylvan_iterator_destroy(iterator);
}

void
zdd_iterator_destroy(sylvan_iterator *iterator)
{
    sylvan_iterator_destroy(iterator);
}

void
lddmc_iterator_destroy(sylvan_iterator *iterator)
{
    sylvan_iterator_destroy(iterator);
}
