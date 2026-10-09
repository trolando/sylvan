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
 * toward whichever end of the order it is nearer -- so that when a bound cuts
 * the excursion short, it is the cheaper half that was spent -- then toward
 * the other end, and finally back to the position where the forest was
 * smallest. Either leg may stop before its end of the order: at the growth
 * bound, at the interaction bound, at the swap or time budget, or at a swap
 * the table refuses. The variable is left at the best position visited, and
 * only an excursion in which neither leg stopped early has visited them all.
 *
 * WHAT MAKES IT CHEAP. Sifting scores a position by the size of the forest,
 * and the obvious implementation recomputes that size after every swap, which
 * is a scan of the entire node table per swap. Here every swap reports its own
 * exact size change and the session keeps the running total, so the driver
 * never scans anything: sylvan_reorder_forest_size() is a load. The same holds
 * for the populations: the driver asks the session for each variable's once,
 * before it starts, and from then on keeps them current itself from the
 * counts every swap reports (see sift_swap).
 *
 * WHAT CANNOT GO WRONG. A swap either succeeds or reports SYLVAN_ERR_OOM
 * having mutated nothing at all. So there is no error handling here in the
 * usual sense: a refusal simply means this variable cannot travel any further
 * in this direction, and the driver treats it exactly like reaching the end of
 * the order. It never aborts the sift and never leaves the forest in a state
 * anyone has to repair.
 *
 * PAIRS, ON REQUEST. bdd_rel_next and bdd_rel_prev are fastest when identity
 * 2k sits directly above identity 2k+1, and an unconstrained sift has no
 * reason to keep them there. With group_pairs the unit of the search is a
 * block -- such a pair, or a variable without a partner -- and everything
 * above applies to blocks: candidates in order of their summed population, a
 * leg crosses one neighbouring block per move, the size is scored only after
 * a whole move, and the bounds and budgets judge moves. Ungrouped, every block
 * is one variable and the search is exactly the one described above.
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
    0.0,    /* time_limit      */
    0,      /* full_legs: the interaction bound is conservative, so bounded
               legs reach the same final size with about a third fewer swaps */
    0       /* group_pairs: a consumer that never calls bdd_rel_next or
               bdd_rel_prev gains nothing from the constraint */
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

/**
 * The driver's share of the per-phase timing, in the diagnostic build that has
 * it; the session's share is kept by reorder.c (see sylvan_reorder_timing).
 * Every sift that opened a session prints both as one SIFT-PHASES line on
 * stderr, with the keys and in the order of the archived per-phase
 * measurements, so that the scripts that read those read this. Without the
 * option every macro is empty.
 */
#if SYLVAN_SIFT_PHASE_TIMING
static double sift_timing_popinit, sift_timing_interact, sift_timing_total;
static int sift_timing_opened;

static void
sift_timing_report(void)
{
    if (!sift_timing_opened) return;
    sylvan_reorder_timing t;
    sylvan_reorder_phase_timing(&t);
    fprintf(stderr,
            "SIFT-PHASES {\"swaps\":%llu,\"gc\":%.6f,\"seed\":%.6f"
            ",\"scatter\":%.6f,\"pin\":%.6f,\"begin_total\":%.6f"
            ",\"classify\":%.6f,\"intern\":%.6f,\"rewrite\":%.6f"
            ",\"reinsert\":%.6f,\"retire\":%.6f,\"release\":%.6f"
            ",\"swap_total\":%.6f,\"end\":%.6f,\"interact\":%.6f"
            ",\"popinit\":%.6f,\"sift_total\":%.6f"
            ",\"remove_batch\":%.6f,\"reinsert_batch\":%.6f}\n",
            (unsigned long long)t.swaps, t.gc, t.seed, t.scatter, t.pin,
            t.begin_total, t.classify, t.intern, t.rewrite, t.reinsert,
            t.retire, t.release, t.swap_total, t.end, sift_timing_interact,
            sift_timing_popinit, sift_timing_total, t.remove_batch,
            t.reinsert_batch);
    fflush(stderr);
}

