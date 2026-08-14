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
 * Dynamic variable reordering: the session and the adjacent variable swap.
 *
 * See <sylvan/internal/reorder_internal.h> for the model. Nothing in this
 * file outlives a session, and a session runs with the world stopped.
 *
 * WHAT THE SESSION OWNS
 *
 * 1. Exact reference counts, one per node slot. A swap has to know which
 *    nodes it killed, both to unhash them immediately (so no node is ever
 *    resurrected) and to score the swap without a marking pass. The counts
 *    are 16 bits per slot plus a sparse overflow map for the rare node with
 *    more than 65534 parents: saturating counts are not acceptable, because a
 *    saturated node could die undetected and bias every later score.
 *
 * 2. The external roots, as a pin bit per node. Everything else is counted
 *    from the arena, so a node that no other node points to is dead unless it
 *    is pinned.
 *
 * 3. Per-identity node lists, so a swap can find "the nodes of variable x"
 *    without scanning the table. Backends that enumerate a variable natively
 *    (llchst) do not need the decision lists, but every backend needs the map
 *    lists: an MTBDDMAP node carries its key in the variable field yet lives
 *    in the shared MAP subtable, where no native enumeration can select one
 *    key.
 *
 * WHAT COUNTS AS AN EDGE. Every live non-leaf node contributes one edge to
 * each of its two children. Leaves are excluded entirely and are never
 * retired, which also means a leaf never appears in the counts.
 *
 * The counts are counts over the *table*, and the session makes the table and
 * the reachable forest the same thing by collecting before it seeds them: see
 * sylvan_reorder_begin. Every count therefore starts at the true number of
 * parents of a reachable node, no slot holds garbage, and a swap's
 * created-minus-died is the exact size change of the forest rather than of the
 * table. A swap still only ever reacts to a count *transition* to zero, so it
 * retires exactly what it killed; after the opening collection there is
 * nothing else left for it to confuse that with.
 */

#include <sylvan/internal.h>
#include <refs.h>

#include <assert.h>
#include <string.h>

/** External root table, defined in mtbdd.c. */
extern refs_table_t mtbdd_protected;

/**
 * Marking callbacks Sylvan registers for itself once mtbdd_init() has run:
 * the per-worker temporary references and the protected pointer table.
 * Anything beyond these belongs to a consumer whose roots a session cannot
 * enumerate; see sylvan_reorder_begin().
 */
#define REORDER_OWN_MARK_HOOKS 2

/** Granularity of the parallel table scans. */
#define REORDER_SCAN_BLOCK 4096

/** Granularity of the parallel phases of a swap. */
#define REORDER_SWAP_BLOCK 64

/** The 40-bit node index inside a handle. */
#define REORDER_INDEX_MASK UINT64_C(0x000000ffffffffff)

/** A reference count of this value means "look in the overflow map". */
#define REORDER_COUNT_SENTINEL UINT16_MAX

static void
reorder_fatal(const char *what)
{
    fprintf(stderr, "sylvan: reorder session: %s. Aborting.\n", what);
    abort();
}

/**
 * A per-identity node list.
 *
 * The list is a SUPERSET of the live nodes of its identity: every live node
 * of that identity is in it, but it may also hold entries that have since
 * been rewritten to another identity, retired, or had their slot reused. That
 * is what makes maintenance trivial -- a swap only ever appends -- and the
 * cost is that a reader filters the list before using it (reorder_collect),
 * which also compacts it back down to the true population.
 */
struct reorder_list {
    uint64_t *items;
    _Atomic(size_t) count;
    size_t capacity;
};

struct reorder_session {
    int active;
    size_t size;                    /* node table size, fixed for a session */
    uint32_t identities;            /* registered identities, ditto        */
    int native_enumerate;           /* backend enumerates a variable itself */

    _Atomic(uint16_t) *counts;      /* per-slot reference count            */
    uint64_t *pinned;               /* bitmap: an external root            */
    uint64_t *retired;              /* bitmap: unhashed, awaiting release  */
    uint64_t *seen;                 /* bitmap: scratch for reorder_collect */

    struct reorder_list *decision;  /* per identity, NULL if native        */
    struct reorder_list *maps;      /* per identity                        */

    /* Sparse overflow map for counts that no longer fit in 16 bits. */
    _Atomic(int) overflow_lock;
    uint64_t *overflow_index;
    uint64_t *overflow_value;
    size_t overflow_count;
    size_t overflow_capacity;
    uint64_t overflow_promotions;

    /* Retirement work queue, filled by the phase that kills a node. */
    uint64_t *queue;
    _Atomic(size_t) queue_count;
    size_t queue_capacity;

    /* Retired slots awaiting physical release. */
    uint64_t *dead;
    size_t dead_count;
    size_t dead_capacity;

    /* Per-swap counter of freshly allocated slots. */
    _Atomic(uint64_t) created;
};

static struct reorder_session session;

/** Configuration; see the header. */
static int reorder_release_at_swap_end = 1;
static int reorder_validate = 0;
static uint32_t reorder_overflow_at = REORDER_COUNT_SENTINEL;

void
sylvan_reorder_set_release_at_swap_end(int enable)
{
    reorder_release_at_swap_end = enable ? 1 : 0;
}

void
sylvan_reorder_set_validate(int enable)
{
    reorder_validate = enable ? 1 : 0;
}

void
sylvan_reorder_set_overflow_at(uint32_t count)
{
    if (session.active) return;
    if (count < 1) count = 1;
    if (count > REORDER_COUNT_SENTINEL) count = REORDER_COUNT_SENTINEL;
    reorder_overflow_at = count;
}

uint64_t
sylvan_reorder_overflow_promotions(void)
{
    return session.overflow_promotions;
}

/**
 * Side bitmaps. All of them are written from one thread at a time -- the pins
 * during session setup, the retired and seen bits from the serial phases --
 * so they need no atomics. No phase writes a mark or flag bit into a node
 * record: those 16 bytes are part of the hashed key, and the only writes to
 * them are the swap's own rewrites.
 */

static inline size_t
bitmap_words(size_t bits)
{
    return (bits + 63u) / 64u;
}

static inline void
bitmap_set(uint64_t *bitmap, uint64_t bit)
{
    bitmap[bit >> 6] |= UINT64_C(1) << (bit & 63u);
}

static inline void
bitmap_clear(uint64_t *bitmap, uint64_t bit)
{
    bitmap[bit >> 6] &= ~(UINT64_C(1) << (bit & 63u));
}

static inline int
bitmap_get(const uint64_t *bitmap, uint64_t bit)
{
    return (bitmap[bit >> 6] >> (bit & 63u)) & 1u;
}

