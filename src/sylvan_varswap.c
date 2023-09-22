#include <sylvan_int.h>
#include <sylvan_align.h>

#define TASK_SIZE 1024
/**
 * @brief Check if a node is dependent on node with label <var> or <var>+1
 */
static inline int is_node_dependent_on(mtbddnode_t node, BDDVAR var)
{
    MTBDD f0 = mtbddnode_getlow(node);
    if (!mtbdd_isleaf(f0)) {
        uint32_t vf0 = mtbdd_getvar(f0);
        if (vf0 == var || vf0 == var + 1) return 1;
    }
    MTBDD f1 = mtbddnode_gethigh(node);
    if (!mtbdd_isleaf(f1)) {
        uint32_t vf1 = mtbdd_getvar(f1);
        if (vf1 == var || vf1 == var + 1) return 1;
    }
    return 0;
}

#if !SYLVAN_USE_LINEAR_PROBING
/*!
   \brief Adjacent variable swap phase 0 (Chaining compatible)
   \details Clear hashes of nodes with var and var+1, Removes exactly the nodes
   that will be changed from the hash table.
*/
VOID_TASK_DECL_6(sylvan_varswap_p0, uint32_t, size_t, size_t, _Atomic (reorder_result_t) *, roaring_bitmap_t*,
                 roaring_bitmap_t*)

#define sylvan_varswap_p0(pos, result, ids, p1) CALL(sylvan_varswap_p0, pos, 0, nodes->table_size, result, ids, p1)
#endif

/*!
   @brief Adjacent variable swap phase 2
   @details Handle all trivial cases where no node is created, mark cases that are not trivial.
   @return number of nodes that were marked
*/
VOID_TASK_DECL_6(sylvan_varswap_p1, uint32_t, size_t, size_t, _Atomic (reorder_result_t) *, roaring_bitmap_t*,
                 roaring_bitmap_t*)

#define sylvan_varswap_p1(pos, result, p1, p2) CALL(sylvan_varswap_p1, pos, 0, nodes->table_size, result, p1, p2)

/*!
   @brief Adjacent variable swap phase 2
   @details Handle the not so trivial cases. (creates new nodes)
*/
VOID_TASK_DECL_5(sylvan_varswap_p2, size_t, size_t, _Atomic (reorder_result_t) *, roaring_bitmap_t*, roaring_bitmap_t*)

#define sylvan_varswap_p2(result, ids, p2) CALL(sylvan_varswap_p2, 0, nodes->table_size, result, ids, p2)

/*!
   @brief Adjacent variable swap phase 3
   @details Recovery phase, restore the nodes that were marked in phase 1.
*/
VOID_TASK_DECL_3(sylvan_varswap_recovery, uint32_t, _Atomic (reorder_result_t) *, roaring_bitmap_t*)

#define sylvan_varswap_recovery(pos, result, node_ids) CALL(sylvan_varswap_recovery, pos, result, node_ids)