#define SIFT_TIMING_CLEAR() \
    (sift_timing_opened = 0, sift_timing_popinit = 0.0, \
     sift_timing_interact = 0.0, sift_timing_total = 0.0)
#define SIFT_TIMING_OPENED() (sift_timing_opened = 1)
#define SIFT_TIMING_START(mark) const double mark = sift_now()
#define SIFT_TIMING_STOP(sum, mark) ((sum) += sift_now() - (mark))
#define SIFT_TIMING_REPORT() sift_timing_report()
#else
#define SIFT_TIMING_CLEAR() ((void)0)
#define SIFT_TIMING_OPENED() ((void)0)
#define SIFT_TIMING_START(mark) ((void)0)
#define SIFT_TIMING_STOP(sum, mark) ((void)0)
#define SIFT_TIMING_REPORT() ((void)0)
#endif

/**
 * One block -- a variable, or with group_pairs the pair (2k, 2k+1) -- named by
 * its first identity, with the population that decides when its turn comes.
 */
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
    uint64_t refusals;    /* swaps and block moves the table refused so far */
    uint64_t attempts;    /* swaps asked for so far, refused or not */
    uint64_t *pop;        /* live node population per identity, for the bounds */
    int      bounded;     /* stop a leg when the interaction bound says to */
    int      grouped;     /* identities 2k and 2k+1 move as one block */
    int      stuck;       /* a block move could be neither undone nor finished */
};

/**
 * Whether the search may still make a move of <swaps> adjacent swaps. A move
 * is made whole or not at all, so the whole of it has to fit in the budget.
 */
static int
sift_may_swap(const struct sift_run *run, uint32_t swaps)
{
    if (run->max_swaps != 0 && run->swaps + swaps > run->max_swaps) return 0;
    if (run->deadline != 0.0 && sift_now() >= run->deadline) return 0;
    return 1;
}

/* The swap attempts every sift refuses by itself; see
   sylvan_sift_set_refusal_window. Only written with no sift running. */
static uint64_t sift_refuse_first;
static uint64_t sift_refuse_count;

void
sylvan_sift_set_refusal_window(uint64_t first, uint64_t count)
{
    sift_refuse_first = first;
    sift_refuse_count = count;
}

/**
 * Swap the variables at ranks <rank> and <rank>+1 and take the new size from
 * the session. Returns what the swap returned; on SYLVAN_ERR_OOM nothing
 * happened, so nothing is counted either. <rewritten>, when not NULL, receives
 * the number of nodes the swap rewrote, which bounds what undoing it needs.
 */
static int
sift_swap(lace_worker *lace, struct sift_run *run, uint32_t rank,
          uint64_t *rewritten)
{
    /* The two variables the swap will move, named before it moves them. */
    const uint32_t upper = sylvan_identity_at(rank);
    const uint32_t lower = sylvan_identity_at(rank + 1u);

    /* A refusal the test hook injects is the table's own answer in every
       respect: nothing is touched, and it is counted like one. */
    run->attempts++;
    if (run->attempts - sift_refuse_first < sift_refuse_count) {
        run->refusals++;
        return SYLVAN_ERR_OOM;
    }

    sylvan_varswap_result result;
    const int status = sylvan_varswap_CALL(lace, rank, &result);
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
    if (rewritten != NULL) *rewritten = result.rewritten + result.map_rewritten;

    /* Keep the two populations the leg bounds read exact, from the counts the
       swap already reports rather than a scan. The upper variable loses every
       case-B node it rewrote to the lower one and gains the children it
       interned; the lower variable gains those same rewritten nodes and loses
       only its own nodes, because the depth-1 death theorem says the only nodes
       a swap can retire are lower-variable ones. Every other variable is
       untouched, so nothing else moves. */
    if (run->pop != NULL) {
        const uint64_t moved = result.rewritten + result.map_rewritten;
        run->pop[upper] = run->pop[upper] + result.created - moved;
        run->pop[lower] = run->pop[lower] + moved - result.died;
    }
    return SYLVAN_OK;
}

