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
     * is 32. 0 moves every variable that has at least one node.
     */
    uint32_t nodes_threshold;

    /**
     * Seconds of wall clock time the search may take, 0 for no limit. The
     * limit is checked between swaps, so a single swap may overrun it.
     */
    double time_limit;
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
    uint32_t vars_sifted;    /* variables whose excursion was run          */
} sylvan_sift_result;

/**
 * Reorder the variables of the BDD/MTBDD forest by sifting.
 *
 * Call from inside a Lace worker. Sifting stops the world for its whole
 * duration, exactly like garbage collection, and it begins by collecting.
 * Every diagram reachable from mtbdd_protect() survives and denotes the same
 * function afterwards; so does every handle, because nodes are rewritten in
 * place. Handles held only on a worker's temporary reference stack do NOT
 * survive, and no decision diagram operation may be in flight -- which also
 * means that only one worker may be inside this call at a time, since a second
 * one would be an operation in flight as far as the first is concerned.
 *
 * Returns SYLVAN_OK, or a negative SYLVAN_ERR_*:
 *
 * - SYLVAN_ERR_INVALID when the forest cannot be reordered at all: MTBDD
 *   support is not initialised, the nodes table implements no reorder
 *   primitives, the ZDD or ListDD subsystem is initialised (both order their
 *   nodes by variable identity rather than by rank), or a garbage collection
 *   mark hook other than Sylvan's own is registered, whose external roots
 *   sifting cannot enumerate. Nothing is changed in any of these cases.
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

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
