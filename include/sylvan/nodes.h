/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2018 Tom van Dijk, Johannes Kepler University Linz
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

#ifndef SYLVAN_NODES_H
#define SYLVAN_NODES_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * Functions for managing decision diagram nodes.
 */

typedef struct nodes_table nodes_table;

/**
 * Retrieve a pointer to the data associated with the index
 */
void* nodes_get_pointer(const nodes_table* dbs, size_t index);

/**
 * Create the table for the decision diagram nodes.
 * This will allocate a set of <max_size> buckets in virtual memory.
 * The actual space used is <initial_size> buckets.
 */
nodes_table* nodes_create(size_t initial_size, size_t max_size);

/**
 * Free the table.
 */
void nodes_free(nodes_table* dbs);

/**
 * Retrieve the maximum size of the nodes table.
 */
size_t nodes_get_max_size(const nodes_table* dbs);

/**
 * Retrieve the current size of the nodes table.
 */
size_t nodes_get_size(const nodes_table* dbs);

/**
 * Estimate the bytes reserved by this nodes backend for <capacity> buckets.
 * This includes its node/index arrays, allocation/custom bitmaps, and any
 * fixed routing metadata, but not allocator page rounding.
 */
size_t nodes_get_capacity_bytes(size_t capacity);

/**
 * Find or insert a decision diagram node.
 * Returns the index if successful, or 0 if the table is full!!
 * @ensures 2 <= \result < nodes_get_size(dbs);
 */
uint64_t nodes_lookup(const nodes_table* dbs, const uint64_t a, const uint64_t b, int *created);

/**
 * Find or insert a decision diagram node using the custom callbacks.
 * This is mainly used for multi-terminal custom leaf support.
 * Returns the index if successful, or 0 if the table is full!!
 * @ensures 2 <= \result < nodes_get_size(dbs);
 */
uint64_t nodes_lookupc(const nodes_table* dbs, const uint64_t a, const uint64_t b, int *created);

/**
 * Internal canonical-index routes. Decision routes use the node's variable
 * identity (currently its var field). Auxiliary routes keep node families
 * that do not belong to an ordered decision variable out of those subtables.
 */
#define NODES_SUBTABLE_DECISION(variable) ((uint32_t)(variable))
#define NODES_SUBTABLE_LEAF             UINT32_C(0x01000000)
#define NODES_SUBTABLE_MAP              UINT32_C(0x01000001)
#define NODES_SUBTABLE_LISTDD           UINT32_C(0x01000002)
#define NODES_SUBTABLE_DEFAULT          UINT32_C(0x01000003)

/**
 * Find or insert a node using a canonical-index routing hint. Backends without
 * subtables deliberately ignore the hint and retain their normal behavior.
 */
uint64_t nodes_lookup_subtable(const nodes_table* dbs, uint32_t subtable,
                               uint64_t a, uint64_t b, int *created);
uint64_t nodes_lookupc_subtable(const nodes_table* dbs, uint32_t subtable,
                                uint64_t a, uint64_t b, int *created);

/**
 * Whether this backend keys a lookup by a hash of the record, so that the slot
 * the lookup will touch is computable before the lookup runs. Only such a
 * backend can be warmed ahead of a loop that interns nodes; a backend that
 * anchors at one of the children has nothing to compute in advance and nothing
 * to gain, because the anchor is the node the caller just touched.
 */
#if defined(SYLVAN_NODES_TABLE_LLCHT)
#define SYLVAN_NODES_PREFETCH 1
#else
#define SYLVAN_NODES_PREFETCH 0
#endif

#if SYLVAN_NODES_PREFETCH
/**
 * Start the memory traffic that nodes_lookup_subtable(dbs, subtable, a, b)
 * will need, without performing the lookup. <depth> 0 fetches the head slot
 * the record hashes to; 1 additionally follows the head, which is only worth
 * doing once an earlier call at depth 0 has had time to land.
 *
 * This is a hint with no effect on the table. It is safe to call concurrently
 * with lookups, and safe to call for a record that is never looked up.
 */
void nodes_prefetch_subtable(const nodes_table* dbs, uint32_t subtable,
                             uint64_t a, uint64_t b, int depth);
#endif

