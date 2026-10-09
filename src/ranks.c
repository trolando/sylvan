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
 * Variable rank metadata. See <sylvan/internal/ranks_internal.h> for the
 * model; this file only holds the storage and its growth policy.
 */

#include <sylvan/internal.h>

#include <string.h>

_Atomic(uint32_t) sylvan_ranks_count;
_Atomic(uint32_t*) sylvan_ranks_rank_of;
_Atomic(uint32_t*) sylvan_ranks_identity_at;

/**
 * One allocation holds both arrays: rank_of in data[0 .. capacity) and
 * identity_at in data[capacity .. 2*capacity). A superseded block is moved to
 * <ranks_retired> instead of being freed, because a concurrent reader may
 * still hold a pointer into it. The list is emptied by sylvan_ranks_free().
 */
struct ranks_block {
    struct ranks_block *retired_next;
    uint32_t capacity;
    uint32_t data[];
};

static struct ranks_block *ranks_current;
static struct ranks_block *ranks_retired;

/**
 * Whether anything has ever rewritten the order. Set by the two functions
 * that can, and cleared again by sylvan_ranks_is_identity() when a scan finds
 * the identity order after all. See the header for why it may not simply stay
 * set once it has been raised.
 */
static _Atomic(int) ranks_permuted;

/** Moved by everything that can rewrite the order; see the header. */
static _Atomic(uint64_t) ranks_generation;

/**
 * Growth and permutation run under this spinlock. The critical sections only
 * allocate and copy; in particular they never construct a node and never
 * reach sylvan_gc(), so a worker spinning here cannot deadlock a
 * stop-the-world collection started by another worker.
 */
static _Atomic(int) ranks_lock;

static void
ranks_lock_acquire(void)
{
    for (;;) {
        int expected = 0;
        if (atomic_compare_exchange_weak_explicit(
                &ranks_lock, &expected, 1,
                memory_order_acquire, memory_order_relaxed)) {
            return;
        }
    }
}

static void
ranks_lock_release(void)
{
    atomic_store_explicit(&ranks_lock, 0, memory_order_release);
}

static struct ranks_block *
ranks_block_alloc(uint32_t capacity)
{
    if (capacity > SYLVAN_RANKS_LIMIT) return NULL;
    struct ranks_block *block = malloc(
        sizeof(struct ranks_block) + (size_t)capacity * 2 * sizeof(uint32_t));
    if (block == NULL) return NULL;
    block->retired_next = NULL;
    block->capacity = capacity;
    return block;
}

/** Publish <block> as the current arrays and make identities < count visible. */
static void
ranks_publish(struct ranks_block *block, uint32_t count)
{
    atomic_store_explicit(
        &sylvan_ranks_rank_of, block->data, memory_order_release);
    atomic_store_explicit(
        &sylvan_ranks_identity_at, block->data + block->capacity,
        memory_order_release);
    atomic_store_explicit(&sylvan_ranks_count, count, memory_order_release);
}

int
sylvan_ranks_reserve(uint32_t count)
{
    if (count > SYLVAN_RANKS_LIMIT) return SYLVAN_ERR_INVALID;
    if (count <= atomic_load_explicit(&sylvan_ranks_count, memory_order_acquire)) {
        return SYLVAN_OK;
    }

    ranks_lock_acquire();

    const uint32_t registered =
        atomic_load_explicit(&sylvan_ranks_count, memory_order_relaxed);
    if (count <= registered) {
        ranks_lock_release();
        return SYLVAN_OK;
    }

    struct ranks_block *block = ranks_current;
    if (block == NULL || block->capacity < count) {
        uint32_t capacity = block == NULL ? 64 : block->capacity;
        while (capacity < count) {
            capacity = capacity > SYLVAN_RANKS_LIMIT / 2
                ? SYLVAN_RANKS_LIMIT : capacity * 2;
        }
        struct ranks_block *grown = ranks_block_alloc(capacity);
        if (grown == NULL) {
            ranks_lock_release();
            return SYLVAN_ERR_OOM;
        }
        if (block != NULL) {
            memcpy(grown->data, block->data,
                   (size_t)registered * sizeof(uint32_t));
            memcpy(grown->data + capacity, block->data + block->capacity,
                   (size_t)registered * sizeof(uint32_t));
            block->retired_next = ranks_retired;
            ranks_retired = block;
        }
        block = grown;
        ranks_current = block;
    }

    /* Newly registered identities take the identity order. */
    for (uint32_t v = registered; v < count; v++) {
        block->data[v] = v;
        block->data[block->capacity + v] = v;
    }

    ranks_publish(block, count);
    ranks_lock_release();
    return SYLVAN_OK;
}

