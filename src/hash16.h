/*
 * Copyright 2011-2014 Formal Methods and Tools, University of Twente
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

#include <stdint.h>

#ifndef HASH16_H
#define HASH16_H

#ifndef rotl64
static inline uint64_t
rotl64(uint64_t x, int8_t r)
{
    return ((x<<r) | (x>>(64-r)));
}
#endif

uint64_t
rehash16_mul(const void *key, const uint64_t seed)
{
    const uint64_t prime = 1099511628211;
    const uint64_t *p = (const uint64_t *)key;

    uint64_t hash = seed;
    hash = hash ^ p[0];
    hash = rotl64(hash, 47);
    hash = hash * prime;
    hash = hash ^ p[1];
    hash = rotl64(hash, 31);
    hash = hash * prime;

    return hash ^ (hash >> 32);
}

uint64_t
hash16_mul(const void *key)
{
    return rehash16_mul(key, 14695981039346656037LLU);
}

#endif
