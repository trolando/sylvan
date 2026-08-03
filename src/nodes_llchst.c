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
 * has one immutable 24-bit-fingerprint/40-bit-next link. Chains only grow by
 * prepending between stop-the-world collections.
 *
 * Descriptor membership is the owner enumeration: GC retains the old head
 * arrays long enough to count and rebuild marked nodes into replacement head
 * arrays. No identity bits or per-node owner array are needed.
 *
 * WARNING: Like the other nodes tables, allocation currently uses a global
 * thread-local region. Do not use multiple nodes tables concurrently.
 */

#define MASK_INDEX UINT64_C(0x000000ffffffffff)
#define MASK_HASH  UINT64_C(0xffffff0000000000)

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
static_assert(SYLVAN_LLCHST_DENSE_LEVELS <= NODES_SUBTABLE_LEAF,
              "SYLVAN_LLCHST_DENSE_LEVELS exceeds the level space");
static_assert(SYLVAN_LLCHST_HEAD_SLACK_LOG2 < 16,
              "SYLVAN_LLCHST_HEAD_SLACK_LOG2 is unreasonable");

typedef struct llchst_subtable {
    _Atomic(uintptr_t) heads;
    uint32_t head_log2;
    uint32_t route;
} llchst_subtable;

static_assert(sizeof(llchst_subtable) == 16,
              "LLCHST hot descriptor must remain 16 bytes");

typedef struct llchst_page {
    llchst_subtable entries[LLCHST_PAGE_SIZE];
} llchst_page;

typedef struct nodes_table {
    uint64_t* next;
    uint8_t* data;
    _Atomic(uint64_t)* bitmap1;
    _Atomic(uint64_t)* bitmap2;
    uint64_t* bitmapc;
    size_t max_size;
    size_t table_size;
    llchst_subtable* decision;
    _Atomic(uint32_t) decision_highwater;
    _Atomic(uintptr_t)* pages;
    _Atomic(uint32_t) page_highwater;
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

static inline size_t
head_count(const llchst_subtable* subtable)
{
    return (size_t)1 << subtable->head_log2;
}

static inline _Atomic(uint64_t)*
subtable_heads(const llchst_subtable* subtable)
{
    return (_Atomic(uint64_t)*)(uintptr_t)atomic_load_explicit(
        &subtable->heads, memory_order_acquire);
}

static void
initialize_descriptor(llchst_subtable* subtable, uint32_t route)
{
    atomic_init(&subtable->heads, (uintptr_t)0);
    subtable->head_log2 = SYLVAN_LLCHST_INITIAL_HEAD_LOG2;
    subtable->route = route;
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
            memory_order_release, memory_order_acquire)) {
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

static _Atomic(uint64_t)*
ensure_heads(nodes_table* dbs, llchst_subtable* subtable)
{
    _Atomic(uint64_t)* heads = subtable_heads(subtable);
    if (heads != NULL) return heads;

    const size_t bytes = head_count(subtable) * sizeof(*heads);
    _Atomic(uint64_t)* candidate =
        (_Atomic(uint64_t)*)sylvan_alloc_aligned(bytes);
    if (candidate == NULL) return NULL;

    uintptr_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &subtable->heads, &expected, (uintptr_t)candidate,
            memory_order_release, memory_order_acquire)) {
        sylvan_free_aligned((void*)candidate, bytes);
        heads = (_Atomic(uint64_t)*)expected;
    } else {
        if (subtable->route < SYLVAN_LLCHST_DENSE_LEVELS) {
            raise_decision_highwater(dbs, subtable->route + 1u);
        }
        sylvan_stats_count(LLCHST_SUBTABLE_CREATED);
        heads = candidate;
    }

    return heads;
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

static void
set_custom_bucket(const nodes_table* dbs, uint64_t index, int on)
{
    uint64_t* ptr = dbs->bitmapc + index / 64u;
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63u);
    if (on) *ptr |= mask;
    else *ptr &= ~mask;
}

