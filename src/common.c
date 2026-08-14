/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
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

/**
 * Optional garbage collection diagnostics.
 *
 * Compile with -DSYLVAN_GC_DIAGNOSTICS=1 to make Sylvan write one line to
 * stderr for every garbage collection, reporting the trigger site, the number
 * of live (marked) nodes, and the nodes table / operation cache sizes before
 * and after the resize hook ran. This is a diagnostic aid only; it is disabled
 * by default and costs nothing when disabled.
 */
#ifndef SYLVAN_GC_DIAGNOSTICS
#define SYLVAN_GC_DIAGNOSTICS 0
#endif

#if SYLVAN_GC_DIAGNOSTICS
#include <inttypes.h>
#include <time.h>

static _Atomic(uint64_t) gc_diag_count;
static void*             gc_diag_caller;
static double            gc_diag_epoch;
static double            gc_diag_previous;

static double
gc_diag_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}
#endif

const char *
sylvan_status_string(int status)
{
    switch (status) {
    case SYLVAN_OK: return "success";
    case SYLVAN_ERR_OOM: return "out of memory";
    case SYLVAN_ERR_INVALID: return "invalid argument";
    case SYLVAN_ERR_IO: return "I/O error";
    case SYLVAN_ERR_CALLBACK: return "callback error";
    case SYLVAN_ERR_OVERFLOW: return "overflow";
    default: return "unknown Sylvan status";
    }
}

/**
 * Implementation of garbage collection
 */

/**
 * Whether garbage collection is enabled or not.
 */
static int gc_enabled = 1;

/**
 * Number of consecutive collections that reclaimed almost nothing from a
 * nodes table that was both at its maximum size and full. See gc_note_progress
 * below, which reports the table as full once there have been too many.
 */
static int gc_unproductive;

/**
 * Enable garbage collection (both automatic and manual).
 */
void
sylvan_gc_enable(void)
{
    gc_enabled = 1;
    gc_unproductive = 0;
}

/**
 * Disable garbage collection (both automatic and manual).
 */
void
sylvan_gc_disable(void)
{
    gc_enabled = 0;
}

/**
 * This variable is used for a cas flag so only one gc runs at one time
 */
static _Atomic(int) gc;

/**
 * Structures for the marking mechanisms
 */
typedef struct gc_hook_entry
{
    struct gc_hook_entry *next;
    gc_hook_cb cb;
} * gc_hook_entry_t;

static gc_hook_entry_t mark_list;
static gc_hook_entry_t pregc_list;
static gc_hook_entry_t postgc_list;
static gc_hook_cb main_hook;

/**
 * Number of registered marking callbacks.
 *
 * The reorder session reads this to find out whether anyone other than Sylvan
 * itself owns external roots. A mark hook can only *mark* the roots it owns,
 * not enumerate them, so a session cannot see those roots and must refuse to
 * run rather than retire one of them.
 */
static size_t mark_count;

size_t
sylvan_gc_mark_hook_count(void)
{
    return mark_count;
}

void
sylvan_gc_hook_pregc(gc_hook_cb callback)
{
    gc_hook_entry_t e = (gc_hook_entry_t)malloc(sizeof(struct gc_hook_entry));
    e->cb = callback;
    e->next = pregc_list;
    pregc_list = e;
}

void
sylvan_gc_hook_postgc(gc_hook_cb callback)
{
    gc_hook_entry_t e = (gc_hook_entry_t)malloc(sizeof(struct gc_hook_entry));
    e->cb = callback;
    e->next = postgc_list;
    postgc_list = e;
}

void
sylvan_gc_add_mark(gc_hook_cb callback)
{
    gc_hook_entry_t e = (gc_hook_entry_t)malloc(sizeof(struct gc_hook_entry));
    e->cb = callback;
    e->next = mark_list;
    mark_list = e;
    mark_count++;
}

void
sylvan_gc_hook_main(gc_hook_cb callback)
{
    main_hook = callback;
}

/**
 * Clear the operation cache.
 */
void sylvan_clear_cache_CALL(lace_worker* lace)
{
   (void)lace;
   cache_clear();
}