/* The slots nodes_reserve holds back for every worker on top of each request:
   one allocation region, which a worker may own and leave part-used. */
#define SIFT_REGION_SLOTS UINT64_C(512)

/**
 * Whether the table can promise the slots for the next swap of a block move,
 * whose upper variable is <upper>, and for undoing it -- plus <undo> more for
 * undoing the swaps of the move made before it.
 *
 * THIS IS WHAT KEEPS A REFUSAL FROM SPLITTING A PAIR. Between the swaps of a
 * move the order splits a block, and the way back is to undo what the move
 * did; but an undo is a swap like any other and could be refused like any
 * other. So a swap of a move is only made while the table can promise both it
 * and the undoing of everything the move has done, which makes the way back
 * one that cannot run out of slots.
 *
 * The bound is the reservation a swap makes, which is at most two slots for
 * every node of its upper variable: that covers the swap, and the same again
 * its undo, whose rewritten nodes are among the ones the swap rewrote. A swap
 * already made is covered by twice the nodes it did rewrite. The extra region
 * per worker is for the regions this swap may leave owned and part-used,
 * which every later reservation discounts.
 */
static int
sift_room(const struct sift_run *run, uint32_t upper, uint64_t undo)
{
    const uint64_t margin = (uint64_t)lace_worker_count() * SIFT_REGION_SLOTS;
    const uint64_t swap = 2u * run->pop[upper];
    return nodes_reserve(nodes, 2u * swap + undo + margin) == SYLVAN_OK;
}

/**
 * The number of variables in the block that starts at <rank>: two if it holds
 * an even identity whose odd partner is right below it and the run is grouped,
 * otherwise one. sift_block_above is the same for the block ending at <rank>.
 * Looking away from the block being moved is what keeps a block of one -- a
 * variable whose partner is somewhere else -- from being mistaken for half of
 * a block with it.
 */
static uint32_t
sift_block_below(const struct sift_run *run, uint32_t rank)
{
    if (!run->grouped || rank + 1u >= run->ranks) return 1;
    const uint32_t identity = sylvan_identity_at(rank);
    if ((identity & 1u) != 0) return 1;
    return sylvan_identity_at(rank + 1u) == identity + 1u ? 2u : 1u;
}

static uint32_t
sift_block_above(const struct sift_run *run, uint32_t rank)
{
    if (!run->grouped || rank == 0) return 1;
    const uint32_t identity = sylvan_identity_at(rank);
    if ((identity & 1u) == 0) return 1;
    return sylvan_identity_at(rank - 1u) == identity - 1u ? 2u : 1u;
}

/**
 * Exchange the block of <upper> variables at rank <top> with the block of
 * <lower> variables right below it. Returns 1 when the order afterwards is the
 * exchanged one and 0 when it is the one before; sets *refused when the table
 * refused anything on the way, which ends the leg the move was part of.
 *
 * Each variable of the lower block, top one first, is carried up through the
 * whole upper block: x x' y becomes y x x' by swap(x', y) and then swap(x, y),
 * and two blocks of two take four swaps. Only the orders before and after are
 * ever scored or left standing; every one in between splits a block.
 *
 * WHEN THE TABLE REFUSES. A move of one swap needs no care: a refused swap
 * changed nothing. In a longer move each swap is made only once sift_room says
 * the table can promise it and the undoing of the move so far, and when it
 * cannot the move is undone, which was promised. What no reservation covers
 * is a swap's heap allocations; when one of those refuses, the move is undone
 * as well, and if the undo is refused it is finished instead, which leaves
 * every block whole too. Only when that is refused as well does a split
 * remain: run->stuck ends the sift where it stands, and
 * sylvan_sift_split_pairs() counts what it left behind.
 */