static int
is_custom_bucket(const nodes_table* dbs, uint64_t index)
{
    const uint64_t* ptr = dbs->bitmapc + index / 64u;
    const uint64_t mask =
        UINT64_C(0x8000000000000000) >> (index & 63u);
    return (*ptr & mask) ? 1 : 0;
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
find_in_chain(const nodes_table* dbs, uint64_t link, uint64_t stop,
              uint64_t masked_hash, uint64_t a, uint64_t b, int custom)
{
    while (link != stop) {
        assert(stop == 0 || link != 0);
        const uint64_t index = link & MASK_INDEX;
        if ((link & MASK_HASH) == masked_hash) {
            const uint64_t* data =
                ((const uint64_t*)dbs->data) + 2 * index;
            if (custom) {
                if (dbs->equals_cb(a, b, data[0], data[1])) return index;
            } else if (data[0] == a && data[1] == b) {
                return index;
            }
        }

        sylvan_stats_count(LLMSSET_LOOKUP);
        link = dbs->next[index];
    }
    return 0;
}

static inline uint64_t
nodes_lookup2(const nodes_table* dbs, uint32_t route,
              uint64_t a, uint64_t b, int* created, const int custom)
{
    llchst_subtable* subtable = get_subtable(dbs, route, 1);
    if (subtable == NULL) return 0;
    _Atomic(uint64_t)* heads = ensure_heads((nodes_table*)dbs, subtable);
    if (heads == NULL) return 0;

    const uint64_t hash = hash_key(dbs, route, a, b, custom);
    const uint64_t masked_hash = hash & MASK_HASH;
    _Atomic(uint64_t)* head = heads + (hash & (head_count(subtable) - 1u));

    uint64_t observed = atomic_load_explicit(head, memory_order_acquire);
    uint64_t found = find_in_chain(
        dbs, observed, 0, masked_hash, a, b, custom);
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

    for (;;) {
        dbs->next[candidate] = observed;
        uint64_t expected = observed;
        if (atomic_compare_exchange_strong_explicit(
                head, &expected, candidate_link,
                memory_order_acq_rel, memory_order_acquire)) {
            if (custom) set_custom_bucket(dbs, candidate, 1);
            *created = 1;
            return candidate;
        }

        sylvan_stats_count(LLCHST_HEAD_CAS_FAILURE);

        found = find_in_chain(
            dbs, expected, observed, masked_hash, a, b, custom);
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
    dbs->next = (uint64_t*)sylvan_alloc_aligned(
        max_size * sizeof(*dbs->next));
    dbs->data = (uint8_t*)sylvan_alloc_aligned(
        max_size * 2 * sizeof(uint64_t));
    dbs->bitmap1 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        region_bitmap_size(max_size));
    dbs->bitmap2 = (_Atomic(uint64_t)*)sylvan_alloc_aligned(
        node_bitmap_size(max_size));
    dbs->bitmapc = (uint64_t*)sylvan_alloc_aligned(
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
        dbs->next, dbs->max_size * sizeof(*dbs->next));
    sylvan_free_aligned(
        dbs->data, dbs->max_size * 2 * sizeof(uint64_t));
    sylvan_free_aligned(
        (void*)dbs->bitmap1, region_bitmap_size(dbs->max_size));
    sylvan_free_aligned(
        (void*)dbs->bitmap2, node_bitmap_size(dbs->max_size));
    sylvan_free_aligned(
        dbs->bitmapc, node_bitmap_size(dbs->max_size));
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
    size_t live;
    size_t population;
    size_t target;
    uint32_t new_log2;
};

struct llchst_measure_context {
    const nodes_table* dbs;
    const _Atomic(uint64_t)* heads;
    _Atomic(size_t) live;
    _Atomic(size_t) population;
};

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

    size_t live = 0;
    size_t population = 0;
    for (size_t i = first; i < first + count; i++) {
        uint64_t link = atomic_load_explicit(
            context->heads + i, memory_order_relaxed);
        while (link != 0) {
            const uint64_t index = link & MASK_INDEX;
            live += (size_t)nodes_is_marked(context->dbs, index);
            population++;
            link = context->dbs->next[index];
        }
    }
    atomic_fetch_add_explicit(
        &context->live, live, memory_order_relaxed);
    atomic_fetch_add_explicit(
        &context->population, population, memory_order_relaxed);
}