/** The 16-byte record of a node index. */
static inline mtbddnode*
reorder_record(uint64_t index)
{
    return (mtbddnode*)nodes_get_pointer(nodes, index);
}

/**
 * The node a handle refers to, if that node participates in the reference
 * counts, and 0 otherwise. Terminals and leaves never do.
 */
static inline uint64_t
reorder_target(uint64_t handle)
{
    const uint64_t index = handle & REORDER_INDEX_MASK;
    if (index < 2) return 0;
    if (mtbddnode_isleaf(reorder_record(index))) return 0;
    return index;
}

/** Whether a handle points at a node of variable identity <variable>. */
static inline int
reorder_has_variable(uint64_t handle, uint32_t variable)
{
    const uint64_t index = handle & REORDER_INDEX_MASK;
    if (index < 2) return 0;
    mtbddnode *record = reorder_record(index);
    if (mtbddnode_isleaf(record)) return 0;
    return mtbddnode_getvariable(record) == variable;
}

/**
 * The overflow map.
 *
 * Linear search behind a spinlock. Both are fine because a node only ever
 * gets in here by having tens of thousands of parents, which makes the map
 * empty in every realistic forest; the lock is contended only while such a
 * node is being counted, and then only by the workers touching that one node.
 *
 * A promoted node is never demoted. The 16-bit slot keeps the sentinel for
 * the rest of the session and every later operation on that node goes through
 * the map, which keeps the count exact at the price of one comparison on the
 * hot path. It also means the map only ever grows on the *transition*: once a
 * node has an entry, every further count on it reuses that entry.
 *
 * The map therefore only grows where the classification phase said it could,
 * which is what keeps promotion out of the swap's list of ways to fail. See
 * reorder_overflow_reserve for the bound and the test that decides whether it
 * is needed at all.
 */

static void
overflow_lock(void)
{
    for (;;) {
        int expected = 0;
        if (atomic_compare_exchange_weak_explicit(
                &session.overflow_lock, &expected, 1,
                memory_order_acquire, memory_order_relaxed)) {
            return;
        }
    }
}

static void
overflow_unlock(void)
{
    atomic_store_explicit(&session.overflow_lock, 0, memory_order_release);
}

/**
 * Room for <extra> further promotions. The only place the map ever grows, and
 * both callers are passes that have not mutated anything yet, so failing here
 * costs nothing.
 */
static int
reorder_overflow_reserve(size_t extra)
{
    const size_t needed = session.overflow_count + extra;
    if (needed <= session.overflow_capacity) return SYLVAN_OK;

    size_t capacity = session.overflow_capacity == 0
        ? 16u : session.overflow_capacity;
    while (capacity < needed) capacity *= 2u;

    uint64_t *indices = (uint64_t*)realloc(
        session.overflow_index, capacity * sizeof(uint64_t));
    if (indices == NULL) return SYLVAN_ERR_OOM;
    session.overflow_index = indices;
    uint64_t *values = (uint64_t*)realloc(
        session.overflow_value, capacity * sizeof(uint64_t));
    if (values == NULL) return SYLVAN_ERR_OOM;
    session.overflow_value = values;
    session.overflow_capacity = capacity;
    return SYLVAN_OK;
}

/** Find, or with <create> add, the overflow slot of <index>. Locked. */
static uint64_t*
overflow_slot(uint64_t index, int create)
{
    for (size_t i = 0; i < session.overflow_count; i++) {
        if (session.overflow_index[i] == index) return session.overflow_value + i;
    }
    if (!create) return NULL;

    if (session.overflow_count == session.overflow_capacity) {
        /* Room was reserved for every promotion this pass can make before it
           was allowed to start, so this cannot happen. */
        assert(!"the overflow map outgrew its reservation");
        reorder_fatal("the overflow map outgrew its reservation");
        return NULL;
    }

    const size_t at = session.overflow_count++;
    session.overflow_index[at] = index;
    session.overflow_value[at] = 0;
    return session.overflow_value + at;
}

/** Add one reference to a counted node. */
static void
reorder_count_up(uint64_t index)
{
    _Atomic(uint16_t) *slot = session.counts + index;
    for (;;) {
        uint16_t count = atomic_load_explicit(slot, memory_order_relaxed);

        if (count == REORDER_COUNT_SENTINEL) {
            /* A node only carries the sentinel because the branch below put
               it there, under this same lock and after creating its entry, so
               the entry is there and no slot is claimed here. */
            overflow_lock();
            uint64_t *value = overflow_slot(index, 0);
            if (value == NULL) reorder_fatal("a promoted node has no overflow entry");
            else (*value)++;
            overflow_unlock();
            return;
        }

        if ((uint32_t)count + 1u >= reorder_overflow_at) {
            overflow_lock();
            uint16_t expected = count;
            if (atomic_compare_exchange_strong_explicit(
                    slot, &expected, REORDER_COUNT_SENTINEL,
                    memory_order_relaxed, memory_order_relaxed)) {
                uint64_t *value = overflow_slot(index, 1);
                *value = (uint64_t)count + 1u;
                session.overflow_promotions++;
                overflow_unlock();
                return;
            }
            overflow_unlock();
            continue;
        }

        if (atomic_compare_exchange_weak_explicit(
                slot, &count, (uint16_t)(count + 1u),
                memory_order_relaxed, memory_order_relaxed)) {
            return;
        }
    }
}

/** Remove one reference. Returns 1 if the node just lost its last one. */
static int
reorder_count_down(uint64_t index)
{
    _Atomic(uint16_t) *slot = session.counts + index;
    for (;;) {
        uint16_t count = atomic_load_explicit(slot, memory_order_relaxed);

        if (count == REORDER_COUNT_SENTINEL) {
            overflow_lock();
            uint64_t *value = overflow_slot(index, 0);
            if (value == NULL || *value == 0) {
                overflow_unlock();
                reorder_fatal("reference count underflow in the overflow map");
                return 0;
            }
            const uint64_t left = --(*value);
            overflow_unlock();
            return left == 0;
        }

        if (count == 0) {
            reorder_fatal("reference count underflow");
            return 0;
        }

        if (atomic_compare_exchange_weak_explicit(
                slot, &count, (uint16_t)(count - 1u),
                memory_order_relaxed, memory_order_relaxed)) {
            return count == 1;
        }
    }
}

/** Read a count back, for validation. */
static uint64_t
reorder_count_get(uint64_t index)
{
    const uint16_t count = atomic_load_explicit(
        session.counts + index, memory_order_relaxed);
    if (count != REORDER_COUNT_SENTINEL) return count;
    overflow_lock();
    const uint64_t *value = overflow_slot(index, 0);
    const uint64_t result = value == NULL ? 0 : *value;
    overflow_unlock();
    return result;
}