/**
 * Clear the nodes table and mark all referenced nodes.
 *
 * This does not clear the hash data or rehash the nodes.
 * After marking, the "destroy" hooks are called for all unmarked nodes,
 * for example to free data of custom MTBDD leaves.
 */
void sylvan_clear_and_mark_CALL(lace_worker* lace)
{
    nodes_clear(nodes);

    for (gc_hook_entry_t e = mark_list; e != NULL; e = e->next) {
        e->cb(lace);
    }

    nodes_cleanup_custom(nodes);
}

/**
 * Clear the hash array of the nodes table and rehash all marked buckets.
 */
void sylvan_rehash_all_CALL(lace_worker* lace)
{
    (void)lace;
    // rebuild the nodes table with the marked nodes
    if (nodes_rebuild(nodes) != 0) {
        fprintf(stderr, "sylvan_rebuild error: not all nodes could be rehashed!\n");
        exit(1);
    }
}

/**
 * Logic for resizing the nodes table and operation cache
 */

/**
 * Helper routine to compute the next size....
 */
size_t
next_size(size_t current_size)
{
#if SYLVAN_SIZE_FIBONACCI
    size_t f1=1, f2=1;
    for (;;) {
        f2 += f1;
        if (f2 > current_size) return f2;
        f1 += f2;
        if (f1 > current_size) return f1;
    }
#else
    return current_size*2;
#endif
}

/**
 * Bookkeeping of the collection that is currently running.
 *
 * These are written by sylvan_gc_go before it calls the main hook and are only
 * read while the world is stopped, so they need no synchronisation. They let
 * the resizing heuristics see how much the collection they are part of
 * actually reclaimed, which is the signal the heuristics below are built on.
 */
static size_t gc_table_before;  /* nodes table size the collection ran on */
static size_t gc_used_before;   /* occupied buckets before marking */
static size_t gc_live_after;    /* occupied buckets after marking */

/**
 * How much the collection currently running reclaimed, as a fraction of the
 * nodes table it ran on. Returns nonzero if that is at least `percent`.
 */
static int
gc_reclaimed_at_least(size_t percent)
{
    if (gc_table_before == 0) return 0;
    const size_t reclaimed =
        gc_used_before > gc_live_after ? gc_used_before - gc_live_after : 0;
    return reclaimed >= (gc_table_before / 100) * percent;
}

/**
 * WHY THE HEURISTICS BELOW GROW IN MORE THAN ONE STEP
 *
 * Every collection throws away the *entire* operation cache (see
 * sylvan_gc_go). That is not an incidental cost: it is by far the dominant
 * one. Marking and rehashing are linear in the table, but losing memoisation
 * means work that was already done has to be done again, which allocates nodes
 * again, which brings the next collection forward, which throws the cache away
 * again. A run that collects often does not merely pay for the collections; it
 * can fail to make progress at all.
 *
 * The number of collections a run performs is therefore the quantity worth
 * minimising, and when a workload is churn dominated that number is decided
 * entirely by the size of the nodes table. If a collection reclaims most of
 * the table, then almost everything in the table was temporary: the table is
 * acting as a nursery, and the next collection will happen once another
 * table_size nodes have been allocated. Growing by a single doubling per
 * collection then costs log2(total_allocations / initial_size) collections.
 * That is not a small number, because sylvan_set_limits' initial_ratio
 * deliberately starts the table three to four orders of magnitude below the
 * maximum: ten to twelve collections just to reach the size the run was always
 * going to need, each one discarding the cache.
 *
 * So a collection that reclaimed at least SYLVAN_GC_CHURN_PERCENT of the table
 * takes more than one doubling. How many more escalates with the number of
 * consecutive churn-dominated collections, up to SYLVAN_GC_CHURN_DOUBLINGS:
 * the first takes one step, the second two, the third three, and so on. A run
 * that collects once and then settles is left alone, while a run that keeps
 * collecting reaches a table that ends the treadmill within a handful of
 * collections, and reaches it early, while the collections are still cheap.
 * The streak resets as soon as a collection is not churn dominated, so a
 * workload whose table fills with live nodes rather than with garbage grows
 * one step at a time exactly as before.
 *
 * Growing early is not free, but it is bounded by what the caller already
 * asked for. nodes_create and cache_create reserve the maximum size in virtual
 * memory up front and sylvan_clear_aligned hands the physical pages back to
 * the operating system on every collection, so oversizing costs only the pages
 * the workload goes on to touch, and never more than the memory cap passed to
 * sylvan_set_limits. What it also costs is locality: a sparsely filled table
 * spreads probes over more cache lines. That is why the growth is tied to
 * observed churn rather than applied unconditionally.
 */