static int
sift_move(lace_worker *lace, struct sift_run *run, uint32_t top,
          uint32_t upper, uint32_t lower, int *refused)
{
    uint32_t sequence[4];
    uint32_t count = 0;
    for (uint32_t j = 0; j < lower; j++) {
        for (uint32_t i = upper; i-- > 0;) sequence[count++] = top + j + i;
    }

    if (count == 1) {
        if (sift_swap(lace, run, sequence[0], NULL) == SYLVAN_OK) return 1;
        *refused = 1;
        return 0;
    }

    uint32_t done = 0;
    uint64_t undo = 0;
    while (done < count) {
        uint64_t rewritten = 0;
        if (!sift_room(run, sylvan_identity_at(sequence[done]), undo)) {
            run->refusals++;
            break;
        }
        if (sift_swap(lace, run, sequence[done], &rewritten) != SYLVAN_OK) {
            break;
        }
        undo += 2u * rewritten;
        done++;
    }
    if (done == count) return 1;

    *refused = 1;
    while (done > 0 &&
           sift_swap(lace, run, sequence[done - 1u], NULL) == SYLVAN_OK) {
        done--;
    }
    if (done == 0) return 0;
    while (done < count &&
           sift_swap(lace, run, sequence[done], NULL) == SYLVAN_OK) {
        done++;
    }
    if (done == count) return 1;
    run->stuck = 1;
    return 0;
}

/** Whether <identity> interacts with any of the <size> variables of <block>. */
static int
sift_interacts(uint32_t identity, const uint32_t *block, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) {
        if (sylvan_reorder_interact(identity, block[i])) return 1;
    }
    return 0;
}

/**
 * Move the block of <size> variables <block>, whose first variable is at rank
 * <top>, as far as it will go, and return the rank its first variable ended
 * up at. <down> moves it toward the bottom of the order, otherwise toward the
 * top. A block crosses one neighbouring block per move.
 *
 * A leg ends at the end of the order, when the swap budget or the time limit
 * has no room for the next move, when the table refuses a move, when the
 * forest has grown past <growth> times the smallest size seen *on this leg*,
 * or -- the interaction bound -- when even the best size still reachable in
 * this direction cannot beat the smallest already seen.
 *
 * THAT LAST BOUND is CUDD's, from ddSiftingUp/ddSiftingDown, and it is what
 * makes the schedule frugal. A variable can only ever exchange nodes with the
 * variables it interacts with. Going down, the summed population of the
 * interacting variables still below is an upper bound on the nodes left to
 * gain, so size minus that sum is a floor on the size reachable further down;
 * once that floor is no better than the best already seen, the leg stops.
 * Going up, the same reasoning gives a lower bound that only ever rises. Both
 * bounds are maintained incrementally: the populations of the block being
 * crossed are all that enter or leave the sum at each move. A block interacts
 * with whatever either of its variables does, which keeps the bound a bound;
 * CUDD's group sifting consults the interactions of one variable per group.
 *
 * The growth bound is per leg rather than per variable on purpose, and that too
 * is what CUDD does: a leg that starts where a previous leg gave up starts from
 * a forest that is already large, and measuring its growth against the
 * variable's global best would abandon it before it had moved at all.
 *
 * <best_top> and <best_size> track the best position over the whole
 * excursion, across both legs, and are what the caller returns the block to.
 */
