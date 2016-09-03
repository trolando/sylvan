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

#ifndef SYLVAN_REORDER_H
#define SYLVAN_REORDER_H

#include <sylvan.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Sylvan dynamic variable reordering
 */
void sylvan_init_reorder(void);

/**
 * When using dynamic variable reordering, it is strongly recommended to use
 * "levels" instead of working directly with the internal variables.
 *
 * Dynamic variable reordering requires that variables are consecutive.
 * Initially, variables are assigned linearly, starting with 0.
 */

/**
 * Create the next level and return the BDD representing the variable (ithlevel)
 */
MTBDD mtbdd_newlevel(void);

/**
 * Create the next <amount> levels
 */
void mtbdd_newlevels(size_t amount);

/**
 * Reset all levels.
 */
void mtbdd_levels_reset(void);

/**
 * Create or get the BDD representing "if <level> then true else false"
 */
MTBDD mtbdd_ithlevel(uint32_t level);

/**
 * Get the current level of the given internal variable <var>
 */
uint32_t mtbdd_var_to_level(uint32_t var);

/**
 * Get the current internal variable of the given level <level>
 */
uint32_t mtbdd_level_to_var(uint32_t level);

/**
 * Return the level of the given internal node.
 */
uint32_t mtbdd_getlevel(MTBDD node);
#define sylvan_level mtbdd_getlevel

/**
 * sylvan_varswap implements parallelized in-place variable swapping.
 *
 * Swaps two consecutive variables in the entire forest.
 * We assume there are only BDD/MTBDD/MAP nodes in the forest.
 * The operation is not thread-safe, so make sure no other Sylvan operations are
 * done when performing variable swapping.
 *
 * Variable swapping consists of two phases. The first phase performs
 * variable swapping on all simple cases. The cases that require node
 * lookups are left marked. The second phase then fixes the marked nodes.
 *
 * It is recommended to clear the cache and perform clear-and-mark (the first part of garbage
 * collection, before resizing and rehashing) before running sylvan_varswap.
 *
 * If the parameter <recovery> is set, then phase 1 only rehashes nodes that have variable "var+1".
 * Phase 2 will not abort on the first error, but try to finish as many nodes as possible.
 *
 * Return values:
 *  0: success
 * -1: cannot rehash in phase 1, no marked nodes remaining
 * -2: cannot rehash in phase 1, and marked nodes remaining
 * -3: cannot rehash in phase 2, no marked nodes remaining
 * -4: cannot create node in phase 2 (ergo marked nodes remaining)
 * -5: cannot rehash and cannot create node in phase 2
 *
 * See the implementation of sylvan_simple_varswap for notes on recovery/rollback.
 *
 * Due to the nature of bounded probe sequences, it is possible that rehashing
 * fails, even when nothing is changed. Worst case: recovery impossible.
 */

TASK_DECL_2(int, sylvan_varswap, uint32_t, int);
#define sylvan_varswap(var, recovery) CALL(sylvan_varswap, var, recovery)

/**
 * Very simply varswap, no iterative recovery, no nodes table resizing.
 * returns 0 if it worked.
 * returns 1 if we had to rollback.
 * aborts with exit(1) if rehashing didn't work during recovery
 */
TASK_DECL_1(int, sylvan_simple_varswap, uint32_t);
#define sylvan_simple_varswap(var) CALL(sylvan_simple_varswap, var)

TASK_DECL_2(int, sylvan_sifting, uint32_t, uint32_t);
#define sylvan_sifting(low, high) CALL(sylvan_sifting, low, high)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
