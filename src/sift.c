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
 * The sifting driver.
 *
 * This is the whole of Sylvan's variable reordering policy. Everything below
 * it -- the reorder session, the adjacent variable swap, the backend index
 * surgery -- is mechanism, and none of it decides anything. What is decided
 * here is which variables to move, in what order, how far, and where to leave
 * them.
 *
 * THE ALGORITHM is Rudell's sifting, in the shape CUDD gave it. Count how many
 * nodes each variable carries; take the variables in order of that count, most
 * populous first, because a variable with few nodes cannot save many wherever
 * it goes. Move each one through the order one adjacent swap at a time, first
 * to whichever end of the order it is nearer -- so that when the growth bound
 * cuts the excursion short, it is the cheaper half that was spent -- then all
 * the way to the other end, and finally back to the position where the forest
 * was smallest. The second leg covers the whole range, so the best position is
 * found even though the first leg is never revisited.
 *
 * WHAT MAKES IT CHEAP. Sifting scores a position by the size of the forest,
 * and the obvious implementation recomputes that size after every swap, which
 * is a scan of the entire node table per swap. Here every swap reports its own
 * exact size change and the session keeps the running total, so the driver
 * never scans anything: sylvan_reorder_forest_size() is a load. The same holds
 * for the populations, which come out of the session's per-identity lists.
 *
 * WHAT CANNOT GO WRONG. A swap either succeeds or reports SYLVAN_ERR_OOM
 * having mutated nothing at all. So there is no error handling here in the
 * usual sense: a refusal simply means this variable cannot travel any further
 * in this direction, and the driver treats it exactly like reaching the end of
 * the order. It never aborts the sift and never leaves the forest in a state
 * anyone has to repair.
 *
 * SERIAL, ON PURPOSE. The swaps underneath are parallel; the loop over them is
 * not. Sifting variables concurrently would mean concurrent swaps, which share
 * the retirement queue, the reference counts and the rank metadata. That is a
 * separate design, not an optimisation of this one.
 */

#include <sylvan/internal.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

/** Defaults for a NULL configuration; see the header. */
static const sylvan_sift_config sift_default_config = {
    0,      /* max_swaps       */
    0,      /* max_vars        */
    1.2f,   /* max_growth      */
    0,      /* nodes_threshold: measured on C880, skipping small variables
               cost up to 2.2x in final size for no measurable time saved,
               because those are exactly the cheap swaps */
    0.0     /* time_limit      */
};

/**
 * Wall clock seconds, for the time limit and nothing else. Monotonic where the
 * platform has a monotonic clock; a build with neither this nor C11's
 * timespec_get has no clock at all, and a time limit is then simply not
 * enforced, which is the one degradation this file accepts silently because
 * the alternative is refusing to sift on a platform that can sift perfectly
 * well.
 */
static double
sift_now(void)
{
    struct timespec now;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
        return (double)now.tv_sec + 1e-9 * (double)now.tv_nsec;
    }
#endif
    if (timespec_get(&now, TIME_UTC) == TIME_UTC) {
        return (double)now.tv_sec + 1e-9 * (double)now.tv_nsec;
    }
    return 0.0;
}

/** One variable, with the population that decides when its turn comes. */
struct sift_candidate {
    uint32_t identity;
    uint64_t population;
};

/** Most populous first; ties by identity, so a run is reproducible. */
static int
sift_compare(const void *left, const void *right)
{
    const struct sift_candidate *a = (const struct sift_candidate*)left;
    const struct sift_candidate *b = (const struct sift_candidate*)right;
    if (a->population != b->population) {
        return a->population < b->population ? 1 : -1;
    }
    if (a->identity != b->identity) return a->identity < b->identity ? -1 : 1;
    return 0;
}

/** Everything one call to the driver carries around. */
struct sift_run {
    uint32_t max_swaps;
    double   growth;
    double   deadline;    /* absolute; 0.0 means no time limit */
    uint32_t ranks;       /* registered ranks, fixed for the session */
    uint64_t size;        /* internal nodes right now */
    uint64_t swaps;       /* swaps made so far */
    uint64_t refusals;    /* swaps the table refused so far */
};

/** Whether the search may still make another swap. */
static int
sift_may_swap(const struct sift_run *run)
{
    if (run->max_swaps != 0 && run->swaps >= run->max_swaps) return 0;
    if (run->deadline != 0.0 && sift_now() >= run->deadline) return 0;
    return 1;
}

/**
 * Swap the variables at ranks <rank> and <rank>+1 and take the new size from
 * the session. Returns what the swap returned; on SYLVAN_ERR_OOM nothing
 * happened, so nothing is counted either.
 */
static int
sift_swap(lace_worker *lace, struct sift_run *run, uint32_t rank)
{
    const int status = sylvan_varswap_CALL(lace, rank, NULL);
    if (status != SYLVAN_OK) {
        /* A refusal is the swap's clean answer, not a failure: it ends the
           current leg where it struck. Count it here, at the one point every
           search and return-leg swap passes through, so the report can say
           whether the final order is the best one seen or a refused one. */
        if (status == SYLVAN_ERR_OOM) run->refusals++;
        return status;
    }
    run->swaps++;
    run->size = sylvan_reorder_forest_size();
    return SYLVAN_OK;
}

/**
 * Move the variable at <rank> as far as it will go, and return where it ended
 * up. <down> moves it toward the bottom of the order, otherwise toward the top.
 *
 * A leg ends at the end of the order, when the swap budget or the time limit
 * runs out, when a swap refuses for lack of room, or when the forest has grown
 * past <growth> times the smallest size seen *on this leg*. That last bound is
 * per leg rather than per variable on purpose, and it is what CUDD does: a leg
 * that starts where a previous leg gave up starts from a forest that is
 * already large, and measuring its growth against the variable's global best
 * would abandon it before it had moved at all.
 *
 * <best_rank> and <best_size> track the best position over the whole
 * excursion, across both legs, and are what the caller returns the variable to.
 */