TASK_IMPL_1(reorder_result_t, sylvan_varswap, uint32_t, pos)
{
    if (pos == sylvan_invalid) return SYLVAN_REORDER_NO_REGISTERED_VARS;

    if ((double) get_nodes_count() > (double) llmsset_get_size(nodes) * SYLVAN_REORDER_MIN_MEM_REQ) {
        return SYLVAN_REORDER_NOT_ENOUGH_MEMORY;
    }

    _Atomic (reorder_result_t) result = SYLVAN_REORDER_SUCCESS;
    sylvan_stats_count(SYLVAN_RE_SWAP_COUNT);

    roaring_bitmap_t p2_ids;
    roaring_bitmap_init_cleared(&p2_ids);

    /// Phase 0: clear hashes of nodes with <var> and <var+1> or all nodes if linear probing is used
#if SYLVAN_USE_LINEAR_PROBING
    llmsset_clear_hashes(nodes);
    /// Phase 1: handle all trivial cases where no node is created, add cases that are not trivial to <p2_ids>
    sylvan_varswap_p1(pos, &result, reorder_db->mrc.node_ids, &p2_ids);
#else
    roaring_bitmap_t p1_ids;
    roaring_bitmap_init_cleared(&p1_ids);
    sylvan_varswap_p0(pos, &result, reorder_db->mrc.node_ids, &p1_ids);
    if (sylvan_reorder_issuccess(result) == 0) return result; // fail fast
    /// Phase 1: handle all trivial cases where no node is created, add cases that are not trivial to <p2_ids>
    sylvan_varswap_p1(pos, &result, reorder_db->mrc.node_ids, &p2_ids);
#endif

    if (sylvan_reorder_issuccess(result) == 0) return result; // fail fast

    if (roaring_bitmap_get_cardinality(&p2_ids) > 0) {
        /// Phase 2: handle the not so trivial cases (creates new nodes)
        sylvan_varswap_p2(&result, &p2_ids, reorder_db->mrc.node_ids);
        if (sylvan_reorder_issuccess(result) == 0) {
            /// Phase 3: recovery
            sylvan_varswap_recovery(pos, &result, reorder_db->mrc.node_ids);
        }
    }

#if SYLVAN_USE_LINEAR_PROBING
    // collect garbage (dead nodes)
    mrc_gc(&reorder_db->mrc, reorder_db->mrc.node_ids);
#else
    // collect garbage (dead nodes)
    mrc_gc(&reorder_db->mrc, &p1_ids);
#endif

    levels_swap(&reorder_db->levels, pos, pos + 1);

    return result;
}

#if !SYLVAN_USE_LINEAR_PROBING
/**
 * Implementation of the zero phase of variable swapping.
 * For all <var+1> nodes, make <var> and rehash.
 *
 * Removes exactly the nodes that will be changed from the hash table.
 */
VOID_TASK_IMPL_6(sylvan_varswap_p0,
                 uint32_t, var,
                 size_t, first,
                 size_t, count,
                 _Atomic (reorder_result_t)*, result,
                 roaring_bitmap_t*, node_ids,
                 roaring_bitmap_t*, p1_ids)
{
    if (count > TASK_SIZE) {
        // standard reduction pattern with local roaring bitmaps collecting new node indices
        size_t split = count / 2;
        roaring_bitmap_t a;
        roaring_bitmap_init_cleared(&a);
        SPAWN(sylvan_varswap_p0, var, first, split, result, node_ids, &a);
        roaring_bitmap_t b;
        roaring_bitmap_init_cleared(&b);
        CALL(sylvan_varswap_p0, var, first + split, count - split, result, node_ids, &b);
        roaring_bitmap_or_inplace(p1_ids, &b);
        roaring_bitmap_clear(&b);
        SYNC(sylvan_varswap_p0);
        roaring_bitmap_or_inplace(p1_ids, &a);
        roaring_bitmap_clear(&a);
        return;
    }
    roaring_uint32_iterator_t it;
    roaring_init_iterator(node_ids, &it);
    roaring_move_uint32_iterator_equalorlarger(&it, first);

    const size_t end = first + count;
    while (it.has_value && it.current_value < end) {
        if (atomic_load_explicit(result, memory_order_relaxed) != SYLVAN_REORDER_SUCCESS) return; // fail fast
        size_t index = it.current_value;
        roaring_advance_uint32_iterator(&it);
        mtbddnode_t node = MTBDD_GETNODE(index);
        if (mtbddnode_isleaf(node)) continue; // a leaf
        uint32_t nvar = mtbddnode_getvariable(node);
        if (nvar == var || nvar == (var + 1)) {
            roaring_bitmap_add(p1_ids, index);
            if (llmsset_clear_one_hash(nodes, index) < 0) {
                atomic_store(result, SYLVAN_REORDER_P0_CLEAR_FAIL);
                return;
            }
        }
    }
}

#endif

/**
 * Implementation of the first phase of variable swapping.
 * For all <var+1> nodes, set variable label to <var> and rehash.
 * For all <var> nodes not depending on <var+1>, set variable label to <var+1> and rehash.
 * For all <var> nodes depending on <var+1>, stay <var> and mark. (no rehash)
 * Returns number of marked nodes left.
 *
 * This algorithm is also used for the recovery phase 1. This is an identical
 * phase, except marked <var> nodes are unmarked. If the recovery flag is set, then only <var+1>
 * nodes are rehashed.
 */
