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

/**
 * The bitmap itself is only ever touched by the one thread that holds the
 * scratch, so the bits are plain memory. The pointer to it is not: a marked
 * traversal may run inside a Lace task (bdd_simplify counts nodes), so the
 * thread that allocates the bitmap and the thread that uses it next need not
 * be the same. The pointer is therefore published with release ordering and
 * picked up with acquire ordering in nodes_scratch_acquire, which every
 * traversal calls before touching a bit; the per-bit accesses then reload it
 * relaxed, which is sound because the acquire already ordered them.
 */
static const nodes_table* scratch_owner = NULL;
static _Atomic(uint64_t*) scratch_bits  = NULL;
static size_t             scratch_bytes = 0;

static inline uint64_t*
scratch_here(void)
{
    return atomic_load_explicit(&scratch_bits, memory_order_relaxed);
}

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

    /* Acquire: pairs with the release store below, so that a bitmap another
       worker allocated is visible here in full. */
    uint64_t* bits = atomic_load_explicit(&scratch_bits, memory_order_acquire);

    const size_t bytes = scratch_size_bytes(nodes_get_max_size(dbs));
    if (bits != NULL && (scratch_owner != dbs || scratch_bytes != bytes)) {
        sylvan_free_aligned(bits, scratch_bytes);
        bits = NULL;
        scratch_bytes = 0;
    }
    if (bits == NULL) {
        /* Sparse, not aligned: a huge page would commit 2 MiB for any bit
           touched in its 16M-index window, not 4 KiB per 32K indices. */
        bits = (uint64_t*)sylvan_alloc_sparse(bytes);
        if (bits == NULL) {
            fprintf(stderr,
                    "nodes_scratch_acquire: Unable to allocate memory: %s!\n",
                    strerror(errno));
            exit(1);
        }
        scratch_bytes = bytes;
    }
    scratch_owner = dbs;
    atomic_store_explicit(&scratch_bits, bits, memory_order_release);
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
    return (scratch_here()[index / 64] & (UINT64_C(1) << (index & 63))) ? 1 : 0;
}

void
nodes_scratch_mark(const nodes_table* dbs, uint64_t index)
{
    assert(dbs == scratch_owner && index < nodes_get_max_size(dbs));
    (void)dbs;
    uint64_t* word = &scratch_here()[index / 64];
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
    uint64_t* word = &scratch_here()[index / 64];
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
    uint64_t* bits = atomic_load_explicit(&scratch_bits, memory_order_acquire);
    if (bits != NULL) sylvan_free_aligned(bits, scratch_bytes);
    atomic_store_explicit(&scratch_bits, NULL, memory_order_release);
    scratch_bytes = 0;
    scratch_owner = NULL;
#ifndef NDEBUG
    scratch_marks = 0;
#endif
}
