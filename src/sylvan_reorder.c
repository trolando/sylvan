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

#include <sys/time.h>
#include <stdatomic.h>
#include "sylvan_align.h"

#define STATS 0 // useful information w.r.t. dynamic reordering for debugging
#define INFO 1  // useful information w.r.t. dynamic reordering

VOID_TASK_DECL_1(sylvan_reorder_stop_world, reordering_type_t)

#define sylvan_reorder_stop_world(type) RUN(sylvan_reorder_stop_world, type)

TASK_DECL_2(reorder_result_t, sylvan_sift, uint32_t, uint32_t)

#define sylvan_sift(v, limit) CALL(sylvan_sift, v, limit)

TASK_DECL_2(reorder_result_t, sylvan_bounded_sift, uint32_t, uint32_t)

#define sylvan_bounded_sift(v, limit) CALL(sylvan_bounded_sift, v, limit)

void sylvan_init_reorder()
{
    if(reorder_db != NULL && reorder_db->is_initialised) return;
    reorder_db = reorder_db_init();
}

void sylvan_quit_reorder()
{
    if(!reorder_db->is_initialised) return;
    reorder_db_deinit(reorder_db);
}

void sylvan_set_reorder_nodes_threshold(uint32_t threshold)
{
    if(!reorder_db->is_initialised) return;
    assert(threshold > 0);
    reorder_db->config.threshold = threshold;
}

void sylvan_set_reorder_maxgrowth(float max_growth)
{
    if(!reorder_db->is_initialised) return;
    assert(max_growth > 1.0f);
    reorder_db->config.max_growth = max_growth;
}

void sylvan_set_reorder_maxswap(uint32_t max_swap)
{
    if(!reorder_db->is_initialised) return;
    assert(max_swap > 1);
    reorder_db->config.max_swap = max_swap;
}

void sylvan_set_reorder_maxvar(uint32_t max_var)
{
    if(!reorder_db->is_initialised) return;
    assert(max_var > 1);
    reorder_db->config.max_var = max_var;
}

void sylvan_set_reorder_timelimit_min(double time_limit)
{
    if(!reorder_db->is_initialised) return;
    assert(time_limit > 0);
    sylvan_set_reorder_timelimit_sec(time_limit * 60);
}

void sylvan_set_reorder_timelimit_sec(double time_limit)
{
    if(!reorder_db->is_initialised) return;
    assert(time_limit > 0);
    sylvan_set_reorder_timelimit_ms(time_limit * 1000);
}

void sylvan_set_reorder_timelimit_ms(double time_limit)
{
    if(!reorder_db->is_initialised) return;
    assert(time_limit > 0);
    reorder_db->config.time_limit_ms = time_limit;
}

void sylvan_set_reorder_verbose(int is_verbose)
{
    if(!reorder_db->is_initialised) return;
    assert(is_verbose >= 0);
    reorder_db->config.print_stat = is_verbose;
}

void sylvan_set_reorder_type(reordering_type_t type)
{
    if(!reorder_db->is_initialised) return;
    reorder_db->config.type = type;
}

void sylvan_set_reorder_print(bool is_on)
{
    reorder_db->config.print_stat = is_on;
}

TASK_IMPL_1(reorder_result_t, sylvan_reorder_perm, const uint32_t*, permutation)
{
    sylvan_pre_reorder(SYLVAN_REORDER_SIFT);
    if (!reorder_db->is_initialised) return SYLVAN_REORDER_NOT_INITIALISED;
    reorder_result_t res = SYLVAN_REORDER_SUCCESS;
    int is_identity = 1;

    // check if permutation is identity
    for (size_t level = 0; level < reorder_db->levels.count; level++) {
        if (permutation[level] != reorder_db->levels.level_to_order[level]) {
            is_identity = 0;
            break;
        }
    }
    if (is_identity) return res;

    for (size_t level = 0; level < reorder_db->levels.count; ++level) {
        uint32_t var = permutation[level];
        uint32_t pos = levels_order_to_level(&reorder_db->levels, var);
        for (; pos < level; pos++) {
            res = sylvan_varswap(pos);
            if (!sylvan_reorder_issuccess(res)) return res;
        }
        for (; pos > level; pos--) {
            res = sylvan_varswap(pos - 1);
            if (!sylvan_reorder_issuccess(res)) return res;
        }
        if (!sylvan_reorder_issuccess(res)) break;
    }

    sylvan_post_reorder();
    return res;
}

void sylvan_test_reduce_heap()
{
    if (reorder_db == NULL || reorder_db->is_initialised == false) return;
    if (llmsset_count_marked(nodes) >= reorder_db->config.size_threshold && reorder_db->call_count < SYLVAN_REORDER_LIMIT) {
        sylvan_reorder_stop_world(reorder_db->config.type);
    }
}

void sylvan_reduce_heap(reordering_type_t type)
{
    if (reorder_db == NULL || reorder_db->is_initialised == false) return;
    sylvan_reorder_stop_world(type);
}

/**
 * This variable is used for a cas flag so only
 * one reordering runs at one time
 */
static _Atomic (int) re;


