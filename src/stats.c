/*
 * Copyright 2011-2014 Formal Methods and Tools, University of Twente
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

#include <string.h> // memset
#include <stats.h>
#include <sys/mman.h>
#include <inttypes.h>
#include <sylvan.h> // for nodes table

#ifdef __ELF__
__thread sylvan_stats_t sylvan_stats;
#else
pthread_key_t sylvan_stats_key;
#endif

VOID_TASK_0(sylvan_stats_reset_perthread)
{
#ifdef __ELF__
    for (int i=0; i<SYLVAN_COUNTER_COUNTER; i++) {
        sylvan_stats.counters[i] = 0;
    }
    for (int i=0; i<SYLVAN_TIMER_COUNTER; i++) {
        sylvan_stats.timers[i] = 0;
    }
#else
    sylvan_stats_t *sylvan_stats = pthread_getspecific(sylvan_stats_key);
    if (sylvan_stats == NULL) {
        mmap(sylvan_stats, sizeof(sylvan_stats_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        // TODO: hwloc
        pthread_setspecific(sylvan_stats_key, sylvan_stats);
    }
    for (int i=0; i<SYLVAN_COUNTER_COUNTER; i++) {
        sylvan_stats->counters[i] = 0;
    }
    for (int i=0; i<SYLVAN_TIMER_COUNTER; i++) {
        sylvan_stats->timers[i] = 0;
    }
#endif
}

VOID_TASK_IMPL_0(sylvan_stats_init)
{
#ifndef __ELF__
    pthread_key_create(&sylvan_stats_key, NULL);
#endif
    TOGETHER(sylvan_stats_reset_perthread);
}

/**
 * Reset all counters (for statistics)
 */
VOID_TASK_IMPL_0(sylvan_stats_reset)
{
    TOGETHER(sylvan_stats_reset_perthread);
}

#define BLACK "\33[22;30m"
#define GRAY "\33[01;30m"
#define RED "\33[22;31m"
#define LRED "\33[01;31m"
#define GREEN "\33[22;32m"
#define LGREEN "\33[01;32m"
#define BLUE "\33[22;34m"
#define LBLUE "\33[01;34m"
#define BROWN "\33[22;33m"
#define YELLOW "\33[01;33m"
#define CYAN "\33[22;36m"
#define LCYAN "\33[22;36m"
#define MAGENTA "\33[22;35m"
#define LMAGENTA "\33[01;35m"
#define NC "\33[0m"
#define BOLD "\33[1m"
#define ULINE "\33[4m" //underline
#define BLINK "\33[5m"
#define INVERT "\33[7m"

VOID_TASK_1(sylvan_stats_sum, sylvan_stats_t*, target)
{
#ifdef __ELF__
    for (int i=0; i<SYLVAN_COUNTER_COUNTER; i++) {
        __sync_fetch_and_add(&target->counters[i], sylvan_stats.counters[i]);
    }
    for (int i=0; i<SYLVAN_TIMER_COUNTER; i++) {
        __sync_fetch_and_add(&target->timers[i], sylvan_stats.timers[i]);
    }
#else
    sylvan_stats_t *sylvan_stats = pthread_getspecific(sylvan_stats_key);
    if (sylvan_stats != NULL) {
        for (int i=0; i<SYLVAN_COUNTER_COUNTER; i++) {
            __sync_fetch_and_add(&target->counters[i], sylvan_stats->counters[i]);
        }
        for (int i=0; i<SYLVAN_TIMER_COUNTER; i++) {
            __sync_fetch_and_add(&target->timers[i], sylvan_stats->timers[i]);
        }
    }
#endif
}