/**
 * Queue a node whose last reference just disappeared.
 *
 * The queue is sized in the classification phase to the number of edges the
 * rewrite drops, which under the depth-1 death theorem is every death there
 * can be. Overflowing it therefore takes two things at once: a violation of
 * the theorem, and enough direct deaths that the deaths the theorem does not
 * allow do not fit in what the direct deaths left over. Deaths past the
 * theorem are counted and reported as <deep_deaths> as long as they fit; past
 * that this aborts, because the alternative is either to allocate at a point
 * where the swap has already rewritten records -- which is the failure mode
 * this reservation exists to remove -- or to drop a death and leave a node
 * that nothing points at hashed in the index forever.
 */
static void
reorder_retire(uint64_t index)
{
    const size_t at = atomic_fetch_add_explicit(
        &session.queue_count, 1, memory_order_relaxed);
    if (at >= session.queue_capacity) {
        assert(!"the depth-1 death theorem does not hold and the queue is full");
        reorder_fatal("more deaths than the retirement queue was sized for");
        return;
    }
    session.queue[at] = index;
}

/** Record the edge <handle>, which has just been created. */
static void
reorder_edge_up(uint64_t handle)
{
    const uint64_t index = reorder_target(handle);
    if (index != 0) reorder_count_up(index);
}

/** Drop the edge to the counted node <index>, retiring it if it was the last. */
static void
reorder_edge_down(uint64_t index)
{
    if (index == 0) return;
    if (!reorder_count_down(index)) return;
    if (bitmap_get(session.pinned, index)) return;
    reorder_retire(index);
}

/**
 * Per-identity lists.
 */

static int
reorder_list_reserve(struct reorder_list *list, size_t extra)
{
    if (list == NULL) return SYLVAN_OK;
    const size_t needed =
        atomic_load_explicit(&list->count, memory_order_relaxed) + extra;
    if (needed <= list->capacity) return SYLVAN_OK;

    size_t capacity = list->capacity == 0 ? 16u : list->capacity;
    while (capacity < needed) capacity *= 2u;
    uint64_t *grown = (uint64_t*)realloc(
        list->items, capacity * sizeof(uint64_t));
    if (grown == NULL) return SYLVAN_ERR_OOM;
    list->items = grown;
    list->capacity = capacity;
    return SYLVAN_OK;
}

/**
 * Append a node. Capacity must have been reserved, because this runs from the
 * parallel phases; losing an entry would silently break the invariant that
 * every live node of an identity is in that identity's list.
 */
static void
reorder_list_append(struct reorder_list *list, uint64_t index)
{
    if (list == NULL) return;
    const size_t at = atomic_fetch_add_explicit(
        &list->count, 1, memory_order_relaxed);
    if (at >= list->capacity) reorder_fatal("node list overflow");
    else list->items[at] = index;
}

/** The list an identity's decision nodes live in, or NULL for native. */
static inline struct reorder_list*
reorder_decision_list(uint32_t identity)
{
    if (session.decision == NULL || identity >= session.identities) return NULL;
    return session.decision + identity;
}

static inline struct reorder_list*
reorder_map_list(uint32_t identity)
{
    if (session.maps == NULL || identity >= session.identities) return NULL;
    return session.maps + identity;
}

/**
 * Reduce a list to exactly the live nodes of <identity> of the requested kind,
 * in place, and return how many there are. Entries that were rewritten to
 * another identity, retired, or whose slot was reused for something else drop
 * out here, and so do duplicates -- a node can be appended twice over several
 * swaps if it moves back and forth between two identities.
 */
static size_t
reorder_collect(struct reorder_list *list, uint32_t identity, int want_map)
{
    const size_t count = atomic_load_explicit(&list->count, memory_order_relaxed);
    size_t out = 0;

    for (size_t i = 0; i < count; i++) {
        const uint64_t index = list->items[i];
        if (index < 2 || index >= session.size) continue;
        if (!nodes_is_marked(nodes, index)) continue;
        if (bitmap_get(session.retired, index)) continue;
        mtbddnode *record = reorder_record(index);
        if (mtbddnode_isleaf(record)) continue;
        if ((mtbddnode_ismapnode(record) != 0) != (want_map != 0)) continue;
        if (mtbddnode_getvariable(record) != identity) continue;
        if (bitmap_get(session.seen, index)) continue;
        bitmap_set(session.seen, index);
        list->items[out++] = index;
    }

    for (size_t i = 0; i < out; i++) bitmap_clear(session.seen, list->items[i]);
    atomic_store_explicit(&list->count, out, memory_order_relaxed);
    return out;
}

/**
 * Session setup.
 *
 * One parallel pass over the arena seeds every reference count and, at the
 * same time, counts how many nodes each identity has; a second parallel pass
 * scatters the identities' nodes into their lists. Both passes read the node
 * bitmap and the records and write nothing but the session's own arrays.
 *
 * Nodes whose identity was never registered are left out of the lists. They
 * cannot be either half of a swap -- a swap names its variables by rank, and
 * only registered identities have one -- and they take part in the reference
 * counts like every other node.
 */

TASK(void, reorder_seed, size_t, first, size_t, count)

void
reorder_seed_CALL(lace_worker *lace, size_t first, size_t count)
{
    if (count > REORDER_SCAN_BLOCK) {
        reorder_seed_SPAWN(lace, first, count / 2);
        reorder_seed_CALL(lace, first + count / 2, count - count / 2);
        reorder_seed_SYNC(lace);
        return;
    }

    const size_t end = first + count;
    if (first < 2) first = 2;

    for (; first < end; first++) {
        if (!nodes_is_marked(nodes, first)) continue;
        mtbddnode *record = reorder_record(first);
        if (mtbddnode_isleaf(record)) continue;

        reorder_edge_up(mtbddnode_getlow(record));
        reorder_edge_up(mtbddnode_gethigh(record));

        const uint32_t identity = mtbddnode_getvariable(record);
        struct reorder_list *list = mtbddnode_ismapnode(record)
            ? reorder_map_list(identity) : reorder_decision_list(identity);
        if (list != NULL) {
            atomic_fetch_add_explicit(&list->count, 1, memory_order_relaxed);
        }
    }
}

TASK(void, reorder_scatter, size_t, first, size_t, count)