static uint32_t
sift_leg(lace_worker *lace, struct sift_run *run, uint32_t top, int down,
         const uint32_t *block, uint32_t size,
         uint32_t *best_top, uint64_t *best_size)
{
    uint64_t limit = run->size;

    if (down) {
        /* reach: the nodes still to be gained below, at most -- the summed
           population of the interacting variables under the sifted block.
           The best size reachable further down is size - reach. */
        uint64_t reach = 0;
        if (run->bounded) {
            for (uint32_t r = top + size; r < run->ranks; r++) {
                const uint32_t below = sylvan_identity_at(r);
                if (sift_interacts(below, block, size)) {
                    reach += run->pop[below];
                }
            }
        }

        while (top + size < run->ranks) {
            const uint32_t next = sift_block_below(run, top + size);
            if (!sift_may_swap(run, size * next)) break;
            if (run->bounded) {
                if (run->size >= limit + reach) break; /* size-reach >= limit */
                for (uint32_t i = 0; i < next; i++) {
                    const uint32_t below = sylvan_identity_at(top + size + i);
                    if (sift_interacts(below, block, size)) {
                        reach -= run->pop[below];
                    }
                }
            }
            int refused = 0;
            if (!sift_move(lace, run, top, size, next, &refused)) break;
            top += next;

            if (run->size < *best_size) { *best_size = run->size; *best_top = top; }
            if (run->size < limit) limit = run->size;
            if (refused) break;
            if ((double)run->size > run->growth * (double)limit) break;
        }
    } else {
        /* bound: a lower bound on the size reachable above. The part below the
           sifted block and the non-interacting part above cannot change; of
           the interacting part, all but the topmost level may vanish. It only
           rises as the block climbs. */
        uint64_t bound = 0;
        if (run->bounded) {
            bound = run->size;
            for (uint32_t r = 1; r < top; r++) {
                const uint32_t above = sylvan_identity_at(r);
                if (sift_interacts(above, block, size)) {
                    bound -= run->pop[above];
                }
            }
            for (uint32_t i = 0; i < size; i++) bound -= run->pop[block[i]];
        }

        while (top > 0) {
            const uint32_t next = sift_block_above(run, top - 1u);
            if (!sift_may_swap(run, size * next)) break;
            if (run->bounded && bound > limit) break;

            int refused = 0;
            if (!sift_move(lace, run, top - next, next, size, &refused)) break;
            top -= next;
            if (run->bounded) {
                for (uint32_t i = 0; i < next; i++) {
                    const uint32_t above = sylvan_identity_at(top + size + i);
                    if (sift_interacts(above, block, size)) {
                        bound += run->pop[above];
                    }
                }
            }

            if (run->size < *best_size) { *best_size = run->size; *best_top = top; }
            if (run->size < limit) limit = run->size;
            if (refused) break;
            if ((double)run->size > run->growth * (double)limit) break;
        }
    }

    return top;
}

/**
 * One block's whole excursion: to the nearer end, to the far end, back to
 * the best position seen.
 *
 * The return leg ignores the swap and time budgets. It is not search -- it is
 * undoing search -- and stopping halfway through it would leave the block at
 * an arbitrary position that the driver had already measured and rejected.
 * It is bounded by how far the legs travelled, which the budgets do bound,
 * and it crosses the same blocks they did, so it takes no more swaps than
 * they took. Its swaps still count toward the total the swap budget is checked
 * against, so they come out of the next block's search.
 * A move that is refused during the return leg leaves the block where it is:
 * still a valid order, still a valid forest, just not the best one found.
 */
static void
sift_block(lace_worker *lace, struct sift_run *run, const uint32_t *block,
           uint32_t size)
{
    uint32_t top = sylvan_rank_of(block[0]);
    if (top + size > run->ranks) return;
    if (size == 2 && sylvan_rank_of(block[1]) != top + 1u) return;

    uint32_t best_top = top;
    uint64_t best_size = run->size;

    /* Distance to the top is <top>, to the bottom <ranks-size-top>. */
    const int down_first = top >= run->ranks - size - top;

    top = sift_leg(lace, run, top, down_first, block, size,
                   &best_top, &best_size);
    if (run->stuck) return;
    top = sift_leg(lace, run, top, !down_first, block, size,
                   &best_top, &best_size);

    while (top != best_top && !run->stuck) {
        int refused = 0;
        if (top < best_top) {
            const uint32_t next = sift_block_below(run, top + size);
            if (!sift_move(lace, run, top, size, next, &refused)) break;
            top += next;
        } else {
            const uint32_t next = sift_block_above(run, top - 1u);
            if (!sift_move(lace, run, top - next, next, size, &refused)) break;
            top -= next;
        }
    }
}

