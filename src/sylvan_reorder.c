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
static uint32_t *var_to_level = NULL;  // get the level of a "real variable"
static uint32_t *level_to_var = NULL;  // get the "real variable" of a level
static MTBDD *levels = NULL;           // array holding the 1-node BDD for each level
static size_t levels_count = 0;        // number of created levels
static size_t levels_size = 0;         // size of the 3 arrays

/**
 * Create the next level and return the BDD representing the variable (ithlevel)
 * The BDDs representing managed levels are always kept during garbage collection.
 * NOTE: not currently thread-safe.
 */
MTBDD
mtbdd_newlevel(void)
{
    mtbdd_newlevels(1);
    return levels[levels_count-1];
}

/**
 * Create the next <amount> levels
 * The BDDs representing managed levels are always kept during garbage collection.
 * NOTE: not currently thread-safe.
 */
void
mtbdd_newlevels(size_t amount)
{
    if (levels_count + amount >= levels_size) {
#if 0
        if (levels_size == 0) levels_size = 1; // start here
        while (levels_count + amount >= levels_size) levels_size *= 2;
#else
        // just round up to the next multiple of 64 value
        // probably better than doubling anyhow...
        levels_size = (levels_count + amount + 63) & (~63LL);
#endif
        levels = realloc(levels, sizeof(MTBDD[levels_size]));
        var_to_level = realloc(var_to_level, sizeof(uint32_t[levels_size]));
        level_to_var = realloc(level_to_var, sizeof(uint32_t[levels_size]));
    }
    for (size_t i=0; i<amount; i++) {
        // reminder: makenode(var, low, high)
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
 * Returns mtbdd_invalid for unmanaged levels.
 */
MTBDD
mtbdd_ithlevel(uint32_t level)
{
    if (level < levels_count) return levels[level_to_var[level]];
    else return mtbdd_invalid;
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

/**
 * This function is called during garbage collection and
 * marks all managed level BDDs so they are kept.
 */
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
VOID_TASK_DECL_3(sylvan_varswap_p0, uint32_t, size_t, size_t);
TASK_DECL_3(uint64_t, sylvan_varswap_p1, uint32_t, size_t, size_t);
VOID_TASK_DECL_4(sylvan_varswap_p2, uint32_t, size_t, size_t, volatile int*);

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
 * -1: failure (needs recovery)
 */
TASK_IMPL_2(int, sylvan_varswap, uint32_t, var, int, recovery)
{
    // first clear hashes of nodes with <var> and <var+1>
    CALL(sylvan_varswap_p0, var, 0, nodes->table_size);
    // handle all trivial cases, mark cases that are not trivial
    uint64_t marked_count = CALL(sylvan_varswap_p1, var, 0, nodes->table_size);
    if (marked_count == 0) return 0;
    // do the not so trivial cases (creates new nodes)
    int flag_full = 0;
    CALL(sylvan_varswap_p2, var, 0, nodes->table_size, &flag_full);
    return flag_full ? -1 : 0;
    (void)recovery;
}

/**
 * Simple variable swap, no iterative recovery or nodes table resizing.
 * Swaps <var> and <var+1>.
 * Returns 0 if swapped, 1 if not swapped (rolled back) but healthy.
 * Aborts with exit(-1) if the table is in bad shape.
 */
TASK_IMPL_1(int, sylvan_simple_varswap, uint32_t, var)
{
    // ensure that the cache is cleared
    sylvan_clear_cache();

    // first clear hashes of nodes with <var> and <var+1>
    CALL(sylvan_varswap_p0, var, 0, nodes->table_size);
    // handle all trivial cases, mark cases that are not trivial
    uint64_t marked_count = CALL(sylvan_varswap_p1, var, 0, nodes->table_size);
    if (marked_count != 0) {
        // do the not so trivial cases (creates new nodes)
        int flag_full = 0;
        CALL(sylvan_varswap_p2, var, 0, nodes->table_size, &flag_full);
        if (flag_full) {
            // clear hashes again of nodes with <var> and <var+1>
            CALL(sylvan_varswap_p0, var, 0, nodes->table_size);
            // handle all trivial cases, mark cases that are not trivial
            uint64_t marked_count = CALL(sylvan_varswap_p1, var, 0, nodes->table_size);
            if (marked_count != 0){
                // do the not so trivial cases (but won't create new nodes this time)
                flag_full = 0;
                CALL(sylvan_varswap_p2, var, 0, nodes->table_size, &flag_full);
                if (flag_full) {
                    // actually, we should not see this!
                    fprintf(stderr, "sylvan: recovery varswap failed!\n");
                    exit(-1);
                }
            }
            return 1;
        }
    }

    // update var_to_level and level_to_var
    level_to_var[var_to_level[var]] = var+1;
    level_to_var[var_to_level[var+1]] = var;
    uint32_t save = var_to_level[var];
    var_to_level[var] = var_to_level[var+1];
    var_to_level[var+1] = save;
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
 * Initialize variable swapping.
 * Removes exactly the nodes that will be changed from the hash table.
 */
VOID_TASK_IMPL_3(sylvan_varswap_p0, uint32_t, var, size_t, first, size_t, count)
{
    // go recursive if count above BLOCKSIZE
    if (count > BLOCKSIZE) {
        SPAWN(sylvan_varswap_p0, var, first, count/2);
        CALL(sylvan_varswap_p0, var, first+count/2, count-count/2);
        SYNC(sylvan_varswap_p0);
        return;
    }

    // skip buckets 0 and 1
    if (first < 2) {
        count = count + first - 2;
        first = 2;
    }

    const size_t end = first + count;

    for (; first < end; first++) {
        if (!llmsset_is_marked(nodes, first)) continue; // an unused bucket
        mtbddnode_t node = MTBDD_GETNODE(first);
        if (mtbddnode_isleaf(node)) continue; // a leaf
        uint32_t nvar = mtbddnode_getvariable(node);
        if (nvar == var || nvar == (var+1)) llmsset_clear_one(nodes, first);
    }
}

/**
 * Implementation of the first phase of variable swapping.
 * For all <var+1> nodes, make <var> and rehash.
 * For all <var> nodes not depending on <var+1>, make <var+1> and rehash.
 * For all <var> nodes depending on <var+1>, stay <var> and mark. (no rehash)
 * Returns number of marked nodes left.
 *
 * This algorithm is also used for the recovery phase 1. This is an identical
 * phase, except marked <var> nodes are unmarked. If the recovery flag is set, then only <var+1>
 * nodes are rehashed.
 */
TASK_IMPL_3(uint64_t, sylvan_varswap_p1, uint32_t, var, size_t, first, size_t, count)
{
    // go recursive if count above BLOCKSIZE
    if (count > BLOCKSIZE) {
        SPAWN(sylvan_varswap_p1, var, first, count/2);
        uint64_t res1 = CALL(sylvan_varswap_p1, var, first+count/2, count-count/2);
        uint64_t res2 = SYNC(sylvan_varswap_p1);
        return res1 + res2;
    }

    // count number of marked
    uint64_t marked = 0;

    // skip buckets 0 and 1
    if (first < 2) {
        count = count + first - 2;
        first = 2;
    }

    const size_t end = first + count;

    for (; first < end; first++) {
        if (!llmsset_is_marked(nodes, first)) continue; // an unused bucket
        mtbddnode_t node = MTBDD_GETNODE(first);
        if (mtbddnode_isleaf(node)) continue; // a leaf
        uint32_t nvar = mtbddnode_getvariable(node);
        if (nvar != (var+1)) {
            // if <var+1>, then replace with <var> and rehash
            mtbddnode_setvariable(node, var);
            llmsset_rehash_bucket(nodes, first);
            continue;
        } else if (nvar != var) {
            continue; // not <var> or <var+1>
        }
        // level = <var>
        if (mtbddnode_getmark(node)) {
            // marked node, remove mark and rehash (we are apparently recovering)
            mtbddnode_setmark(node, 0);
            llmsset_rehash_bucket(nodes, first);
            continue;
        }
        if (mtbddnode_ismapnode(node)) {
            MTBDD f0 = mtbddnode_getlow(node);
            if (f0 == mtbdd_false) {
                // we are at the end of a chain
                mtbddnode_setvariable(node, var+1);
                llmsset_rehash_bucket(nodes, first);
            } else {
                // not the end of a chain, so f0 is the next in chain
                uint32_t vf0 = mtbdd_getvar(f0);
                if (vf0 > var+1) {
                    // next in chain wasn't <var+1>...
                    mtbddnode_setvariable(node, var+1);
                    llmsset_rehash_bucket(nodes, first);
                } else {
                    // mark for phase 2
                    mtbddnode_setmark(node, 1);
                    marked++;
                }
            }
        } else {
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
                // mark for phase 2
                mtbddnode_setmark(node, 1);
                marked++;
            } else {
                mtbddnode_setvariable(node, var+1);
                llmsset_rehash_bucket(nodes, first);
            }
        }
    }

    return marked;
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
VOID_TASK_IMPL_4(sylvan_varswap_p2, uint32_t, var, size_t, first, size_t, count, volatile int*, flag_full)
{
    /* divide and conquer (if count above BLOCKSIZE) */
    if (count > BLOCKSIZE) {
        SPAWN(sylvan_varswap_p2, var, first, count/2, flag_full);
        CALL(sylvan_varswap_p2, var, first+count/2, count-count/2, flag_full);
        SYNC(sylvan_varswap_p2);
        return;
    }

    /* skip buckets 0 and 1 */
    if (first < 2) {
        count = count + first - 2;
        first = 2;
    }

    const size_t end = first + count;

    for (; first < end; first++) {
        if (*flag_full) return; // the table is full
        if (!llmsset_is_marked(nodes, first)) continue; // an unused bucket
        mtbddnode_t node = MTBDD_GETNODE(first);
        if (mtbddnode_isleaf(node)) continue; // a leaf
        if (!mtbddnode_getmark(node)) continue; // an unmarked node

        if (mtbddnode_ismapnode(node)) {
            // it is a map node, swap places with next in chain
            MTBDD f0 = mtbddnode_getlow(node);
            MTBDD f1 = mtbddnode_gethigh(node);
            mtbddnode_t n0 = MTBDD_GETNODE(f0);
            MTBDD f00 = node_getlow(f0, n0);
            MTBDD f01 = node_gethigh(f0, n0);
            f0 = mtbdd_varswap_makemapnode(var+1, f00, f1);
            if (f0 == mtbdd_invalid) {
                *flag_full = 1;
                return;
            } else {
                mtbddnode_makemapnode(node, var, f0, f01);
                llmsset_rehash_bucket(nodes, first);
            }
        } else {
            // obtain cofactors
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
            // compute new f0 and f1
            f0 = mtbdd_varswap_makenode(var+1, f00, f10);
            f1 = mtbdd_varswap_makenode(var+1, f01, f11);
            if (f0 == mtbdd_invalid || f1 == mtbdd_invalid) {
                *flag_full = 1;
                return;
            } else {
                // update node, which also removes the mark
                mtbddnode_makenode(node, var, f0, f1);
                llmsset_rehash_bucket(nodes, first);
            }
        }
    }
}

/**
 * Count the number of nodes per real variable level.
 *
 * Results are stored atomically in arr.
 *
 * To make this somewhat scalable, we use a standard binary reduction pattern with local arrays...
 * Fortunately, we only do this once per call to dynamic variable reordering.
 */
VOID_TASK_3(sylvan_count_nodes, size_t*, arr, size_t, first, size_t, count)
{
    if (count > 4096) {
        /* 4096, because that is not very small, and not very large */
        /* typical kind of parameter that is open to tweaking, though I don't expect it matters so much */
        /* too small is bad for the atomic operations, too large is bad for work-stealing */
        /* with 2^20 - 2^25 nodes table size, this is 256 - 8192 tasks */
        SPAWN(sylvan_count_nodes, arr, first, count/2);
        CALL(sylvan_count_nodes, arr, first+count/2, count-count/2);
        SYNC(sylvan_count_nodes);
    } else {
        size_t tmp[levels_count], i;
        for (i=0; i<levels_count; i++) tmp[i] = 0;

        mtbddnode_t node = MTBDD_GETNODE(first);
        const size_t end = first + count;

        for (; first < end; node++, first++) {
            /* skip unused buckets */
            if (!llmsset_is_marked(nodes, first)) continue;
            /* skip leaves */
            if (mtbddnode_isleaf(node)) continue;
            /* update on real variable */
            tmp[mtbddnode_getvariable(node)]++;
        }

        /* these are atomic operations on a hot location with false sharing inside another
           thread's program stack... can't get much worse! */
        for (i=0; i<levels_count; i++) __sync_add_and_fetch(&arr[i], tmp[i]);
    }
}

/**
 * Sifting in CUDD:
 * First: obtain number of nodes per variable, then sort.
 * Then perform sifting until max var, or termination callback, or time limit
 * Only variables between "lower" and "upper" that are not "bound"
 *
 * Sifting a variable between "low" and "high:
 * go to closest end first.
 * siftingUp/siftingDown --> siftingBackward
 *
 * Parameters
 * - siftMaxVar - maximum number of vars sifted
 *     default: 1000
 * - siftMaxSwap - maximum number of swaps
 *     default: 2000000
 * - double maxGrowth - some maximum % growth (from the start of a sift of a part. variable)
 *     default: 1.2
 * if a lower size is found, the limitSize is updated...
 */

TASK_IMPL_2(int, sylvan_sifting, uint32_t, low, uint32_t, high)
{
    // SHOULD run first gc

    if (high == 0) {
        high = levels_count-1;
    }

    size_t before_size = llmsset_count_marked(nodes);

    // now count all variable levels (parallel...)
    size_t level_counts[levels_count];
    for (size_t i=0; i<levels_count; i++) level_counts[i] = 0;
    CALL(sylvan_count_nodes, level_counts, 0, nodes->table_size);
    // for (size_t i=0; i<levels_count; i++) printf("Level %zu has %zu nodes\n", i, level_counts[i]);

    // we want to sort it
    int level[levels_count];
    for (unsigned int i=0; i<levels_count; i++) {
        if (level_counts[level_to_var[i]] < 128) /* threshold */ level[i] = -1;
        else level[i]=i;
    }

    // just use gnome sort because meh
    unsigned int i=1, j=2;
    while (i < levels_count) {
        long p = level[i-1] == -1 ? -1 : (long)level_counts[level_to_var[level[i-1]]];
        long q = level[i] == -1 ? -1 : (long)level_counts[level_to_var[level[i]]];
        if (p < q) {
            int t = level[i];
            level[i] = level[i-1];
            level[i-1] = t;
            if (--i) continue;
        }
        i = j++;
    }

    /*
    printf("chosen order: ");
    for (size_t i=0; i<levels_count; i++) printf("%d ", level[i]);
    printf("\n");
    */

    // sift a thing
/*
    int cur_var = level_to_var[lvl];
    int best = lvl;
  */

    size_t cursize = llmsset_count_marked(nodes);

    for (unsigned int i=0; i<levels_count; i++) {
        int lvl = level[i];
        if (lvl == -1) break; // done
        size_t pos = level_to_var[lvl];

        // printf("now moving level %u, currently at position %zu\n", lvl, pos);

        size_t bestsize = cursize, bestpos = pos;
        size_t oldsize = cursize, oldpos = pos;

        for (; pos<high; pos++) {
            if (CALL(sylvan_simple_varswap, pos) != 0) {
                printf("UH OH\n");
                exit(-1);
            }
            size_t after = llmsset_count_marked(nodes);
            // printf("swap(DN): from %zu to %zu\n", cursize, after);
            cursize = after;
            if (cursize < bestsize) {
                bestsize = cursize;
                bestpos = pos;
            }
            if (cursize >= 2*bestsize) break;
        }
        for (; pos>low; pos--) {
            if (CALL(sylvan_simple_varswap, pos-1) != 0) {
                printf("UH OH\n");
                exit(-1);
            }
            size_t after = llmsset_count_marked(nodes);
            // printf("swap(UP): from %zu to %zu\n", cursize, after);
            cursize = after;
            if (cursize < bestsize) {
                bestsize = cursize;
                bestpos = pos;
            }
            if (cursize >= 2*bestsize) break;
        }
        printf("best: %zu (old %zu) at %zu (old %zu)\n", bestpos, oldpos, bestsize, oldsize);
        for (; pos<bestpos; pos++) {
            if (CALL(sylvan_simple_varswap, pos) != 0) {
                printf("UH OH\n");
                exit(-1);
            }
        }
        for (; pos>bestpos; pos--) {
            if (CALL(sylvan_simple_varswap, pos-1) != 0) {
                printf("UH OH\n");
                exit(-1);
            }
        }
            
    }

    /*for (size_t i=0; i<levels_count; i++) level_counts[i] = 0;
    CALL(sylvan_count_nodes, level_counts, 0, nodes->table_size);
    for (size_t i=0; i<levels_count; i++) printf("Level %zu has %zu nodes\n", i, level_counts[i]);*/
    // only keep variables between low and high (if high != 0)
/*
    // sort levels by count
    int n_vars = high-low+1;


    // now in a loop from i to max_sift_vars...
    size_t count = max_sift_vars < nvars ? max_sift_vars : nvars;
    for (size_t i=0; i<count; i++) {
        // check terminating condition (number of swaps)
        

        // set x to next var

        // find closest boundary (bot or top)

        // loop to closest boundary, remember best size and position. (best_size, best_y)
        // terminate that loop given heuristics

        // loop back to x, then loop to furthest boundary, same story.

        // loop to best y.
        //
        //
        //
        //
        //
        //
    }

*/

    size_t after_size = llmsset_count_marked(nodes);
    printf("Result of sifting: from %zu to %zu nodes.\n", before_size, after_size);

    return 0;
}

