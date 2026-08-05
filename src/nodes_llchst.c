/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2018 Tom van Dijk, Johannes Kepler University Linz
 * Copyright 2019-2026 Formal Methods and Tools, University of Twente
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
#include <stdlib.h>
#include <string.h>

/**
 * Lockless chained hash subtables over one dense global node arena.
 *
 * Decision nodes are routed to independently sized subtables. Leaves, maps,
 * ListDD nodes, and legacy direct users have auxiliary subtables. Each node
 * has one immutable link with a 3-bit owner class, 21-bit hash fingerprint,
 * and 40-bit next index. Chains only grow by prepending between stop-the-world
 * collections.
 *
 * GC estimates ordinary populations from contiguous head arrays, retains an
 * exact chain walk for initial/saturated tables, and rebuilds normal marked
 * nodes by scanning the node bitmap. Explicitly routed non-canonical decision
 * entries use the old chains as a compatibility fallback. No identity bits or
 * per-node owner array are needed.
 *
 * WARNING: Like the other nodes tables, allocation currently uses a global
 * thread-local region. Do not use multiple nodes tables concurrently.
 */

#define MASK_INDEX UINT64_C(0x000000ffffffffff)
#define MASK_HASH  UINT64_C(0x1fffff0000000000)
#define MASK_OWNER UINT64_C(0xe000000000000000)
#define MASK_LINK  (MASK_HASH | MASK_INDEX)
#define OWNER_RAW_DECISION UINT64_C(0xa000000000000000)
#define LLCHST_HEAD_LOG2_MASK UINT32_C(0x0000003f)
#define LLCHST_HAS_RAW        UINT32_C(0x80000000)
/* The descriptor size has been fixed and must not be reseeded. */
#define LLCHST_HEAD_SIZED     UINT32_C(0x40000000)
/* The size came from a measured rebuild rather than from a seed estimate. */
#define LLCHST_HEAD_MEASURED  UINT32_C(0x20000000)
/* Routes fit in 25 bits (NODES_SUBTABLE_DEFAULT is 0x01000003), so the top
   bits of the route word carry the maintenance state of the subtable. */
#define LLCHST_ROUTE_MASK     UINT32_C(0x01ffffff)
/* A lookup walked a chain far past its target length here. */
#define LLCHST_WANTS_RESIZE   UINT32_C(0x80000000)
/* A maintenance pass found that a larger head array would not help. */
#define LLCHST_RESIZE_DENIED  UINT32_C(0x40000000)

#define LLCHST_INDEX_BITS       40u
#define LLCHST_LEVEL_BITS       24u
#define LLCHST_PAGE_BITS         8u
#define LLCHST_PAGE_SIZE        (UINT32_C(1) << LLCHST_PAGE_BITS)
#define LLCHST_PAGE_COUNT       (UINT32_C(1) << (LLCHST_LEVEL_BITS - LLCHST_PAGE_BITS))
#define LLCHST_AUX_COUNT         4u
#define LLCHST_REBUILD_GRAIN  4096u

#ifndef SYLVAN_LLCHST_LOAD_FACTOR
#define SYLVAN_LLCHST_LOAD_FACTOR 2u
#endif

#ifndef SYLVAN_LLCHST_INITIAL_HEAD_LOG2
#define SYLVAN_LLCHST_INITIAL_HEAD_LOG2 8u
#endif

/**
 * Chain length that makes a lookup ask for the subtable to be resized.
 *
 * Chains are Poisson distributed around the load factor, so the threshold has
 * to sit far enough into the tail that a well sized subtable practically never
 * trips it: every spurious request costs a stop-the-world pass. At a load
 * factor of 2 the probability of a chain reaching 8 (the obvious "4x the load
 * factor") is about 1e-3 per lookup, which would request maintenance
 * continuously; reaching 16 has probability 5e-10, while an overloaded
 * subtable at a load factor of 5 reaches it within a few thousand lookups.
 * Hence 8x rather than 4x the load factor.
 */
#ifndef SYLVAN_LLCHST_RESIZE_CHAIN_STEPS
#define SYLVAN_LLCHST_RESIZE_CHAIN_STEPS (8u * SYLVAN_LLCHST_LOAD_FACTOR)
#endif

/**
 * Upper bound on the adaptively seeded size of a fresh subtable, as a log2
 * head count. A new subtable has no measurement to go on, so its seed is a
 * guess; this caps how much a single guess may claim before the first
 * measurement corrects it. 2^16 heads is 512 KiB.
 */
#ifndef SYLVAN_LLCHST_SEED_HEAD_LOG2_MAX
#define SYLVAN_LLCHST_SEED_HEAD_LOG2_MAX 16u
#endif

#ifndef SYLVAN_LLCHST_DENSE_LEVELS
#define SYLVAN_LLCHST_DENSE_LEVELS UINT32_C(65536)
#endif

#ifndef SYLVAN_LLCHST_HEAD_SLACK_LOG2
#define SYLVAN_LLCHST_HEAD_SLACK_LOG2 0u
#endif

static_assert(SYLVAN_LLCHST_LOAD_FACTOR >= 1 &&
              SYLVAN_LLCHST_LOAD_FACTOR <= 1048576,
              "SYLVAN_LLCHST_LOAD_FACTOR must be in [1, 1048576]");
static_assert(SYLVAN_LLCHST_INITIAL_HEAD_LOG2 >= 3 &&
              SYLVAN_LLCHST_INITIAL_HEAD_LOG2 < 40,
              "SYLVAN_LLCHST_INITIAL_HEAD_LOG2 must be in [3, 39]");
static_assert(SYLVAN_LLCHST_SEED_HEAD_LOG2_MAX >=
                  SYLVAN_LLCHST_INITIAL_HEAD_LOG2 &&
              SYLVAN_LLCHST_SEED_HEAD_LOG2_MAX < 40,
              "SYLVAN_LLCHST_SEED_HEAD_LOG2_MAX must be in "
              "[SYLVAN_LLCHST_INITIAL_HEAD_LOG2, 39]");
static_assert(SYLVAN_LLCHST_DENSE_LEVELS <= NODES_SUBTABLE_LEAF,
              "SYLVAN_LLCHST_DENSE_LEVELS exceeds the level space");
static_assert(SYLVAN_LLCHST_HEAD_SLACK_LOG2 < 16,
              "SYLVAN_LLCHST_HEAD_SLACK_LOG2 is unreasonable");
static_assert(LLCHST_AUX_COUNT <= 7,
              "LLCHST owner tag has three bits");
static_assert(SYLVAN_LLCHST_RESIZE_CHAIN_STEPS >= 2 &&
              SYLVAN_LLCHST_RESIZE_CHAIN_STEPS <= 4096,
              "SYLVAN_LLCHST_RESIZE_CHAIN_STEPS must be in [2, 4096]");
static_assert(NODES_SUBTABLE_DEFAULT <= LLCHST_ROUTE_MASK,
              "LLCHST route flags overlap the route space");

typedef struct llchst_subtable {
    _Atomic(uintptr_t) heads;
    _Atomic(uint32_t) head_log2;
    _Atomic(uint32_t) route;
} llchst_subtable;

static_assert(sizeof(llchst_subtable) == 16,
              "LLCHST hot descriptor must remain 16 bytes");

typedef struct llchst_page {
    llchst_subtable entries[LLCHST_PAGE_SIZE];
} llchst_page;

typedef struct nodes_table {
    _Atomic(uint64_t)* next;
    uint8_t* data;
    _Atomic(uint64_t)* bitmap1;
    _Atomic(uint64_t)* bitmap2;
    _Atomic(uint64_t)* bitmapc;
    size_t max_size;
    size_t table_size;
    llchst_subtable* decision;
    _Atomic(uint32_t) decision_highwater;
    _Atomic(uintptr_t)* pages;
    _Atomic(uint32_t) page_highwater;
    /* Subtables that currently own a head array, and the total number of head
       slots those arrays hold. Both are maintained off the lookup fast path:
       they change only when a subtable first allocates and at stop-the-world
       rebuilds. They exist to seed a new subtable from the table's actual
       shape instead of from a fixed constant. */
    _Atomic(uint32_t) live_subtables;
    _Atomic(uint64_t) allocated_heads;
    llchst_subtable aux[LLCHST_AUX_COUNT];
    nodes_hash_cb hash_cb;
    nodes_equals_cb equals_cb;
    nodes_create_cb create_cb;
    nodes_destroy_cb destroy_cb;
} nodes_table;

static size_t
region_bitmap_size(size_t table_size)
{
    const size_t regions = table_size / 512u;
    return ((regions + 63u) / 64u) * sizeof(uint64_t);
}

static size_t
node_bitmap_size(size_t table_size)
{
    return ((table_size + 63u) / 64u) * sizeof(uint64_t);
}

static size_t
head_budget(size_t table_size)
{
    size_t budget =
        (table_size + SYLVAN_LLCHST_LOAD_FACTOR - 1u) /
        SYLVAN_LLCHST_LOAD_FACTOR;
    const size_t maximum = (size_t)1 << LLCHST_INDEX_BITS;
    if (budget > (maximum >> SYLVAN_LLCHST_HEAD_SLACK_LOG2)) {
        return maximum;
    }
    return budget << SYLVAN_LLCHST_HEAD_SLACK_LOG2;
}

static uint32_t
floor_log2(size_t value)
{
    uint32_t result = 0;
    while (value > 1) {
        value >>= 1;
        result++;
    }
    return result;
}

static inline size_t
head_count(const llchst_subtable* subtable)
{
    const uint32_t value = atomic_load_explicit(
        &subtable->head_log2, memory_order_relaxed);
    return (size_t)1 << (value & LLCHST_HEAD_LOG2_MASK);
}

static inline _Atomic(uint64_t)*
subtable_heads(const llchst_subtable* subtable)
{
    return (_Atomic(uint64_t)*)(uintptr_t)atomic_load_explicit(
        &subtable->heads, memory_order_acquire);
}

static inline uint32_t
subtable_route(const llchst_subtable* subtable)
{
    return atomic_load_explicit(
        &subtable->route, memory_order_relaxed) & LLCHST_ROUTE_MASK;
}

static void
initialize_descriptor(llchst_subtable* subtable, uint32_t route)
{
    atomic_init(&subtable->heads, (uintptr_t)0);
    atomic_init(
        &subtable->head_log2, SYLVAN_LLCHST_INITIAL_HEAD_LOG2);
    atomic_init(&subtable->route, route);
}

