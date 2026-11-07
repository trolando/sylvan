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

#include <sylvan_int.h>
#include <sylvan_platform.h>

/* One bitmap per active traversal. Idle bitmaps are pooled until quit.
 * Only the pool is locked; the walk and allocation never hold the lock.
 * Release/acquire on the pool lock publishes cleared bits to their next user.
 * Each walk is synchronous on one thread, including across Lace safepoints;
 * a nested walk saves and restores that thread's previous scratch context.
 */
struct scratch {
    struct scratch *next;
    llmsset_t owner;
    uint64_t *bits;
    size_t size, bytes;
#ifndef NDEBUG
    size_t marks;
#endif
};

static struct scratch *scratch_pool;
static SYLVAN_TLS struct scratch *scratch_current;
static atomic_flag scratch_lock = ATOMIC_FLAG_INIT;
#ifndef NDEBUG
static size_t scratch_active;
#endif

static void lock_pool(void)
{
    while (atomic_flag_test_and_set_explicit(&scratch_lock, memory_order_acquire)) {}
}

static void unlock_pool(void)
{
    atomic_flag_clear_explicit(&scratch_lock, memory_order_release);
}

/* The hash table can shrink below live indices; scratch covers the data range.
 */
static size_t scratch_data_size(const llmsset_t dbs)
{
    return llmsset_get_data_size(dbs);
}

void nodes_scratch_acquire(const llmsset_t dbs)
{
    lock_pool();
    struct scratch *s = scratch_pool;
    if (s != NULL) scratch_pool = s->next;
#ifndef NDEBUG
    scratch_active++;
#endif
    unlock_pool();

    if (s == NULL) s = calloc(1, sizeof(*s));
    if (s == NULL) goto fail;
    const size_t size = scratch_data_size(dbs);
    if (s->size < size) {
        sylvan_free_aligned(s->bits, s->bytes);
        s->bytes = ((size + 63) / 64) * sizeof(uint64_t);
        /* The platform allocator is zero-filled and uses sparse virtual
         * memory when enabled; it does not request huge pages in 1.x. */
        s->bits = sylvan_alloc_aligned(s->bytes);
        if (s->bits == NULL) goto fail;
        s->size = size;
    }
    s->owner = dbs;
    s->next = scratch_current;
    scratch_current = s;
    return;
fail:
    fprintf(stderr, "nodes_scratch_acquire: Unable to allocate traversal bitmap!\n");
    exit(1);
}

void nodes_scratch_release(const llmsset_t dbs)
{
    struct scratch *s = scratch_current;
    assert(s != NULL && s->owner == dbs);
    (void)dbs;
    assert(s->marks == 0 && "traversal left scratch marks behind");
    scratch_current = s->next;
    lock_pool();
    s->next = scratch_pool;
    scratch_pool = s;
#ifndef NDEBUG
    scratch_active--;
#endif
    unlock_pool();
}

int nodes_scratch_ismarked(const llmsset_t dbs, uint64_t index)
{
    struct scratch *s = scratch_current;
    assert(s != NULL && s->owner == dbs && index < s->size);
    (void)dbs;
    return (s->bits[index / 64] & (UINT64_C(1) << (index & 63))) != 0;
}

void nodes_scratch_mark(const llmsset_t dbs, uint64_t index)
{
    struct scratch *s = scratch_current;
    assert(s != NULL && s->owner == dbs && index < s->size);
    (void)dbs;
    uint64_t *word = &s->bits[index / 64];
    const uint64_t bit = UINT64_C(1) << (index & 63);
#ifndef NDEBUG
    if ((*word & bit) == 0) s->marks++;
#endif
    *word |= bit;
}

void nodes_scratch_unmark(const llmsset_t dbs, uint64_t index)
{
    struct scratch *s = scratch_current;
    assert(s != NULL && s->owner == dbs && index < s->size);
    (void)dbs;
    uint64_t *word = &s->bits[index / 64];
    const uint64_t bit = UINT64_C(1) << (index & 63);
#ifndef NDEBUG
    if ((*word & bit) != 0) s->marks--;
#endif
    *word &= ~bit;
}

/* Like destroying the node table itself, this requires quiescent workers. */
void nodes_scratch_forget(void)
{
    assert(scratch_active == 0);
    while (scratch_pool != NULL) {
        struct scratch *s = scratch_pool;
        scratch_pool = s->next;
        sylvan_free_aligned(s->bits, s->bytes);
        free(s);
    }
}