void
reorder_scatter_CALL(lace_worker *lace, size_t first, size_t count)
{
    if (count > REORDER_SCAN_BLOCK) {
        reorder_scatter_SPAWN(lace, first, count / 2);
        reorder_scatter_CALL(lace, first + count / 2, count - count / 2);
        reorder_scatter_SYNC(lace);
        return;
    }

    const size_t end = first + count;
    if (first < 2) first = 2;

    for (; first < end; first++) {
        if (!nodes_is_marked(nodes, first)) continue;
        mtbddnode *record = reorder_record(first);
        if (mtbddnode_isleaf(record)) continue;

        const uint32_t identity = mtbddnode_getvariable(record);
        reorder_list_append(mtbddnode_ismapnode(record)
            ? reorder_map_list(identity) : reorder_decision_list(identity),
            first);
    }
}

/** Pin one external root so that the session never retires it. */
static void
reorder_pin(uint64_t handle)
{
    const uint64_t index = reorder_target(handle);
    if (index == 0 || index >= session.size) return;
    if (!nodes_is_marked(nodes, index)) return;
    bitmap_set(session.pinned, index);
}

/**
 * Cache the external roots.
 *
 * One root table is read here, and only this one: the dereferenced addresses
 * of mtbdd_protect(). A consumer that owns roots through its own marking hook
 * is refused in sylvan_reorder_begin(), because the hook interface can mark
 * those roots but not enumerate them.
 */
static void
reorder_pin_roots(void)
{
    if (mtbdd_protected.refs_table != NULL) {
        uint64_t *iterator = protect_iter(
            &mtbdd_protected, 0, mtbdd_protected.refs_size);
        while (iterator != NULL) {
            const MTBDD *root = (const MTBDD*)(uintptr_t)protect_next(
                &mtbdd_protected, &iterator, mtbdd_protected.refs_size);
            reorder_pin(*root);
        }
    }
}

static void
reorder_free_lists(struct reorder_list *lists)
{
    if (lists == NULL) return;
    for (uint32_t i = 0; i < session.identities; i++) free(lists[i].items);
    free(lists);
}

static void
reorder_free_state(void)
{
    free(session.counts);
    free(session.pinned);
    free(session.retired);
    free(session.seen);
    reorder_free_lists(session.decision);
    reorder_free_lists(session.maps);
    free(session.overflow_index);
    free(session.overflow_value);
    free(session.queue);
    free(session.dead);
    memset(&session, 0, sizeof(session));
}

int
sylvan_reorder_begin_CALL(lace_worker *lace)
{
    const uint32_t caps = nodes_reorder_caps(nodes);

    if (session.active) return SYLVAN_ERR_INVALID;
    if (!mtbdd_is_initialized()) return SYLVAN_ERR_INVALID;
    if ((caps & NODES_CAP_REMOVE) == 0) return SYLVAN_ERR_INVALID;
    if ((caps & NODES_CAP_RELEASE) == 0) return SYLVAN_ERR_INVALID;
    if (zdd_is_initialized() || listdd_is_initialized()) {
        return SYLVAN_ERR_INVALID;
    }
    if (sylvan_gc_mark_hook_count() != REORDER_OWN_MARK_HOOKS) {
        return SYLVAN_ERR_INVALID;
    }

    /* A full collection is the first act of every session, and the reason is
       the scoring. A sifting driver compares positions by the size of the
       forest, so a swap's created-minus-died has to be the size change of the
       *forest*; counted over a table that still holds pre-session garbage it
       would be the size change of the table, which is the same number only by
       accident. Collecting first makes the table content exactly the reachable
       set, and a session never collects again, so it stays that way.
       This is the collection body, not sylvan_gc(): the world is already
       stopped and this is the frame the collection has to run in.

       It also subsumes the separate clear of the operation cache the session
       used to do. A collection discards the cache unconditionally (see
       sylvan_gc_go), which is what makes reusing a retired slot sound -- a
       surviving entry would answer a query about the new node with the old
       node's result -- and doing it here means the cache is already gone
       before the session allocates anything, so there is no window in which a
       half-built session that then fails has invalidated it for nothing. */
    sylvan_gc_go_CALL(lace);

    memset(&session, 0, sizeof(session));
    /* After the collection, and not before: the resize hook may have grown
       the table. */
    session.size = nodes_get_size(nodes);
    session.identities = sylvan_ranks_registered();
    session.native_enumerate = (caps & NODES_CAP_ENUMERATE) != 0;

    const size_t words = bitmap_words(session.size);
    const size_t lists = session.identities == 0 ? 1u : session.identities;
    session.counts = (_Atomic(uint16_t)*)calloc(
        session.size, sizeof(_Atomic(uint16_t)));
    session.pinned = (uint64_t*)calloc(words, sizeof(uint64_t));
    session.retired = (uint64_t*)calloc(words, sizeof(uint64_t));
    session.seen = (uint64_t*)calloc(words, sizeof(uint64_t));
    session.maps = (struct reorder_list*)calloc(
        lists, sizeof(struct reorder_list));
    if (!session.native_enumerate) {
        session.decision = (struct reorder_list*)calloc(
            lists, sizeof(struct reorder_list));
    }

    if (session.counts == NULL || session.pinned == NULL ||
        session.retired == NULL || session.seen == NULL ||
        session.maps == NULL ||
        (!session.native_enumerate && session.decision == NULL)) {
        reorder_free_state();
        return SYLVAN_ERR_OOM;
    }

    /* Seeding promotes too, and it is the pass most likely to: it is the one
       that counts every edge in the table. Counts only rise while it runs, so
       when it is done every node it promoted has at least the ceiling many
       parents at the same time; parent edges are not shared and there are at
       most two per node, which is the bound. See reorder_classify, which
       reserves the same way for the swaps. */
    if (reorder_overflow_reserve(
            2u * session.size / reorder_overflow_at + 1u) != SYLVAN_OK) {
        reorder_free_state();
        return SYLVAN_ERR_OOM;
    }

    /* Pass one: reference counts, and how big each identity's list must be. */
    reorder_seed_CALL(lace, 0, session.size);

    for (uint32_t identity = 0; identity < session.identities; identity++) {
        struct reorder_list *both[2] = {
            reorder_decision_list(identity), reorder_map_list(identity)
        };
        for (int i = 0; i < 2; i++) {
            struct reorder_list *list = both[i];
            if (list == NULL) continue;
            const size_t population =
                atomic_load_explicit(&list->count, memory_order_relaxed);
            atomic_store_explicit(&list->count, 0, memory_order_relaxed);
            if (reorder_list_reserve(list, population) != SYLVAN_OK) {
                reorder_free_state();
                return SYLVAN_ERR_OOM;
            }
        }
    }

    /* Pass two: fill the lists. */
    reorder_scatter_CALL(lace, 0, session.size);

    reorder_pin_roots();

    session.active = 1;
    return SYLVAN_OK;
}