/**
 * To perform garbage collection, the user is responsible that no lookups are performed during the process.
 *
 * 1) call nodes_clear
 * 2) call nodes_mark_rec for every root, which marks it and everything below it
 * 3) call nodes_cleanup_custom, for the custom nodes that were not marked
 * 4) call nodes_rebuild
 */

/**
 * Frees all nodes, but do not change any data
 * Clears all data to prepare for reinsertion/rehashing
 */
static inline void nodes_clear(nodes_table* dbs);

/**
 * Check if an index is a valid node and/or is marked to be reinserted after
 * garbage collection.
 */
int nodes_is_marked(const nodes_table* dbs, uint64_t index);

/**
 * The marks of the indices 64*<first> to 64*(<first>+<count>)-1, 64 to a word,
 * into <words>[0] to <words>[<count>-1]. Index i lands in <words>[i/64 -
 * <first>], at bit (63 - i%64), so the first index of each word is its most
 * significant bit; a bit is set exactly when nodes_is_marked(dbs, i) would
 * say so. <first> + <count> must not exceed (nodes_get_size(dbs) + 63) / 64.
 *
 * For a pass over the whole table that only wants the occupied slots, such as
 * the opening passes of a reorder session: an empty stretch of table then
 * costs a load per 64 slots and a call per <count> words, rather than a call
 * per slot, which is what keeps a pass over a large, sparsely used table
 * cheap.
 */
void nodes_marked_words(const nodes_table* dbs, size_t first, size_t count,
                        uint64_t* words);

/**
 * A bound on the occupied part of the table: every index i for which
 * nodes_is_marked(dbs, i) holds is below it. At most nodes_get_size(dbs).
 *
 * The bound is the end of the highest stretch of slots the allocator has
 * ever handed out from, so it rises as the table fills and never falls: a
 * collection leaves the nodes that survive at their indices, wherever they
 * are, and does not lower it. A pass over the occupied slots -- see
 * nodes_marked_words -- can stop here instead of at the end of the table, so
 * that on a table sized for far more than it ever held, the part that was
 * never used costs nothing. A backend without a better answer returns
 * nodes_get_size(dbs).
 */
size_t nodes_used_bound(const nodes_table* dbs);

/**
 * Recursively mark a node and its descendants.
 */
static inline void nodes_mark_rec(const nodes_table* dbs, uint64_t index);

/**
 * Rebuild the nodes table by reinserting/rehashing all marked nodes.
 * Returns 0 if successful, or the number of buckets that could not be
 * reinserted otherwise.
 */
static inline int nodes_rebuild(nodes_table* dbs);

/**
 * Retrieve number of nodes in the table.
 */
static inline size_t nodes_count_nodes(nodes_table* dbs);

/**
 * After garbage collection, this method calls the destroy callback
 * for all 'custom' data that is not kept.
 */
static inline void nodes_cleanup_custom(nodes_table* dbs);

/**
 * Perform table maintenance requested by the table itself.
 *
 * A backend may notice during a lookup that its internal index has degraded,
 * for example that a hash chain has grown far past its target length, and ask
 * for a maintenance pass by setting sylvan_maintenance_requested. Sylvan then
 * calls this from a stop-the-world Lace frame, exactly like garbage
 * collection, but without clearing the node bitmap, without marking and
 * without clearing the operation cache.
 *
 * Maintenance may only reorganize the backend's own index. It must not free,
 * move or renumber nodes: every node index that was valid before the pass is
 * still valid and still denotes the same node afterwards.
 *
 * Backends that never need this leave it empty and never set the flag.
 */
static inline void nodes_maintain(nodes_table* dbs);

// Definitions of tasks
TASK(void, nodes_clear, nodes_table*, dbs)
TASK(void, nodes_mark_rec, const nodes_table*, dbs, uint64_t, index)
TASK(int, nodes_rebuild, nodes_table*, dbs)
TASK(size_t, nodes_count_nodes, nodes_table*, dbs)
TASK(void, nodes_cleanup_custom, nodes_table*, dbs)
TASK(void, nodes_maintain, nodes_table*, dbs)