static void
measure_entry(lace_worker* lace, const nodes_table* dbs,
              struct llchst_rebuild_entry* entry)
{
    struct llchst_measure_context context = {
        .dbs = dbs,
        .heads = entry->old_heads
    };
    atomic_init(&context.live, 0);
    atomic_init(&context.population, 0);
    llchst_measure_chains_CALL(
        lace, &context, 0, entry->old_count);
    entry->live = atomic_load_explicit(
        &context.live, memory_order_relaxed);
    entry->population = atomic_load_explicit(
        &context.population, memory_order_relaxed);
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

static int
compare_promotion_priority(const void* left, const void* right)
{
    const struct llchst_rebuild_entry* a =
        (const struct llchst_rebuild_entry*)left;
    const struct llchst_rebuild_entry* b =
        (const struct llchst_rebuild_entry*)right;
    const size_t a_count = (size_t)1 << a->new_log2;
    const size_t b_count = (size_t)1 << b->new_log2;
    const long double a_priority = a->live == 0 ? 0.0L :
        (long double)a->target / (long double)a_count;
    const long double b_priority = b->live == 0 ? 0.0L :
        (long double)b->target / (long double)b_count;
    if (a_priority < b_priority) return 1;
    if (a_priority > b_priority) return -1;
    return 0;
}

static void
allocate_head_budget(struct llchst_rebuild_entry* entries,
                     size_t count, size_t table_size)
{
    size_t population = 0;
    size_t active = 0;
    for (size_t i = 0; i < count; i++) {
        population += entries[i].population;
        active += entries[i].live != 0;
    }
    if (population == 0) return;

    size_t budget = head_budget(table_size);
    if (budget < active) budget = active;
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        struct llchst_rebuild_entry* entry = entries + i;
        if (entry->live == 0) {
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
        if (entry->live == 0) continue;
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
        dbs->next[index] = observed;
        atomic_store_explicit(head, link, memory_order_relaxed);
    } else {
        do {
            dbs->next[index] = observed;
        } while (!atomic_compare_exchange_weak_explicit(
            head, &observed, link,
            memory_order_release, memory_order_relaxed));
    }
    sylvan_stats_count(LLCHST_NODE_REHASHED);
}

struct llchst_rehash_context {
    nodes_table* dbs;
    uint32_t route;
    const _Atomic(uint64_t)* old_heads;
    _Atomic(uint64_t)* new_heads;
    size_t new_mask;
    int parallel;
};

TASK(void, llchst_rehash_marked,
     struct llchst_rehash_context*, context,
     size_t, first, size_t, count)

void
llchst_rehash_marked_CALL(
    lace_worker* lace, struct llchst_rehash_context* context,
    size_t first, size_t count)
{
    if (count > LLCHST_REBUILD_GRAIN && context->parallel) {
        const size_t split = count / 2;
        llchst_rehash_marked_SPAWN(
            lace, context, first, split);
        llchst_rehash_marked_CALL(
            lace, context, first + split, count - split);
        llchst_rehash_marked_SYNC(lace);
        return;
    }

    for (size_t i = first; i < first + count; i++) {
        uint64_t link = atomic_load_explicit(
            context->old_heads + i, memory_order_relaxed);
        while (link != 0) {
            const uint64_t index = link & MASK_INDEX;
            const uint64_t next = context->dbs->next[index];
            if (nodes_is_marked(context->dbs, index)) {
                rehash_node(
                    context->dbs, context->route,
                    context->new_heads, context->new_mask, index,
                    context->parallel);
            }
            link = next;
        }
    }
}

static int
rebuild_entry(lace_worker* lace, nodes_table* dbs,
              struct llchst_rebuild_entry* entry)
{
    llchst_subtable* subtable = entry->subtable;
    if (entry->live == 0) {
        subtable->head_log2 = SYLVAN_LLCHST_INITIAL_HEAD_LOG2;
        atomic_store_explicit(
            &subtable->heads, (uintptr_t)0, memory_order_release);
        sylvan_free_aligned(
            (void*)entry->old_heads,
            entry->old_count * sizeof(*entry->old_heads));
        return 0;
    }