/**
 * Recompute every reference count from the arena and compare.
 *
 * This is the cheap form of the design's SYLVAN_SIFT_VALIDATE build: instead
 * of running a second set of counters alongside the real ones all session
 * long, it rebuilds them once when the session closes. It catches a count
 * that drifted, which is the failure that matters, but not the exact swap
 * that caused it.
 */
static int
reorder_validate_counts(void)
{
    uint64_t *shadow = (uint64_t*)calloc(session.size, sizeof(uint64_t));
    if (shadow == NULL) return SYLVAN_ERR_OOM;

    for (uint64_t index = 2; index < session.size; index++) {
        if (!nodes_is_marked(nodes, index)) continue;
        if (bitmap_get(session.retired, index)) continue;
        mtbddnode *record = reorder_record(index);
        if (mtbddnode_isleaf(record)) continue;
        const uint64_t low = reorder_target(mtbddnode_getlow(record));
        const uint64_t high = reorder_target(mtbddnode_gethigh(record));
        if (low != 0) shadow[low]++;
        if (high != 0) shadow[high]++;
    }

    int status = SYLVAN_OK;
    for (uint64_t index = 2; index < session.size; index++) {
        const uint64_t expected = shadow[index];
        const uint64_t actual = reorder_count_get(index);
        if (expected == actual) continue;
        fprintf(stderr,
                "sylvan: reorder session: node %llu has reference count "
                "%llu, recomputation says %llu\n",
                (unsigned long long)index,
                (unsigned long long)actual,
                (unsigned long long)expected);
        status = SYLVAN_ERR_INVALID;
        break;
    }

    free(shadow);
    return status;
}

/**
 * Give every retired slot back to the allocator.
 *
 * This never runs inside a parallel phase. A worker may still be holding a
 * chain link that names a retired node, and reusing that index before every
 * removal of the phase has completed would hand the walker a different node
 * under the same index.
 */
static void
reorder_release_dead(void)
{
    for (size_t i = 0; i < session.dead_count; i++) {
        const uint64_t index = session.dead[i];
        bitmap_clear(session.retired, index);
        if (nodes_release(nodes, index) != SYLVAN_OK) {
            reorder_fatal("a retired node could not be released");
        }
    }
    session.dead_count = 0;
}

int
sylvan_reorder_end_CALL(lace_worker *lace)
{
    (void)lace;
    if (!session.active) return SYLVAN_ERR_INVALID;

    int status = SYLVAN_OK;
    if (reorder_validate) status = reorder_validate_counts();

    reorder_release_dead();

    const uint64_t promotions = session.overflow_promotions;
    reorder_free_state();
    session.overflow_promotions = promotions;
    return status;
}

/**
 * ---------------------------------------------------------------------------
 * The swap.
 *
 * Ranks r and r+1 hold identities x and y, x above y. Every node of x that
 * does not mention y, every node of y, and everything above and below them,
 * keeps its record and its handle. What is left is:
 *
 *   case B    a decision node f = (x, low, high) with a y-child. It becomes
 *             (y, g0, g1) at the same handle, where g0 = f|y=0 and g1 = f|y=1
 *             are x-nodes interned during the swap.
 *   map case B  a map node m1 = (key x, next m2, value v1) whose next m2 has
 *             key y. It becomes (key y, next m1', value v2) at the same
 *             handle, where m1' = (key x, next m3, value v1) is interned
 *             during the swap.
 *
 * Both rewrites are injective and both produce a record no node can already
 * hold: before the swap y is below x, so nothing rooted at y can have an
 * x-child. That is what lets nodes_reinsert treat an equal node as a fatal
 * invariant violation rather than a case to handle.
 *
 * The children g0 and g1 are legal x-nodes both before and after the swap:
 * their children are the four cofactors of f by x and y, all of which have
 * rank strictly below y, because they are children of y-nodes or of an
 * x-node that skipped y. In particular a g-node is never itself case B, so
 * interning one can neither discover nor create work.
 *
 * PHASES. Classify and reserve (read-only, serial); intern the new children
 * (parallel); unhash and rewrite the records (parallel); re-index them
 * (parallel); retire what died (serial); release the slots. Splitting the
 * rewrite from the re-indexing is one barrier more than the design asks for,
 * and it buys the absence of a data race: nodes_reinsert compares records
 * along a chain, and in a fused phase it could read a record another worker
 * is halfway through rewriting.
 * ---------------------------------------------------------------------------
 */

/** A decision node scheduled for rewriting. */
struct reorder_caseb {
    uint64_t index;
    uint64_t low, high;               /* the old children, as handles     */
    uint64_t low_target, high_target; /* the same, as counted node indices */
    uint64_t g0, g1;                  /* the new children, filled by intern */
};

/** A map node scheduled for rewriting. */
struct reorder_caseb_map {
    uint64_t index;
    uint64_t next_target, value_target; /* m2 and v1, as counted indices  */
    uint64_t rest;                      /* m3, the tail beyond m2         */
    uint64_t value;                     /* v1, m1's own value             */
    uint64_t other;                     /* v2, m2's value                 */
    uint64_t inner;                     /* m1', filled by intern          */
};

/* The plan of the swap that is currently running. Only one swap runs at a
   time, and it runs with the world stopped, so these are plain statics. */
static uint32_t swap_upper;   /* x, moving down */
static uint32_t swap_lower;   /* y, moving up   */
static struct reorder_caseb *swap_nodes;
static size_t swap_node_count;
static size_t swap_node_capacity;
static struct reorder_caseb_map *swap_maps;
static size_t swap_map_count;
static size_t swap_map_capacity;

static void
reorder_plan_free(void)
{
    free(swap_nodes);
    free(swap_maps);
    swap_nodes = NULL;
    swap_maps = NULL;
    swap_node_count = swap_node_capacity = 0;
    swap_map_count = swap_map_capacity = 0;
}

/**
 * Intern a decision node without any chance of garbage collection. The
 * reservation taken in the classification phase is what makes this safe: the
 * table cannot refuse.
 */