uint32_t
sylvan_ranks_registered(void)
{
    return atomic_load_explicit(&sylvan_ranks_count, memory_order_acquire);
}

int
sylvan_ranks_swap_adjacent(uint32_t rank)
{
    ranks_lock_acquire();

    struct ranks_block *block = ranks_current;
    const uint32_t count =
        atomic_load_explicit(&sylvan_ranks_count, memory_order_relaxed);
    if (block == NULL || count < 2 || rank >= count - 1) {
        ranks_lock_release();
        return SYLVAN_ERR_INVALID;
    }

    uint32_t *rank_of = block->data;
    uint32_t *identity_at = block->data + block->capacity;
    const uint32_t upper = identity_at[rank];
    const uint32_t lower = identity_at[rank + 1];
    identity_at[rank] = lower;
    identity_at[rank + 1] = upper;
    rank_of[lower] = rank;
    rank_of[upper] = rank + 1;

    atomic_store_explicit(&ranks_permuted, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&ranks_generation, 1, memory_order_relaxed);
    ranks_publish(block, count);
    ranks_lock_release();
    return SYLVAN_OK;
}

int
sylvan_ranks_permute_unsafe(const uint32_t *identity_at, uint32_t count)
{
    if (identity_at == NULL) return SYLVAN_ERR_INVALID;

    ranks_lock_acquire();

    struct ranks_block *block = ranks_current;
    if (block == NULL ||
        count != atomic_load_explicit(&sylvan_ranks_count, memory_order_relaxed)) {
        ranks_lock_release();
        return SYLVAN_ERR_INVALID;
    }

    uint8_t *seen = calloc(count == 0 ? 1 : count, sizeof(uint8_t));
    if (seen == NULL) {
        ranks_lock_release();
        return SYLVAN_ERR_OOM;
    }
    for (uint32_t rank = 0; rank < count; rank++) {
        const uint32_t identity = identity_at[rank];
        if (identity >= count || seen[identity]) {
            free(seen);
            ranks_lock_release();
            return SYLVAN_ERR_INVALID;
        }
        seen[identity] = 1;
    }
    free(seen);

    for (uint32_t rank = 0; rank < count; rank++) {
        const uint32_t identity = identity_at[rank];
        block->data[identity] = rank;
        block->data[block->capacity + rank] = identity;
    }

    atomic_store_explicit(&ranks_permuted, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&ranks_generation, 1, memory_order_relaxed);
    ranks_publish(block, count);
    ranks_lock_release();
    return SYLVAN_OK;
}

int
sylvan_ranks_is_identity(void)
{
    if (!atomic_load_explicit(&ranks_permuted, memory_order_acquire)) return 1;

    ranks_lock_acquire();

    const struct ranks_block *block = ranks_current;
    const uint32_t count =
        atomic_load_explicit(&sylvan_ranks_count, memory_order_relaxed);

    /* Only a successful permutation raises the mark, and one cannot succeed
       without a block, so <block> is non-NULL here whenever <count> is not 0. */
    int identity = 1;
    for (uint32_t rank = 0; rank < count && identity; rank++) {
        if (block->data[block->capacity + rank] != rank) identity = 0;
    }

    /* A permutation that put everything back is indistinguishable from never
       having happened, so let the next call take the cheap path again. */
    if (identity) {
        atomic_store_explicit(&ranks_permuted, 0, memory_order_release);
    }

    ranks_lock_release();
    return identity;
}

uint64_t
sylvan_ranks_generation(void)
{
    return atomic_load_explicit(&ranks_generation, memory_order_relaxed);
}

void
sylvan_ranks_free(void)
{
    ranks_lock_acquire();

    atomic_store_explicit(&ranks_permuted, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&ranks_generation, 1, memory_order_relaxed);
    atomic_store_explicit(&sylvan_ranks_count, 0, memory_order_release);
    atomic_store_explicit(&sylvan_ranks_rank_of, NULL, memory_order_release);
    atomic_store_explicit(&sylvan_ranks_identity_at, NULL, memory_order_release);

    while (ranks_retired != NULL) {
        struct ranks_block *block = ranks_retired;
        ranks_retired = block->retired_next;
        free(block);
    }
    free(ranks_current);
    ranks_current = NULL;

    ranks_lock_release();
}
