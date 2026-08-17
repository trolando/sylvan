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

/* Do not include this file directly. Instead, include sylvan.h */

/**
 * Dynamic variable reordering.
 *
 * Sylvan reorders on request and never on its own. This header is the whole
 * public interface: one call that reorders the variables of the BDD/MTBDD
 * forest by sifting, and a configuration and a result to go with it. The
 * machinery underneath -- the reorder session and the adjacent variable swap --
 * is internal, because a swap only makes sense inside a session and a session
 * runs with the world stopped.
 *
 * WHAT SIFTING DOES. Every variable is moved through the order one adjacent
 * swap at a time, the size of the forest is measured at each position, and the
 * variable is left where the forest was smallest. Variables are taken in order
 * of how many nodes they carry, most first, because those are where a better
 * position pays the most. The functions the forest represents do not change;
 * only the order of the variables, and with it the number of nodes needed to
 * represent them, does.
 */

#ifndef SYLVAN_REORDER_H
#define SYLVAN_REORDER_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * What sifting is allowed to spend, and when it should give up.
 *
 * Pass NULL to sylvan_sift for the defaults given below. A configuration that
 * is passed is used as it stands, so zero-initialising the struct and setting
 * one field turns the other limits *off* rather than leaving them at their
 * defaults. The single exception is <max_growth>, where zero would mean "give
 * up at the first swap that does not help" and is far more likely to be an
 * uninitialised field than a request; see below.
 */
typedef struct sylvan_sift_config {
    /**
     * Total number of adjacent swaps the search may make. 0 is unlimited.
     * Swaps that move a variable back to its best position are not search and
     * are not counted against this, so that the budget can never strand a
     * variable somewhere worse than where it started.
     */
    uint32_t max_swaps;

    /**
     * Number of variables to move, taken from the most populous downwards.
     * 0 means every variable that passes <nodes_threshold>.
     */
    uint32_t max_vars;

    /**
     * How much the forest may grow while one variable is being moved, as a
     * factor of the smallest size seen so far on the current leg of its
     * excursion. Reaching it abandons that leg. The default is 1.2. Values
     * below 1 are raised to 1, and 0 selects the default.
     */
    float max_growth;

    /**
     * Variables with fewer nodes than this are not moved at all. The default
     * is 0: every variable with at least one node is sifted. Skipping small
     * variables does not save time worth having -- their swaps are the cheap
     * ones -- and was measured to cost up to 2.2x in final size, so raise
     * this only to bound the number of variables visited, not to save time.
     */
    uint32_t nodes_threshold;

    /**
     * Seconds of wall clock time the search may take, 0 for no limit. The
     * limit is checked between swaps, so a single swap may overrun it.
     */
    double time_limit;

    /**
     * Nonzero walks every leg to the end of the order. The default, 0, stops
     * a leg as soon as the interaction bound proves that no position further
     * in that direction can beat the best already seen: a variable can only
     * exchange nodes with the variables some function depends on it together
     * with, so the population of those still ahead bounds what the rest of
     * the leg could save. The bound is conservative -- both settings find the
     * same positions and the same final size, measured as well as argued --
     * and it prunes about a third of all swaps, so the full walk exists for
     * measurement and for distrust, not for quality.
     */
    int full_legs;
} sylvan_sift_config;

/**
 * What one call to sylvan_sift did.
 *
 * The sizes count internal nodes -- decision nodes and MTBDDMAP nodes -- and
 * not leaves or terminals, which reordering neither creates nor destroys.
 * <initial_size> is measured after the collection that opens the reorder
 * session, so it is the size of the reachable forest and not of the table.
 */
typedef struct sylvan_sift_result {
    uint64_t initial_size;   /* internal nodes before sifting              */
    uint64_t final_size;     /* internal nodes afterwards                  */
    uint64_t swaps;          /* adjacent swaps performed, search and return */
    uint64_t refusals;       /* swaps the table refused (SYLVAN_ERR_OOM);
                                each ends a search or return leg early    */
    uint32_t vars_sifted;    /* variables whose excursion was run          */
} sylvan_sift_result;