static uint64_t
reorder_make_node(uint32_t variable, MTBDD low, MTBDD high)
{
    if (low == high) return low;

    /* Canonical form: the low edge carries no complement. */
    const MTBDD complement = low & bdd_complement;
    low ^= complement;
    high ^= complement;

    struct mtbddnode record;
    mtbddnode_makenode(&record, variable, low, high);

    int created;
    const uint64_t index = nodes_lookup_subtable(
        nodes, NODES_SUBTABLE_DECISION(variable), record.a, record.b, &created);
    if (index == 0) {
        reorder_fatal("the nodes table refused an allocation it had promised");
        return 0;
    }

    if (created) {
        sylvan_stats_count(BDD_NODES_CREATED);
        atomic_fetch_add_explicit(&session.created, 1, memory_order_relaxed);
        reorder_edge_up(low);
        reorder_edge_up(high);
        reorder_list_append(reorder_decision_list(variable), index);
    } else {
        sylvan_stats_count(BDD_NODES_REUSED);
    }

    return complement | index;
}

/** The same for a map node. Map nodes have no reduction rule. */
static uint64_t
reorder_make_map_node(uint32_t key, MTBDD next, MTBDD value)
{
    struct mtbddnode record;
    mtbddnode_makemapnode(&record, key, next, value);

    int created;
    const uint64_t index = nodes_lookup_subtable(
        nodes, NODES_SUBTABLE_MAP, record.a, record.b, &created);
    if (index == 0) {
        reorder_fatal("the nodes table refused an allocation it had promised");
        return 0;
    }

    if (created) {
        sylvan_stats_count(BDD_NODES_CREATED);
        atomic_fetch_add_explicit(&session.created, 1, memory_order_relaxed);
        reorder_edge_up(next);
        reorder_edge_up(value);
        reorder_list_append(reorder_map_list(key), index);
    } else {
        sylvan_stats_count(BDD_NODES_REUSED);
    }

    return index;
}

/**
 * Classification. Read-only: it looks at x's nodes, decides which of them
 * mention y, and records what the rewriting phases will need. It reads the
 * grandchildren here, where nothing is being written, so that the parallel
 * phases never have to read a record another worker may be rewriting.
 */

static int
reorder_classify_decision(uint64_t index)
{
    mtbddnode *record = reorder_record(index);
    const uint64_t low = mtbddnode_getlow(record);
    const uint64_t high = mtbddnode_gethigh(record);

    if (!reorder_has_variable(low, swap_lower) &&
        !reorder_has_variable(high, swap_lower)) {
        return SYLVAN_OK;
    }

    if (swap_node_count == swap_node_capacity) {
        const size_t capacity = swap_node_capacity == 0
            ? 32u : swap_node_capacity * 2u;
        struct reorder_caseb *grown = (struct reorder_caseb*)realloc(
            swap_nodes, capacity * sizeof(struct reorder_caseb));
        if (grown == NULL) return SYLVAN_ERR_OOM;
        swap_nodes = grown;
        swap_node_capacity = capacity;
    }

    struct reorder_caseb *entry = swap_nodes + swap_node_count++;
    entry->index = index;
    entry->low = low;
    entry->high = high;
    entry->low_target = reorder_target(low);
    entry->high_target = reorder_target(high);
    entry->g0 = 0;
    entry->g1 = 0;
    return SYLVAN_OK;
}

static int
reorder_classify_map(uint64_t index)
{
    mtbddnode *record = reorder_record(index);
    const uint64_t next = mtbddnode_getlow(record);
    if (next < 2 || next >= session.size) return SYLVAN_OK;

    mtbddnode *following = reorder_record(next);
    if (mtbddnode_isleaf(following)) return SYLVAN_OK;
    if (!mtbddnode_ismapnode(following)) return SYLVAN_OK;
    if (mtbddnode_getvariable(following) != swap_lower) return SYLVAN_OK;

    if (swap_map_count == swap_map_capacity) {
        const size_t capacity = swap_map_capacity == 0
            ? 8u : swap_map_capacity * 2u;
        struct reorder_caseb_map *grown = (struct reorder_caseb_map*)realloc(
            swap_maps, capacity * sizeof(struct reorder_caseb_map));
        if (grown == NULL) return SYLVAN_ERR_OOM;
        swap_maps = grown;
        swap_map_capacity = capacity;
    }

    struct reorder_caseb_map *entry = swap_maps + swap_map_count++;
    entry->index = index;
    entry->value = mtbddnode_gethigh(record);
    entry->rest = mtbddnode_getlow(following);
    entry->other = mtbddnode_gethigh(following);
    entry->next_target = reorder_target(next);
    entry->value_target = reorder_target(entry->value);
    entry->inner = 0;
    return SYLVAN_OK;
}

/** Visitor for the backends that enumerate a variable natively. */
static int
reorder_enumerate_cb(uint64_t index, void *context)
{
    (void)context;
    if (index < 2 || index >= session.size) return SYLVAN_OK;
    if (!nodes_is_marked(nodes, index)) return SYLVAN_OK;
    if (bitmap_get(session.retired, index)) return SYLVAN_OK;
    mtbddnode *record = reorder_record(index);
    if (mtbddnode_isleaf(record)) return SYLVAN_OK;
    if (mtbddnode_ismapnode(record)) return SYLVAN_OK;
    if (mtbddnode_getvariable(record) != swap_upper) return SYLVAN_OK;
    return reorder_classify_decision(index);
}

/**
 * Classify x's nodes and reserve, before anything is mutated, every resource
 * the rest of the swap will take: node slots for the new children, and room
 * in the node lists, in the retirement queue, in the array of retired slots
 * and in the overflow map.
 *
 * THIS IS THE ONLY PHASE OF A SWAP THAT MAY FAIL. Everything after it either
 * succeeds or is a bug, which is why the later phases report a refusal by
 * aborting: there is no half-swapped forest anyone could use. On failure here
 * the forest, the index and the rank metadata are all exactly as they were,
 * because classification only reads them.
 *
 * The bounds, with k case-B decision nodes and k_map case-B map nodes:
 *
 *   new node slots      2k + k_map   two interned children per decision node,
 *                                    one interned inner node per map node
 *   deaths              2(k + k_map) the rewrite drops two edges per entry and
 *                                    nothing else drops one, so this bounds
 *                                    both the queue and the retired slots
 *   count increments    6k + 4k_map  per decision node: two inside each of the
 *                                    two interned children plus the two edges
 *                                    onto them; per map node: two inside the
 *                                    interned inner node plus its edge and the
 *                                    edge onto the other value
 *
 * The death bound is what the queue was always sized to, and the array of
 * retired slots now shares it: the retirement loop appends one entry per node
 * it takes off the queue.
 *
 * PROMOTIONS. The overflow map grows only when a count crosses the ceiling,
 * and the increment bound above bounds that too, but crudely: it would have a
 * large swap reserve tens of thousands of entries for a map that in any real
 * forest stays empty. The second bound is what makes it cheap. Every promotion
 * of this swap happens in the intern phase, where counts only rise, so at the
 * end of that phase every node promoted by it simultaneously has at least
 * <reorder_overflow_at> parents. Parent edges are not shared between nodes and
 * there are at most two per node in the table, so no more than
 * 2 * size / reorder_overflow_at nodes can be in that position at one time.
 * At the default ceiling that is a handful of entries however big the swap is,
 * and the increment bound only wins for the small swaps where it is small.
 *
 * Deciding instead whether a promotion is possible at all would need the
 * largest count in the forest, which means either a scan or a maximum
 * maintained on the hot path of every single count; neither is worth it
 * against a bound that is already this small.
 */