VOID_TASK_IMPL_6(sylvan_varswap_p1,
                 uint32_t, var,
                 size_t, first,
                 size_t, count,
                 _Atomic (reorder_result_t)*, result,
                 roaring_bitmap_t*, p1_ids,
                 roaring_bitmap_t*, p2_ids)
{
    if (count > TASK_SIZE) {
        size_t split = count / 2;
        roaring_bitmap_t a;
        roaring_bitmap_init_cleared(&a);
        SPAWN(sylvan_varswap_p1, var, first, split, result, p1_ids, &a);
        roaring_bitmap_t b;
        roaring_bitmap_init_cleared(&b);
        CALL(sylvan_varswap_p1, var, first + split, count - split, result, p1_ids, &b);
        roaring_bitmap_or_inplace(p2_ids, &b);
        roaring_bitmap_clear(&b);
        SYNC(sylvan_varswap_p1);
        roaring_bitmap_or_inplace(p2_ids, &a);
        roaring_bitmap_clear(&a);
        return;
    }

    // initialize the iterator on stack to speed it up and bind lifetime to this scope
    roaring_uint32_iterator_t it;
    roaring_init_iterator(p1_ids, &it);
    if (!roaring_move_uint32_iterator_equalorlarger(&it, first)) return;

    // standard reduction pattern with local variables to avoid hotspots
    int var_diff = 0;
    int var_plus_one_diff = 0;

    const size_t end = first + count;
    while (it.has_value && it.current_value < end) {
        if (atomic_load_explicit(result, memory_order_relaxed) != SYLVAN_REORDER_SUCCESS) {
            return; // fail fast
        }

        size_t index = it.current_value;
        roaring_advance_uint32_iterator(&it);

        mtbddnode_t node = MTBDD_GETNODE(index);
        uint32_t nvar = mtbddnode_getvariable(node);

        if (nvar == (var + 1)) {
            // if <var+1>, then replace with <var> and rehash
            var_diff++;
            var_plus_one_diff--;
            mtbddnode_setvariable(node, var);
            if (llmsset_rehash_bucket(nodes, index) != 1) {
                atomic_store(result, SYLVAN_REORDER_P1_REHASH_FAIL);
                return;
            }
            continue;
        } else if (nvar != var) {
            continue; // not <var> or <var+1>
        }

        if (mtbddnode_ismapnode(node)) {
            MTBDD f0 = mtbddnode_getlow(node);
            if (f0 == mtbdd_false) {
                // we are at the end of a chain
                var_plus_one_diff++;
                var_diff--;
                mtbddnode_setvariable(node, var + 1);
                if (llmsset_rehash_bucket(nodes, index) != 1) {
                    atomic_store(result, SYLVAN_REORDER_P1_REHASH_FAIL);
                    return;
                }
            } else {
                // not the end of a chain, so f0 is the next in chain
                uint32_t vf0 = mtbdd_getvar(f0);
                if (vf0 > var + 1) {
                    // next in chain wasn't <var+1>...
                    var_plus_one_diff++;
                    var_diff--;
                    mtbddnode_setvariable(node, var + 1);
                    if (llmsset_rehash_bucket(nodes, index) != 1) {
                        atomic_store(result, SYLVAN_REORDER_P1_REHASH_FAIL);
                        return;
                    }
                } else {
                    // add for phase 2
                    roaring_bitmap_add(p2_ids, index);
                }
            }
        } else {
            if (is_node_dependent_on(node, var)) {
                // add for phase 2
                roaring_bitmap_add(p2_ids, index);
            } else {
                var_plus_one_diff++;
                var_diff--;
                mtbddnode_setvariable(node, var + 1);
                if (llmsset_rehash_bucket(nodes, index) != 1) {
                    atomic_store(result, SYLVAN_REORDER_P1_REHASH_FAIL);
                    return;
                }
            }
        }
    }

    if (var_diff != 0) mrc_var_nnodes_add(&reorder_db->mrc, var, var_diff);
    if (var_plus_one_diff != 0) mrc_var_nnodes_add(&reorder_db->mrc, var + 1, var_plus_one_diff);
}