static void
raise_page_highwater(nodes_table* dbs, uint32_t highwater)
{
    uint32_t observed = atomic_load_explicit(
        &dbs->page_highwater, memory_order_relaxed);
    while (observed < highwater &&
           !atomic_compare_exchange_weak_explicit(
               &dbs->page_highwater, &observed, highwater,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

static void
raise_decision_highwater(nodes_table* dbs, uint32_t highwater)
{
    uint32_t observed = atomic_load_explicit(
        &dbs->decision_highwater, memory_order_relaxed);
    while (observed < highwater &&
           !atomic_compare_exchange_weak_explicit(
               &dbs->decision_highwater, &observed, highwater,
               memory_order_relaxed, memory_order_relaxed)) {
    }
}

static llchst_page*
get_page(const nodes_table* table, uint32_t page_index, int create)
{
    nodes_table* dbs = (nodes_table*)table;
    uintptr_t observed = atomic_load_explicit(
        dbs->pages + page_index, memory_order_acquire);
    if (observed != 0 || !create) return (llchst_page*)observed;

    llchst_page* candidate = (llchst_page*)sylvan_alloc_aligned(
        sizeof(llchst_page));
    if (candidate == NULL) return NULL;

    const uint32_t first_route = page_index << LLCHST_PAGE_BITS;
    for (uint32_t i = 0; i < LLCHST_PAGE_SIZE; i++) {
        initialize_descriptor(candidate->entries + i, first_route + i);
    }

    uintptr_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            dbs->pages + page_index, &expected, (uintptr_t)candidate,
            memory_order_acq_rel, memory_order_acquire)) {
        sylvan_free_aligned(candidate, sizeof(llchst_page));
        candidate = (llchst_page*)expected;
    } else {
        raise_page_highwater(dbs, page_index + 1u);
    }

    return candidate;
}

static llchst_subtable*
get_subtable(const nodes_table* dbs, uint32_t route, int create)
{
    if (route < NODES_SUBTABLE_LEAF) {
        if (route < SYLVAN_LLCHST_DENSE_LEVELS) {
            return (llchst_subtable*)dbs->decision + route;
        }
        const uint32_t page_index = route >> LLCHST_PAGE_BITS;
        const uint32_t offset = route & (LLCHST_PAGE_SIZE - 1u);
        llchst_page* page = get_page(dbs, page_index, create);
        return page == NULL ? NULL : page->entries + offset;
    }

    if (route >= NODES_SUBTABLE_LEAF &&
        route <= NODES_SUBTABLE_DEFAULT) {
        return (llchst_subtable*)dbs->aux +
               (route - NODES_SUBTABLE_LEAF);
    }

    return NULL;
}

/**
 * Choose the size of a subtable that is about to allocate its first head
 * array. A fresh subtable has no measurement of its own, so it takes an equal
 * share of the global head budget with the subtables that already exist, plus
 * room for the ones that do not exist yet.
 *
 * Memory: the share never exceeds what is left of head_budget(table_size),
 * which nodes_get_capacity_bytes already reports, so seeding cannot push the
 * table past its advertised footprint. Once the budget is exhausted new
 * subtables fall back to SYLVAN_LLCHST_INITIAL_HEAD_LOG2, which is the
 * unbudgeted worst case and is unchanged from before adaptive seeding: at
 * most 2 KiB for every subtable that ever allocates heads, bounded by
 * SYLVAN_LLCHST_DENSE_LEVELS plus the sparse pages actually touched.
 * A single seed is additionally capped at SYLVAN_LLCHST_SEED_HEAD_LOG2_MAX so
 * an early guess made when few subtables exist cannot claim the whole budget.
 */
static uint32_t
seed_head_log2(const nodes_table* dbs)
{
    const size_t budget = head_budget(dbs->table_size);
    const size_t used = (size_t)atomic_load_explicit(
        &dbs->allocated_heads, memory_order_relaxed);
    const size_t live = (size_t)atomic_load_explicit(
        &dbs->live_subtables, memory_order_relaxed);

    /* Reserve for the auxiliary routes, which every workload eventually
       touches, so that the very first subtable does not take everything. */
    size_t share = budget / (live + LLCHST_AUX_COUNT);
    const size_t remaining = budget > used ? budget - used : 0;
    if (share > remaining) share = remaining;

    uint32_t result = share == 0 ? 0 : floor_log2(share);
    if (result < SYLVAN_LLCHST_INITIAL_HEAD_LOG2) {
        result = SYLVAN_LLCHST_INITIAL_HEAD_LOG2;
    }
    if (result > SYLVAN_LLCHST_SEED_HEAD_LOG2_MAX) {
        result = SYLVAN_LLCHST_SEED_HEAD_LOG2_MAX;
    }
    return result;
}

static _Atomic(uint64_t)*
ensure_heads(nodes_table* dbs, llchst_subtable* subtable)
{
    for (;;) {
        _Atomic(uint64_t)* heads = subtable_heads(subtable);
        if (heads != NULL) return heads;

        uint32_t state = atomic_load_explicit(
            &subtable->head_log2, memory_order_relaxed);
        if ((state & LLCHST_HEAD_SIZED) == 0) {
            /* Fix the size before the array can be observed. Readers derive
               their bucket mask from a relaxed load of head_log2 after an
               acquire load of heads, so the low bits must already be final
               when the pointer is published. Racing threads may compute
               different seeds, so exactly one of them wins this
               compare-exchange and everyone allocates the size it fixed. */
            const uint32_t sized =
                seed_head_log2(dbs) | LLCHST_HEAD_SIZED;
            atomic_compare_exchange_strong_explicit(
                &subtable->head_log2, &state, sized,
                memory_order_relaxed, memory_order_relaxed);
            continue;
        }

        const size_t count =
            (size_t)1 << (state & LLCHST_HEAD_LOG2_MASK);
        const size_t bytes = count * sizeof(uint64_t);
        _Atomic(uint64_t)* candidate =
            (_Atomic(uint64_t)*)sylvan_alloc_aligned(bytes);
        if (candidate == NULL) return NULL;

        uintptr_t expected = 0;
        if (!atomic_compare_exchange_strong_explicit(
                &subtable->heads, &expected, (uintptr_t)candidate,
                memory_order_acq_rel, memory_order_acquire)) {
            sylvan_free_aligned((void*)candidate, bytes);
            return (_Atomic(uint64_t)*)expected;
        }

        const uint32_t route = subtable_route(subtable);
        if (route < SYLVAN_LLCHST_DENSE_LEVELS) {
            raise_decision_highwater(dbs, route + 1u);
        }
        atomic_fetch_add_explicit(
            &dbs->live_subtables, 1u, memory_order_relaxed);
        atomic_fetch_add_explicit(
            &dbs->allocated_heads, (uint64_t)count, memory_order_relaxed);
        sylvan_stats_count(LLCHST_SUBTABLE_CREATED);
        return candidate;
    }
}

void*
nodes_get_pointer(const nodes_table* dbs, size_t index)
{
    return dbs->data + index * 16;
}

size_t
nodes_get_max_size(const nodes_table* dbs)
{
    return dbs->max_size;
}

size_t
nodes_get_size(const nodes_table* dbs)
{
    return dbs->table_size;
}

size_t
nodes_get_capacity_bytes(size_t capacity)
{
    const size_t estimated_heads =
        (capacity + SYLVAN_LLCHST_LOAD_FACTOR - 1u) /
        SYLVAN_LLCHST_LOAD_FACTOR *
        ((size_t)1 << SYLVAN_LLCHST_HEAD_SLACK_LOG2);
    return capacity * (2 * sizeof(uint64_t) + sizeof(uint64_t)) +
           estimated_heads * sizeof(uint64_t) +
           region_bitmap_size(capacity) + 2 * node_bitmap_size(capacity) +
           LLCHST_PAGE_COUNT * sizeof(uintptr_t) +
           SYLVAN_LLCHST_DENSE_LEVELS * sizeof(llchst_subtable);
}

void
nodes_set_size(nodes_table* dbs, size_t size)
{
    if (size > 128 && size >= dbs->table_size && size <= dbs->max_size) {
        dbs->table_size = size;
    }
}

static SYLVAN_TLS uint64_t my_region = UINT64_MAX;

TASK(void, nodes_reset_region)

void
nodes_reset_region_CALL(lace_worker* lace)
{
    (void)lace;
    my_region = UINT64_MAX;
}

static uint64_t
claim_next_region(const nodes_table* dbs, uint64_t start_region)
{
    const uint64_t regions = dbs->table_size / 512u;
    if (regions == 0) return UINT64_MAX;
    start_region %= regions;

    for (uint64_t offset = 0; offset < regions; offset++) {
        const uint64_t region = (start_region + offset) % regions;
        _Atomic(uint64_t)* ptr = dbs->bitmap1 + (region / 64u);
        const uint64_t mask =
            UINT64_C(0x8000000000000000) >> (region & 63u);
        uint64_t value = atomic_load_explicit(ptr, memory_order_relaxed);

        for (;;) {
            if (value & mask) break;
            if (atomic_compare_exchange_weak_explicit(
                    ptr, &value, value | mask,
                    memory_order_relaxed, memory_order_relaxed)) {
                return region;
            }
        }
    }

    return UINT64_MAX;
}

static uint64_t
claim_data_bucket(const nodes_table* dbs)
{
    for (;;) {
        if (my_region != UINT64_MAX) {
            _Atomic(uint64_t)* ptr = dbs->bitmap2 + my_region * 8u;
            for (int i = 0; i < 8; i++, ptr++) {
                const uint64_t value = atomic_load_explicit(
                    ptr, memory_order_relaxed);
                if (value != UINT64_MAX) {
                    const unsigned int bit = clz_uint64(~value);
                    const uint64_t mask =
                        UINT64_C(0x8000000000000000) >> bit;
                    atomic_store_explicit(
                        ptr, value | mask, memory_order_relaxed);
                    return (my_region * 8u + (uint64_t)i) * 64u + bit;
                }
            }

            const uint64_t claimed = claim_next_region(dbs, my_region + 1u);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        } else {
            const uint64_t regions = dbs->table_size / 512u;
            uint64_t start = 0;
            lace_worker* worker = lace_get_worker();
            if (regions != 0 && worker != NULL) {
                start = ((uint64_t)worker->worker * regions) /
                        (uint64_t)lace_worker_count();
            }

            const uint64_t claimed = claim_next_region(dbs, start);
            if (claimed == UINT64_MAX) return UINT64_MAX;
            my_region = claimed;
        }
    }
}

static void
release_data_bucket(const nodes_table* dbs, uint64_t index)
{
    _Atomic(uint64_t)* ptr = dbs->bitmap2 + index / 64u;
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63u);
    const uint64_t value = atomic_load_explicit(ptr, memory_order_relaxed);
    atomic_store_explicit(ptr, value & ~mask, memory_order_relaxed);
}

/*
 * The insertion and cleanup paths do not need read-modify-write on bitmapc:
 * during insertion each worker owns a whole data region via bitmap1, and
 * cleanup/rebuild is stop-the-world relative to lookup. The words are atomic
 * only so that the reorder primitives, which do not own a region, can clear a
 * single bit without racing with those relaxed accesses.
 */