static int
reorder_classify(void)
{
    int status = SYLVAN_OK;

    if (session.native_enumerate) {
        status = nodes_enumerate_var(
            nodes, swap_upper, reorder_enumerate_cb, NULL);
    } else {
        struct reorder_list *list = reorder_decision_list(swap_upper);
        if (list != NULL) {
            const size_t count = reorder_collect(list, swap_upper, 0);
            for (size_t i = 0; i < count && status == SYLVAN_OK; i++) {
                status = reorder_classify_decision(list->items[i]);
            }
        }
    }
    if (status != SYLVAN_OK) return status;

    struct reorder_list *maps = reorder_map_list(swap_upper);
    if (maps != NULL) {
        const size_t count = reorder_collect(maps, swap_upper, 1);
        for (size_t i = 0; i < count && status == SYLVAN_OK; i++) {
            status = reorder_classify_map(maps->items[i]);
        }
    }
    if (status != SYLVAN_OK) return status;

    const size_t k = swap_node_count;
    const size_t k_map = swap_map_count;
    if (k == 0 && k_map == 0) return SYLVAN_OK;

    /* Worst case: two fresh x-children per case-B node and one fresh inner
       map node per case-B map node. */
    status = nodes_reserve(nodes, 2u * k + k_map);
    if (status != SYLVAN_OK) return status;

    status = reorder_list_reserve(reorder_decision_list(swap_upper), 2u * k);
    if (status == SYLVAN_OK) {
        status = reorder_list_reserve(reorder_decision_list(swap_lower), k);
    }
    if (status == SYLVAN_OK) {
        status = reorder_list_reserve(reorder_map_list(swap_upper), k_map);
    }
    if (status == SYLVAN_OK) {
        status = reorder_list_reserve(reorder_map_list(swap_lower), k_map);
    }
    if (status != SYLVAN_OK) return status;

    /* Deaths: the queue they are announced on, and the array of slots the
       retirement loop appends one entry to for each of them. The array is not
       emptied at every swap -- the deferred release policy keeps it until the
       session ends -- so it is what it already holds plus this swap's bound. */
    const size_t deaths = 2u * (k + k_map);
    if (deaths > session.queue_capacity) {
        uint64_t *grown = (uint64_t*)realloc(
            session.queue, deaths * sizeof(uint64_t));
        if (grown == NULL) return SYLVAN_ERR_OOM;
        session.queue = grown;
        session.queue_capacity = deaths;
    }

    if (session.dead_count + deaths > session.dead_capacity) {
        const size_t capacity = session.dead_count + deaths;
        uint64_t *grown = (uint64_t*)realloc(
            session.dead, capacity * sizeof(uint64_t));
        if (grown == NULL) return SYLVAN_ERR_OOM;
        session.dead = grown;
        session.dead_capacity = capacity;
    }

    /* Promotions into the overflow map. */
    const size_t by_increments = 6u * k + 4u * k_map;
    const size_t by_edges = 2u * session.size / reorder_overflow_at + 1u;
    status = reorder_overflow_reserve(
        by_increments < by_edges ? by_increments : by_edges);
    if (status != SYLVAN_OK) return status;

    return SYLVAN_OK;
}

/**
 * The parallel phases. They run over one index space covering the decision
 * entries first and the map entries after them, so that one task shape drives
 * all of them.
 */
enum reorder_phase {
    REORDER_PHASE_INTERN = 0,
    REORDER_PHASE_REWRITE = 1,
    REORDER_PHASE_REINSERT = 2
};

static void
reorder_apply_decision(int phase, struct reorder_caseb *entry)
{
    switch (phase) {
    case REORDER_PHASE_INTERN: {
        /* The four cofactors of f by x and y. A child that is not a y-node
           supplies both of its cofactors itself. */
        uint64_t low_zero = entry->low, low_one = entry->low;
        uint64_t high_zero = entry->high, high_one = entry->high;

        if (reorder_has_variable(entry->low, swap_lower)) {
            mtbddnode *child = reorder_record(entry->low & REORDER_INDEX_MASK);
            low_zero = mtbddnode_followlow(entry->low, child);
            low_one = mtbddnode_followhigh(entry->low, child);
        }
        if (reorder_has_variable(entry->high, swap_lower)) {
            mtbddnode *child = reorder_record(entry->high & REORDER_INDEX_MASK);
            high_zero = mtbddnode_followlow(entry->high, child);
            high_one = mtbddnode_followhigh(entry->high, child);
        }

        entry->g0 = reorder_make_node(swap_upper, low_zero, high_zero);
        entry->g1 = reorder_make_node(swap_upper, low_one, high_one);
        reorder_edge_up(entry->g0);
        reorder_edge_up(entry->g1);
        break;
    }

    case REORDER_PHASE_REWRITE: {
        if (nodes_remove(nodes, entry->index) != SYLVAN_OK) {
            reorder_fatal("a case-B node was not in the index");
        }
        /* g0 is the low edge of the new node and must carry no complement.
           It cannot: its own low edge is a low edge of a y-node, or the low
           edge of f itself, and neither of those carries one. */
        assert(!MTBDD_HASMARK(entry->g0));
        mtbddnode_makenode(
            reorder_record(entry->index), swap_lower, entry->g0, entry->g1);
        reorder_edge_down(entry->low_target);
        reorder_edge_down(entry->high_target);
        break;
    }

    case REORDER_PHASE_REINSERT:
    default:
        if (nodes_reinsert(nodes, entry->index) != SYLVAN_OK) {
            reorder_fatal("a rewritten node could not be re-indexed");
        }
        reorder_list_append(reorder_decision_list(swap_lower), entry->index);
        break;
    }
}