#define index(x) ((x) & SYLVAN_TABLE_MASK_INDEX)
/**
 * Implementation of second phase of variable swapping.
 * For all nodes marked in the first phase:
 * - determine F00, F01, F10, F11
 * - obtain nodes F0 [var+1,F00,F10] and F1 [var+1, F01, F11]
 *   (and F0<>F1, trivial proof)
 * - in-place substitute outgoing edges with new F0 and F1
 * - and rehash into hash table
 * Returns 0 if there was no error, or 1 if nodes could not be
 * rehashed, or 2 if nodes could not be created, or 3 if both.
 */
VOID_TASK_IMPL_5(sylvan_varswap_p2,
                 size_t, first,
                 size_t, count,
                 _Atomic (reorder_result_t)*, result,
                 roaring_bitmap_t*, p2_ids,
                 roaring_bitmap_t*, node_ids)
{
    if (count > TASK_SIZE) {
        size_t split = count / 2;
        // standard reduction pattern with local roaring bitmaps collecting new node indices
        roaring_bitmap_t a;
        roaring_bitmap_init_cleared(&a);
        SPAWN(sylvan_varswap_p2, first, split, result, p2_ids, &a);
        roaring_bitmap_t b;
        roaring_bitmap_init_cleared(&b);
        CALL(sylvan_varswap_p2, first + split, count - split, result, p2_ids, &b);
        roaring_bitmap_or_inplace(node_ids, &b);
        roaring_bitmap_clear(&b);
        SYNC(sylvan_varswap_p2);
        roaring_bitmap_or_inplace(node_ids, &a);
        roaring_bitmap_clear(&a);
        return;
    }

    roaring_uint32_iterator_t it;
    roaring_init_iterator(p2_ids, &it);
    if (!roaring_move_uint32_iterator_equalorlarger(&it, first)) return;

    int new_nnodes = 0;
    unsigned short var_new_nnodes[reorder_db->levels.count];
    memset(&var_new_nnodes, 0x00, sizeof(unsigned short) * reorder_db->levels.count);

    const size_t end = first + count;
    while (it.has_value && it.current_value < end) {
        if (atomic_load_explicit(result, memory_order_relaxed) != SYLVAN_REORDER_SUCCESS) {
            return;  // fail fast
        }
        size_t index = it.current_value;
        roaring_advance_uint32_iterator(&it);
        mtbddnode_t node = MTBDD_GETNODE(index);

        BDDVAR var = mtbddnode_getvariable(node);
        if (mtbddnode_ismapnode(node)) {
            MTBDD newf, f1, f0, f01, f00;
            int created = 0;

            // it is a map node, swap places with next in chain
            f0 = mtbddnode_getlow(node);
            f1 = mtbddnode_gethigh(node);
            mtbddnode_t n0 = MTBDD_GETNODE(f0);
            f00 = node_getlow(f0, n0);
            f01 = node_gethigh(f0, n0);

            newf = mtbdd_varswap_makemapnode(var + 1, f00, f1, &created);
            if (newf == mtbdd_invalid) {
                atomic_store(result, SYLVAN_REORDER_P2_MAPNODE_CREATE_FAIL);
                return;
            }
            mtbddnode_makemapnode(node, var, f0, f01);
            llmsset_rehash_bucket(nodes, index);

            mrc_ref_nodes_add(&reorder_db->mrc, index(f0), -1);
            mrc_ref_nodes_add(&reorder_db->mrc, index(newf), 1);

            if (created) {
                new_nnodes++;
                var_new_nnodes[var + 1]++;
                mrc_ref_nodes_add(&reorder_db->mrc, index(f00), 1);
                mrc_ref_nodes_add(&reorder_db->mrc, index(f1), 1);
                roaring_bitmap_add(node_ids, index(newf));
            }
        } else {
            MTBDD newf1, newf0, f1, f0, f11, f10, f01, f00;
            int created0, created1 = 0;

            // obtain cofactors
            f0 = mtbddnode_getlow(node);
            f1 = mtbddnode_gethigh(node);

            f01 = f00 = f0;
            if (!mtbdd_isleaf(f0) && mtbdd_getvar(f0) == var) {
                f00 = mtbdd_getlow(f0);
                f01 = mtbdd_gethigh(f0);
            }

            f11 = f10 = f1;
            if (!mtbdd_isleaf(f1) && mtbdd_getvar(f1) == var) {
                f10 = mtbdd_getlow(f1);
                f11 = mtbdd_gethigh(f1);
            }

            // The new nodes required at level i (i.e., (xi, F01, F11) and (xi, F00, F10)) may be
            // degenerate nodes (e.g., in the case that F11 = F01 or F10 == F00),
            // or may already exist in the DAG as required to implement other functions.

            newf1 = mtbdd_varswap_makenode(var + 1, f01, f11, &created1);
            if (newf1 == mtbdd_invalid) {
                atomic_store(result, SYLVAN_REORDER_P2_CREATE_FAIL);
                return;
            }

            newf0 = mtbdd_varswap_makenode(var + 1, f00, f10, &created0);
            if (newf0 == mtbdd_invalid) {
                atomic_store(result, SYLVAN_REORDER_P2_CREATE_FAIL);
                return;
            }

            // update node, which also removes the mark
            mtbddnode_makenode(node, var, newf0, newf1);
            llmsset_rehash_bucket(nodes, index);

            mrc_ref_nodes_add(&reorder_db->mrc, index(f1), -1);
            mrc_ref_nodes_add(&reorder_db->mrc, index(newf1), 1);

            if (created1) {
                new_nnodes++;
                var_new_nnodes[var + 1]++;
                mrc_ref_nodes_add(&reorder_db->mrc, index(f11), 1);
                mrc_ref_nodes_add(&reorder_db->mrc, index(f01), 1);
                roaring_bitmap_add(node_ids, index(newf1));
            }

            mrc_ref_nodes_add(&reorder_db->mrc, index(f0), -1);
            mrc_ref_nodes_add(&reorder_db->mrc, index(newf0), 1);
            if (created0) {
                new_nnodes++;
                var_new_nnodes[var + 1]++;
                mrc_ref_nodes_add(&reorder_db->mrc, index(f00), 1);
                mrc_ref_nodes_add(&reorder_db->mrc, index(f10), 1);
                roaring_bitmap_add(node_ids, index(newf0));
            }
        }

    }

    if (new_nnodes > 0) mrc_nnodes_add(&reorder_db->mrc, new_nnodes);
    for (size_t i = 0; i < reorder_db->levels.count; ++i) {
        if (var_new_nnodes[i] > 0) mrc_var_nnodes_add(&reorder_db->mrc, i, var_new_nnodes[i]);
    }
}

