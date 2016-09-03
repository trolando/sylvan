/*
 * Copyright 2016 Tom van Dijk, Johannes Kepler University Linz
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

/**
 * Block size tunes the granularity of the parallel distribution
 */
#define BLOCKSIZE 128

/**
 * Handling of variable levels
 */
static MTBDD *levels = NULL;
static uint32_t *var_to_level = NULL;
static uint32_t *level_to_var = NULL;
static size_t levels_count = 0;

static size_t levels_size = 0;

/**
 * Create the next level and return the BDD representing the variable (ithlevel)
 */
MTBDD
mtbdd_newlevel(void)
{
    mtbdd_newlevels(1);
    return levels[levels_count-1];
}

/**
 * Create the next <amount> levels
 */
void
mtbdd_newlevels(size_t amount)
{
    if (levels_count + amount >= levels_size) {
        if (levels_size == 0) levels_size = 1; // start here
        while (levels_count + amount >= levels_size) levels_size *= 2;
        levels = realloc(levels, sizeof(MTBDD[levels_size]));
        var_to_level = realloc(var_to_level, sizeof(uint32_t[levels_size]));
        level_to_var = realloc(level_to_var, sizeof(uint32_t[levels_size]));
    }
    for (size_t i=0; i<amount; i++) {
        levels[levels_count] = mtbdd_makenode(levels_count, mtbdd_false, mtbdd_true);
        var_to_level[levels_count] = levels_count;
        level_to_var[levels_count] = levels_count;
        levels_count++;
    }
}

/**
 * Reset all levels.
 */
void
mtbdd_levels_reset(void)
{
    levels_count = 0;
}

/**
 * Create or get the BDD representing "if <level> then true else false"
 */
MTBDD
mtbdd_ithlevel(uint32_t level)
{
    if (levels_count) {
        if (level >= levels_count) return mtbdd_invalid;
        else return levels[level_to_var[level]];
    } else {
        return mtbdd_makenode(level, mtbdd_false, mtbdd_true);
    }
}

/**
 * Get the current level of the given internal variable <var>
 */
uint32_t
mtbdd_var_to_level(uint32_t var)
{
    if (var < levels_count) return var_to_level[var];
    else return var;
}

/**
 * Get the current internal variable of the given level <level>
 */
uint32_t
mtbdd_level_to_var(uint32_t level)
{
    if (level < levels_count) return level_to_var[level];
    else return level;
}

/**
 * Return the level of the given internal node.
 */
uint32_t
mtbdd_getlevel(MTBDD node)
{
    return mtbdd_var_to_level(mtbdd_getvar(node));
}

VOID_TASK_0(mtbdd_gc_mark_managed_refs)
{
    for (size_t i=0; i<levels_count; i++) {
        llmsset_mark(nodes, MTBDD_STRIPMARK(levels[i]));
    }
}

/**
 * Initialize and quit functions
 */

static int mtbdd_reorder_initialized = 0;

static void
reorder_quit()
{
    if (levels_size != 0) {
        free(levels);
        free(var_to_level);
        free(level_to_var);
        levels_count = 0;
        levels_size = 0;
    }

    mtbdd_reorder_initialized = 0;
}

void
sylvan_init_reorder()
{
    sylvan_init_mtbdd();

    if (mtbdd_reorder_initialized) return;
    mtbdd_reorder_initialized = 1;

    sylvan_register_quit(reorder_quit);
    sylvan_gc_add_mark(TASK(mtbdd_gc_mark_managed_refs));
}

/**
 * Function declarations (implementations below)
 */
TASK_DECL_4(int, sylvan_varswap_p1, uint32_t, size_t, size_t, int);
TASK_DECL_4(int, sylvan_varswap_p2, uint32_t, size_t, size_t, volatile int*);

/**
 * Main implementation of variable swapping.
 *
 * Variable swapping consists of two phases. The first phase performs
 * variable swapping on all simple cases. The cases that require node
 * lookups are left marked. The second phase then fixes the marked nodes.
 *
 * If the "recovery" parameter is set, then phase 1 only rehashes nodes that are <var+1>,
 * and phase 2 will not abort on the first error, but try to finish as many nodes as possible.
 *
 * Return values:
 *  0: success
 * -1: cannot rehash in phase 1, no marked nodes remaining
 * -2: cannot rehash in phase 1, and marked nodes remaining
 * -3: cannot rehash in phase 2, no marked nodes remaining
 * -4: cannot create node in phase 2 (so marked nodes remaining)
 * -5: cannot rehash and cannot create node in phase 2
 */