/**
 * ---------------------------------------------------------------------------
 * Traversal scratch bitmap.
 *
 * One dense bit per node index, allocated lazily on first use and kept until
 * the table is destroyed.
 *
 * Recursive diagnostics -- the node and leaf counters, the .dot writers, the
 * structural hashes -- have to remember which nodes they already visited.
 * They mark here and not in a spare bit of the node's own 16-byte record,
 * because that bit would sit in the word the unique table hashes and
 * compares: a traversal would rewrite the key of a live node for the duration
 * of the walk, so a lookup running concurrently could miss the node and
 * intern a duplicate.
 *
 * REENTRANCY. There is one scratch bitmap per table, so two marked traversals
 * must not overlap -- neither nested on one thread, nor on two threads at
 * once. nodes_scratch_acquire asserts in debug builds that nobody else holds
 * the scratch.
 *
 * CONCURRENCY. The accesses are plain, non-atomic loads and stores. That is
 * sound because of the reentrancy rule: only the thread that acquired the
 * scratch ever touches it. Other workers may keep building nodes meanwhile;
 * they never read or write this bitmap.
 *
 * CLEARING. A traversal that marks must unmark exactly what it marked, which
 * every caller does along the same recursion it marked with. The bitmap is
 * therefore all zeroes between traversals and is never bulk-cleared: on a
 * table sized for 2^40 nodes it spans 128 GiB of address space. With
 * SYLVAN_USE_MMAP only the pages a traversal touched become resident -- base
 * pages, since the bitmap comes from sylvan_alloc_sparse, which keeps it off
 * transparent huge pages -- while without it the allocation is zeroed, and so
 * made resident, up front. Debug builds assert on release that no mark was
 * left behind.
 * ---------------------------------------------------------------------------
 */

/**
 * Begin a marked traversal of <dbs>. Allocates the bitmap on first use;
 * failing to allocate is fatal, as it is for the arrays of nodes_create.
 */
void nodes_scratch_acquire(const nodes_table* dbs);

/**
 * End a marked traversal of <dbs>. Every mark must have been undone.
 */
void nodes_scratch_release(const nodes_table* dbs);

int  nodes_scratch_ismarked(const nodes_table* dbs, uint64_t index);
void nodes_scratch_mark(const nodes_table* dbs, uint64_t index);
void nodes_scratch_unmark(const nodes_table* dbs, uint64_t index);

/**
 * Drop the scratch bitmap of <dbs>. Sylvan calls this when it destroys the
 * table; it is a separate call rather than part of nodes_free because the
 * bitmap is backend-independent and nodes_free is not.
 */
void nodes_scratch_forget(const nodes_table* dbs);

/**
 * Helper callbacks for dealing with custom leaves.
 * This is used by the MT support (mt.c)
 * hash(a, b, seed)
 * equals(lhs_a, lhs_b, rhs_a, rhs_b)
 * create(a, b) -- with a,b pointers, allows changing pointers on create of node,
 *                 but must keep hash/equals same; returns 0 on success and a
 *                 negative error code on failure
 * destroy(a, b)
 */
typedef uint64_t (*nodes_hash_cb)(uint64_t, uint64_t, uint64_t);
typedef int (*nodes_equals_cb)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef int (*nodes_create_cb)(uint64_t *, uint64_t *);
typedef void (*nodes_destroy_cb)(uint64_t, uint64_t);

/**
 * Set custom functions
 */
void nodes_set_custom(nodes_table* dbs, nodes_hash_cb hash_cb, nodes_equals_cb equals_cb, nodes_create_cb create_cb, nodes_destroy_cb destroy_cb);

/**
 * Set the table size of the set.
 * This should only be called internally during garbage collection.
 */
void nodes_set_size(nodes_table* dbs, size_t size);

/**
 * ---------------------------------------------------------------------------
 * Internal and experimental: reorder primitives.
 *
 * This section is the backend contract that dynamic variable reordering is
 * built on. It is not a stable interface: only Sylvan's own reorder module is
 * meant to use it, and it may change or disappear without notice.
 *
 * SAFEPOINT ONLY. Every primitive in this section requires that the world is
 * stopped: no nodes_lookup, no nodes_lookup_subtable, no garbage collection
 * and no maintenance pass may run concurrently with any of them, on any
 * thread. The only concurrency they tolerate is other workers of the same
 * reorder session calling these same primitives on the same table. A backend
 * that is more restrictive than that says so in its own source file.
 *
 * All of them return SYLVAN_OK (0) on success and a negative SYLVAN_ERR_*
 * code on failure. A failure never leaves the index half-changed, with one
 * exception: nodes_reinsert_batch, see there.
 *
 * CALLER ERRORS. Some refusals below are documented as caller errors rather
 * than conditions: removing a node that is not in the index, reinserting one
 * that was not removed first. A debug build may assert on those before it
 * refuses, and every backend that implements them does on at least one, so
 * only a release build (NDEBUG) is guaranteed to see the SYLVAN_ERR_INVALID;
 * a test of such a refusal has to run without assertions. The other
 * refusals -- an index out of range, a capability the backend does not
 * report, nodes_release of a node that is still indexed -- are plain return
 * values in every build.
 * ---------------------------------------------------------------------------
 */