static void
set_custom_bucket(const nodes_table* dbs, uint64_t index, int on)
{
    _Atomic(uint64_t)* ptr = dbs->bitmapc + index / 64u;
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63u);
    const uint64_t value = atomic_load_explicit(ptr, memory_order_relaxed);
    atomic_store_explicit(
        ptr, on ? (value | mask) : (value & ~mask), memory_order_relaxed);
}

static int
is_custom_bucket(const nodes_table* dbs, uint64_t index)
{
    const _Atomic(uint64_t)* ptr = dbs->bitmapc + index / 64u;
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63u);
    return (atomic_load_explicit(ptr, memory_order_relaxed) & mask) ? 1 : 0;
}

static inline uint64_t
hash_key(const nodes_table* dbs, uint32_t route,
         uint64_t a, uint64_t b, int custom)
{
    const uint64_t seed = UINT64_C(14695981039346656037);
    if (custom) return dbs->hash_cb(a, b, seed);
    if (route < NODES_SUBTABLE_LEAF) b &= MASK_INDEX;
    return sylvan_tabhash16(a, b, seed);
}

static inline uint64_t
owner_tag(uint32_t route, uint64_t b)
{
    if (route < NODES_SUBTABLE_LEAF) {
        if ((uint32_t)(b >> LLCHST_INDEX_BITS) == route) return 0;
        return OWNER_RAW_DECISION;
    }
    return (uint64_t)(route - NODES_SUBTABLE_LEAF + 1u) << 61;
}

static inline uint32_t
owner_route(uint64_t tagged_next, const uint64_t* data)
{
    const uint32_t owner = (uint32_t)(
        (tagged_next & MASK_OWNER) >> 61);
    if (owner == 0) {
        return (uint32_t)(data[1] >> LLCHST_INDEX_BITS);
    }
    return NODES_SUBTABLE_LEAF + owner - 1u;
}

static inline uint64_t
find_in_chain(const nodes_table* dbs, uint64_t link, uint64_t stop,
              uint64_t masked_hash, uint64_t a, uint64_t b, int custom,
              size_t* steps)
{
    size_t walked = 0;
    while ((link & MASK_LINK) != stop) {
        assert(stop == 0 || (link & MASK_LINK) != 0);
        const uint64_t index = link & MASK_INDEX;
        if ((link & MASK_HASH) == masked_hash) {
            const uint64_t* data =
                ((const uint64_t*)dbs->data) + 2 * index;
            if (custom) {
                if (dbs->equals_cb(a, b, data[0], data[1])) {
                    *steps = walked;
                    return index;
                }
            } else if (data[0] == a && data[1] == b) {
                *steps = walked;
                return index;
            }
        }

        walked++;
        sylvan_stats_count(LLMSSET_LOOKUP);
        link = atomic_load_explicit(dbs->next + index, memory_order_relaxed);
    }
    *steps = walked;
    return 0;
}

/**
 * Ask for this subtable to be given a larger head array.
 *
 * This runs on the lookup path, so it must not do a read-modify-write: the
 * request is a plain relaxed store, and every thread that reaches it stores
 * the same value. The bits it writes are only ever cleared while the world is
 * stopped, so there is nothing for the store to lose. The global flag is what
 * sylvan_gc_test polls; the per-subtable bit is what tells the maintenance
 * pass which chains to walk, so that the pass costs nothing for the subtables
 * that are fine.
 */
static void
request_resize(llchst_subtable* subtable)
{
    const uint32_t route = atomic_load_explicit(
        &subtable->route, memory_order_relaxed);
    if ((route & (LLCHST_WANTS_RESIZE | LLCHST_RESIZE_DENIED)) != 0) return;
    atomic_store_explicit(
        &subtable->route, route | LLCHST_WANTS_RESIZE,
        memory_order_relaxed);
    atomic_store_explicit(
        &sylvan_maintenance_requested, 1, memory_order_relaxed);
    sylvan_stats_count(LLCHST_RESIZE_REQUESTED);
}

static inline uint64_t
nodes_lookup2(const nodes_table* dbs, uint32_t route,
              uint64_t a, uint64_t b, int* created, const int custom)
{
    sylvan_stats_count(LLMSSET_LOOKUP_CALLS);
    llchst_subtable* subtable = get_subtable(dbs, route, 1);
    if (subtable == NULL) return 0;
    _Atomic(uint64_t)* heads = ensure_heads((nodes_table*)dbs, subtable);
    if (heads == NULL) return 0;

    const uint64_t hash = hash_key(dbs, route, a, b, custom);
    const uint64_t masked_hash = hash & MASK_HASH;
    _Atomic(uint64_t)* head = heads + (hash & (head_count(subtable) - 1u));

    uint64_t observed = atomic_load_explicit(head, memory_order_acquire);
    size_t steps = 0;
    uint64_t found = find_in_chain(
        dbs, observed, 0, masked_hash, a, b, custom, &steps);
    /* The walk above already measured this chain, so noticing that the
       subtable has outgrown its head array is free. */
    if (SYLVAN_UNLIKELY(steps >= SYLVAN_LLCHST_RESIZE_CHAIN_STEPS)) {
        request_resize(subtable);
    }
    if (found != 0) {
        *created = 0;
        return found;
    }

    const uint64_t candidate = claim_data_bucket(dbs);
    if (candidate == UINT64_MAX) return 0;

    if (custom) {
        const int status = dbs->create_cb(&a, &b);
        if (status != 0) {
            release_data_bucket(dbs, candidate);
            *created = status;
            return 0;
        }
    }

    uint64_t* data = ((uint64_t*)dbs->data) + 2 * candidate;
    data[0] = a;
    data[1] = b;
    const uint64_t candidate_link = masked_hash | candidate;
    const uint64_t candidate_owner = owner_tag(route, b);

    for (;;) {
        atomic_store_explicit(
            dbs->next + candidate,
            candidate_owner | (observed & MASK_LINK),
            memory_order_relaxed);
        uint64_t expected = observed;
        if (atomic_compare_exchange_strong_explicit(
                head, &expected, candidate_link,
                memory_order_acq_rel, memory_order_acquire)) {
            if (candidate_owner == OWNER_RAW_DECISION) {
                atomic_fetch_or_explicit(
                    &subtable->head_log2, LLCHST_HAS_RAW,
                    memory_order_relaxed);
            }
            if (custom) set_custom_bucket(dbs, candidate, 1);
            *created = 1;
            return candidate;
        }

        sylvan_stats_count(LLCHST_HEAD_CAS_FAILURE);

        found = find_in_chain(
            dbs, expected, observed, masked_hash, a, b, custom, &steps);
        if (found != 0) {
            if (custom) dbs->destroy_cb(a, b);
            release_data_bucket(dbs, candidate);
            *created = 0;
            return found;
        }
        observed = expected;
    }
}

uint64_t
nodes_lookup(const nodes_table* dbs, uint64_t a, uint64_t b, int* created)
{
    return nodes_lookup2(
        dbs, NODES_SUBTABLE_DEFAULT, a, b, created, 0);
}

uint64_t
nodes_lookupc(const nodes_table* dbs, uint64_t a, uint64_t b, int* created)
{
    return nodes_lookup2(
        dbs, NODES_SUBTABLE_DEFAULT, a, b, created, 1);
}

uint64_t
nodes_lookup_subtable(const nodes_table* dbs, uint32_t subtable,
                      uint64_t a, uint64_t b, int* created)
{
    return nodes_lookup2(dbs, subtable, a, b, created, 0);
}

uint64_t
nodes_lookupc_subtable(const nodes_table* dbs, uint32_t subtable,
                       uint64_t a, uint64_t b, int* created)
{
    return nodes_lookup2(dbs, subtable, a, b, created, 1);
}

nodes_table*
nodes_create(size_t initial_size, size_t max_size)
{
    nodes_table* dbs = (nodes_table*)sylvan_alloc_aligned(
        sizeof(nodes_table));
    if (dbs == NULL) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n",
                strerror(errno));
        exit(1);
    }

    if (initial_size > max_size) {
        fprintf(stderr, "nodes_create: initial_size > max_size!\n");
        exit(1);
    }
    if (max_size > UINT64_C(0x0000010000000000)) {
        fprintf(stderr, "nodes_create: max_size exceeds the 40-bit index!\n");
        exit(1);
    }
    if (initial_size < 512 || initial_size % 512 != 0 ||
        max_size % 512 != 0) {
        fprintf(stderr,
                "nodes_create: table sizes must be multiples of 512!\n");
        exit(1);
    }

    dbs->max_size = max_size;
    dbs->table_size = initial_size;
    dbs->next = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        max_size * sizeof(*dbs->next));
    dbs->data = (uint8_t*)sylvan_alloc_aligned(
        max_size * 2 * sizeof(uint64_t));
    dbs->bitmap1 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        region_bitmap_size(max_size));
    dbs->bitmap2 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        node_bitmap_size(max_size));
    dbs->bitmapc = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        node_bitmap_size(max_size));
    dbs->decision = (llchst_subtable*)sylvan_alloc_aligned(
        SYLVAN_LLCHST_DENSE_LEVELS * sizeof(*dbs->decision));
    dbs->pages = (_Atomic(uintptr_t)*)sylvan_alloc_aligned(
        LLCHST_PAGE_COUNT * sizeof(*dbs->pages));

    if (dbs->next == NULL || dbs->data == NULL || dbs->bitmap1 == NULL ||
        dbs->bitmap2 == NULL || dbs->bitmapc == NULL ||
        dbs->decision == NULL || dbs->pages == NULL) {
        fprintf(stderr, "nodes_create: Unable to allocate memory: %s!\n",
                strerror(errno));
        exit(1);
    }

    atomic_init(&dbs->decision_highwater, 0);
    atomic_init(&dbs->page_highwater, 0);
    atomic_init(&dbs->live_subtables, 0);
    atomic_init(&dbs->allocated_heads, 0);
    for (uint32_t i = 0; i < SYLVAN_LLCHST_DENSE_LEVELS; i++) {
        initialize_descriptor(dbs->decision + i, i);
    }
    for (uint32_t i = 0; i < LLCHST_AUX_COUNT; i++) {
        initialize_descriptor(
            dbs->aux + i, NODES_SUBTABLE_LEAF + i);
    }

    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);
    dbs->hash_cb = NULL;
    dbs->equals_cb = NULL;
    dbs->create_cb = NULL;
    dbs->destroy_cb = NULL;

    nodes_reset_region_TOGETHER();
    sylvan_init_hash();
    return dbs;
}

static void
free_descriptor_heads(llchst_subtable* subtable)
{
    _Atomic(uint64_t)* heads = subtable_heads(subtable);
    if (heads != NULL) {
        sylvan_free_aligned(
            (void*)heads, head_count(subtable) * sizeof(*heads));
    }
}