TASK_IMPL_2(int, sylvan_varswap, uint32_t, var, int, recovery)
{
    /*
     * clear hash array first...
     */

    llmsset_clear_hashes(nodes);

    /*
     * phase 1; for each node in the table:
     * - leaf, or not <var> or <var+1>: rehash
     * - marked node: unmark and rehash (for during recovery)
     * - <var+1> node: update to <var> and rehash
     * - independent <var> node: update to <var+1> and rehash
     * - dependent <var> node: mark for phase 2
     */

    int result = CALL(sylvan_varswap_p1, var, 0, nodes->table_size, recovery);

    /*
     * if result 0, then no marked nodes, and we're good
     */
    if (result == 0) return 0;

    /*
     * an error here means that we could not rehash...
     * if we cannot rehash, then we cannot run phase 2!
     * return -1
     */
    if (result == 1) return -1;
    if (result == 3) return -2;

    /*
     * after phase 1, the marked nodes are left to be fixed
     * all other nodes are done
     *
     * phase 2
     * - obtain new <var+1> nodes for marked <var> nodes
     *   (during a recovery, this does not create nodes)
     * - update and rehash <var> nodes
     */

    result = 0;
    result = CALL(sylvan_varswap_p2, var, 0, nodes->table_size, recovery ? NULL : &result);

    /*
     * if there is an error, that means either rehashing did not work
     * or the table was full when creating nodes
     * - result == 1, return -3: could not rehash
     * - result == 2, return -4: could not create node
     * - result == 3, return -5: could not create node and could not rehash
     */
    if (result != 0) return -2 - result;

    /*
     * no error, so we are done
     */

    return 0;
}

/**
 * Custom makenode that doesn't trigger garbage collection.
 * Instead, returns mtbdd_invalid if we can't create the node.
 */
MTBDD
mtbdd_varswap_makenode(uint32_t var, MTBDD low, MTBDD high)
{
    struct mtbddnode n;
    uint64_t index;
    int mark, created;

    if (low == high) return low;

    if (MTBDD_HASMARK(low)) {
        mark = 1;
        low = MTBDD_TOGGLEMARK(low);
        high = MTBDD_TOGGLEMARK(high);
    } else {
        mark = 0;
    }

    mtbddnode_makenode(&n, var, low, high);

    index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) return mtbdd_invalid;

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return mark ? index | mtbdd_complement : index;
}

/**
 * Custom makemapnode that doesn't trigger garbage collection.
 * Instead, returns mtbdd_invalid if we can't create the node.
 */
MTBDD
mtbdd_varswap_makemapnode(uint32_t var, MTBDD low, MTBDD high)
{
    struct mtbddnode n;
    uint64_t index;
    int created;

    // in an MTBDDMAP, the low edges eventually lead to 0 and cannot have a low mark
    assert(!MTBDD_HASMARK(low));

    mtbddnode_makemapnode(&n, var, low, high);

    index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) return mtbdd_invalid;

    if (created) sylvan_stats_count(BDD_NODES_CREATED);
    else sylvan_stats_count(BDD_NODES_REUSED);

    return index;
}

/**
 * Implementation of the first phase of variable swapping.
 * For all nodes that will NOT be changed, rehash.
 * For all <var+1> nodes, make <var> and rehash.
 * For all <var> nodes not depending on <var+1>, make <var+1> and rehash.
 * For all <var> nodes depending on <var+1>, stay <var> and mark. (no rehash)
 * Returns 0 if all good and no nodes marked, or 1 if we were unable to rehash some node,
 * or 2 if we are all good and nodes were marked, or 3 if 1 and 2.
 *
 * This algorithm is also used for the recovery phase 1. This is an identical
 * phase, except marked <var> nodes are unmarked. If the recovery flag is set, then only <var+1>
 * nodes are rehashed.
 */