VOID_TASK_IMPL_1(sylvan_reorder_stop_world, reordering_type_t, type)
{
    reorder_result_t result = SYLVAN_REORDER_SUCCESS;
    if (!reorder_db->is_initialised) result = SYLVAN_REORDER_NOT_INITIALISED;
    if (reorder_db->levels.count < 1) result = SYLVAN_REORDER_NO_REGISTERED_VARS;
    if (sylvan_reorder_issuccess(result) == 0) {
        sylvan_print_reorder_res(result);
        return;
    }
    int zero = 0;
    if (atomic_compare_exchange_strong(&re, &zero, 1)) {
        sylvan_pre_reorder(type);
        switch (type) {
            case SYLVAN_REORDER_SIFT:
                result = NEWFRAME(sylvan_sift, 0, 0);
                break;
            case SYLVAN_REORDER_BOUNDED_SIFT:
                result = NEWFRAME(sylvan_bounded_sift, 0, 0);
                break;
        }
        re = 0;
        sylvan_post_reorder();
        if (sylvan_reorder_issuccess(result) == 0) {
            sylvan_print_reorder_res(result);
        }
    } else {
        /* wait for new frame to appear */
        while (atomic_load_explicit(&lace_newframe.t, memory_order_relaxed) == 0) {}
        lace_yield(__lace_worker, __lace_dq_head);
    }
}

TASK_IMPL_2(reorder_result_t, sylvan_sift, uint32_t, low, uint32_t, high)
{
    // if high == 0, then we sift all variables
    if (high == 0) high = reorder_db->levels.count - 1;

    // count all variable levels (parallel...)
    size_t level_counts[reorder_db->levels.count];
    for (size_t i = 0; i < reorder_db->levels.count; i++) {
        level_counts[i] = mrc_var_nnodes_get(&reorder_db->mrc, reorder_db->levels.level_to_order[i]);
    }
    // mark and sort variable levels based on the threshold
    int ordered_levels[reorder_db->levels.count];
    levels_mark_threshold(&reorder_db->levels, ordered_levels, level_counts, reorder_db->config.threshold);
    levels_gnome_sort(&reorder_db->levels, ordered_levels, level_counts);

    reorder_result_t res = SYLVAN_REORDER_SUCCESS;

    size_t cursize = get_nodes_count();

    for (int i = 0; i < (int) reorder_db->levels.count; i++) {
        int lvl = ordered_levels[i];
        if (lvl < 0) break; // done
        size_t pos = reorder_db->levels.level_to_order[lvl];

        size_t bestpos = pos;
        size_t bestsize = cursize;

        if (pos < low || pos > high) continue;

        reorder_db->config.varswap_count = 0;

        if ((pos - low) > (high - pos)) {
            // we are in the lower half of the levels, so sift down first and then up
            // sifting down
            for (; pos < high; pos++) {
                res = sylvan_varswap(pos);
                if (sylvan_reorder_issuccess(res) == 0) break;
                cursize = get_nodes_count();
                reorder_db->config.varswap_count++;
                if (should_terminate_sifting(&reorder_db->config)) break;
                if ((double) cursize > (double) bestsize * reorder_db->config.max_growth) {
                    pos++;
                    break;
                }
                if (cursize < bestsize) {
                    bestsize = cursize;
                    bestpos = pos;
                }
            }
            if (sylvan_reorder_issuccess(res)) {
                // sifting up
                for (; pos > low; pos--) {
                    res = sylvan_varswap(pos - 1);
                    if (sylvan_reorder_issuccess(res) == 0) break;
                    cursize = get_nodes_count();
                    reorder_db->config.varswap_count++;
                    if (should_terminate_sifting(&reorder_db->config)) break;
                    if ((double) cursize > (double) bestsize * reorder_db->config.max_growth) {
                        pos--;
                        break;
                    }
                    if (cursize < bestsize) {
                        bestsize = cursize;
                        bestpos = pos;
                    }
                }
            }
        } else {
            // we are in the upper half of the levels, so sift up first and then down
            // sifting up
            for (; pos > low; pos--) {
                res = sylvan_varswap(pos - 1);
                if (sylvan_reorder_issuccess(res) == 0) break;
                cursize = get_nodes_count();
                reorder_db->config.varswap_count++;
                if (should_terminate_sifting(&reorder_db->config)) break;
                if ((double) cursize > (double) bestsize * reorder_db->config.max_growth) {
                    pos--;
                    break;
                }
                if (cursize < bestsize) {
                    bestsize = cursize;
                    bestpos = pos;
                }

            }
            if (sylvan_reorder_issuccess(res)) {
                // sifting down
                for (; pos < high; pos++) {
                    res = sylvan_varswap(pos);
                    if (sylvan_reorder_issuccess(res) == 0) break;
                    cursize = get_nodes_count();
                    reorder_db->config.varswap_count++;
                    if (should_terminate_sifting(&reorder_db->config)) break;
                    if ((double) cursize > (double) bestsize * reorder_db->config.max_growth) {
                        pos++;
                        break;
                    }
                    if (cursize < bestsize) {
                        bestsize = cursize;
                        bestpos = pos;
                    }
                }
            }
        }
        reorder_result_t old_res = res;

        // optimum variable position restoration
        for (; pos < bestpos; pos++) {
            res = sylvan_varswap(pos);
            if (sylvan_reorder_issuccess(res) == 0) break;
            reorder_db->config.varswap_count++;
        }
        for (; pos > bestpos; pos--) {
            res = sylvan_varswap(pos - 1);
            if (sylvan_reorder_issuccess(res) == 0) break;
            reorder_db->config.varswap_count++;
        }

        cursize = get_nodes_count();

        if (!sylvan_reorder_issuccess(res) || !sylvan_reorder_issuccess(old_res)) break;
        reorder_db->config.total_num_var++;

        // if we managed to reduce size call progress hooks
        if (bestsize < cursize) {
            reorder_db_call_progress_hooks();
        }

        if (should_terminate_reordering(&reorder_db->config)) break;
    }

    return res;
}