static void
reorder_apply_map(int phase, struct reorder_caseb_map *entry)
{
    switch (phase) {
    case REORDER_PHASE_INTERN:
        entry->inner = reorder_make_map_node(
            swap_upper, entry->rest, entry->value);
        reorder_edge_up(entry->inner);
        reorder_edge_up(entry->other);
        break;

    case REORDER_PHASE_REWRITE:
        if (nodes_remove(nodes, entry->index) != SYLVAN_OK) {
            reorder_fatal("a case-B map node was not in the index");
        }
        mtbddnode_makemapnode(
            reorder_record(entry->index), swap_lower,
            entry->inner, entry->other);
        reorder_edge_down(entry->next_target);
        reorder_edge_down(entry->value_target);
        break;

    case REORDER_PHASE_REINSERT:
    default:
        if (nodes_reinsert(nodes, entry->index) != SYLVAN_OK) {
            reorder_fatal("a rewritten map node could not be re-indexed");
        }
        reorder_list_append(reorder_map_list(swap_lower), entry->index);
        break;
    }
}

TASK(void, reorder_phase, int, phase, size_t, first, size_t, count)

void
reorder_phase_CALL(lace_worker *lace, int phase, size_t first, size_t count)
{
    if (count > REORDER_SWAP_BLOCK) {
        reorder_phase_SPAWN(lace, phase, first, count / 2);
        reorder_phase_CALL(lace, phase, first + count / 2, count - count / 2);
        reorder_phase_SYNC(lace);
        return;
    }

    const size_t end = first + count;
    for (; first < end; first++) {
        if (first < swap_node_count) {
            reorder_apply_decision(phase, swap_nodes + first);
        } else {
            reorder_apply_map(phase, swap_maps + (first - swap_node_count));
        }
    }
}

/**
 * Retirement.
 *
 * Serial, because the set is small: the death theorem says only the old
 * direct children of the rewritten nodes can die, and the queue asserts it.
 * If the assert ever fires the queue keeps going -- correctness does not
 * depend on the theorem, only the claim that the node lists elsewhere cannot
 * go stale does -- and the swap reports how many deaths were deeper than the
 * theorem allows.
 */
static void
reorder_run_retirement(uint64_t *died, uint64_t *deep)
{
    const size_t direct = atomic_load_explicit(
        &session.queue_count, memory_order_relaxed);
    size_t processed = 0;
    uint64_t retired = 0;

    while (processed < atomic_load_explicit(
               &session.queue_count, memory_order_relaxed)) {
        const uint64_t index = session.queue[processed++];
        if (bitmap_get(session.retired, index)) continue;
        bitmap_set(session.retired, index);

        if (nodes_remove(nodes, index) != SYLVAN_OK) {
            reorder_fatal("a dead node was not in the index");
        }

        /* One entry per node this loop retires, and the loop cannot retire
           more nodes than the queue holds, which the classification phase
           sized this array for. */
        if (session.dead_count == session.dead_capacity) {
            assert(!"the retired-slot array outgrew its reservation");
            reorder_fatal("the retired-slot array outgrew its reservation");
        }
        session.dead[session.dead_count++] = index;
        retired++;

        mtbddnode *record = reorder_record(index);
        reorder_edge_down(reorder_target(mtbddnode_getlow(record)));
        reorder_edge_down(reorder_target(mtbddnode_gethigh(record)));
    }

    const size_t total = atomic_load_explicit(
        &session.queue_count, memory_order_relaxed);
    atomic_store_explicit(&session.queue_count, 0, memory_order_relaxed);

    *died = retired;
    *deep = total > direct ? (uint64_t)(total - direct) : 0;
    assert(total == direct && "the depth-1 death theorem does not hold");
}

int
sylvan_varswap_CALL(lace_worker *lace, uint32_t rank,
                    sylvan_varswap_result *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));
    if (!session.active) return SYLVAN_ERR_INVALID;
    if (session.identities < 2 || rank >= session.identities - 1u) {
        return SYLVAN_ERR_INVALID;
    }

    swap_upper = sylvan_identity_at(rank);
    swap_lower = sylvan_identity_at(rank + 1);

    int status = reorder_classify();
    if (status != SYLVAN_OK) {
        reorder_plan_free();
        return status;
    }

    atomic_store_explicit(&session.created, 0, memory_order_relaxed);

    const size_t total = swap_node_count + swap_map_count;
    if (total != 0) {
        reorder_phase_CALL(lace, REORDER_PHASE_INTERN, 0, total);
        reorder_phase_CALL(lace, REORDER_PHASE_REWRITE, 0, total);
        reorder_phase_CALL(lace, REORDER_PHASE_REINSERT, 0, total);
    }

    uint64_t died = 0, deep = 0;
    reorder_run_retirement(&died, &deep);
    if (reorder_release_at_swap_end) reorder_release_dead();

    if (sylvan_ranks_swap_adjacent(rank) != SYLVAN_OK) {
        reorder_fatal("the rank metadata refused an adjacent swap");
    }

    if (result != NULL) {
        result->swaps = 1;
        result->rewritten = swap_node_count;
        result->map_rewritten = swap_map_count;
        result->created = atomic_load_explicit(
            &session.created, memory_order_relaxed);
        result->died = died;
        result->deep_deaths = deep;
        result->delta = (int64_t)result->created - (int64_t)result->died;
    }

    reorder_plan_free();
    return SYLVAN_OK;
}

/**
 * Test entry points: a session of one or more swaps, in its own Lace frame,
 * the way sylvan_gc() enters sylvan_gc_go.
 */

TASK(int, reorder_session_go, const uint32_t*, ranks, size_t, count,
     sylvan_varswap_result*, result)

int
reorder_session_go_CALL(lace_worker *lace, const uint32_t *ranks, size_t count,
                        sylvan_varswap_result *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));

    int status = sylvan_reorder_begin_CALL(lace);
    if (status != SYLVAN_OK) return status;

    for (size_t i = 0; i < count && status == SYLVAN_OK; i++) {
        sylvan_varswap_result one;
        status = sylvan_varswap_CALL(lace, ranks[i], &one);
        if (status != SYLVAN_OK || result == NULL) continue;
        result->swaps += one.swaps;
        result->rewritten += one.rewritten;
        result->map_rewritten += one.map_rewritten;
        result->created += one.created;
        result->died += one.died;
        result->deep_deaths += one.deep_deaths;
        result->delta += one.delta;
    }

    const int closed = sylvan_reorder_end_CALL(lace);
    return status != SYLVAN_OK ? status : closed;
}

int
sylvan_test_varswap(uint32_t rank, sylvan_varswap_result *result)
{
    return reorder_session_go_NEWFRAME(&rank, 1, result);
}

int
sylvan_test_varswap_sequence(const uint32_t *ranks, size_t count,
                             sylvan_varswap_result *result)
{
    return reorder_session_go_NEWFRAME(ranks, count, result);
}