/**
 * Put the pair (<even>, <even>+1) back together, the even identity directly
 * above the odd one, before any block moves: an ungrouped sift, or a
 * refusal that left a sift stuck, may have split it.
 *
 * Whichever of the two has fewer nodes travels, through whole blocks, to its
 * partner; on a tie the odd one does. The partner stays put on purpose: its
 * position is the one the earlier sift chose for the more populous variable,
 * the one whose position the size depends on most -- which is why sifting
 * takes the most populous first -- so it is the half of that order most worth
 * keeping. Neither choice is cheaper in swaps: both cross the same variables.
 *
 * Neither budget applies, because a pair that is apart cannot be sifted as a
 * block at all; the swaps still count toward the total. A refused move leaves
 * the pair apart. It is then not a candidate, its two variables move only
 * when other blocks cross them, and sylvan_sift_split_pairs() counts it.
 */
static void
sift_reunite(lace_worker *lace, struct sift_run *run, uint32_t even)
{
    const uint32_t odd = even + 1u;
    const uint32_t mover = run->pop[even] < run->pop[odd] ? even : odd;
    const uint32_t partner = mover ^ 1u;

    while (!run->stuck) {
        const uint32_t at = sylvan_rank_of(mover);
        const uint32_t goal = sylvan_rank_of(partner);
        if (mover == odd ? at == goal + 1u : at + 1u == goal) return;

        int refused = 0;
        if (at < goal) {
            const uint32_t next = sift_block_below(run, at + 1u);
            if (!sift_move(lace, run, at, 1, next, &refused)) return;
        } else {
            const uint32_t next = sift_block_above(run, at - 1u);
            if (!sift_move(lace, run, at - next, next, 1, &refused)) return;
        }
    }
}

uint32_t
sylvan_sift_split_pairs(void)
{
    const uint32_t ranks = sylvan_ranks_registered();
    uint32_t split = 0;
    for (uint32_t even = 0; even + 1u < ranks; even += 2u) {
        if (sylvan_rank_of(even + 1u) != sylvan_rank_of(even) + 1u) split++;
    }
    return split;
}

/**
 * WHEN A SIFT IS WORTH RUNNING
 *
 * The whole of Sylvan's answer, and it is deliberately a small one. Sifting is
 * expensive and its benefit is not monotone: run on a forest that has barely
 * changed it finds nothing and costs a stop-the-world pass. What pays is a
 * forest that has GROWN, because growth is where a bad order compounds -- the
 * same order that cost 20% on a small forest costs a factor on a large one.
 * So: recommend a sift once the live forest is <factor> times the size it was
 * when it was last sifted, or when the process started if it never was.
 *
 * THE MEASUREMENT IS FREE, which is the other half of the design. Every
 * collection already counts the buckets that survive marking, because the
 * table sizing heuristics are built on that number (see gc_note_churn in
 * common.c). The recommendation is one comparison against it, made where it is
 * already in hand. Nothing is sampled between collections, nothing is counted
 * per operation, and sylvan_sift_recommended() is a load.
 *
 * THE FLOOR exists because a ratio is meaningless at the bottom. A forest of
 * two hundred nodes that grows to five hundred has tripled and has nothing
 * worth reordering; the cost of finding that out exceeds anything it could
 * save. Below the floor no growth recommends anything.
 */

/* Live buckets below which growth is not evidence of anything. */
#ifndef SYLVAN_SIFT_RECOMMEND_FLOOR
#define SYLVAN_SIFT_RECOMMEND_FLOOR UINT64_C(4096)
#endif

/* Growth factor; 0 disables the recommendation entirely. */
static double sift_recommend_factor = 2.0;

/* Live buckets the growth is measured against: the size the forest was left at
   by the last sift, floored. Only written with the world stopped. */
static uint64_t sift_baseline = SYLVAN_SIFT_RECOMMEND_FLOOR;

/* The recommendation itself, updated once per collection. Read from any
   thread, written only with the world stopped. */
static _Atomic(int) sift_recommend;

/* Sifts that opened a session, ever. See sylvan_sift_count. */
static _Atomic(uint64_t) sift_performed;

/* The automatic sift: NULL configuration means off, which is the default. */
static int sift_auto_enabled;
static sylvan_sift_config sift_auto_config;

uint64_t
sylvan_sift_count(void)
{
    return atomic_load_explicit(&sift_performed, memory_order_relaxed);
}

