/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
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

/* Do not include this file directly. Instead, include <sylvan/internal.h>. */

/**
 * Internals for ListDDs
 */

#ifndef SYLVAN_LISTDD_INT_H
#define SYLVAN_LISTDD_INT_H

/**
 * ListDD node structure
 *
 * RRRR RRRR RRVV VV-- | DDDD DDDD DDVV VV--
 *
 */
typedef struct mddnode {
    uint64_t a, b;
} mddnode; // 16 bytes

static_assert(sizeof(struct mddnode) == 16, "mddnode should be a 16 byte struct");

static inline mddnode*
LDD_GETNODE(LISTDD mdd)
{
    return ((mddnode*)nodes_get_pointer(nodes, mdd));
}

static inline uint32_t SYLVAN_UNUSED
mddnode_getvalue(const mddnode* n)
{
    return ((n->a >> 40) & 0xffff) | ((n->b >> 24) & 0xffff0000);
}

static inline uint8_t SYLVAN_UNUSED
mddnode_getmark(const mddnode* n)
{
    return n->a & 0x4000000000000000 ? 1 : 0;
}

static inline uint8_t SYLVAN_UNUSED
mddnode_getcopy(const mddnode* n)
{
    return n->a & 0x8000000000000000 ? 1 : 0;
}

static inline uint64_t SYLVAN_UNUSED
mddnode_getright(const mddnode* n)
{
    return n->a & 0x000000ffffffffff;
}

static inline uint64_t SYLVAN_UNUSED
mddnode_getdown(const mddnode* n)
{
    return n->b & 0x000000ffffffffff;
}

static inline uint32_t SYLVAN_UNUSED
mddnode_old_getvalue(const mddnode* n)
{
    return *(uint32_t*)((uint8_t*)n+6);
}

static inline uint8_t SYLVAN_UNUSED
mddnode_old_getmark(const mddnode* n)
{
    return n->a & 1;
}

static inline uint8_t SYLVAN_UNUSED
mddnode_old_getcopy(const mddnode* n)
{
    return n->b & 0x10000 ? 1 : 0;
}

static inline uint64_t SYLVAN_UNUSED
mddnode_old_getright(const mddnode* n)
{
    return (n->a & 0x0000ffffffffffff) >> 1;
}

static inline uint64_t SYLVAN_UNUSED
mddnode_old_getdown(const mddnode* n)
{
    return n->b >> 17;
}

static inline void SYLVAN_UNUSED
mddnode_setmark(mddnode* n, uint8_t mark)
{
    // FIXME this should not exist at all!!
    //       we need an alternative mechanism to mark stuff
    n->a = (n->a & 0xbfffffffffffffff) | (mark ? 0x4000000000000000 : 0);
}

static inline void SYLVAN_UNUSED
mddnode_make(mddnode* n, uint32_t value, uint64_t right, uint64_t down)
{
    n->a = right | (((uint64_t)value & 0x0000ffff) << 40);
    n->b = down  | (((uint64_t)value & 0xffff0000) << 24);
}

static inline void SYLVAN_UNUSED
mddnode_makecopy(mddnode* n, uint64_t right, uint64_t down)
{
    n->a = right | 0x8000000000000000;
    n->b = down;
}

/**
 * Advanced migration access to canonical metadata encodings.
 *
 * New code should use the typed builders in <sylvan/listdd.h>.
 */
int listdd_projection_create_raw(listdd_projection **result, LISTDD raw);
LISTDD listdd_projection_raw(const listdd_projection *projection);
int listdd_relation_layout_create_raw(listdd_relation_layout **result, LISTDD raw);
LISTDD listdd_relation_layout_raw(const listdd_relation_layout *layout);

int listdd_relation_add_raw(
    LISTDD *result,
    LISTDD relation,
    const uint32_t *values,
    const int *copy,
    size_t count);
int listdd_relation_contains_raw(
    LISTDD relation,
    const uint32_t *values,
    const int *copy,
    size_t count);
int listdd_relation_singleton_raw(
    LISTDD *result,
    const uint32_t *values,
    const int *copy,
    size_t count);

TASK(int, listdd_match_raw, LISTDD*, result, LISTDD, a, LISTDD, b, LISTDD, projection)
TASK(int, listdd_rel_next_raw, LISTDD*, result, LISTDD, set, LISTDD, relation, LISTDD, metadata)
TASK(int, listdd_rel_next_union_raw, LISTDD*, result, LISTDD, set, LISTDD, relation, LISTDD, metadata, LISTDD, un)
TASK(int, listdd_rel_prev_raw, LISTDD*, result, LISTDD, dd, LISTDD, relation, LISTDD, metadata, LISTDD, universe)
TASK(int, listdd_project_raw, LISTDD*, result, LISTDD, dd, LISTDD, projection)
TASK(int, listdd_project_diff_raw, LISTDD*, result, LISTDD, dd, LISTDD, projection, LISTDD, avoid)
TASK(int, listdd_join_raw, LISTDD*, result, LISTDD, a, LISTDD, b, LISTDD, a_projection, LISTDD, b_projection)

#endif