/* Fraction of the table a collection must reclaim to count as churn
 * dominated, in percent. */
#ifndef SYLVAN_GC_CHURN_PERCENT
#define SYLVAN_GC_CHURN_PERCENT 50
#endif

/* Most doublings a single collection may take, reached after that many
 * consecutive churn-dominated collections. */
#ifndef SYLVAN_GC_CHURN_DOUBLINGS
#define SYLVAN_GC_CHURN_DOUBLINGS 4
#endif

/* Fraction of the table below which a collection counts as having failed to
 * relieve the pressure, in percent. The conservative heuristic grows on this
 * even when its "more than half is live" test says otherwise, so that it
 * cannot settle into an equilibrium where every collection reclaims just
 * enough to continue and never enough to stop collecting. */
#ifndef SYLVAN_GC_STARVED_PERCENT
#define SYLVAN_GC_STARVED_PERCENT 25
#endif

/**
 * Apply up to `doublings` growth steps to `current`, never exceeding `max`.
 */
static size_t
gc_grown_size(size_t current, size_t max, int doublings)
{
    while (doublings-- > 0 && current < max) {
        const size_t next = next_size(current);
        current = next > max ? max : next;
    }
    return current;
}

/**
 * Length of the current run of churn-dominated collections, capped at
 * SYLVAN_GC_CHURN_DOUBLINGS. Updated once per collection by gc_note_churn,
 * which sylvan_gc_go calls before the main hook, so that both heuristics see
 * the same value however they end up using it.
 */
static int gc_churn_streak;

static void
gc_note_churn(void)
{
    if (!gc_reclaimed_at_least(SYLVAN_GC_CHURN_PERCENT)) {
        gc_churn_streak = 0;
    } else if (gc_churn_streak < SYLVAN_GC_CHURN_DOUBLINGS) {
        gc_churn_streak++;
    }
}

/**
 * How many doublings this collection has earned.
 */
static int
gc_growth_doublings(void)
{
    return gc_churn_streak > 1 ? gc_churn_streak : 1;
}

/**
 * Resizing heuristic that always grows the tables when running gc (until max).
 * The nodes table and operation cache are both resized until their maximum size.
 * A collection that reclaimed most of the table grows it by several doublings
 * at once; see the commentary above.
 */
void sylvan_gc_aggressive_resize_CALL(lace_worker* lace)
{
    (void)lace;
    const int doublings = gc_growth_doublings();

    size_t nodes_size = nodes_get_size(nodes);
    size_t nodes_max = nodes_get_max_size(nodes);
    if (nodes_size < nodes_max) {
        nodes_set_size(nodes, gc_grown_size(nodes_size, nodes_max, doublings));
    }
    size_t cache_size = cache_getsize();
    size_t cache_max = cache_getmaxsize();
    if (cache_size < cache_max) {
        cache_setsize(gc_grown_size(cache_size, cache_max, doublings));
    }
}

/**
 * Resizing heuristic that only resizes when more than 50% is marked, or when
 * the collection failed to reclaim a worthwhile fraction of the table.
 * The operation cache is only resized if the nodes table is resized.
 */
void sylvan_gc_normal_resize_CALL(lace_worker* lace)
{
    (void)lace;
    size_t nodes_size = nodes_get_size(nodes);
    size_t nodes_max = nodes_get_max_size(nodes);
    if (nodes_size < nodes_max) {
        const size_t marked = gc_live_after;
        if (marked*2 > nodes_size ||
            !gc_reclaimed_at_least(SYLVAN_GC_STARVED_PERCENT)) {
            const int doublings = gc_growth_doublings();
            nodes_set_size(nodes, gc_grown_size(nodes_size, nodes_max, doublings));

            // also increase the operation cache
            size_t cache_size = cache_getsize();
            size_t cache_max = cache_getmaxsize();
            if (cache_size < cache_max) {
                cache_setsize(gc_grown_size(cache_size, cache_max, doublings));
            }
        }
    }
}