TASK_IMPL_4(int, sylvan_varswap_p1, uint32_t, var, size_t, first, size_t, count, int, recovery)
{
    /* divide and conquer (if count above BLOCKSIZE) */
    if (count > BLOCKSIZE) {
        SPAWN(sylvan_varswap_p1, var, first, count/2, recovery);
        int res1 = CALL(sylvan_varswap_p1, var, first+count/2, count-count/2, recovery);
        int res2 = SYNC(sylvan_varswap_p1);
        return res1 | res2;
    }

    /* skip buckets 0 and 1 */
    if (first <= 1) {
        count = count + first - 2;
        first = 2;
    }

    // flags
    int error = 0, marked = 0;

    mtbddnode_t node = MTBDD_GETNODE(first);
    const size_t end = first + count;

    for (; first < end; node++, first++) {
        /* skip unused buckets */
        if (!llmsset_is_marked(nodes, first)) continue;
        int rehash = recovery ? 0 : 1;
        if (!mtbddnode_isleaf(node)) {
            uint32_t nvar = mtbddnode_getvariable(node);
            if (nvar == var) {
                if (mtbddnode_getmark(node)) {
                    /* marked node, remove mark and rehash */
                    /* only during recovery */
                    assert(recovery);
                    mtbddnode_setmark(node, 0);
                } else if (mtbddnode_ismapnode(node)) {
                    /* this is not a normal node but a MAP node */
                    MTBDD f0 = mtbddnode_getlow(node);
                    if (f0 == mtbdd_false) {
                        /* we are actually end of chain */
                        mtbddnode_setvariable(node, var+1);
                        if (recovery) rehash = 1; // rehash during recovery
                    } else {
                        /* not end of chain, so f0 is the next in chain */
                        uint32_t vf0 = mtbdd_getvar(f0);
                        if (vf0 > var+1) {
                            /* it wasn't var+1... */
                            mtbddnode_setvariable(node, var+1);
                            if (recovery) rehash = 1; // rehash during recovery
                        } else {
                            /* mark for phase 2 */
                            mtbddnode_setmark(node, 1);
                            rehash = 0;
                            marked = 2;
                        }
                    }
                } else {
                    /* this is a normal <var> node */
                    /* check if it has <var+1> children */
                    int p2 = 0;
                    MTBDD f0 = mtbddnode_getlow(node);
                    if (!mtbdd_isleaf(f0)) {
                        uint32_t vf0 = mtbdd_getvar(f0);
                        if (vf0 == var || vf0 == var+1) p2 = 1;
                    }
                    if (!p2) {
                        MTBDD f1 = mtbddnode_gethigh(node);
                        if (!mtbdd_isleaf(f1)) {
                            uint32_t vf1 = mtbdd_getvar(f1);
                            if (vf1 == var || vf1 == var+1) p2 = 1;
                        }
                    }
                    if (p2) {
                        /* mark for phase 2 */
                        mtbddnode_setmark(node, 1);
                        rehash = 0;
                        marked = 2;
                    } else {
                        mtbddnode_setvariable(node, var+1);
                        if (recovery) rehash = 1; // rehash during recovery
                    }
                }
            } else if (nvar == var+1) {
                /* if <var+1>, then replace with <var> */
                mtbddnode_setvariable(node, var);
            } else {
                /* if not <var> or <var+1>, then rehash */
            }
        }
        if (rehash) {
            if (!llmsset_rehash_bucket(nodes, first)) error = 1;
        }
    }

    return error | marked;
}

/**
 * Implementation of second phase of variable swapping.
 * For all nodes marked in the first phase:
 * - determine F00, F01, F10, F11
 * - obtain nodes F0 [var+1,F00,F10] and F1 [var+1,F01,F11]
 *   (and F0<>F1, trivial proof)
 * - in-place substitute outgoing edges with new F0 and F1
 * - and rehash into hash table
 * Returns 0 if there was no error, or 1 if nodes could not be
 * rehashed, or 2 if nodes could not be created, or 3 if both.
 */