TASK_IMPL_2(reorder_result_t, sylvan_bounded_sift, uint32_t, low, uint32_t, high)
{
    // if high == 0, then we sift all variables
    if (high == 0) high = reorder_db->levels.count - 1;

    // count all variable levels
    size_t level_counts[reorder_db->levels.count];
    for (size_t i = 0; i < reorder_db->levels.count; i++) {
        level_counts[i] = mrc_var_nnodes_get(&reorder_db->mrc, reorder_db->levels.level_to_order[i]);
    }
    // mark and sort variable levels based on the threshold
    int ordered_levels[reorder_db->levels.count];
    levels_mark_threshold(&reorder_db->levels, ordered_levels, level_counts, reorder_db->config.threshold);
    levels_gnome_sort(&reorder_db->levels, ordered_levels, level_counts);

    // remember the order of the levels, since it will change during the sifting
    uint32_t level_to_order[reorder_db->levels.count];
    for (size_t i = 0; i < reorder_db->levels.count; i++) {
        level_to_order[i] = reorder_db->levels.level_to_order[i];
    }

    reorder_result_t res = SYLVAN_REORDER_SUCCESS;
    sifting_state_t s_state;

    s_state.pos = 0;
    s_state.best_pos = 0;
    s_state.size = (int) get_nodes_count();
    s_state.best_size = s_state.size;
    s_state.low = low;
    s_state.high = high;

#if STATS
    printf("\n");
    interact_print(&reorder_db->matrix);

    for (size_t i = 0; i < levels_count_get(&reorder_db->levels); i++) {
        int lvl = ordered_levels[i];
        printf("level %d \t has %zu nodes\n", lvl, level_counts[lvl]);
    }
    printf("\n");
#endif

    for (int i = 0; i < (int) reorder_db->levels.count; i++) {
        int lvl = ordered_levels[i];
        if (lvl == -1) break;
        s_state.pos = reorder_db->levels.order_to_level[level_to_order[lvl]];
        if (s_state.pos < s_state.low || s_state.pos > s_state.high) continue;

        reorder_db->config.varswap_count = 0;

        s_state.best_pos = s_state.pos;
        s_state.best_size = s_state.size;
#if STATS
        printf("sifting level %d with pos %d\n", s_state.pos, lvl);
#endif
        if (s_state.pos == s_state.low) {
            res = sylvan_siftdown(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
            // at this point pos --> high unless bounding occurred.
            // move backward and stop at best position.
            res = sylvan_siftback(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
        } else if (s_state.pos == s_state.high) {
            res = sylvan_siftup(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
            // at this point pos --> low unless bounding occurred.
            // move backward and stop at best position.
            res = sylvan_siftback(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
        } else if ((s_state.pos - s_state.low) > (s_state.high - s_state.pos)) {
            // we are in the lower half, so sift down first and then up
            res = sylvan_siftdown(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
            res = sylvan_siftup(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
            res = sylvan_siftback(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
        } else {
            // we are in the upper half, so sift up first and then down
            res = sylvan_siftup(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
            res = sylvan_siftdown(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
            res = sylvan_siftback(&s_state);
            if (!sylvan_reorder_issuccess(res)) goto siftingFailed;
        }

        if (should_terminate_reordering(&reorder_db->config)) break;

        // if we managed to reduce size call progress hooks
        if (s_state.best_size < s_state.size) {
            reorder_db_call_progress_hooks();
        }

        reorder_db->config.total_num_var++;

#if STATS
        if (i > 1) exit(1);
#endif
        roaring_bitmap_run_optimize(reorder_db->mrc.node_ids);

        continue;

        siftingFailed:
#if INFO
            sylvan_print_reorder_res(res);
#endif
        if (res == SYLVAN_REORDER_P2_CREATE_FAIL || res == SYLVAN_REORDER_P3_CLEAR_FAIL ||
            res == SYLVAN_REORDER_NOT_ENOUGH_MEMORY) {

            sylvan_post_reorder();
            sylvan_gc();
            sylvan_pre_reorder(SYLVAN_REORDER_BOUNDED_SIFT);

            return sylvan_bounded_sift(low, high);
        } else {
            return res;
        }
    }

    return res;
}