/**
 * GIVING UP HONESTLY
 *
 * When the tables are at their maximum and every collection reclaims almost
 * nothing, Sylvan cannot make progress: each collection frees just enough for
 * the one insertion that triggered it, the retry succeeds, and the table is
 * full again immediately. Nothing in that loop ever reaches the "Unique table
 * full" path, so the run grinds on indefinitely instead of reporting that it
 * needs a larger memory budget.
 *
 * After SYLVAN_GC_MAX_UNPRODUCTIVE consecutive collections that ran on a
 * full-size table that really was full and reclaimed less than
 * SYLVAN_GC_UNPRODUCTIVE_PERCENT of it, we report the table as full and stop,
 * the same way every other unsatisfiable allocation in Sylvan does. The
 * defaults are deliberately generous: a workload that is merely slow keeps
 * running, only one that has stopped making progress stops.
 *
 * Reporting here rather than merely declining to collect and letting the next
 * failed insertion report is deliberate. Sylvan's operations do return
 * SYLVAN_ERR_OOM and propagate it correctly, but callers are not obliged to
 * check, and one that does not gets an invalid handle and a crash instead of a
 * message. Turning a run that grinds forever into a run that dies confusingly
 * would not be an improvement.
 */

/* Fraction of a full-size table a collection must reclaim to count as
 * productive, in percent. */
#ifndef SYLVAN_GC_UNPRODUCTIVE_PERCENT
#define SYLVAN_GC_UNPRODUCTIVE_PERCENT 5
#endif

/* Occupancy at which the table counts as full, in percent. Collections of a
 * table that still had room say nothing about whether progress is possible:
 * they reclaim little because there was little to reclaim. */
#ifndef SYLVAN_GC_FULL_PERCENT
#define SYLVAN_GC_FULL_PERCENT 90
#endif

/* Number of consecutive unproductive collections tolerated before Sylvan
 * reports the nodes table as full. */
#ifndef SYLVAN_GC_MAX_UNPRODUCTIVE
#define SYLVAN_GC_MAX_UNPRODUCTIVE 12
#endif

static void
gc_note_progress(void)
{
    if (gc_table_before < nodes_get_max_size(nodes) ||
        gc_used_before < (gc_table_before / 100) * SYLVAN_GC_FULL_PERCENT ||
        gc_reclaimed_at_least(SYLVAN_GC_UNPRODUCTIVE_PERCENT)) {
        gc_unproductive = 0;
        return;
    }

    if (++gc_unproductive < SYLVAN_GC_MAX_UNPRODUCTIVE) return;

    fprintf(stderr,
            "BDD Unique table full, %zu of %zu buckets filled!\n"
            "sylvan_gc: %d consecutive collections of the full nodes table each "
            "reclaimed less than %d%% of it, so no amount of further collection "
            "will make room; raise the memory cap passed to sylvan_set_limits.\n",
            gc_live_after, gc_table_before,
            gc_unproductive, SYLVAN_GC_UNPRODUCTIVE_PERCENT);
    exit(1);
}

/**
 * Collections that have run, for callers that need to establish that one did.
 * See sylvan_gc_count in <sylvan/common.h>.
 */
static _Atomic(uint64_t) gc_performed;

uint64_t
sylvan_gc_count(void)
{
    return atomic_load_explicit(&gc_performed, memory_order_relaxed);
}

/**
 * Actual implementation of garbage collection
 */