void
nodes_free(nodes_table* dbs)
{
    const uint32_t decision_highwater = atomic_load_explicit(
        &dbs->decision_highwater, memory_order_relaxed);
    for (uint32_t i = 0; i < decision_highwater; i++) {
        free_descriptor_heads(dbs->decision + i);
    }
    const uint32_t highwater = atomic_load_explicit(
        &dbs->page_highwater, memory_order_relaxed);
    for (uint32_t page_index = 0; page_index < highwater; page_index++) {
        llchst_page* page = (llchst_page*)(uintptr_t)atomic_load_explicit(
            dbs->pages + page_index, memory_order_relaxed);
        if (page == NULL) continue;
        for (uint32_t i = 0; i < LLCHST_PAGE_SIZE; i++) {
            free_descriptor_heads(page->entries + i);
        }
        sylvan_free_aligned(page, sizeof(llchst_page));
    }
    for (uint32_t i = 0; i < LLCHST_AUX_COUNT; i++) {
        free_descriptor_heads(dbs->aux + i);
    }

    sylvan_free_aligned(
        dbs->decision,
        SYLVAN_LLCHST_DENSE_LEVELS * sizeof(*dbs->decision));
    sylvan_free_aligned(
        (void*)dbs->pages,
        LLCHST_PAGE_COUNT * sizeof(*dbs->pages));
    sylvan_free_aligned(
        (void*)dbs->next, dbs->max_size * sizeof(*dbs->next));
    sylvan_free_aligned(
        dbs->data, dbs->max_size * 2 * sizeof(uint64_t));
    sylvan_free_aligned(
        (void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_free_aligned(
        (void*)dbs->bitmap2, node_bitmap_size(dbs->max_size));
    sylvan_free_aligned(
        (void*)dbs->bitmapc, node_bitmap_size(dbs->max_size));
    sylvan_free_aligned(dbs, sizeof(nodes_table));
}

void
nodes_clear_CALL(lace_worker* lace, nodes_table* dbs)
{
    (void)lace;
    sylvan_clear_aligned(
        (void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_clear_aligned(
        (void*)dbs->bitmap2, node_bitmap_size(dbs->max_size));

    /* Keep old chain heads and links as the descriptor-owner enumeration.
       Rebuild replaces each head array after counting and reinserting its
       marked population. Normal lookup is stopped throughout this interval. */
    dbs->bitmap2[0] = UINT64_C(0xc000000000000000);
    nodes_reset_region_TOGETHER();
}

int
nodes_is_marked(const nodes_table* dbs, uint64_t index)
{
    const _Atomic(uint64_t)* ptr = dbs->bitmap2 + index / 64u;
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63u);
    return (atomic_load_explicit(ptr, memory_order_relaxed) & mask) ? 1 : 0;
}

void
nodes_mark_rec_CALL(lace_worker* lace, const nodes_table* dbs, uint64_t index)
{
    if (index == 0 || index == 1) return;

    _Atomic(uint64_t)* ptr = dbs->bitmap2 + index / 64u;
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63u);
    uint64_t value = atomic_load_explicit(ptr, memory_order_relaxed);
    for (;;) {
        if (value & mask) return;
        if (atomic_compare_exchange_weak_explicit(
                ptr, &value, value | mask,
                memory_order_relaxed, memory_order_relaxed)) {
            const uint64_t* data =
                ((const uint64_t*)dbs->data) + 2 * index;
            const uint64_t a = data[0];
            const uint64_t b = data[1];
            if (a & UINT64_C(0x4000000000000000)) return;
            nodes_mark_rec_SPAWN(lace, dbs, b & MASK_INDEX);
            nodes_mark_rec_CALL(lace, dbs, a & MASK_INDEX);
            nodes_mark_rec_SYNC(lace);
            return;
        }
    }
}

struct llchst_rebuild_entry {
    llchst_subtable* subtable;
    _Atomic(uint64_t)* old_heads;
    size_t old_count;
    int active;
    size_t population;
    size_t target;
    _Atomic(uint64_t)* new_heads;
    uint32_t new_log2;
    int has_raw;
};

struct llchst_measure_context {
    const nodes_table* dbs;
    const _Atomic(uint64_t)* heads;
    _Atomic(size_t) live;
    _Atomic(size_t) population;
    _Atomic(int) has_raw;
};

struct llchst_head_measure_context {
    const _Atomic(uint64_t)* heads;
    _Atomic(size_t) occupied;
};

TASK(void, llchst_measure_heads,
     struct llchst_head_measure_context*, context,
     size_t, first, size_t, count)

void
llchst_measure_heads_CALL(
    lace_worker* lace, struct llchst_head_measure_context* context,
    size_t first, size_t count)
{
    if (count > LLCHST_REBUILD_GRAIN && lace_worker_count() > 1) {
        const size_t split = count / 2u;
        llchst_measure_heads_SPAWN(
            lace, context, first, split);
        llchst_measure_heads_CALL(
            lace, context, first + split, count - split);
        llchst_measure_heads_SYNC(lace);
        return;
    }

    size_t occupied = 0;
    for (size_t i = first; i < first + count; i++) {
        occupied += atomic_load_explicit(
            context->heads + i, memory_order_relaxed) != 0;
    }
    atomic_fetch_add_explicit(
        &context->occupied, occupied, memory_order_relaxed);
}

TASK(void, llchst_measure_chains,
     struct llchst_measure_context*, context,
     size_t, first, size_t, count)

void
llchst_measure_chains_CALL(
    lace_worker* lace, struct llchst_measure_context* context,
    size_t first, size_t count)
{
    if (count > LLCHST_REBUILD_GRAIN && lace_worker_count() > 1) {
        const size_t split = count / 2;
        llchst_measure_chains_SPAWN(
            lace, context, first, split);
        llchst_measure_chains_CALL(
            lace, context, first + split, count - split);
        llchst_measure_chains_SYNC(lace);
        return;
    }

    /* The chain terminates on a zero *link*, not a zero word: every auxiliary
       route and OWNER_RAW_DECISION keeps a nonzero owner tag in the top three
       bits, so the last node's next word is "tag | 0". Testing the raw word
       walks one step past the end onto index 0, which nodes_clear keeps marked
       and which therefore inflates both the live count and the population. */
    size_t live = 0;
    size_t population = 0;
    int has_raw = 0;
    for (size_t i = first; i < first + count; i++) {
        uint64_t link = atomic_load_explicit(
            context->heads + i, memory_order_relaxed);
        while ((link & MASK_LINK) != 0) {
            const uint64_t index = link & MASK_INDEX;
            live += (size_t)nodes_is_marked(context->dbs, index);
            population++;
            const uint64_t next = atomic_load_explicit(
                context->dbs->next + index, memory_order_relaxed);
            has_raw |= (next & MASK_OWNER) == OWNER_RAW_DECISION;
            link = next;
        }
    }
    atomic_fetch_add_explicit(
        &context->live, live, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &context->population, population, memory_order_relaxed);
    if (has_raw) {
        atomic_store_explicit(
            &context->has_raw, 1, memory_order_relaxed);
    }
}

static size_t
estimate_population(size_t heads, size_t occupied)
{
    if (occupied == 0) return 0;
    if (occupied == heads) {
        if (heads > SIZE_MAX / 8u) return SIZE_MAX;
        return heads * 8u;
    }

    /* Sixteen terms of -log(1-x) stay within a few percent below the exact
       measurement threshold and avoid a libm dependency in the backend. */
    const long double x =
        (long double)occupied / (long double)heads;
    long double term = x;
    long double series = 0.0L;
    for (unsigned int i = 1; i <= 16; i++) {
        series += term / (long double)i;
        term *= x;
    }
    const long double estimate = (long double)heads * series;
    const size_t result = (size_t)(estimate + 0.999999L);
    return result < occupied ? occupied : result;
}

static void
measure_entry(lace_worker* lace, const nodes_table* dbs,
              struct llchst_rebuild_entry* entry)
{
    struct llchst_head_measure_context head_context = {
        .heads = entry->old_heads
    };
    atomic_init(&head_context.occupied, 0);
    llchst_measure_heads_CALL(
        lace, &head_context, 0, entry->old_count);
    const size_t occupied = atomic_load_explicit(
        &head_context.occupied, memory_order_relaxed);
    const uint32_t descriptor_state = atomic_load_explicit(
        &entry->subtable->head_log2, memory_order_relaxed);
    entry->has_raw = (descriptor_state & LLCHST_HAS_RAW) != 0;

    if (occupied == 0) {
        entry->active = 0;
        entry->population = 0;
        return;
    }

    /* A size that came from a seed rather than from a previous measurement
       does not encode the level's activity yet, and a nearly saturated table
       cannot be estimated reliably from its occupied heads. Pay for an exact
       chain walk only in those cases. */
    if ((descriptor_state & LLCHST_HEAD_MEASURED) != 0 &&
        occupied < entry->old_count - entry->old_count / 8u) {
        entry->population = estimate_population(
            entry->old_count, occupied);
        entry->active = 1;
        return;
    }

    struct llchst_measure_context context = {
        .dbs = dbs,
        .heads = entry->old_heads
    };
    atomic_init(&context.live, 0);
    atomic_init(&context.population, 0);
    atomic_init(&context.has_raw, 0);
    llchst_measure_chains_CALL(
        lace, &context, 0, entry->old_count);
    entry->active = atomic_load_explicit(
        &context.live, memory_order_relaxed) != 0;
    entry->population = atomic_load_explicit(
        &context.population, memory_order_relaxed);
    entry->has_raw |= atomic_load_explicit(
        &context.has_raw, memory_order_relaxed);
}

TASK(void, llchst_measure_entries,
     const nodes_table*, dbs, struct llchst_rebuild_entry*, entries,
     size_t, first, size_t, count)

void
llchst_measure_entries_CALL(
    lace_worker* lace, const nodes_table* dbs,
    struct llchst_rebuild_entry* entries,
    size_t first, size_t count)
{
    if (count > 1 && lace_worker_count() > 1) {
        const size_t split = count / 2;
        llchst_measure_entries_SPAWN(
            lace, dbs, entries, first, split);
        llchst_measure_entries_CALL(
            lace, dbs, entries, first + split, count - split);
        llchst_measure_entries_SYNC(lace);
        return;
    }

    for (size_t i = first; i < first + count; i++) {
        measure_entry(lace, dbs, entries + i);
    }
}

static int
compare_promotion_priority(const void* left, const void* right)
{
    const struct llchst_rebuild_entry* a =
        (const struct llchst_rebuild_entry*)left;
    const struct llchst_rebuild_entry* b =
        (const struct llchst_rebuild_entry*)right;
    const size_t a_count = (size_t)1 << a->new_log2;
    const size_t b_count = (size_t)1 << b->new_log2;
    const long double a_priority = !a->active ? 0.0L :
        (long double)a->target / (long double)a_count;
    const long double b_priority = !b->active ? 0.0L :
        (long double)b->target / (long double)b_count;
    if (a_priority < b_priority) return 1;
    if (a_priority > b_priority) return -1;
    return 0;
}

static void
allocate_head_budget(struct llchst_rebuild_entry* entries,
                     size_t count, size_t table_size)
{
    /* Only active subtables receive a share, so only they may contribute to
       the divisor. Counting the population of subtables that are about to be
       released diluted every surviving share by the dead levels' size.

       The share uses <population>, the pre-collection chain length, and not
       the live count that measure_entry computed alongside it. Two reasons:
       the fast estimating path never produces a live count at all, so mixing
       the two would systematically favour the estimated entries; and a
       subtable's pre-collection size is the better predictor of what it will
       hold again before the next collection. Over-provisioning only costs
       head memory that the next collection reclaims, whereas
       under-provisioning costs chain steps on every lookup until the next
       maintenance pass. */
    size_t population = 0;
    size_t active = 0;
    for (size_t i = 0; i < count; i++) {
        if (!entries[i].active) continue;
        population += entries[i].population;
        active++;
    }
    if (population == 0) return;

    size_t budget = head_budget(table_size);
    if (budget < active) budget = active;
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        struct llchst_rebuild_entry* entry = entries + i;
        if (!entry->active) {
            entry->target = 0;
            entry->new_log2 = SYLVAN_LLCHST_INITIAL_HEAD_LOG2;
            continue;
        }

        size_t target = (size_t)(
            (long double)entry->population * (long double)budget /
            (long double)population);
        if (target == 0) target = 1;
        entry->target = target;
        entry->new_log2 = floor_log2(target);
        used += (size_t)1 << entry->new_log2;
    }

    /* Power-of-two masks make the lookup cheap. Start with the floor for
       every proportional share, then spend the remaining global slots on
       the descriptors closest to their next power of two. */
    qsort(entries, count, sizeof(*entries), compare_promotion_priority);
    for (size_t i = 0; i < count; i++) {
        struct llchst_rebuild_entry* entry = entries + i;
        if (!entry->active) continue;
        const size_t current = (size_t)1 << entry->new_log2;
        if (current < entry->target && used <= budget &&
            current <= budget - used) {
            entry->new_log2++;
            used += current;
        }
    }
}

static void
rehash_node(nodes_table* dbs, uint32_t route,
            _Atomic(uint64_t)* heads, size_t mask, uint64_t index,
            int parallel)
{
    const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
    const int custom = is_custom_bucket(dbs, index);
    const uint64_t hash = hash_key(
        dbs, route, data[0], data[1], custom);
    const uint64_t link = (hash & MASK_HASH) | index;
    _Atomic(uint64_t)* head = heads + (hash & mask);

    uint64_t observed = atomic_load_explicit(head, memory_order_relaxed);
    if (!parallel) {
        atomic_store_explicit(
            dbs->next + index,
            owner_tag(route, data[1]) | (observed & MASK_LINK),
            memory_order_relaxed);
        atomic_store_explicit(head, link, memory_order_relaxed);
    } else {
        /* Rebuild is stop-the-world: no lookup can observe the temporary
           head-before-link order, and all link stores complete before the
           Lace join publishes the rebuilt table. One exchange is therefore
           sufficient; a compare/exchange retry loop only adds contention. */
        observed = atomic_exchange_explicit(
            head, link, memory_order_relaxed);
        atomic_store_explicit(
            dbs->next + index,
            owner_tag(route, data[1]) | (observed & MASK_LINK),
            memory_order_relaxed);
    }
    sylvan_stats_count(LLCHST_NODE_REHASHED);
}

static int
prepare_entry(struct llchst_rebuild_entry* entry)
{
    entry->new_heads = NULL;
    if (!entry->active) return 0;

    sylvan_stats_count(LLCHST_SUBTABLE_REBUILT);
    const size_t new_count = (size_t)1 << entry->new_log2;
    entry->new_heads =
        (_Atomic(uint64_t)*)sylvan_alloc_aligned(
            new_count * sizeof(*entry->new_heads));
    return entry->new_heads == NULL;
}

TASK(int, llchst_prepare_entries,
     struct llchst_rebuild_entry*, entries,
     size_t, first, size_t, count)

int
llchst_prepare_entries_CALL(
    lace_worker* lace, struct llchst_rebuild_entry* entries,
    size_t first, size_t count)
{
    if (count > 1 && lace_worker_count() > 1) {
        const size_t split = count / 2u;
        llchst_prepare_entries_SPAWN(
            lace, entries, first, split);
        const int right = llchst_prepare_entries_CALL(
            lace, entries, first + split, count - split);
        return right + llchst_prepare_entries_SYNC(lace);
    }

    int bad = 0;
    for (size_t i = first; i < first + count; i++) {
        bad += prepare_entry(entries + i);
    }
    return bad;
}

TASK(void, llchst_rehash_nodes,
     nodes_table*, dbs, size_t, first, size_t, count, int, parallel)

TASK(void, llchst_rehash_raw_entries,
     nodes_table*, dbs, struct llchst_rebuild_entry*, entries,
     size_t, first, size_t, count, int, parallel)

void
llchst_rehash_raw_entries_CALL(
    lace_worker* lace, nodes_table* dbs,
    struct llchst_rebuild_entry* entries,
    size_t first, size_t count, int parallel)
{
    if (count > 1 && parallel) {
        const size_t split = count / 2u;
        llchst_rehash_raw_entries_SPAWN(
            lace, dbs, entries, first, split, parallel);
        llchst_rehash_raw_entries_CALL(
            lace, dbs, entries, first + split, count - split, parallel);
        llchst_rehash_raw_entries_SYNC(lace);
        return;
    }

    for (size_t e = first; e < first + count; e++) {
        struct llchst_rebuild_entry* entry = entries + e;
        if (!entry->has_raw || !entry->active) continue;

        const uint32_t route = subtable_route(entry->subtable);
        const size_t new_mask =
            ((size_t)1 << entry->new_log2) - 1u;
        for (size_t i = 0; i < entry->old_count; i++) {
            uint64_t link = atomic_load_explicit(
                entry->old_heads + i, memory_order_relaxed);
            while ((link & MASK_LINK) != 0) {
                const uint64_t index = link & MASK_INDEX;
                const uint64_t next = atomic_load_explicit(
                    dbs->next + index, memory_order_relaxed);
                if ((next & MASK_OWNER) == OWNER_RAW_DECISION &&
                    nodes_is_marked(dbs, index)) {
                    rehash_node(
                        dbs, route, entry->new_heads, new_mask,
                        index, parallel);
                }
                link = next;
            }
        }
    }
}

void
llchst_rehash_nodes_CALL(
    lace_worker* lace, nodes_table* dbs,
    size_t first, size_t count, int parallel)
{
    if (count > LLCHST_REBUILD_GRAIN && parallel) {
        const size_t split = count / 2u;
        llchst_rehash_nodes_SPAWN(
            lace, dbs, first, split, parallel);
        llchst_rehash_nodes_CALL(
            lace, dbs, first + split, count - split, parallel);
        llchst_rehash_nodes_SYNC(lace);
        return;
    }

    const _Atomic(uint64_t)* marked = dbs->bitmap2 + first / 64u;
    uint64_t mask =
        UINT64_C(0x8000000000000000) >> (first & 63u);
    for (size_t offset = 0; offset < count; offset++) {
        const size_t index = first + offset;
        if (index > 1 &&
            (atomic_load_explicit(marked, memory_order_relaxed) & mask)) {
            const uint64_t tagged_next = atomic_load_explicit(
                dbs->next + index, memory_order_relaxed);
            if ((tagged_next & MASK_OWNER) == OWNER_RAW_DECISION) {
                goto next_index;
            }
            const uint64_t* data =
                ((const uint64_t*)dbs->data) + 2 * index;
            const uint32_t route = owner_route(tagged_next, data);
            llchst_subtable* subtable = get_subtable(dbs, route, 0);
            assert(subtable != NULL);
            _Atomic(uint64_t)* heads = subtable_heads(subtable);
            assert(heads != NULL);
            rehash_node(
                dbs, route, heads, head_count(subtable) - 1u,
                index, parallel);
        }
next_index:
        mask >>= 1;
        if (mask == 0) {
            marked++;
            mask = UINT64_C(0x8000000000000000);
        }
    }
}

TASK(void, llchst_free_old_entries,
     struct llchst_rebuild_entry*, entries,
     size_t, first, size_t, count)

void
llchst_free_old_entries_CALL(
    lace_worker* lace, struct llchst_rebuild_entry* entries,
    size_t first, size_t count)
{
    if (count > 8 && lace_worker_count() > 1) {
        const size_t split = count / 2u;
        llchst_free_old_entries_SPAWN(
            lace, entries, first, split);
        llchst_free_old_entries_CALL(
            lace, entries, first + split, count - split);
        llchst_free_old_entries_SYNC(lace);
        return;
    }

    for (size_t i = first; i < first + count; i++) {
        sylvan_free_aligned(
            (void*)entries[i].old_heads,
            entries[i].old_count * sizeof(*entries[i].old_heads));
    }
}

/**
 * Visit every subtable that currently owns a head array.
 * The three storage regions (dense decision levels, sparse decision pages,
 * auxiliary routes) are enumerated in that order, which is the order in which
 * both the rebuild and the maintenance pass expect their entries.
 */
typedef void (*llchst_subtable_visitor)(
    llchst_subtable* subtable, _Atomic(uint64_t)* heads, void* context);

static void
for_each_live_subtable(const nodes_table* dbs,
                       llchst_subtable_visitor visit, void* context)
{
    const uint32_t decision_highwater = atomic_load_explicit(
        &dbs->decision_highwater, memory_order_relaxed);
    for (uint32_t i = 0; i < decision_highwater; i++) {
        llchst_subtable* subtable = dbs->decision + i;
        _Atomic(uint64_t)* heads = subtable_heads(subtable);
        if (heads != NULL) visit(subtable, heads, context);
    }

    const uint32_t highwater = atomic_load_explicit(
        &dbs->page_highwater, memory_order_relaxed);
    for (uint32_t page_index = 0; page_index < highwater; page_index++) {
        llchst_page* page = (llchst_page*)(uintptr_t)atomic_load_explicit(
            dbs->pages + page_index, memory_order_relaxed);
        if (page == NULL) continue;
        for (uint32_t i = 0; i < LLCHST_PAGE_SIZE; i++) {
            llchst_subtable* subtable = page->entries + i;
            _Atomic(uint64_t)* heads = subtable_heads(subtable);
            if (heads != NULL) visit(subtable, heads, context);
        }
    }

    for (uint32_t i = 0; i < LLCHST_AUX_COUNT; i++) {
        llchst_subtable* subtable = (llchst_subtable*)dbs->aux + i;
        _Atomic(uint64_t)* heads = subtable_heads(subtable);
        if (heads != NULL) visit(subtable, heads, context);
    }
}

struct llchst_collect_context {
    struct llchst_rebuild_entry* entries;
    size_t count;
};

static void
collect_rebuild_entry(llchst_subtable* subtable,
                      _Atomic(uint64_t)* heads, void* context)
{
    struct llchst_collect_context* collect =
        (struct llchst_collect_context*)context;
    if (collect->entries != NULL) {
        collect->entries[collect->count] = (struct llchst_rebuild_entry) {
            .subtable = subtable,
            .old_heads = heads,
            .old_count = head_count(subtable)
        };
    }
    collect->count++;
}

static size_t
collect_rebuild_entries(const nodes_table* dbs,
                        struct llchst_rebuild_entry* entries)
{
    struct llchst_collect_context collect = {
        .entries = entries, .count = 0
    };
    for_each_live_subtable(dbs, collect_rebuild_entry, &collect);
    return collect.count;
}

int
nodes_rebuild_CALL(lace_worker* lace, nodes_table* dbs)
{
    const size_t count = collect_rebuild_entries(dbs, NULL);
    if (count == 0) return 0;

    struct llchst_rebuild_entry* entries =
        (struct llchst_rebuild_entry*)sylvan_alloc_aligned(
            count * sizeof(*entries));
    if (entries == NULL) return 1;
    const size_t filled = collect_rebuild_entries(dbs, entries);
    assert(filled == count);
    if (filled != count) {
        sylvan_free_aligned(entries, count * sizeof(*entries));
        return 1;
    }

    llchst_measure_entries_CALL(lace, dbs, entries, 0, count);
    allocate_head_budget(entries, count, dbs->table_size);
    const int bad = llchst_prepare_entries_CALL(
        lace, entries, 0, count);
    if (bad != 0) {
        for (size_t i = 0; i < count; i++) {
            if (entries[i].new_heads != NULL) {
                sylvan_free_aligned(
                    (void*)entries[i].new_heads,
                    ((size_t)1 << entries[i].new_log2) *
                    sizeof(*entries[i].new_heads));
            }
        }
        sylvan_free_aligned(entries, count * sizeof(*entries));
        return bad;
    }

    for (size_t i = 0; i < count; i++) {
        llchst_subtable* subtable = entries[i].subtable;
        /* Every subtable is about to be resized to its measured population,
           so any outstanding maintenance request is satisfied by definition,
           and a subtable that was refused a resize gets to ask again. */
        atomic_store_explicit(
            &subtable->route, subtable_route(subtable),
            memory_order_relaxed);
        if (!entries[i].active) {
            atomic_store_explicit(
                &subtable->head_log2,
                SYLVAN_LLCHST_INITIAL_HEAD_LOG2,
                memory_order_relaxed);
            atomic_store_explicit(
                &subtable->heads, (uintptr_t)0, memory_order_release);
        } else {
            atomic_store_explicit(
                &subtable->head_log2,
                entries[i].new_log2 | LLCHST_HEAD_SIZED |
                    LLCHST_HEAD_MEASURED |
                    (entries[i].has_raw ? LLCHST_HAS_RAW : 0u),
                memory_order_relaxed);
            atomic_store_explicit(
                &subtable->heads, (uintptr_t)entries[i].new_heads,
                memory_order_release);
        }
    }

    const int parallel = lace_worker_count() > 1;
    llchst_rehash_raw_entries_CALL(
        lace, dbs, entries, 0, count, parallel);
    /* Owner tags make the direct marked-node pass independent of the old
       chains. Release those pages before touching the replacement heads so
       GC does not keep both generations resident at once. */
    llchst_free_old_entries_CALL(lace, entries, 0, count);
    llchst_rehash_nodes_CALL(
        lace, dbs, 0, dbs->table_size, parallel);

    for (size_t i = 0; i < count; i++) {
        struct llchst_rebuild_entry* entry = entries + i;
        if (entry->new_heads == NULL) continue;
        const size_t new_count = (size_t)1 << entry->new_log2;
        size_t j = 0;
        while (j < new_count && atomic_load_explicit(
               entry->new_heads + j, memory_order_relaxed) == 0) {
            j++;
        }
        if (j == new_count) {
            atomic_store_explicit(
                &entry->subtable->heads, (uintptr_t)0,
                memory_order_release);
            atomic_store_explicit(
                &entry->subtable->head_log2,
                SYLVAN_LLCHST_INITIAL_HEAD_LOG2,
                memory_order_relaxed);
            sylvan_free_aligned(
                (void*)entry->new_heads,
                new_count * sizeof(*entry->new_heads));
            entry->new_heads = NULL;
        }
    }

    /* Rebuilding replaced every head array, so the seeding accounting starts
       again from what this collection actually handed out. */
    uint32_t live = 0;
    uint64_t heads_total = 0;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].new_heads == NULL) continue;
        live++;
        heads_total += (uint64_t)1 << entries[i].new_log2;
    }
    atomic_store_explicit(
        &dbs->live_subtables, live, memory_order_relaxed);
    atomic_store_explicit(
        &dbs->allocated_heads, heads_total, memory_order_relaxed);
    atomic_store_explicit(
        &sylvan_maintenance_requested, 0, memory_order_relaxed);

    sylvan_free_aligned(entries, count * sizeof(*entries));
    return 0;
}