/**
 * Which reorder primitives this backend implements.
 *
 * A primitive whose capability is not reported is not a partial or
 * best-effort operation: it changes nothing and returns SYLVAN_ERR_INVALID.
 * A backend that reports no capability at all is excluded from reordering
 * entirely and also refuses nodes_reserve.
 */
uint32_t nodes_reorder_caps(const nodes_table* dbs);

#define NODES_CAP_REMOVE     (1u << 0)  /* nodes_remove and nodes_reinsert */
#define NODES_CAP_RELEASE    (1u << 1)  /* nodes_release */
#define NODES_CAP_ENUMERATE  (1u << 2)  /* nodes_enumerate_var */
#define NODES_CAP_BATCH      (1u << 3)  /* nodes_remove/reinsert_batch */

/**
 * Unhash one node. Its 16-byte record and its index stay intact and valid;
 * only the backend's index entry disappears, so a later lookup of the same
 * key misses and the record may be rewritten in place.
 *
 * The bucket, chain, subtable or anchor is derived from the record itself and
 * from the backend's own per-node metadata. There is no route parameter,
 * because the record is the truth.
 *
 * Returns SYLVAN_ERR_INVALID if the node is not in the index, and changes
 * nothing. That is a caller error -- removing a node twice, or one that was
 * never inserted -- so see CALLER ERRORS above for what a debug build does.
 * Without a preceding nodes_prepare_reorder a backend may also refuse with
 * SYLVAN_ERR_OOM, likewise having changed nothing.
 */
int nodes_remove(nodes_table* dbs, uint64_t index);

/**
 * Re-index a node whose record was rewritten in place after nodes_remove.
 * The key is read from the mutated record, so the node may land in a
 * different bucket, chain, subtable or anchor family than it came from.
 *
 * The node's own link is published before the node becomes reachable, with
 * release ordering, so a concurrent walker never observes it with a stale
 * successor.
 *
 * Finding an equal node at another index is not an error but a violation of
 * the caller's invariant: the unique table would hold the same key twice and
 * continuing would corrupt the forest. Debug builds assert, release builds
 * print a diagnostic and abort.
 *
 * Returns SYLVAN_ERR_INVALID for a node that was not removed first, which is
 * a caller error (see CALLER ERRORS above). A backend that allocates parts of
 * its index lazily may also refuse with SYLVAN_ERR_OOM for a route
 * nodes_prepare_reorder was not told about.
 */
int nodes_reinsert(nodes_table* dbs, uint64_t index);

/**
 * The same two operations over a whole set of nodes at once.
 *
 * WHY A BATCH IS A DIFFERENT OPERATION and not a convenience. An index whose
 * removal touches slots that no single compare-and-swap relates has to
 * exclude every other worker while it does so, and per node that means one
 * worker's throughput however many are available. Given the whole set, the
 * same backend can order the work as phases that are each internally
 * parallel -- read the victims' positions, unlink them all, repair what
 * depended on them, reinsert -- with a barrier between phases instead of an
 * exclusion per node. The trie backend is where this matters and where the
 * argument is written down; see the block above nodes_remove_batch in
 * src/nodes_trie.c.
 *
 * A backend that does not report NODES_CAP_BATCH does not implement these and
 * returns SYLVAN_ERR_INVALID; the caller falls back to the single-node calls.
 *
 * COLLECTIVE. Unlike the single-node primitives these are not called by every
 * worker. One worker calls them from inside a Lace frame, on behalf of all of
 * them, and no other reorder primitive may run while one is in progress -- the
 * batch spawns onto the same workers. <indices> must hold distinct node
 * indices and stays owned by the caller.
 *
 * REMOVAL FAILS ONLY BEFORE IT STARTS, for the failure a batch can see
 * cheaply. If any index is out of range, nodes_remove_batch changes nothing
 * and returns SYLVAN_ERR_INVALID: the check is a comparison per index and the
 * pass that makes it is read-only.
 *
 * AN INDEX THAT IS IN RANGE BUT NOT IN THE INDEX IS A CALLER ERROR, not a
 * condition: debug builds assert and release builds abort, exactly as the
 * single-node primitives do for the errors only they can see. Distinguishing
 * it before mutating would cost a read-only walk of every victim's family, and
 * no caller can use the distinction -- removing a node the index does not hold
 * means the caller's own bookkeeping is already wrong, and the session that
 * drives these calls treats a refusal as fatal anyway. A backend that
 * discovers absence for free while doing its own work may still refuse
 * cleanly, and the trie does in a release build (a debug build asserts there
 * as well); callers may not rely on it.
 *
 * Reinsertion reports SYLVAN_ERR_INVALID if any node was already indexed --
 * in a release build; a debug build asserts, as nodes_reinsert does -- and
 * aborts on an equal node exactly as nodes_reinsert does. It is the one
 * refusal in this section that is not clean: every other node of the set has
 * been re-indexed by the time it is reported. The session treats it as fatal.
 */