void sylvan_gc_go_CALL(lace_worker* lace)
{
    sylvan_stats_count(SYLVAN_GC_COUNT);
    sylvan_timer_start(SYLVAN_GC);
    atomic_fetch_add_explicit(&gc_performed, 1, memory_order_relaxed);

    // call pre gc hooks
    for (gc_hook_entry_t e = pregc_list; e != NULL; e = e->next) {
        e->cb(lace);
    }

    /*
     * The operation cache is discarded in full, and unconditionally. This is
     * the most expensive thing a collection does -- see the resizing
     * heuristics above, which exist to make collections rarer for exactly that
     * reason -- so it is worth saying why it cannot be avoided.
     *
     * Node indices are stable across a collection. The data array is never
     * compacted, marking only re-establishes the occupancy bitmap, and
     * rehashing rebuilds the hash array around the indices the surviving nodes
     * already had. A cache entry that mentions only surviving nodes is
     * therefore still correct afterwards. That makes it tempting to keep the
     * cache when nothing moved -- but nothing ever moves, so that is not the
     * condition that matters.
     *
     * What matters is reclamation. The index of a node that did not survive
     * returns to the free pool and will be handed out to an unrelated node
     * later. A surviving cache entry mentioning that index would then answer a
     * query about the new node with the old node's result, silently and
     * wrongly. Keeping the cache is sound only for a collection that reclaimed
     * nothing at all, which is precisely the collection that was not worth
     * running.
     *
     * Clearing selectively is not available either. Cache entries are opaque
     * 64-bit words and operations may store things in them that are not node
     * handles: counts, variable indices, encoded doubles. Nothing in an entry
     * says which of its words would have to be checked against the occupancy
     * bitmap, so a selective sweep would need per-operation annotations, and a
     * single wrong annotation would buy a silently wrong answer.
     */
    sylvan_clear_cache_CALL(lace);

    /* Occupied buckets before marking. Together with the number that survive
     * marking this tells the resizing heuristic how much this collection
     * reclaimed, which is what it sizes the tables on. */
    gc_used_before = nodes_count_nodes_CALL(lace, nodes);

    sylvan_clear_and_mark_CALL(lace);

    gc_live_after = nodes_count_nodes_CALL(lace, nodes);
    gc_table_before = nodes_get_size(nodes);
    gc_note_churn();

#if SYLVAN_GC_DIAGNOSTICS
    const uint64_t diag_seq = atomic_fetch_add(&gc_diag_count, 1) + 1;
    const double diag_now = gc_diag_now();
    if (gc_diag_epoch == 0.0) {
        gc_diag_epoch = diag_now;
        fprintf(stderr, "[sylvan-gc] sylvan_gc_go_CALL=%p\n",
                (void*)(uintptr_t)&sylvan_gc_go_CALL);
    }
    const size_t diag_cache_before = cache_getsize();
#endif

    // call hooks for resizing and all that
    main_hook(lace);

    gc_note_progress();

#if SYLVAN_GC_DIAGNOSTICS
    {
        const size_t diag_table_after = nodes_get_size(nodes);
        const size_t diag_cache_after = cache_getsize();
        fprintf(stderr,
                "[sylvan-gc] #%" PRIu64 " t=%.3f dt=%.3f caller=%p "
                "used=%zu live=%zu freed=%zu (%.1f%% of table) "
                "table=%zu->%zu/%zu (%.1f%% live) cache=%zu->%zu/%zu\n",
                diag_seq, diag_now - gc_diag_epoch,
                gc_diag_previous == 0.0 ? 0.0 : diag_now - gc_diag_previous,
                gc_diag_caller,
                gc_used_before, gc_live_after, gc_used_before - gc_live_after,
                100.0 * (double)(gc_used_before - gc_live_after) / (double)gc_table_before,
                gc_table_before, diag_table_after, nodes_get_max_size(nodes),
                100.0 * (double)gc_live_after / (double)gc_table_before,
                diag_cache_before, diag_cache_after, cache_getmaxsize());
        fflush(stderr);
        gc_diag_previous = diag_now;
    }
#endif

    sylvan_rehash_all_CALL(lace);

    // call post gc hooks
    for (gc_hook_entry_t e = postgc_list; e != NULL; e = e->next) {
        e->cb(lace);
    }

    sylvan_timer_stop(SYLVAN_GC);
}

/**
 * Table maintenance: a stop-the-world pass that lets the nodes table
 * reorganize its own index. It reuses the garbage collection barrier and
 * nothing else. In particular it does not clear the operation cache, does not
 * touch the node bitmap and does not mark, which is what makes it cheap enough
 * for the table to ask for it between collections.
 */
_Atomic(int) sylvan_maintenance_requested;

/**
 * Only one maintenance pass runs at a time.
 */
static _Atomic(int) maintenance;