/**
 * Table maintenance: grow the head arrays of subtables that outgrew them,
 * without collecting anything.
 *
 * A subtable that keeps accumulating nodes between collections used to stay at
 * whatever size it was given, because head_log2 was only ever written by
 * nodes_rebuild. Lookups now notice, for free, when a chain has grown far past
 * the target load factor and flag the subtable; this pass, which Sylvan runs
 * from the same stop-the-world Lace frame that garbage collection uses, gives
 * those subtables a head array that matches their measured population.
 *
 * The pass deliberately does not touch the node bitmap, does not mark and does
 * not clear the operation cache. It only walks the chains of the flagged
 * subtables, which is what makes it cheap enough to trigger on chain length.
 *
 * Memory: a resized subtable receives at most <population> heads, and
 * populations are disjoint, so the head arrays handed out by maintenance sum
 * to at most table_size entries. Together with the arrays of the subtables
 * that were not resized, head memory between two collections is bounded by
 * twice head_budget(table_size); the next collection brings it back under the
 * budget.
 */
struct llchst_resize_entry {
    llchst_subtable* subtable;
    _Atomic(uint64_t)* old_heads;
    size_t old_count;
    size_t population;
    _Atomic(uint64_t)* new_heads;
    uint32_t new_log2;
};

struct llchst_resize_collect {
    struct llchst_resize_entry* entries;
    size_t count;
};