static inline int nodes_remove_batch(nodes_table* dbs, const uint64_t* indices,
                                     size_t count);
static inline int nodes_reinsert_batch(nodes_table* dbs,
                                       const uint64_t* indices, size_t count);

TASK(int, nodes_remove_batch, nodes_table*, dbs, const uint64_t*, indices,
     size_t, count)
TASK(int, nodes_reinsert_batch, nodes_table*, dbs, const uint64_t*, indices,
     size_t, count)

/**
 * Return a removed node's slot to the allocator, so a later lookup may claim
 * it again.
 *
 * Requires that the node has been removed from the index, that nothing refers
 * to it any more, and that the operation cache has been cleared: a reused
 * index denotes a different node, and a surviving cache entry would answer
 * with it. Callers should additionally defer this call to a barrier that
 * orders every removal of the current phase before any reuse, which is what
 * closes the ABA window on a chain link another worker may still hold.
 *
 * A custom (multi-terminal) node has its destroy callback invoked here,
 * because the slot may be reused long before the next nodes_cleanup_custom.
 *
 * A released slot is indistinguishable from one that was never used. Every
 * per-node word the backend keeps outside the record -- the chain link, the
 * owner class, the custom bit -- is reset, because an insertion is entitled
 * to whatever a fresh table would have given it. That is not cosmetic: the
 * list backend appends at the tail of a family and never writes the link of
 * the node it inserts, precisely because a fresh slot has always had a zero
 * one.
 *
 * Returns SYLVAN_ERR_INVALID if the node is still in the index.
 */
int nodes_release(nodes_table* dbs, uint64_t index);

/** Visitor for nodes_enumerate_var; return 0 to continue. */
typedef int (*nodes_visit_cb)(uint64_t index, void* context);

/**
 * Visit every node the backend has routed to one variable identity. The
 * callback returns 0 to continue; any other value stops the enumeration and
 * becomes the result. Order is unspecified; every node is visited once.
 *
 * MAP NODES ARE NOT COVERED. On llchst, the only backend with native
 * enumeration, a decision node lives in the subtable of its variable
 * identity, but an MTBDDMAP node lives in the shared MAP auxiliary subtable,
 * which this function cannot select a single key from. Enumerating a variable
 * therefore yields its decision nodes only. Covering map nodes is the session
 * layer's job -- it builds those lists from an arena scan for every backend
 * anyway -- and is deliberately not attempted here.
 *
 * Unlike the other primitives this one is read-only and is NOT safe against
 * concurrent nodes_remove or nodes_reinsert. It belongs to a read-only
 * classification phase.
 */
int nodes_enumerate_var(nodes_table* dbs, uint32_t variable,
                        nodes_visit_cb cb, void* context);

