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

#include <stdint.h>
#include <sylvan.h>

#ifndef SYLVAN_SKIPLIST_H
#define SYLVAN_SKIPLIST_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Implementation of a simple limited-depth skiplist.
 * The skiplist is used by the serialization mechanism in Sylvan.
 * Each stored MTBDD is assigned a number starting with 1.
 * Each bucket takes 32 bytes.
 */

typedef struct skiplist *skiplist_t;

/**
 * Allocate a new skiplist of maximum size <size>.
 * Only supports at most 0x7fffffff (max int32) buckets
 */
skiplist_t skiplist_alloc(size_t size);

/**
 * Free the given skiplist.
 */
void skiplist_free(skiplist_t sl);

/**
 * Get the number assigned to the given node <dd>.
 * Returns 0 if no number was assigned.
 */
uint64_t skiplist_get(skiplist_t sl, MTBDD dd);

/**
 * Assign the next number (starting at 1) to the given node <dd>.
 */
VOID_TASK_DECL_2(skiplist_assign_next, skiplist_t, MTBDD);
#define skiplist_assign_next(sl, dd) CALL(skiplist_assign_next, sl, dd)

/**
 * Give the number of assigned nodes. (numbers 1,2,...,N)
 */
size_t skiplist_count(skiplist_t sl);

/**
 * Get the MTBDD assigned to the number <index>, with the index 1,...,count.
 */
MTBDD skiplist_getr(skiplist_t sl, uint64_t index);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