TASK_IMPL_4(int, sylvan_varswap_p2, uint32_t, var, size_t, first, size_t, count, volatile int*, aborttrap)
{
    /* divide and conquer (if count above BLOCKSIZE) */
    if (count > BLOCKSIZE) {
        SPAWN(sylvan_varswap_p2, var, first, count/2, aborttrap);
        int error1 = CALL(sylvan_varswap_p2, var, first+count/2, count-count/2, aborttrap);
        int error2 = SYNC(sylvan_varswap_p2);
        return error1 | error2;
    }

    /* skip buckets 0 and 1 */
    if (first <= 1) {
        count = count + first - 2;
        first = 2;
    }

    mtbddnode_t node = MTBDD_GETNODE(first);
    const size_t end = first + count;

    /* set error = 1 (cannot create) or 2 (cannot rehash) or 3 (both) */
    int error = 0;

    for (; first < end; node++, first++) {
        /* abort on error */
        if (aborttrap != NULL && *aborttrap) return *aborttrap;
        /* skip unused buckets */
        if (!llmsset_is_marked(nodes, first)) continue;
        /* skip leaves */
        if (mtbddnode_isleaf(node)) continue;
        /* skip unmarked nodes */
        if (!mtbddnode_getmark(node)) continue;

        if (mtbddnode_ismapnode(node)) {
            /* this is not a normal node but a MAP node */
            /* we have to swap places with next in chain */
            MTBDD f0 = mtbddnode_getlow(node);
            MTBDD f1 = mtbddnode_gethigh(node);
            mtbddnode_t n0 = MTBDD_GETNODE(f0);
            MTBDD f00 = node_getlow(f0, n0);
            MTBDD f01 = node_gethigh(f0, n0);
            f0 = mtbdd_varswap_makemapnode(var+1, f00, f1);
            if (f0 == mtbdd_invalid) {
                error |= 2;
                if (aborttrap != NULL) *aborttrap |= 2;
            } else {
                mtbddnode_makemapnode(node, var, f0, f01);
                if (!llmsset_rehash_bucket(nodes, first)) {
                    error |= 1;
                    if (aborttrap != NULL) *aborttrap |= 1;
                }
            }
        } else {
            /* obtain cofactors */
            MTBDD f0 = mtbddnode_getlow(node);
            MTBDD f1 = mtbddnode_gethigh(node);
            MTBDD f00, f01, f10, f11;
            f00 = f01 = f0;
            if (!mtbdd_isleaf(f0)) {
                mtbddnode_t n0 = MTBDD_GETNODE(f0);
                if (mtbddnode_getvariable(n0) == var) {
                    f00 = node_getlow(f0, n0);
                    f01 = node_gethigh(f0, n0);
                }
            }
            f10 = f11 = f1;
            if (!mtbdd_isleaf(f1)) {
                mtbddnode_t n1 = MTBDD_GETNODE(f1);
                if (mtbddnode_getvariable(n1) == var) {
                    f10 = node_getlow(f1, n1);
                    f11 = node_gethigh(f1, n1);
                }
            }
            /* compute new f0 and f1 */
            f0 = mtbdd_varswap_makenode(var+1, f00, f10);
            f1 = mtbdd_varswap_makenode(var+1, f01, f11);
            if (f0 == mtbdd_invalid || f1 == mtbdd_invalid) {
                error |= 2;
                if (aborttrap != NULL) *aborttrap |= 2;
            } else {
                /* update node, which also removes the mark */
                mtbddnode_makenode(node, var, f0, f1);
                if (!llmsset_rehash_bucket(nodes, first)) {
                    error |= 1;
                    if (aborttrap != NULL) *aborttrap |= 1;
                }
            }
        }
    }

    return error;
}

/**
 * Very simply varswap, no iterative recovery, no nodes table resizing.
 * returns 0 if it worked.
 * returns 1 if we had to rollback.
 * aborts with exit(-1) if rehashing didn't work during recovery
 */
TASK_IMPL_1(int, sylvan_simple_varswap, uint32_t, var)
{
    sylvan_clear_cache(); // do this only once if you have multiple varswaps
    sylvan_clear_and_mark(); // recommended before every normal (not recovery) varswap

    int res, res2;

    res = sylvan_varswap(var, 0);
    if (res == 0) {
        // update var_to_level and level_to_var
        level_to_var[var_to_level[var]] = var+1;
        level_to_var[var_to_level[var+1]] = var;
        uint32_t save = var_to_level[var];
        var_to_level[var] = var_to_level[var+1];
        var_to_level[var+1] = save;
        return 0;
    }

    // if varswap fails, then the table is full (likely: could not [re]hash a node)
    // if we do clean-and-mark now, we may lose original <var+1> nodes!
    // the best course of action depends on how far we got in the varswap

    if (res == -1 || res == -2) {
        // we failed in phase 1... not a nice situation (table full before creating new nodes)
        // but we can try to rollback (without setting the recovery settings)
        res2 = sylvan_varswap(var, 0);
        if (res2 == 0) return 1;
        // sanity check, if we only did phase 1, then rollback should also do only phase 1
        assert(res2 == -1);
        // if the original varswap failed, probably the rollback failed too...
    } else {
        // we failed in phase 2, so there are probably some new nodes, probably some marked nodes
        // very likely rehashing all live nodes will fail, so only rehash <var+1> nodes
        res2 = sylvan_varswap(var, 1);
        // sanity check, results -4 and -5 are not possible on the first rollback
        assert(res2 != -4 && res2 != -5);
    }

    // CASE A: all nodes are updated, but not all are rehashed
    // - if original varswap only did phase 1
    // - if rollback succeeded phase 1
    // - if rollback failed phase 1 but without marked nodes
    // Clear-and-mark, then rehash again. If that fails, we *should* resize.
    //
    // CASE B: not all nodes are updated (recovery returned -2)
    // even rehashing only the <var+1> nodes failed (very very unlikely!)

    if (res2 == -2) {
        // unless we resize or perform an iterative recovery approach, this is it...
        fprintf(stderr, "sylvan: cannot rehash during sifting!\n");
        exit(-1);
    }

    sylvan_clear_and_mark();
    sylvan_rehash_all(); // this will abort on failure...
    return 1;
}