static void
collect_resize_entry(llchst_subtable* subtable,
                     _Atomic(uint64_t)* heads, void* context)
{
    struct llchst_resize_collect* collect =
        (struct llchst_resize_collect*)context;
    const uint32_t route = atomic_load_explicit(
        &subtable->route, memory_order_relaxed);
    if ((route & LLCHST_WANTS_RESIZE) == 0) return;
    if (collect->entries != NULL) {
        collect->entries[collect->count] = (struct llchst_resize_entry) {
            .subtable = subtable,
            .old_heads = heads,
            .old_count = head_count(subtable)
        };
    }
    collect->count++;
}

static size_t
collect_resize_entries(const nodes_table* dbs,
                       struct llchst_resize_entry* entries)
{
    struct llchst_resize_collect collect = {
        .entries = entries, .count = 0
    };
    for_each_live_subtable(dbs, collect_resize_entry, &collect);
    return collect.count;
}

struct llchst_population_context {
    const nodes_table* dbs;
    const _Atomic(uint64_t)* heads;
    _Atomic(size_t) population;
};

TASK(void, llchst_count_population,
     struct llchst_population_context*, context,
     size_t, first, size_t, count)

void
llchst_count_population_CALL(
    lace_worker* lace, struct llchst_population_context* context,
    size_t first, size_t count)
{
    if (count > LLCHST_REBUILD_GRAIN && lace_worker_count() > 1) {
        const size_t split = count / 2u;
        llchst_count_population_SPAWN(lace, context, first, split);
        llchst_count_population_CALL(
            lace, context, first + split, count - split);
        llchst_count_population_SYNC(lace);
        return;
    }

    size_t population = 0;
    for (size_t i = first; i < first + count; i++) {
        uint64_t link = atomic_load_explicit(
            context->heads + i, memory_order_relaxed);
        while ((link & MASK_LINK) != 0) {
            population++;
            link = atomic_load_explicit(
                context->dbs->next + (link & MASK_INDEX),
                memory_order_relaxed);
        }
    }
    atomic_fetch_add_explicit(
        &context->population, population, memory_order_relaxed);
}

TASK(void, llchst_migrate_chains,
     nodes_table*, dbs, struct llchst_resize_entry*, entry,
     uint32_t, route, size_t, first, size_t, count, int, parallel)

void
llchst_migrate_chains_CALL(
    lace_worker* lace, nodes_table* dbs,
    struct llchst_resize_entry* entry, uint32_t route,
    size_t first, size_t count, int parallel)
{
    if (count > LLCHST_REBUILD_GRAIN && parallel) {
        const size_t split = count / 2u;
        llchst_migrate_chains_SPAWN(
            lace, dbs, entry, route, first, split, parallel);
        llchst_migrate_chains_CALL(
            lace, dbs, entry, route, first + split, count - split, parallel);
        llchst_migrate_chains_SYNC(lace);
        return;
    }

    const size_t new_mask = ((size_t)1 << entry->new_log2) - 1u;
    for (size_t i = first; i < first + count; i++) {
        uint64_t link = atomic_load_explicit(
            entry->old_heads + i, memory_order_relaxed);
        while ((link & MASK_LINK) != 0) {
            const uint64_t index = link & MASK_INDEX;
            /* rehash_node overwrites next[index], so read the rest of the old
               chain out of the way first. */
            const uint64_t next = atomic_load_explicit(
                dbs->next + index, memory_order_relaxed);
            rehash_node(
                dbs, route, entry->new_heads, new_mask, index, parallel);
            link = next;
        }
    }
}

/**
 * Round the target head count up to a power of two, so that the load factor
 * after the pass lands in (LOAD_FACTOR/2, LOAD_FACTOR]. Rounding down instead
 * would leave the subtable at up to twice the target load factor, which is
 * close enough to the request threshold to make it ask again immediately.
 */
static uint32_t
resize_target_log2(size_t population)
{
    size_t target =
        (population + SYLVAN_LLCHST_LOAD_FACTOR - 1u) /
        SYLVAN_LLCHST_LOAD_FACTOR;
    if (target == 0) target = 1;
    uint32_t result = floor_log2(target);
    if (((size_t)1 << result) < target) result++;
    if (result >= LLCHST_INDEX_BITS) result = LLCHST_INDEX_BITS - 1u;
    return result;
}