static uint32_t
sift_leg(lace_worker *lace, struct sift_run *run, uint32_t rank, int down,
         uint32_t *best_rank, uint64_t *best_size)
{
    uint64_t limit = run->size;

    while (down ? rank + 1u < run->ranks : rank > 0) {
        if (!sift_may_swap(run)) break;
        if (sift_swap(lace, run, down ? rank : rank - 1u) != SYLVAN_OK) break;
        rank = down ? rank + 1u : rank - 1u;

        if (run->size < *best_size) {
            *best_size = run->size;
            *best_rank = rank;
        }
        if (run->size < limit) limit = run->size;
        if ((double)run->size > run->growth * (double)limit) break;
    }

    return rank;
}

/**
 * One variable's whole excursion: to the nearer end, to the far end, back to
 * the best position seen.
 *
 * The return leg ignores the swap and time budgets. It is not search -- it is
 * undoing search -- and stopping halfway through it would leave the variable
 * at an arbitrary position that the driver had already measured and rejected.
 * It is bounded by how far the legs travelled, which the budgets do bound.
 * A swap that refuses during the return leg leaves the variable where it is:
 * still a valid order, still a valid forest, just not the best one found.
 */
static void
sift_variable(lace_worker *lace, struct sift_run *run, uint32_t identity)
{
    uint32_t rank = sylvan_rank_of(identity);
    if (rank >= run->ranks) return;

    uint32_t best_rank = rank;
    uint64_t best_size = run->size;

    /* Distance to the top is <rank>, to the bottom <ranks-1-rank>. */
    const int down_first = rank >= run->ranks - 1u - rank;

    rank = sift_leg(lace, run, rank, down_first, &best_rank, &best_size);
    rank = sift_leg(lace, run, rank, !down_first, &best_rank, &best_size);

    while (rank != best_rank) {
        const int down = rank < best_rank;
        if (sift_swap(lace, run, down ? rank : rank - 1u) != SYLVAN_OK) break;
        rank = down ? rank + 1u : rank - 1u;
    }
}

/**
 * The session, from the inside of the stop-the-world frame.
 *
 * Note the order: the session is opened before anything is measured, because
 * opening it collects, and every number the driver works with has to describe
 * the forest that survives that collection rather than the table before it.
 */
TASK(int, sift_go, const sylvan_sift_config*, config, sylvan_sift_result*, result)

int
sift_go_CALL(lace_worker *lace, const sylvan_sift_config *config,
             sylvan_sift_result *result)
{
    int status = sylvan_reorder_begin_CALL(lace);
    if (status != SYLVAN_OK) return status;

    struct sift_run run;
    run.max_swaps = config->max_swaps;
    run.growth = (double)config->max_growth;
    run.deadline = config->time_limit > 0.0
        ? sift_now() + config->time_limit : 0.0;
    run.ranks = sylvan_ranks_registered();
    run.size = sylvan_reorder_forest_size();
    run.swaps = 0;
    run.refusals = 0;

    const uint64_t initial = run.size;
    uint32_t candidates = 0;
    uint32_t sifted = 0;
    struct sift_candidate *order = NULL;

    if (run.ranks >= 2) {
        order = (struct sift_candidate*)malloc(
            (size_t)run.ranks * sizeof(struct sift_candidate));
        if (order == NULL) {
            status = SYLVAN_ERR_OOM;
        } else {
            for (uint32_t rank = 0; rank < run.ranks; rank++) {
                const uint32_t identity = sylvan_identity_at(rank);
                const uint64_t population = sylvan_reorder_population(identity);
                /* A variable with no nodes cannot change the size of anything
                   by moving, whatever the threshold says. */
                if (population == 0) continue;
                if (population < config->nodes_threshold) continue;
                order[candidates].identity = identity;
                order[candidates].population = population;
                candidates++;
            }
            qsort(order, candidates, sizeof(struct sift_candidate),
                  sift_compare);
            if (config->max_vars != 0 && candidates > config->max_vars) {
                candidates = config->max_vars;
            }
        }
    }

    for (uint32_t i = 0; i < candidates; i++) {
        if (!sift_may_swap(&run)) break;
        sift_variable(lace, &run, order[i].identity);
        sifted++;
    }
    free(order);

    const uint64_t final_size = run.size;
    const uint64_t swaps = run.swaps;
    const uint64_t refusals = run.refusals;

    const int closed = sylvan_reorder_end_CALL(lace);
    if (status == SYLVAN_OK) status = closed;

    if (result != NULL && status == SYLVAN_OK) {
        result->initial_size = initial;
        result->final_size = final_size;
        result->swaps = swaps;
        result->refusals = refusals;
        result->vars_sifted = sifted;
    }
    return status;
}

int
sylvan_sift_report(const sylvan_sift_config *config, sylvan_sift_result *result)
{
    if (result != NULL) memset(result, 0, sizeof(*result));

    sylvan_sift_config effective =
        config == NULL ? sift_default_config : *config;
    /* Zero is far more likely to be a field nobody filled in than a request to
       give up at the first swap that does not help. */
    if (!(effective.max_growth > 0.0f)) {
        effective.max_growth = sift_default_config.max_growth;
    }
    if (effective.max_growth < 1.0f) effective.max_growth = 1.0f;

    return sift_go_NEWFRAME(&effective, result);
}

int
sylvan_sift(const sylvan_sift_config *config)
{
    return sylvan_sift_report(config, NULL);
}