void
sylvan_stats_report(FILE *target, int color)
{
#if !SYLVAN_STATS
    (void)target;
    return;
#endif
    (void)color;

    sylvan_stats_t totals;
    memset(&totals, 0, sizeof(sylvan_stats_t));

    LACE_ME;
    TOGETHER(sylvan_stats_sum, &totals);

    fprintf(target, LRED  "****************\n");
    fprintf(target,      "* ");
    fprintf(target, NC BOLD"SYLVAN STATS");
    fprintf(target, NC LRED             " *\n");
    fprintf(target,      "****************\n");
    fprintf(target, "\n");

    fprintf(target, NC ULINE "BDD operations count (cache reuse)\n" NC LBLUE);
    if (totals.counters[BDD_ITE]) fprintf(target, "ITE: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_ITE], totals.counters[BDD_ITE_CACHED]);
    if (totals.counters[BDD_EXISTS]) fprintf(target, "Exists: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_EXISTS], totals.counters[BDD_EXISTS_CACHED]);
    if (totals.counters[BDD_AND_EXISTS]) fprintf(target, "AndExists: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_AND_EXISTS], totals.counters[BDD_AND_EXISTS_CACHED]);
    if (totals.counters[BDD_RELNEXT]) fprintf(target, "RelNext: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_RELNEXT], totals.counters[BDD_RELNEXT_CACHED]);
    if (totals.counters[BDD_RELPREV]) fprintf(target, "RelPrev: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_RELPREV], totals.counters[BDD_RELPREV_CACHED]);
    if (totals.counters[BDD_CLOSURE]) fprintf(target, "Closure: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_CLOSURE], totals.counters[BDD_CLOSURE_CACHED]);
    if (totals.counters[BDD_COMPOSE]) fprintf(target, "Compose: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_COMPOSE], totals.counters[BDD_COMPOSE_CACHED]);
    if (totals.counters[BDD_RESTRICT]) fprintf(target, "Restrict: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_RESTRICT], totals.counters[BDD_RESTRICT_CACHED]);
    if (totals.counters[BDD_CONSTRAIN]) fprintf(target, "Constrain: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_CONSTRAIN], totals.counters[BDD_CONSTRAIN_CACHED]);
    if (totals.counters[BDD_SUPPORT]) fprintf(target, "Support: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_SUPPORT], totals.counters[BDD_SUPPORT_CACHED]);
    if (totals.counters[BDD_SATCOUNT]) fprintf(target, "SatCount: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_SATCOUNT], totals.counters[BDD_SATCOUNT_CACHED]);
    if (totals.counters[BDD_PATHCOUNT]) fprintf(target, "PathCount: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_PATHCOUNT], totals.counters[BDD_PATHCOUNT_CACHED]);
    if (totals.counters[BDD_ISBDD]) fprintf(target, "IsBDD: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[BDD_ISBDD], totals.counters[BDD_ISBDD_CACHED]);
    if (totals.counters[BDD_NODES_CREATED]) fprintf(target, "BDD Nodes created: %'"PRIu64"\n", totals.counters[BDD_NODES_CREATED]);
    if (totals.counters[BDD_NODES_REUSED]) fprintf(target, "BDD Nodes reused: %'"PRIu64"\n", totals.counters[BDD_NODES_REUSED]);

    fprintf(target, "\n");
    fprintf(target, NC ULINE "LDD operations count (cache reuse)\n" NC LBLUE);
    if (totals.counters[LDD_UNION]) fprintf(target, "Union: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_UNION], totals.counters[LDD_UNION_CACHED]);
    if (totals.counters[LDD_MINUS]) fprintf(target, "Minus: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_MINUS], totals.counters[LDD_MINUS_CACHED]);
    if (totals.counters[LDD_INTERSECT]) fprintf(target, "Intersect: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_INTERSECT], totals.counters[LDD_INTERSECT_CACHED]);
    if (totals.counters[LDD_RELPROD]) fprintf(target, "RelProd: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_RELPROD], totals.counters[LDD_RELPROD_CACHED]);
    if (totals.counters[LDD_RELPREV]) fprintf(target, "RelPrev: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_RELPREV], totals.counters[LDD_RELPREV_CACHED]);
    if (totals.counters[LDD_PROJECT]) fprintf(target, "Project: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_PROJECT], totals.counters[LDD_PROJECT_CACHED]);
    if (totals.counters[LDD_JOIN]) fprintf(target, "Join: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_JOIN], totals.counters[LDD_JOIN_CACHED]);
    if (totals.counters[LDD_MATCH]) fprintf(target, "Match: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_MATCH], totals.counters[LDD_MATCH_CACHED]);
    if (totals.counters[LDD_SATCOUNT]) fprintf(target, "SatCount: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_SATCOUNT], totals.counters[LDD_SATCOUNT_CACHED]);
    if (totals.counters[LDD_SATCOUNTL]) fprintf(target, "SatCountL: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_SATCOUNTL], totals.counters[LDD_SATCOUNTL_CACHED]);
    if (totals.counters[LDD_ZIP]) fprintf(target, "Zip: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_ZIP], totals.counters[LDD_ZIP_CACHED]);
    if (totals.counters[LDD_RELPROD_UNION]) fprintf(target, "RelProdUnion: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_RELPROD_UNION], totals.counters[LDD_RELPROD_UNION_CACHED]);
    if (totals.counters[LDD_PROJECT_MINUS]) fprintf(target, "ProjectMinus: %'"PRIu64 " (%'"PRIu64")\n", totals.counters[LDD_PROJECT_MINUS], totals.counters[LDD_PROJECT_MINUS_CACHED]);
    if (totals.counters[LDD_NODES_CREATED]) fprintf(target, "LDD Nodes created: %'"PRIu64"\n", totals.counters[LDD_NODES_CREATED]);
    if (totals.counters[LDD_NODES_REUSED]) fprintf(target, "LDD Nodes reused: %'"PRIu64"\n", totals.counters[LDD_NODES_REUSED]);

    fprintf(target, "\n");
    fprintf(target, NC ULINE "Garbage collection\n" NC LBLUE);
    fprintf(target, "Number of GC executions: %'"PRIu64"\n", totals.counters[SYLVAN_GC_COUNT]);
    fprintf(target, "Total time spent: %'"PRIu64".%'"PRIu64" sec.\n", totals.timers[SYLVAN_GC]/1000000000UL, (totals.timers[SYLVAN_GC]%1000000000)/1000000);

    fprintf(target, "\n");
    fprintf(target, "BDD Unique table: %zu of %zu buckets filled.\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
    fprintf(target, LRED  "****************" NC " \n");
}