int
sylvan_sift_recommended(void)
{
    return atomic_load_explicit(&sift_recommend, memory_order_relaxed);
}

void
sylvan_sift_set_recommend_factor(double factor)
{
    if (!(factor > 0.0)) {
        /* Off. Clear the flag as well, so that switching the recommendation
           off does not leave a stale one behind for an automatic sift to act
           on before the next collection. */
        sift_recommend_factor = 0.0;
        atomic_store_explicit(&sift_recommend, 0, memory_order_relaxed);
        return;
    }
    sift_recommend_factor = factor < 1.0 ? 1.0 : factor;
}

void
sylvan_sift_reset_recommendation(void)
{
    sift_baseline = SYLVAN_SIFT_RECOMMEND_FLOOR;
    atomic_store_explicit(&sift_recommend, 0, memory_order_relaxed);
}

void
sylvan_sift_note_collection(uint64_t live_nodes)
{
    const int recommend =
        sift_recommend_factor > 0.0 &&
        (double)live_nodes >= sift_recommend_factor * (double)sift_baseline;
    atomic_store_explicit(&sift_recommend, recommend, memory_order_relaxed);
}

/** Rebaseline on the forest a sift has just left behind. */
static void
sift_note_sift(uint64_t live_nodes)
{
    sift_baseline = live_nodes > SYLVAN_SIFT_RECOMMEND_FLOOR
        ? live_nodes : SYLVAN_SIFT_RECOMMEND_FLOOR;
    atomic_store_explicit(&sift_recommend, 0, memory_order_relaxed);
}

void
sylvan_gc_set_auto_sift(const sylvan_sift_config *config)
{
    if (config == NULL) {
        sift_auto_enabled = 0;
        return;
    }
    sift_auto_config = *config;
    sift_auto_enabled = 1;
}