void
nodes_maintain_CALL(lace_worker* lace, nodes_table* dbs)
{
    atomic_store_explicit(
        &sylvan_maintenance_requested, 0, memory_order_relaxed);

    const size_t count = collect_resize_entries(dbs, NULL);
    if (count == 0) return;

    struct llchst_resize_entry* entries =
        (struct llchst_resize_entry*)sylvan_alloc_aligned(
            count * sizeof(*entries));
    if (entries == NULL) return;
    const size_t filled = collect_resize_entries(dbs, entries);
    assert(filled == count);
    if (filled != count) {
        sylvan_free_aligned(entries, count * sizeof(*entries));
        return;
    }

    const int parallel = lace_worker_count() > 1;
    for (size_t i = 0; i < count; i++) {
        struct llchst_resize_entry* entry = entries + i;
        llchst_subtable* subtable = entry->subtable;
        const uint32_t route = subtable_route(subtable);

        struct llchst_population_context context = {
            .dbs = dbs, .heads = entry->old_heads
        };
        atomic_init(&context.population, 0);
        llchst_count_population_CALL(
            lace, &context, 0, entry->old_count);
        entry->population = atomic_load_explicit(
            &context.population, memory_order_relaxed);
        entry->new_log2 = resize_target_log2(entry->population);

        if (((size_t)1 << entry->new_log2) <= entry->old_count) {
            /* Not actually overloaded. Either the request was a statistical
               accident, or the keys in this subtable collide beyond what a
               bigger array can separate; in the latter case, refuse further
               requests until the next collection rather than walk these
               chains again on every lookup. */
            uint32_t state = subtable_route(subtable);
            if (entry->population <= entry->old_count) {
                state |= LLCHST_RESIZE_DENIED;
                sylvan_stats_count(LLCHST_RESIZE_REFUSED);
            }
            atomic_store_explicit(
                &subtable->route, state, memory_order_relaxed);
            continue;
        }

        const size_t new_count = (size_t)1 << entry->new_log2;
        entry->new_heads = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
            new_count * sizeof(*entry->new_heads));
        if (entry->new_heads == NULL) {
            /* Out of memory for the bigger array; the subtable keeps working,
               just more slowly. Clear the request rather than leave it set,
               because request_resize will not raise the global flag again
               while this subtable already has one outstanding. */
            atomic_store_explicit(
                &subtable->route, route, memory_order_relaxed);
            continue;
        }

        llchst_migrate_chains_CALL(
            lace, dbs, entry, route, 0, entry->old_count, parallel);

        /* Publish the size before the pointer: readers acquire <heads> and
           then take a relaxed load of head_log2, so the array they find must
           already match the mask they will build. Nothing observes either
           until this frame ends, but keep the order that lookups require. */
        const uint32_t old_state = atomic_load_explicit(
            &subtable->head_log2, memory_order_relaxed);
        atomic_store_explicit(
            &subtable->head_log2,
            entry->new_log2 | LLCHST_HEAD_SIZED | LLCHST_HEAD_MEASURED |
                (old_state & LLCHST_HAS_RAW),
            memory_order_relaxed);
        atomic_store_explicit(
            &subtable->heads, (uintptr_t)entry->new_heads,
            memory_order_release);
        atomic_store_explicit(
            &subtable->route, route, memory_order_relaxed);

        sylvan_free_aligned(
            (void*)entry->old_heads,
            entry->old_count * sizeof(*entry->old_heads));
        atomic_fetch_add_explicit(
            &dbs->allocated_heads,
            (uint64_t)new_count - (uint64_t)entry->old_count,
            memory_order_relaxed);
        sylvan_stats_count(LLCHST_SUBTABLE_RESIZED);
    }

    sylvan_free_aligned(entries, count * sizeof(*entries));
}

TASK(size_t, nodes_count_nodes_par,
     nodes_table*, dbs, size_t, first, size_t, count)

size_t
nodes_count_nodes_par_CALL(lace_worker* lace, nodes_table* dbs,
                           size_t first, size_t count)
{
    if (count > 512) {
        const size_t split = count / 2;
        nodes_count_nodes_par_SPAWN(lace, dbs, first, split);
        const size_t right = nodes_count_nodes_par_CALL(
            lace, dbs, first + split, count - split);
        return right + nodes_count_nodes_par_SYNC(lace);
    }

    size_t result = 0;
    const _Atomic(uint64_t)* ptr = dbs->bitmap2 + first / 64u;
    if (count == 512 && (first & 63u) == 0) {
        for (size_t i = 0; i < 8; i++) {
            result += popcnt_uint64(atomic_load_explicit(
                ptr + i, memory_order_relaxed));
        }
    } else {
        uint64_t mask =
            UINT64_C(0x8000000000000000) >> (first & 63u);
        for (size_t i = 0; i < count; i++) {
            if (atomic_load_explicit(ptr, memory_order_relaxed) & mask) {
                result++;
            }
            mask >>= 1;
            if (mask == 0) {
                ptr++;
                mask = UINT64_C(0x8000000000000000);
            }
        }
    }
    return result;
}

size_t
nodes_count_nodes_CALL(lace_worker* lace, nodes_table* dbs)
{
    return nodes_count_nodes_par_CALL(lace, dbs, 0, dbs->table_size);
}

TASK(void, nodes_destroy_par,
     nodes_table*, dbs, size_t, first_word, size_t, word_count)

void
nodes_destroy_par_CALL(lace_worker* lace, nodes_table* dbs,
                       size_t first_word, size_t word_count)
{
    if (word_count > 16) {
        const size_t split = word_count / 2;
        nodes_destroy_par_SPAWN(lace, dbs, first_word, split);
        nodes_destroy_par_CALL(
            lace, dbs, first_word + split, word_count - split);
        nodes_destroy_par_SYNC(lace);
        return;
    }

    for (size_t word = first_word; word < first_word + word_count; word++) {
        const uint64_t marked = atomic_load_explicit(
            dbs->bitmap2 + word, memory_order_relaxed);
        const uint64_t custom = atomic_load_explicit(
            dbs->bitmapc + word, memory_order_relaxed);
        uint64_t dead = custom & ~marked;
        while (dead != 0) {
            const unsigned int bit = clz_uint64(dead);
            const size_t index = word * 64u + bit;
            const uint64_t mask =
                UINT64_C(0x8000000000000000) >> bit;
            if (index < dbs->table_size && dbs->destroy_cb != NULL) {
                const uint64_t* data =
                    ((const uint64_t*)dbs->data) + 2 * index;
                dbs->destroy_cb(data[0], data[1]);
            }
            dead &= ~mask;
        }
        atomic_store_explicit(
            dbs->bitmapc + word, custom & marked, memory_order_relaxed);
    }
}

void
nodes_cleanup_custom_CALL(lace_worker* lace, nodes_table* dbs)
{
    nodes_destroy_par_CALL(
        lace, dbs, 0, (dbs->table_size + 63u) / 64u);
}

void
nodes_set_custom(nodes_table* dbs,
                 nodes_hash_cb hash_cb, nodes_equals_cb equals_cb,
                 nodes_create_cb create_cb, nodes_destroy_cb destroy_cb)
{
    dbs->hash_cb = hash_cb;
    dbs->equals_cb = equals_cb;
    dbs->create_cb = create_cb;
    dbs->destroy_cb = destroy_cb;
}

/**
 * Reorder primitives.
 *
 * The chain surgery is the same as in nodes_llcht: nodes_remove splices the
 * node out of its chain with compare-and-swap behind a delete-in-progress
 * link, nodes_reinsert prepends it again and publishes its own link before
 * the head. See the comment on nodes_remove in nodes_llcht.c for why the
 * delete-in-progress link is needed, why the walk cannot step past its own
 * node or splice the wrong link, and why the retries terminate.
 *
 * What is specific here is the routing. A node's subtable is not derived from
 * its key but from the owner class in its own next word: owner 0 means a
 * canonically routed decision node, whose route is the variable identity in
 * the top 24 bits of b, and the auxiliary owners name their fixed routes. The
 * delete-in-progress link keeps those three owner bits, so both the route and
 * the intra-subtable hash are still recoverable while the node is unlinked,
 * and a caller that rewrites the identity field between remove and reinsert
 * moves the node from one decision subtable to another for free.
 *
 * Explicitly routed decision entries (OWNER_RAW_DECISION, a node placed in a
 * subtable that does not match its own identity field) are the one case the
 * record cannot answer for: their route is not stored anywhere. They are a
 * compatibility path for direct nodes_lookup_subtable users, never produced
 * by the reorder session, and nodes_remove refuses them with
 * SYLVAN_ERR_INVALID rather than guess.
 */

/* A link part of all ones in the fingerprint and zero in the index, with the
   node's three owner bits preserved. No published link can look like this: a
   link names an index >= 2, and a chain end has a zero link part. */
#define LINK_REMOVING_BITS MASK_HASH

static inline int
link_is_removing(uint64_t link)
{
    return (link & MASK_LINK) == LINK_REMOVING_BITS;
}

uint32_t
nodes_reorder_caps(const nodes_table* dbs)
{
    (void)dbs;
    return NODES_CAP_REMOVE | NODES_CAP_RELEASE | NODES_CAP_ENUMERATE;
}

static void
reorder_duplicate_abort(uint64_t index, uint64_t other)
{
    assert(!"nodes_reinsert: an equal node is already in the index");
    fprintf(stderr,
            "sylvan: nodes_reinsert: node %llu is equal to node %llu, which "
            "is already in the index; the unique table invariant is broken. "
            "Aborting.\n",
            (unsigned long long)index, (unsigned long long)other);
    abort();
}

/**
 * Resolve the subtable a node currently belongs to, from its owner class and
 * its record. Returns NULL for an explicitly routed decision entry and for a
 * subtable that does not exist.
 */
static llchst_subtable*
reorder_subtable(const nodes_table* dbs, uint64_t tagged_next,
                 const uint64_t* data, uint32_t* route_out)
{
    const uint32_t route = owner_route(tagged_next, data);
    *route_out = route;
    if ((tagged_next & MASK_OWNER) == OWNER_RAW_DECISION) return NULL;
    return get_subtable(dbs, route, 0);
}

