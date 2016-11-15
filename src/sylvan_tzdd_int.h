/*
 * Copyright 2016 Tom van Dijk, Johannes Kepler University Linz
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
 * Internals for multi-terminal ternary TZDDs (clause ZDDs)
 *
 * A TZDD has three outgoing edges: positive, negative, zero
 *
 * Limitations due to allocating 16 bytes per node:
 * - 32 bits to address nodes (so max 2^32 = 4 billion nodes = 96 GB memory)
 * - 28 bits for variables (so max 2^28 = 268,435,456 variables)
 * - No support (yet) for complement edges
 *
 * Bits (not as they are actually in memory)
 * 127        1 leaf            set if this is a MT leaf node
 * 126        1 mark            used for node marking
 * 125        1 map             is a MAP node, for compose etc
 * 124        1 unused
 * 123..96   28 variable        variable of this node
 *  95..64   32 true index      index of the true edge
 *  63..31   32 zero index      index of the zero edge
 *  31..0    32 false index     index of the false edge
 *
 * Lil endian: TTTT TTTT VVVV VVxV | FFFF FFFF NNNN NNNN
 * Big endian: xVVV VVVV TTTT TTTT | NNNN NNNN FFFF FFFF
 * Leaf nodes: x*** **** TTTT TTTT | VVVV VVVV VVVV VVVV (big endian)
 *
 * Edges:      0000 0000 IIII IIII  ( no complement edges )
 */

#ifndef SYLVAN_TZDD_INT_H
#define SYLVAN_TZDD_INT_H

/**
 * TZDD node structure
 */
typedef struct __attribute__((packed)) tzddnode {
    uint64_t a, b;
} * tzddnode_t; // 16 bytes

/**
 * Macros to work with the TZDD type
 * Currently complement marks are not supported; maybe in the future
 */
#define TZDD_GETINDEX(tzdd)          ((tzdd & 0x00000000ffffffff))
#define TZDD_GETNODE(tzdd)           ((tzddnode_t)llmsset_index_to_ptr(nodes, TZDD_GETINDEX(tzdd)))

static inline int __attribute__((unused))
tzddnode_isleaf(tzddnode_t n)
{
    return n->a & 0x8000000000000000 ? 1 : 0;
}

static inline uint32_t __attribute__((unused))
tzddnode_gettype(tzddnode_t n)
{
    return (uint32_t)n->a;
}

static inline uint64_t __attribute__((unused))
tzddnode_getvalue(tzddnode_t n)
{
    return n->b;
}

static inline uint64_t __attribute__((unused))
tzddnode_getpos(tzddnode_t n)
{
    return (uint32_t)n->a;
}

static inline uint64_t __attribute__((unused))
tzddnode_getneg(tzddnode_t n)
{
    return (uint32_t)n->b;
}

static inline uint64_t __attribute__((unused))
tzddnode_getzero(tzddnode_t n)
{
    return (uint32_t)(n->b >> 32);
}

static inline uint32_t __attribute__((unused))
tzddnode_getvariable(tzddnode_t n)
{
    return (uint32_t)((n->a >> 32) & 0x000000000fffffff);
}

static inline int __attribute__((unused))
tzddnode_getmark(tzddnode_t n)
{
    return n->a & 0x4000000000000000 ? 1 : 0;
}

static inline void __attribute__((unused))
tzddnode_setmark(tzddnode_t n, int mark)
{
    if (mark) n->a |= 0x4000000000000000;
    else n->a &= 0xbfffffffffffffff;
}

static inline void __attribute__((unused))
tzddnode_makeleaf(tzddnode_t n, uint32_t type, uint64_t value)
{
    n->a = 0x8000000000000000 | (uint64_t)type;
    n->b = value;
}

static inline void __attribute__((unused))
tzddnode_makenode(tzddnode_t n, uint32_t var, uint64_t pos, uint64_t neg, uint64_t zero)
{
    n->a = pos | ((uint64_t)var << 32);
    n->b = neg | (zero << 32);
}

static inline void __attribute__((unused))
tzddnode_makemapnode(tzddnode_t n, uint32_t var, uint64_t pos, uint64_t neg)
{
    n->a = 0x2000000000000000 | pos | ((uint64_t)var << 32);
    n->b = neg;
}

static inline int __attribute__((unused))
tzddnode_ismapnode(tzddnode_t n)
{
    return n->a & 0x2000000000000000 ? 1 : 0;
}

static TZDD __attribute__((unused))
tzddnode_pos(TZDD tzdd, tzddnode_t node)
{
    return tzddnode_getpos(node);
    (void)tzdd;
}

static TZDD __attribute__((unused))
tzddnode_neg(TZDD tzdd, tzddnode_t node)
{
    return tzddnode_getneg(node);
    (void)tzdd;
}

static TZDD __attribute__((unused))
tzddnode_zero(TZDD tzdd, tzddnode_t node)
{
    return tzddnode_getzero(node);
    (void)tzdd;
}

#endif