TASK(void, sylvan_maintenance_go)

void sylvan_maintenance_go_CALL(lace_worker* lace)
{
    nodes_maintain_CALL(lace, nodes);
}

void sylvan_maintenance(lace_worker* lace)
{
    (void)lace;

    /* A collection resizes everything anyway, and starting a maintenance frame
       from inside a collection frame would deadlock on the Lace barrier. */
    if (atomic_load_explicit(&gc, memory_order_relaxed) != 0) return;

    int zero = 0;
    if (!atomic_compare_exchange_strong(&maintenance, &zero, 1)) {
        /* Another worker is running the pass. Do not wait for it: this worker
           joins its frame at the next lace_check_yield, and unlike garbage
           collection there is nothing here that the caller must observe before
           continuing. */
        return;
    }

    if (atomic_load_explicit(&gc, memory_order_relaxed) == 0) {
        sylvan_maintenance_go_NEWFRAME();
    }

    maintenance = 0;
}

/**
 * Perform garbage collection
 */
void sylvan_gc_CALL(lace_worker* lace)
{
    if (gc_enabled) {
        int zero = 0;
        if (atomic_compare_exchange_strong(&gc, &zero, 1)) {
#if SYLVAN_GC_DIAGNOSTICS
            gc_diag_caller = __builtin_return_address(0);
#endif
            sylvan_gc_go_NEWFRAME();
            gc = 0;
        } else {
            /* wait for new frame to appear */
            while (atomic_load_explicit(&lace_newframe.t, memory_order_relaxed) == 0) {}
            lace_yield(lace);
        }
    }
}

/**
 * The unique table
 */

nodes_table* nodes;

/**
 * Allocations a backend made where nodes_prepare_reorder had promised there
 * would be none. Kept here rather than in a backend because only one backend
 * is ever compiled in, and a caller should not have to know which.
 */
static _Atomic(uint64_t) late_allocations;

void
nodes_note_late_allocation(void)
{
    atomic_fetch_add_explicit(&late_allocations, 1, memory_order_relaxed);
}

uint64_t
nodes_late_allocations(void)
{
    return atomic_load_explicit(&late_allocations, memory_order_relaxed);
}

static size_t table_min = 0, table_max = 0, cache_min = 0, cache_max = 0;

static size_t
table_memory_bytes(size_t node_capacity, size_t cache_capacity)
{
    return nodes_get_capacity_bytes(node_capacity) + cache_capacity * 44;
}

static int
is_power_of_two(size_t size)
{
    return (size != 0) && ((size & (size-1)) == 0);
}

void
sylvan_set_sizes(size_t min_tablesize, size_t max_tablesize, size_t min_cachesize, size_t max_cachesize)
{
    /* Some sanity checks */
    if (min_tablesize > max_tablesize) min_tablesize = max_tablesize;
    if (min_cachesize > max_cachesize) min_cachesize = max_cachesize;

    if (!is_power_of_two(min_tablesize) || !is_power_of_two(max_tablesize) ||
        !is_power_of_two(min_cachesize) || !is_power_of_two(max_cachesize)) {
        fprintf(stderr, "sylvan_set_sizes error: parameters not powers of 2!\n");
        exit(1);
    }

    if (max_tablesize > UINT64_C(0x0000010000000000)) {
        fprintf(stderr, "sylvan_set_sizes error: tablesize must be <= 40 bits!\n");
        exit(1);
    }

    table_min = min_tablesize;
    table_max = max_tablesize;
    cache_min = min_cachesize;
    cache_max = max_cachesize;
}

