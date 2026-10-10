/*
 * Copyright 2026 Tom van Dijk, Formal Methods and Tools, University of Twente
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

/**
 * The anchor of a node in the list and trie nodes tables: the slot whose
 * family the node joins, and so the one family that a lookup of its record
 * searches. The lookup, the rebuild after a collection and the reorder
 * primitives each derive it from the record on their own, so it has to be a
 * function of the record and of nothing else.
 *
 * Every leaf joins the family anchored at slot 1, which no node occupies. A
 * decision or map node joins the family of one of its two children, chosen by
 * SYLVAN_NODES_ANCHOR:
 *
 *   max     (the default) the child with the larger index. A node is
 *           normally made right after its children, so this is the child made
 *           last, and it is never the terminal, index 0, unless both are.
 *   min     the child with the smaller index, unless that is the terminal.
 *   parity  one bit of a multiplicative hash of the pair picks the child,
 *           unless that is the terminal. A child that many nodes share then
 *           anchors about half of them, where the other two rules give it
 *           either nearly all of them or nearly none.
 *
 * The backends rely only on the anchor being one of the children: a node is
 * never its own anchor, a node nothing refers to anchors nothing, and no two
 * nodes can each anchor the other.
 */

#ifndef SYLVAN_NODES_ANCHOR_H
#define SYLVAN_NODES_ANCHOR_H

#include <stdint.h>

#define NODES_ANCHOR_MAX    0
#define NODES_ANCHOR_MIN    1
#define NODES_ANCHOR_PARITY 2

#ifndef SYLVAN_NODES_ANCHOR
#define SYLVAN_NODES_ANCHOR NODES_ANCHOR_MAX
#endif

static inline uint64_t
nodes_anchor(uint64_t a, uint64_t b)
{
    if (a & UINT64_C(0x4000000000000000)) return 1; // it's a leaf
    const uint64_t A = a & UINT64_C(0x000000ffffffffff);
    const uint64_t B = b & UINT64_C(0x000000ffffffffff);
#if SYLVAN_NODES_ANCHOR == NODES_ANCHOR_MAX
    return A > B ? A : B;
#elif SYLVAN_NODES_ANCHOR == NODES_ANCHOR_MIN || \
      SYLVAN_NODES_ANCHOR == NODES_ANCHOR_PARITY
    if (A == 0) return B;
    if (B == 0) return A;
#if SYLVAN_NODES_ANCHOR == NODES_ANCHOR_MIN
    return A < B ? A : B;
#else
    return (((A ^ B) * UINT64_C(0x9e3779b97f4a7c15)) >> 63) ? A : B;
#endif
#else
#error "SYLVAN_NODES_ANCHOR must be NODES_ANCHOR_MAX, _MIN or _PARITY"
#endif
}

#endif