/**
 * Reorder the variables of the BDD/MTBDD forest by sifting.
 *
 * Call from inside a Lace worker. Sifting stops the world for its whole
 * duration, exactly like garbage collection, and it begins by collecting.
 *
 * EVERYTHING A COLLECTION WOULD KEEP, A SIFT KEEPS. The session pins every
 * root it is shown -- the mtbdd_protect() table, every worker's temporary
 * mtbdd_refs_push/pushptr stack, and every root yielded by a hook registered
 * with sylvan_gc_add_roots() -- and a pinned node is never retired. All of
 * them denote the same function afterwards, and all of them keep their
 * handles, because nodes are rewritten in place and never moved.
 *
 * What is still forbidden is an operation in flight. That is not about roots:
 * a suspended operation has already read node fields and would resume and
 * combine its sub-results under a rank order that no longer holds. Its
 * *handles* would survive the sift; its *reasoning* would not. This also means
 * only one worker may be inside this call at a time, since a second one would
 * be an operation in flight as far as the first is concerned.
 *
 * Returns SYLVAN_OK, or a negative SYLVAN_ERR_*:
 *
 * - SYLVAN_ERR_INVALID when the forest cannot be reordered at all: MTBDD
 *   support is not initialised, the nodes table implements no reorder
 *   primitives, or the ZDD or ListDD subsystem is initialised (both order
 *   their nodes by variable identity rather than by rank). Nothing is changed
 *   in any of these cases.
 * - SYLVAN_ERR_OOM only when the session itself cannot be built. Running out
 *   of room during the search is not a failure: a swap that cannot reserve
 *   what it needs mutates nothing and is treated as "this variable cannot move
 *   any further in this direction", so sifting continues and returns
 *   SYLVAN_OK with whatever improvement it managed.
 *
 * The forest is valid and consistent on every one of these paths.
 */
int sylvan_sift(const sylvan_sift_config *config);

/**
 * sylvan_sift, reporting what it did. <result> may be NULL, in which case this
 * is exactly sylvan_sift. On a failure <result> is zeroed rather than partly
 * filled: a session that never opened has nothing to report.
 */
int sylvan_sift_report(const sylvan_sift_config *config,
                       sylvan_sift_result *result);

/**
 * How many sifts have run since the process started.
 *
 * Monotone and never reset, not even by sylvan_quit(); the companion of
 * sylvan_gc_count(). Only a sift that opened a session counts, so a refused
 * call does not move it. It exists so that a caller can establish whether an
 * automatic sift happened, which is otherwise unobservable.
 */
uint64_t sylvan_sift_count(void);

/**
 * WHEN IS SIFTING WORTH IT
 *
 * Reordering is not free and its benefit is not monotone: sifting a forest
 * that has barely changed since the last time costs a stop-the-world pass and
 * finds nothing. What does pay is sifting a forest that has *grown*, because
 * growth is where a bad order compounds. So the recommendation Sylvan offers
 * is exactly that and nothing cleverer: the live forest has grown by at least
 * <factor> since the last sift, or since startup if there has not been one.
 *
 * The measurement is free. Every garbage collection already counts the buckets
 * that survive marking, in order to size the tables; the recommendation is one
 * comparison against that number, made once per collection. Nothing is
 * measured between collections, so the flag changes only when one runs, and
 * sylvan_sift_recommended() is a load.
 *
 * A forest smaller than a few thousand nodes never recommends, whatever the
 * factor: doubling a tiny forest says nothing about its order.
 *
 * sylvan_init_package() forgets the baseline, so a reinitialised package
 * measures growth from its own start. It does not touch the factor or the
 * automatic configuration below: those are caller policy, like the table
 * sizes, and are set once.
 */
int sylvan_sift_recommended(void);

/**
 * Set the growth factor the recommendation uses. The default is 2.0, meaning
 * a forest that has doubled. 0 disables the recommendation, so
 * sylvan_sift_recommended() is then always false and an automatic sift never
 * triggers. Values above 0 but below 1 are raised to 1. May be called at any
 * time; it takes effect at the next collection.
 */
void sylvan_sift_set_recommend_factor(double factor);

/**
 * Sift automatically after an explicit collection, when one is recommended.
 *
 * Pass a configuration to enable it -- the configuration is copied and is used
 * for every automatic sift, exactly as sylvan_sift() would use it -- or NULL
 * to disable it. Disabled is the default, and nothing about Sylvan's behaviour
 * changes until this is called.
 *
 * ONLY sylvan_gc() TRIGGERS IT, never the internal collection an operation
 * runs when the nodes table fills. That restriction is the whole reason this
 * facility is safe. The internal collection happens in the middle of an
 * operation's recursion, with sub-results already computed under the current
 * rank order and not yet combined; reordering there would leave the operation
 * to combine them under a different order, which is a wrong answer and not a
 * detectable one. sylvan_gc() has no such caller: its caller owns the
 * precondition that no operation is in flight, which is exactly the
 * precondition sifting needs. See sylvan_gc_collect() in <sylvan/common.h> for
 * the other side of the same distinction.
 *
 * The sift runs after the collection has finished, in a Lace frame of its own,
 * and opens a session which collects again. That second collection is the
 * price of keeping the two operations independent, and it is paid only when a
 * sift was going to run anyway.
 */
void sylvan_gc_set_auto_sift(const sylvan_sift_config *config);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