int
nodes_remove(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
    uint64_t tagged = atomic_load_explicit(
        dbs->next + index, memory_order_acquire);
    if (link_is_removing(tagged)) return SYLVAN_ERR_INVALID;

    uint32_t route = 0;
    llchst_subtable* subtable = reorder_subtable(dbs, tagged, data, &route);
    if (subtable == NULL) return SYLVAN_ERR_INVALID;
    _Atomic(uint64_t)* heads = subtable_heads(subtable);
    if (heads == NULL) return SYLVAN_ERR_INVALID;

    const uint64_t hash = hash_key(
        dbs, route, data[0], data[1], is_custom_bucket(dbs, index));
    _Atomic(uint64_t)* head = heads + (hash & (head_count(subtable) - 1u));

    /* Claim the node and take its successor with it. The compare-and-swap
       retries because next[index] is not private: when the node behind this
       one is removed, its remover splices by writing this very link. */
    uint64_t successor;
    for (;;) {
        successor = atomic_load_explicit(
            dbs->next + index, memory_order_acquire);
        if (link_is_removing(successor)) return SYLVAN_ERR_INVALID;
        const uint64_t claimed =
            (successor & MASK_OWNER) | LINK_REMOVING_BITS;
        if (atomic_compare_exchange_weak_explicit(
                dbs->next + index, &successor, claimed,
                memory_order_acq_rel, memory_order_acquire)) {
            break;
        }
    }
    const uint64_t spliced = successor & MASK_LINK;

    for (;;) {
        uint64_t link = atomic_load_explicit(head, memory_order_acquire);
        if ((link & MASK_INDEX) == index) {
            if (atomic_compare_exchange_strong_explicit(
                    head, &link, spliced,
                    memory_order_acq_rel, memory_order_acquire)) {
                return SYLVAN_OK;
            }
            continue;
        }

        for (;;) {
            const uint64_t previous = link & MASK_INDEX;
            if (previous == 0) {
                assert(!"nodes_remove: node is not in its own chain");
                return SYLVAN_ERR_INVALID;
            }

            uint64_t value = atomic_load_explicit(
                dbs->next + previous, memory_order_acquire);
            if (link_is_removing(value)) break;
            if ((value & MASK_INDEX) == index) {
                const uint64_t replacement =
                    (value & MASK_OWNER) | spliced;
                if (atomic_compare_exchange_strong_explicit(
                        dbs->next + previous, &value, replacement,
                        memory_order_acq_rel, memory_order_acquire)) {
                    return SYLVAN_OK;
                }
                break;
            }
            link = value;
        }
    }
}

/**
 * Walk a chain looking for a node equal to the given key.
 * Returns the index of an equal node, 0 if there is none, or UINT64_MAX if
 * the walk met a removal in progress and has to be repeated.
 */
static uint64_t
reorder_find_equal(const nodes_table* dbs, uint64_t link,
                   uint64_t masked_hash, uint64_t a, uint64_t b, int custom)
{
    while ((link & MASK_INDEX) != 0) {
        const uint64_t index = link & MASK_INDEX;
        if ((link & MASK_HASH) == masked_hash) {
            const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
            if (custom) {
                if (dbs->equals_cb(a, b, data[0], data[1])) return index;
            } else if (data[0] == a && data[1] == b) {
                return index;
            }
        }
        link = atomic_load_explicit(dbs->next + index, memory_order_acquire);
        if (link_is_removing(link)) return UINT64_MAX;
    }
    return 0;
}

int
nodes_reinsert(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    const uint64_t* record = ((const uint64_t*)dbs->data) + 2 * index;
    const uint64_t tagged = atomic_load_explicit(
        dbs->next + index, memory_order_relaxed);
    if (!link_is_removing(tagged)) {
        assert(!"nodes_reinsert: the node was not removed first");
        return SYLVAN_ERR_INVALID;
    }
    if ((tagged & MASK_OWNER) == OWNER_RAW_DECISION) {
        return SYLVAN_ERR_INVALID;
    }

    const uint64_t a = record[0];
    const uint64_t b = record[1];
    /* The route follows the rewritten record: a decision node whose identity
       field changed belongs to another subtable now. */
    const uint32_t route = owner_route(tagged, record);
    llchst_subtable* subtable = get_subtable(dbs, route, 1);
    if (subtable == NULL) return SYLVAN_ERR_OOM;
    _Atomic(uint64_t)* heads = ensure_heads(dbs, subtable);
    if (heads == NULL) return SYLVAN_ERR_OOM;

    const int custom = is_custom_bucket(dbs, index);
    const uint64_t hash = hash_key(dbs, route, a, b, custom);
    const uint64_t masked_hash = hash & MASK_HASH;
    _Atomic(uint64_t)* head = heads + (hash & (head_count(subtable) - 1u));
    const uint64_t link = masked_hash | index;
    const uint64_t owner = owner_tag(route, b);
    assert(owner != OWNER_RAW_DECISION);

    uint64_t observed = atomic_load_explicit(head, memory_order_acquire);
    for (;;) {
        const uint64_t equal = reorder_find_equal(
            dbs, observed, masked_hash, a, b, custom);
        if (equal == UINT64_MAX) {
            observed = atomic_load_explicit(head, memory_order_acquire);
            continue;
        }
        if (equal != 0) reorder_duplicate_abort(index, equal);

        /* Publish this node's own link before the node itself: the release
           compare-and-swap on the head orders the store in front of it. */
        atomic_store_explicit(
            dbs->next + index, owner | (observed & MASK_LINK),
            memory_order_relaxed);
        uint64_t expected = observed;
        if (atomic_compare_exchange_strong_explicit(
                head, &expected, link,
                memory_order_acq_rel, memory_order_acquire)) {
            return SYLVAN_OK;
        }
        observed = expected;
    }
}

/**
 * Return a removed slot to the allocator.
 *
 * The bitmap updates are read-modify-writes and not the plain stores the
 * allocation path uses: a worker that allocates owns the whole region its
 * slot lies in, but a worker that releases a retired node does not, so two
 * releases can land in the same 64-slot word.
 *
 * The custom bit has to go before the slot becomes claimable. A reused slot
 * that still carries it would be hashed and compared through the custom
 * callbacks, and the next nodes_cleanup_custom would destroy its payload a
 * second time.
 */
int
nodes_release(nodes_table* dbs, uint64_t index)
{
    if (index < 2 || index >= dbs->table_size) return SYLVAN_ERR_INVALID;

    /* The delete-in-progress link that nodes_remove leaves behind is exactly
       the evidence that this node is out of the index. */
    if (!link_is_removing(atomic_load_explicit(
            dbs->next + index, memory_order_acquire))) {
        return SYLVAN_ERR_INVALID;
    }

    /* Leave the slot looking unused, owner class and all. An insertion here
       writes the tagged link before it publishes the node, so this backend
       does not depend on it, but a released slot that still says "removal in
       progress" is a trap for anything that reads the link array without
       that knowledge. */
    atomic_store_explicit(dbs->next + index, 0, memory_order_relaxed);

    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63u);
    if (is_custom_bucket(dbs, index)) {
        if (dbs->destroy_cb != NULL) {
            const uint64_t* data = ((const uint64_t*)dbs->data) + 2 * index;
            dbs->destroy_cb(data[0], data[1]);
        }
        atomic_fetch_and_explicit(
            dbs->bitmapc + index / 64u, ~mask, memory_order_relaxed);
    }
    atomic_fetch_and_explicit(
        dbs->bitmap2 + index / 64u, ~mask, memory_order_release);
    return SYLVAN_OK;
}

/**
 * Visit every node in one variable's decision subtable, by walking its head
 * array and the chain hanging off each head.
 *
 * This covers exactly the nodes that lookups route to that variable, which
 * includes the explicitly routed entries whose own identity field says
 * something else: they are in these chains, so a caller enumerating "the
 * nodes of variable v" sees them.
 *
 * MAP NODES ARE NOT COVERED, by design. An MTBDDMAP node carries its key in
 * the variable field like a decision node does, but it is routed to the
 * shared MAP auxiliary subtable, not to a per-identity one, so there is no
 * head array to walk for "the map nodes of variable v". The session layer
 * builds its per-identity lists including map nodes from an arena scan for
 * every backend, and that is where map nodes belong; see the contract in
 * <sylvan/nodes.h>.
 */
int
nodes_enumerate_var(nodes_table* dbs, uint32_t variable,
                    nodes_visit_cb cb, void* context)
{
    if (cb == NULL || variable >= NODES_SUBTABLE_LEAF) {
        return SYLVAN_ERR_INVALID;
    }

    llchst_subtable* subtable = get_subtable(dbs, variable, 0);
    if (subtable == NULL) return SYLVAN_OK;
    _Atomic(uint64_t)* heads = subtable_heads(subtable);
    if (heads == NULL) return SYLVAN_OK;

    const size_t count = head_count(subtable);
    for (size_t i = 0; i < count; i++) {
        uint64_t link = atomic_load_explicit(
            heads + i, memory_order_acquire);
        while ((link & MASK_LINK) != 0) {
            const uint64_t index = link & MASK_INDEX;
            assert(!link_is_removing(link));
            const int status = cb(index, context);
            if (status != 0) return status;
            link = atomic_load_explicit(
                dbs->next + index, memory_order_acquire);
        }
    }
    return SYLVAN_OK;
}

/**
 * Preflight <count> further allocations. See <sylvan/nodes.h> for the exact
 * invariant; the short version is that this counts the free slots of the
 * regions no worker owns yet, because a worker exhausts the region it owns
 * before claiming another one and can only claim an unowned region. Free
 * slots inside owned regions are therefore reachable by exactly one worker
 * and are not counted, which is what makes the answer conservative rather
 * than optimistic and removes the need for a per-worker margin.
 *
 * The scan stops as soon as <count> is reached, so a small reservation on a
 * table with room costs a handful of popcounts.
 */
static uint64_t
reserve_free_slots(const nodes_table* dbs, uint64_t needed)
{
    const uint64_t regions = dbs->table_size / 512u;
    uint64_t free_slots = 0;

    for (uint64_t region = 0; region < regions; region++) {
        const uint64_t owned = atomic_load_explicit(
            dbs->bitmap1 + region / 64u, memory_order_relaxed);
        const uint64_t owner_mask =
            UINT64_C(0x8000000000000000) >> (region & 63u);
        if (owned & owner_mask) continue;

        const _Atomic(uint64_t)* words = dbs->bitmap2 + region * 8u;
        for (unsigned int i = 0; i < 8; i++) {
            free_slots += 64u - popcnt_uint64(atomic_load_explicit(
                words + i, memory_order_relaxed));
        }
        if (free_slots >= needed) return free_slots;
    }
    return free_slots;
}

int
nodes_reserve(nodes_table* dbs, uint64_t count)
{
    if (count == 0) return SYLVAN_OK;
    if (nodes_reorder_caps(dbs) == 0) return SYLVAN_ERR_INVALID;

    /* Index capacity as well as node slots. A subtable growing to absorb
       <count> more nodes at the target load factor must stay inside the
       bound the maintenance pass already documents for head memory between
       two collections, which is twice the head budget. This does not make a
       first head array or a sparse descriptor page unfailable -- those are
       heap allocations whose target is unknown until the node is inserted --
       and <sylvan/nodes.h> says so. */
    const uint64_t needed_heads =
        (count + SYLVAN_LLCHST_LOAD_FACTOR - 1u) / SYLVAN_LLCHST_LOAD_FACTOR;
    const uint64_t used = atomic_load_explicit(
        &dbs->allocated_heads, memory_order_relaxed);
    const uint64_t budget = (uint64_t)head_budget(dbs->table_size);
    if (used + needed_heads > 2u * budget) return SYLVAN_ERR_OOM;

    return reserve_free_slots(dbs, count) >= count
        ? SYLVAN_OK : SYLVAN_ERR_OOM;
}