    sylvan_stats_count(LLCHST_SUBTABLE_REBUILT);

    const uint32_t new_log2 = entry->new_log2;
    const size_t new_count = (size_t)1 << new_log2;
    _Atomic(uint64_t)* new_heads =
        (_Atomic(uint64_t)*)sylvan_alloc_aligned(
            new_count * sizeof(*new_heads));
    if (new_heads == NULL) return 1;

    struct llchst_rehash_context context = {
        .dbs = dbs,
        .route = subtable->route,
        .old_heads = entry->old_heads,
        .new_heads = new_heads,
        .new_mask = new_count - 1u,
        .parallel =
            lace_worker_count() > 1 &&
            entry->old_count > LLCHST_REBUILD_GRAIN
    };
    llchst_rehash_marked_CALL(
        lace, &context, 0, entry->old_count);

    subtable->head_log2 = new_log2;
    atomic_store_explicit(
        &subtable->heads, (uintptr_t)new_heads, memory_order_release);
    sylvan_free_aligned(
        (void*)entry->old_heads,
        entry->old_count * sizeof(*entry->old_heads));
    return 0;
}

TASK(int, llchst_rebuild_entries,
     nodes_table*, dbs, struct llchst_rebuild_entry*, entries,
     size_t, first, size_t, count)

int
llchst_rebuild_entries_CALL(
    lace_worker* lace, nodes_table* dbs,
    struct llchst_rebuild_entry* entries,
    size_t first, size_t count)
{
    if (count > 1 && lace_worker_count() > 1) {
        const size_t split = count / 2u;
        llchst_rebuild_entries_SPAWN(
            lace, dbs, entries, first, split);
        const int right = llchst_rebuild_entries_CALL(
            lace, dbs, entries, first + split, count - split);
        return right + llchst_rebuild_entries_SYNC(lace);
    }

    int bad = 0;
    for (size_t i = first; i < first + count; i++) {
        bad += rebuild_entry(lace, dbs, entries + i);
    }
    return bad;
}

static size_t
collect_rebuild_entries(const nodes_table* dbs,
                        struct llchst_rebuild_entry* entries)
{
    size_t count = 0;
    const uint32_t decision_highwater = atomic_load_explicit(
        &dbs->decision_highwater, memory_order_relaxed);
    for (uint32_t i = 0; i < decision_highwater; i++) {
        llchst_subtable* subtable = dbs->decision + i;
        _Atomic(uint64_t)* heads = subtable_heads(subtable);
        if (heads == NULL) continue;
        if (entries != NULL) {
            entries[count] = (struct llchst_rebuild_entry) {
                .subtable = subtable,
                .old_heads = heads,
                .old_count = head_count(subtable)
            };
        }
        count++;
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
            if (heads == NULL) continue;
            if (entries != NULL) {
                entries[count] = (struct llchst_rebuild_entry) {
                    .subtable = subtable,
                    .old_heads = heads,
                    .old_count = head_count(subtable)
                };
            }
            count++;
        }
    }

    for (uint32_t i = 0; i < LLCHST_AUX_COUNT; i++) {
        llchst_subtable* subtable = (llchst_subtable*)dbs->aux + i;
        _Atomic(uint64_t)* heads = subtable_heads(subtable);
        if (heads == NULL) continue;
        if (entries != NULL) {
            entries[count] = (struct llchst_rebuild_entry) {
                .subtable = subtable,
                .old_heads = heads,
                .old_count = head_count(subtable)
            };
        }
        count++;
    }
    return count;
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
    const int bad = llchst_rebuild_entries_CALL(
        lace, dbs, entries, 0, count);
    sylvan_free_aligned(entries, count * sizeof(*entries));
    return bad;
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
        const uint64_t custom = dbs->bitmapc[word];
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
        dbs->bitmapc[word] = custom & marked;
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
