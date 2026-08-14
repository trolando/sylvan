/*
 * Copyright 2019-2025 Tom van Dijk, Johannes Kepler University Linz
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
#include <sylvan/platform.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * The traversal scratch bitmap. See <sylvan/nodes.h> for the contract.
 *
 * This lives outside the nodes backends on purpose. The bitmap needs nothing
 * from a backend beyond the index space it hands out and its maximum size,
 * both of which are already part of the nodes interface, so keeping it here
 * gives every backend the same implementation instead of five copies.
 *
 * Sylvan uses a single nodes table, so a single association is enough. A
 * traversal on a table that is not the current owner reallocates, which also
 * covers the case of a second table outliving the first.
 */

static const nodes_table* scratch_owner = NULL;
static uint64_t*          scratch_bits  = NULL;
static size_t             scratch_bytes = 0;

#ifndef NDEBUG
/* Held between acquire and release; catches overlapping marked traversals. */
static _Atomic(int) scratch_held = 0;
/* Number of bits currently set; catches a traversal that fails to unmark. */
static size_t scratch_marks = 0;
#endif

static size_t
scratch_size_bytes(size_t max_size)
{
    return ((max_size + 63u) / 64u) * sizeof(uint64_t);
}

void
nodes_scratch_acquire(const nodes_table* dbs)
{
    assert(dbs != NULL);

#ifndef NDEBUG
    const int was_held = atomic_exchange(&scratch_held, 1);
    assert(was_held == 0 &&
           "overlapping marked traversals share one scratch bitmap");
    (void)was_held;
#endif

    const size_t bytes = scratch_size_bytes(nodes_get_max_size(dbs));
    if (scratch_bits != NULL && (scratch_owner != dbs || scratch_bytes != bytes)) {
        sylvan_free_aligned(scratch_bits, scratch_bytes);
        scratch_bits = NULL;
        scratch_bytes = 0;
    }
    if (scratch_bits == NULL) {
        scratch_bits = (uint64_t*)sylvan_alloc_aligned(bytes);
        if (scratch_bits == NULL) {
            fprintf(stderr,
                    "nodes_scratch_acquire: Unable to allocate memory: %s!\n",
                    strerror(errno));
            exit(1);
        }
        scratch_bytes = bytes;
    }
    scratch_owner = dbs;
}

void
nodes_scratch_release(const nodes_table* dbs)
{
    assert(dbs == scratch_owner);
    (void)dbs;

#ifndef NDEBUG
    assert(scratch_marks == 0 &&
           "a marked traversal must unmark every node it marked");
    const int was_held = atomic_exchange(&scratch_held, 0);
    assert(was_held == 1);
    (void)was_held;
#endif
}

int
nodes_scratch_ismarked(const nodes_table* dbs, uint64_t index)
{
    assert(dbs == scratch_owner && index < nodes_get_max_size(dbs));
    (void)dbs;
    return (scratch_bits[index / 64] & (UINT64_C(1) << (index & 63))) ? 1 : 0;
}

void
nodes_scratch_mark(const nodes_table* dbs, uint64_t index)
{
    assert(dbs == scratch_owner && index < nodes_get_max_size(dbs));
    (void)dbs;
    uint64_t* word = &scratch_bits[index / 64];
    const uint64_t bit = UINT64_C(1) << (index & 63);
#ifndef NDEBUG
    if ((*word & bit) == 0) scratch_marks++;
#endif
    *word |= bit;
}

void
nodes_scratch_unmark(const nodes_table* dbs, uint64_t index)
{
    assert(dbs == scratch_owner && index < nodes_get_max_size(dbs));
    (void)dbs;
    uint64_t* word = &scratch_bits[index / 64];
    const uint64_t bit = UINT64_C(1) << (index & 63);
#ifndef NDEBUG
    if ((*word & bit) != 0) scratch_marks--;
#endif
    *word &= ~bit;
}

void
nodes_scratch_forget(const nodes_table* dbs)
{
    if (dbs != scratch_owner) return;
    if (scratch_bits != NULL) sylvan_free_aligned(scratch_bits, scratch_bytes);
    scratch_bits = NULL;
    scratch_bytes = 0;
    scratch_owner = NULL;
#ifndef NDEBUG
    scratch_marks = 0;
#endif
}