VOID_TASK_IMPL_3(sylvan_varswap_recovery, uint32_t, pos, _Atomic (reorder_result_t)*, result, roaring_bitmap_t*, node_ids)
{
    printf("\nReordering: Running recovery after running out of memory...\n");

    roaring_bitmap_t p2_ids;
    roaring_bitmap_init_cleared(&p2_ids);

#if SYLVAN_USE_LINEAR_PROBING
    // clear the entire table
    llmsset_clear_hashes(nodes);
    // at this point we already have nodes marked from P2 so we will unmark them now in P1
    sylvan_varswap_p1(pos, result, node_ids, &p2_ids);
#else
    roaring_bitmap_t p1_ids;
    roaring_bitmap_init_cleared(&p1_ids);
    // clear hashes of nodes with <var> and <var+1>
    sylvan_varswap_p0(pos, result, node_ids, &p1_ids);
    if (sylvan_reorder_issuccess(*result) == 0) return; // fail fast
    // at this point we already have nodes marked from P2 so we will unmark them now in P1
    sylvan_varswap_p1(pos, result, &p1_ids, &p2_ids);
#endif

    if (sylvan_reorder_issuccess(*result) == 0) return; // fail fast
    if (roaring_bitmap_get_cardinality(&p2_ids) > 0 && sylvan_reorder_issuccess(*result)) {
        // do the not so trivial cases (but won't create new nodes this time)
        sylvan_varswap_p2(result, &p2_ids, reorder_db->mrc.node_ids);
        if (sylvan_reorder_issuccess(*result) == 0) return; // fail fast
    }
}