void
sylvan_set_limits(size_t memorycap, int table_ratio, int initial_ratio)
{
    /* This method ONLY limits the "unique nodes table" and the "operation cache" which are the main
     * datastructures of Sylvan.
     *
     * Suggested limits:
     * - memorycap to whatever you want at most, in bytes.
     * - table_ratio to a number between -3 (8x bigger cache) and 3 (8x bigger nodes table)
     * - inital_ratio to something like 10 (start with 1024x smaller tables initially)
     */
    if (table_ratio < -10 || table_ratio > 10) {
        fprintf(stderr, "sylvan_set_limits: table_ratio unreasonable (between -10 and 10)\n");
        exit(1);
    }

    size_t max_t = 1;
    size_t max_c = 1;
    if (table_ratio > 0) {
        max_t <<= table_ratio;
    } else {
        max_c <<= -table_ratio;
    }

    const size_t cur = table_memory_bytes(max_t, max_c);
    if (cur > memorycap) {
        fprintf(stderr, "sylvan_set_limits: memory cap incompatible with requested table ratio\n");
    }

    while (max_t < UINT64_C(0x0000010000000000)) {
        const size_t next_t = max_t * 2;
        const size_t next_c = max_c * 2;
        const size_t next_bytes = table_memory_bytes(next_t, next_c);
        if (next_bytes > memorycap) break;
        max_t = next_t;
        max_c = next_c;
    }

    if (initial_ratio < 0) {
        fprintf(stderr, "sylvan_set_limits: initial_ratio unreasonable (may not be negative)\n");
        exit(1);
    }

    size_t min_t = max_t, min_c = max_c;
    while (initial_ratio > 0 && min_t > 0x1000 && min_c > 0x1000) {
        min_t >>= 1;
        min_c >>= 1;
        initial_ratio--;
    }

    table_min = min_t;
    table_max = max_t;
    cache_min = min_c;
    cache_max = max_c;
}

/**
 * Initializes Sylvan.
 */
int
sylvan_init_package(void)
{
    if (table_max == 0) {
        fprintf(stderr, "sylvan_init_package error: table sizes not set (sylvan_set_sizes or sylvan_set_limits)!\n");
        return SYLVAN_ERR_INVALID;
    }

    /* Create tables. Note that nodes_create still exits the process on an
       allocation failure rather than returning NULL; see the exit(1) cleanup
       item. The check below is what a returning nodes_create would need. */
    nodes = nodes_create(table_min, table_max);
    if (nodes == NULL) return SYLVAN_ERR_OOM;

    const int cache_status = cache_create(cache_min, cache_max);
    if (cache_status != SYLVAN_OK) {
        nodes_free(nodes);
        nodes = NULL;
        return cache_status;
    }

    /* Initialize garbage collection */
    gc = 0;
    maintenance = 0;
    sylvan_maintenance_requested = 0;
    gc_unproductive = 0;
    gc_churn_streak = 0;
    gc_table_before = 0;
    gc_used_before = 0;
    gc_live_after = 0;

    // FIXME better configuration!!
#if SYLVAN_AGGRESSIVE_RESIZE
    main_hook = sylvan_gc_aggressive_resize_CALL;
#else
    main_hook = sylvan_gc_normal_resize_CALL;
#endif

    /* Cannot fail: it only resets per-worker counters. */
    sylvan_stats_init();
    return SYLVAN_OK;
}

struct reg_quit_entry
{
    struct reg_quit_entry *next;
    quit_cb cb;
};

static struct reg_quit_entry *quit_register = NULL;

void
sylvan_register_quit(quit_cb cb)
{
    struct reg_quit_entry *e = (struct reg_quit_entry*)malloc(sizeof(struct reg_quit_entry));
    e->next = quit_register;
    e->cb = cb;
    quit_register = e;
}

void
sylvan_quit(void)
{
    while (quit_register != NULL) {
        struct reg_quit_entry *e = quit_register;
        quit_register = e->next;
        e->cb();
        free(e);
    }

    while (pregc_list != NULL) {
        gc_hook_entry_t e = pregc_list;
        pregc_list = e->next;
        free(e);
    }

    while (postgc_list != NULL) {
        gc_hook_entry_t e = postgc_list;
        postgc_list = e->next;
        free(e);
    }

    while (mark_list != NULL) {
        gc_hook_entry_t e = mark_list;
        mark_list = e->next;
        free(e);
    }
    mark_count = 0;

    cache_free();
    nodes_free(nodes);
}

/**
 * Calculate table usage (in parallel)
 */
void sylvan_table_usage_CALL(lace_worker* lace, size_t* filled, size_t* total)
{
    (void)lace;
    size_t tot = nodes_get_size(nodes);
    if (filled != NULL) *filled = nodes_count_nodes(nodes);
    if (total != NULL) *total = tot;
}