/**
 * Preflight <count> further node allocations.
 *
 * INVARIANT. On SYLVAN_OK, the next <count> allocating lookups on this table
 * are guaranteed to find a free slot, provided no other allocation happens in
 * between. On SYLVAN_ERR_OOM nothing has been changed and no promise is made.
 * The answer may be conservative -- refusing although the allocations would in
 * fact have succeeded -- and is never optimistic.
 *
 * MECHANISM. The guarantee is derived from the region allocator rather than
 * from a global free-slot count. A worker claims a whole 512-slot region, uses
 * every free slot of it, and only then gives it back and claims another one
 * that nobody owns. An owned region is therefore exactly one worker's current
 * region, and there are never more owned regions than there are workers.
 *
 * Reserve counts the free slots of the regions that are currently unclaimed,
 * and requires <count> plus one whole region per worker of them. The margin is
 * not slack: free slots inside an owned region are reachable by one worker and
 * no other, so they cannot be counted, and a worker that stops allocating
 * halfway through its region leaves the rest of it stranded for the duration.
 * Each counted region that is claimed inside the window is either used up --
 * every free slot it had became an allocation -- or is some worker's current
 * region when the window ends, and at most <workers> regions can be in that
 * second state. So at least <count> of the counted slots are reached however
 * the workers divide the allocations between them, which is exactly the
 * promise. Counting <count> alone would not be enough: with W workers the
 * answer could be short by a region each.
 *
 * GIVING REGIONS BACK keeps the count accurate over a long stop-the-world
 * session. Garbage collection resets every region, but a reorder session may
 * not collect and is the only thing that releases individual slots. If
 * used-up regions stayed owned, the slots a session released into them would
 * drop out of this count -- and out of the allocator -- one region at a time,
 * until reserve refused on a table that was almost entirely free.
 *
 * The count is recomputed on every call -- a popcount over the node bitmap of
 * the unclaimed regions, stopping as soon as the target is reached -- and never
 * cached. A cache would have to assume that every allocation since it was
 * seeded had been charged to a reservation, which the table cannot observe
 * without a counter on the lookup path, and one uncharged allocation would
 * make the answer optimistic.
 *
 * NOT ADDITIVE. Reservations do not compose. Every call answers "can <count>
 * allocations succeed now", so a caller needing <count> slots issues one call
 * for <count> and must not let another reservation's allocations run inside
 * its window.
 *
 * INDEX CAPACITY. Node slots are the only resource reserve decides exactly.
 * llchst additionally refuses when <count> more nodes would not fit the
 * head-array budget its maintenance pass already documents. Neither the first
 * head array of a subtable nor the descriptor page of a route beyond the dense
 * range is covered here; those are what nodes_prepare_reorder is for.
 */
int nodes_reserve(nodes_table* dbs, uint64_t count);

/**
 * Preflight everything the index will need, other than node slots.
 *
 * INVARIANT. On SYLVAN_OK, and until the table is resized, every nodes_remove,
 * every nodes_release, and every nodes_reinsert or allocating lookup whose
 * route is one of <routes> runs without a heap allocation of its own, and can
 * therefore not fail for lack of memory. On SYLVAN_ERR_OOM nothing the caller
 * can observe has changed and no promise is made. Unlike nodes_reserve this
 * one is additive and idempotent: it is a statement about structures that,
 * once allocated, stay allocated.
 *
 * WHY IT IS SEPARATE FROM nodes_reserve. Reserve answers a question about the
 * node arena, which is a fixed array a backend never has to grow. The rest of
 * an index is not: llchst allocates a subtable's head array on the first
 * insertion into it and a descriptor page for the first route beyond its dense
 * range, and the trie's removal rebuilds a region of the index and needs
 * somewhere to hold it. Those are heap allocations whose failure would land in
 * the middle of an operation that has already begun changing the index, which
 * is exactly what a caller that must not be left half-applied cannot have.
 * Naming the routes up front is what makes them unfailable afterwards.
 *
 * <routes> are canonical-index routes, the same values nodes_lookup_subtable
 * takes; a backend without subtables ignores them. Every backend accepts a
 * count of zero, which still preflights the route-independent part.
 *
 * Requires that the world is stopped, like everything else in this section,
 * and returns SYLVAN_ERR_INVALID from a backend that reports no reorder
 * capabilities at all.
 */
int nodes_prepare_reorder(nodes_table* dbs, const uint32_t* routes,
                          size_t count);

/**
 * Record that a backend allocated on a path a successful
 * nodes_prepare_reorder had promised would not allocate.
 *
 * Only a backend calls this, and only where it has an argument for why the
 * path is unreachable after a preflight; the counter is how a test checks that
 * argument instead of trusting it. Monotone and never reset.
 */
void nodes_note_late_allocation(void);

/** How many such allocations have happened. Expected to stay at zero. */
uint64_t nodes_late_allocations(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