void
sylvan_sift_auto(void)
{
    if (!sift_auto_enabled) return;
    if (!atomic_load_explicit(&sift_recommend, memory_order_relaxed)) return;

    /* A refusal here is not an error the caller asked about: the consumer
       asked for a collection, and reordering was Sylvan's idea. Whatever the
       reason -- ZDD in use, a table without the reorder primitives -- the
       collection stands and the forest is untouched. */
    (void)sylvan_sift(&sift_auto_config);
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
    SIFT_TIMING_START(timing_sift);
    int status = sylvan_reorder_begin_CALL(lace);
    if (status != SYLVAN_OK) return status;
    SIFT_TIMING_OPENED();

    /* The session opened, so this call counts as a sift however little it goes
       on to do. A refused call does not, which is what makes the counter
       usable for establishing that an automatic sift did or did not run. */
    atomic_fetch_add_explicit(&sift_performed, 1, memory_order_relaxed);

    struct sift_run run;
    run.max_swaps = config->max_swaps;
    run.growth = (double)config->max_growth;
    run.deadline = config->time_limit > 0.0
        ? sift_now() + config->time_limit : 0.0;
    run.ranks = sylvan_ranks_registered();
    run.size = sylvan_reorder_forest_size();
    run.swaps = 0;
    run.refusals = 0;
    run.attempts = 0;
    run.pop = NULL;
    run.bounded = !config->full_legs;
    run.grouped = config->group_pairs != 0;
    run.stuck = 0;

    const uint64_t initial = run.size;
    uint32_t candidates = 0;
    uint32_t sifted = 0;
    struct sift_candidate *order = NULL;

    if (run.ranks >= 2) {
        order = (struct sift_candidate*)malloc(
            (size_t)run.ranks * sizeof(struct sift_candidate));
        run.pop = (uint64_t*)malloc((size_t)run.ranks * sizeof(uint64_t));
        if (order == NULL || run.pop == NULL) {
            status = SYLVAN_ERR_OOM;
        } else {
            /* Every variable's live population, which the candidate order and
               the leg bounds both read. Identities are dense, so this indexes
               straight by identity. */
            SIFT_TIMING_START(timing_popinit);
            for (uint32_t id = 0; id < run.ranks; id++) {
                run.pop[id] = sylvan_reorder_population(id);
            }
            SIFT_TIMING_STOP(sift_timing_popinit, timing_popinit);

            /* The interaction matrix the leg bounds consult. If it cannot be
               built the driver still runs -- every pair then reads as
               interacting, which is exactly an unbounded sift -- so its failure
               is not this function's failure. A full-legs run never consults
               it, so it is not built either. */
            SIFT_TIMING_START(timing_interact);
            if (run.bounded) (void)sylvan_reorder_interact_build();
            SIFT_TIMING_STOP(sift_timing_interact, timing_interact);

            /* A pair that is apart is not a block, so before any block moves
               every one is brought back together. */
            if (run.grouped) {
                for (uint32_t even = 0; even + 1u < run.ranks; even += 2u) {
                    if (run.stuck) break;
                    if (sylvan_rank_of(even + 1u) == sylvan_rank_of(even) + 1u) {
                        continue;
                    }
                    sift_reunite(lace, &run, even);
                }
            }

            const uint32_t width = run.grouped ? 2u : 1u;
            for (uint32_t first = 0; first < run.ranks; first += width) {
                const uint32_t size = first + width <= run.ranks ? width : 1u;
                if (size == 2 &&
                    sylvan_rank_of(first + 1u) != sylvan_rank_of(first) + 1u) {
                    continue;
                }
                uint64_t population = 0;
                for (uint32_t i = 0; i < size; i++) {
                    population += run.pop[first + i];
                }
                /* A block with no nodes cannot change the size of anything
                   by moving, whatever the threshold says. */
                if (population == 0) continue;
                if (population < config->nodes_threshold) continue;
                order[candidates].identity = first;
                order[candidates].population = population;
                candidates++;
            }
            qsort(order, candidates, sizeof(struct sift_candidate),
                  sift_compare);
        }
    }

    for (uint32_t i = 0; i < candidates && !run.stuck; i++) {
        if (!sift_may_swap(&run, 1)) break;
        const uint32_t first = order[i].identity;
        const uint32_t size = run.grouped && first + 1u < run.ranks ? 2u : 1u;
        /* <max_vars> counts variables, and a block is sifted whole or not at
           all, so the first block that does not fit ends the sift. */
        if (config->max_vars != 0 && sifted + size > config->max_vars) break;
        const uint32_t block[2] = { first, first + 1u };
        sift_block(lace, &run, block, size);
        sifted += size;
    }
    free(order);
    free(run.pop);

    const uint64_t final_size = run.size;
    const uint64_t swaps = run.swaps;
    const uint64_t refusals = run.refusals;
    const uint32_t split_pairs = sylvan_sift_split_pairs();

    const int closed = sylvan_reorder_end_CALL(lace);
    if (status == SYLVAN_OK) status = closed;

    /* The session is closed and every retired slot is back in the pool, so
       this is the number the next collection will measure. Rebaselining the
       recommendation on it is what "a sift clears the recommendation" means:
       growth is measured from here on. */
    sift_note_sift(nodes_count_nodes_CALL(lace, nodes));

    /* Whenever the session opened, which is the line sylvan_sift_count draws
       too: a driver that could not allocate its scratch reports the forest
       unchanged and no swaps, and a self-check that failed on closing reports
       the sift it checked. Only a call refused before opening reports
       nothing, because sylvan_sift_report zeroed <result> and nothing here
       ran. */
    if (result != NULL) {
        result->initial_size = initial;
        result->final_size = final_size;
        result->swaps = swaps;
        result->refusals = refusals;
        result->vars_sifted = sifted;
        result->split_pairs = split_pairs;
    }
    SIFT_TIMING_STOP(sift_timing_total, timing_sift);
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

    SIFT_TIMING_CLEAR();
    const int status = sift_go_NEWFRAME(&effective, result);
    SIFT_TIMING_REPORT();
    return status;
}

int
sylvan_sift(const sylvan_sift_config *config)
{
    return sylvan_sift_report(config, NULL);
}
