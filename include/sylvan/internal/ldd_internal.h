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

/* Do not include this file directly. Instead, include sylvan_int.h */

/**
 * Internals for LDDs
 */

#ifndef SYLVAN_LDD_INT_H
#define SYLVAN_LDD_INT_H

/**
 * LDD node structure
 *
 * RRRR RRRR RRVV VV-- | DDDD DDDD DDVV VV--
 * 
 */
typedef struct __attribute__((packed)) mddnode {
    uint64_t a, b;
} * mddnode_t; // 16 bytes

static inline mddnode_t
LDD_GETNODE(MDD mdd)
{
    return ((mddnode_t)nodes_get_pointer(nodes, mdd));
}

static inline uint32_t __attribute__((unused))
mddnode_getvalue(mddnode_t n)
{
    return ((n->a >> 40) & 0xffff) | ((n->b >> 24) & 0xffff0000);
}

static inline uint8_t __attribute__((unused))
mddnode_getmark(mddnode_t n)
{
    return n->a & 0x4000000000000000 ? 1 : 0;
}

static inline uint8_t __attribute__((unused))
mddnode_getcopy(mddnode_t n)
{
    return n->a & 0x8000000000000000 ? 1 : 0;
}

static inline uint64_t __attribute__((unused))
mddnode_getright(mddnode_t n)
{
    return n->a & 0x000000ffffffffff;
}

static inline uint64_t __attribute__((unused))
mddnode_getdown(mddnode_t n)
{
    return n->b & 0x000000ffffffffff;
}

static inline uint32_t __attribute__((unused))
mddnode_old_getvalue(mddnode_t n)
{
    return *(uint32_t*)((uint8_t*)n+6);
}

static inline uint8_t __attribute__((unused))
mddnode_old_getmark(mddnode_t n)
{
    return n->a & 1;
}

static inline uint8_t __attribute__((unused))
mddnode_old_getcopy(mddnode_t n)
{
    return n->b & 0x10000 ? 1 : 0;
}

static inline uint64_t __attribute__((unused))
mddnode_old_getright(mddnode_t n)
{
    return (n->a & 0x0000ffffffffffff) >> 1;
}

static inline uint64_t __attribute__((unused))
mddnode_old_getdown(mddnode_t n)
{
    return n->b >> 17;
}

static inline void __attribute__((unused))
mddnode_setmark(mddnode_t n, uint8_t mark)
{
    // FIXME this should not exist at all!!
    n->a = (n->a & 0xbfffffffffffffff) | (mark ? 0x4000000000000000 : 0);
}

static inline void __attribute__((unused))
mddnode_make(mddnode_t n, uint32_t value, uint64_t right, uint64_t down)
{
    n->a = right | (((uint64_t)value & 0x0000ffff) << 40);
    n->b = down  | (((uint64_t)value & 0xffff0000) << 24);
}

static inline void __attribute__((unused))
mddnode_makecopy(mddnode_t n, uint64_t right, uint64_t down)
{
    n->a = right | 0x8000000000000000;
    n->b = down;
}

#endif
