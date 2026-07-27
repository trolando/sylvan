/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
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

#ifndef SYLVAN_STATS_H
#define SYLVAN_STATS_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define OPCOUNTER(NAME) NAME, NAME ## _CACHEDPUT, NAME ## _CACHED

typedef enum {
    /* Creating nodes */
    BDD_NODES_CREATED,
    BDD_NODES_REUSED,
    LDD_NODES_CREATED,
    LDD_NODES_REUSED,
    ZDD_NODES_CREATED,
    ZDD_NODES_REUSED,

    /* BDD operations */
    OPCOUNTER(BDD_ITE),
    OPCOUNTER(BDD_AND),
    OPCOUNTER(BDD_XOR),
    OPCOUNTER(BDD_EXISTS),
    OPCOUNTER(BDD_UNIQUE),
    OPCOUNTER(BDD_PROJECT),
    OPCOUNTER(BDD_AND_EXISTS),
    OPCOUNTER(BDD_AND_PROJECT),
    OPCOUNTER(BDD_RELNEXT),
    OPCOUNTER(BDD_RELPREV),
    OPCOUNTER(BDD_SAT_COUNT_U64),
    OPCOUNTER(BDD_SAT_COUNT_DOUBLE),
    OPCOUNTER(BDD_SAT_COUNT_GMP),
    OPCOUNTER(BDD_COMPOSE),
    OPCOUNTER(BDD_EVAL),
    OPCOUNTER(BDD_SIMPLIFY),
    OPCOUNTER(BDD_CONSTRAIN),
    OPCOUNTER(BDD_CLOSURE),
    OPCOUNTER(BDD_ISBDD),
    OPCOUNTER(BDD_SUPPORT),
    OPCOUNTER(BDD_PATHCOUNT),
    OPCOUNTER(BDD_DISJOINT),
    OPCOUNTER(BDD_INTERSECTION_WITNESS),
    OPCOUNTER(BDD_APPLY_ABSTRACT),
    OPCOUNTER(BDD_PICK_REPRESENTATIVES),
    OPCOUNTER(BDD_PROBABILITY),
    OPCOUNTER(BDD_PROBABILITY_BATCH),
    OPCOUNTER(BDD_PROBABILITY_GRADIENT),
    OPCOUNTER(BDD_CARDINALITY),

    /* MTBDD operations */
    OPCOUNTER(MTBDD_APPLY),
    OPCOUNTER(MTBDD_UAPPLY),
    OPCOUNTER(MTBDD_MAP),
    OPCOUNTER(MTBDD_ABSTRACT),
    OPCOUNTER(MTBDD_MAP_REDUCE),
    OPCOUNTER(MTBDD_COMBINE_REDUCE),
    OPCOUNTER(MTBDD_ITE),
    OPCOUNTER(MTBDD_ALL_EQUAL_ABS),
    OPCOUNTER(MTBDD_ALL_EQUAL_REL),
    OPCOUNTER(MTBDD_ALL_LEQ),
    OPCOUNTER(MTBDD_ALL_LT),
    OPCOUNTER(MTBDD_ALL_GEQ),
    OPCOUNTER(MTBDD_ALL_GT),
    OPCOUNTER(MTBDD_AND_ABSTRACT_PLUS),
    OPCOUNTER(MTBDD_AND_ABSTRACT_MAX),
    OPCOUNTER(MTBDD_COMPOSE),
    OPCOUNTER(MTBDD_EVAL),
    OPCOUNTER(MTBDD_MINIMUM),
    OPCOUNTER(MTBDD_MAXIMUM),
    OPCOUNTER(MTBDD_EVAL_COMPOSE),
    OPCOUNTER(MTBDD_ANY_LEQ),
    OPCOUNTER(MTBDD_ANY_LT),
    OPCOUNTER(MTBDD_ANY_GEQ),
    OPCOUNTER(MTBDD_ANY_GT),
    OPCOUNTER(MTBDD_ANY_EQUAL_ABS),
    OPCOUNTER(MTBDD_ANY_EQUAL_REL),
    OPCOUNTER(MTBDD_COMPARE_LEQ),
    OPCOUNTER(MTBDD_COMPARE_LT),
    OPCOUNTER(MTBDD_COMPARE_GEQ),
    OPCOUNTER(MTBDD_COMPARE_GT),
    OPCOUNTER(MTBDD_COMPARE_EQUAL_ABS),
    OPCOUNTER(MTBDD_COMPARE_EQUAL_REL),
    OPCOUNTER(MTBDD_SAT_COUNT_U64),
    OPCOUNTER(MTBDD_SAT_COUNT_DOUBLE),
    OPCOUNTER(MTBDD_SAT_COUNT_GMP),

    /* LDD operations */
    OPCOUNTER(LDD_UNION),
    OPCOUNTER(LDD_MINUS),
    OPCOUNTER(LDD_INTERSECT),
    OPCOUNTER(LDD_RELPROD),
    OPCOUNTER(LDD_RELPREV),
    OPCOUNTER(LDD_PROJECT),
    OPCOUNTER(LDD_JOIN),
    OPCOUNTER(LDD_MATCH),
    OPCOUNTER(LDD_SATCOUNT),
    OPCOUNTER(LDD_SATCOUNTL),
    OPCOUNTER(LDD_SATCOUNT_GMP),
    OPCOUNTER(LDD_ZIP),
    OPCOUNTER(LDD_RELPROD_UNION),
    OPCOUNTER(LDD_PROJECT_MINUS),

    /* ZDD operations */
    OPCOUNTER(ZDD_FROM_MTBDD),
    OPCOUNTER(ZDD_TO_MTBDD),
    OPCOUNTER(ZDD_UNION_CUBE),
    OPCOUNTER(ZDD_EXTEND_DOMAIN),
    OPCOUNTER(ZDD_SUPPORT),
    OPCOUNTER(ZDD_PATHCOUNT),
    OPCOUNTER(ZDD_AND),
    OPCOUNTER(ZDD_OR),
    OPCOUNTER(ZDD_XOR),
    OPCOUNTER(ZDD_ITE),
    OPCOUNTER(ZDD_NOT),
    OPCOUNTER(ZDD_DIFF),
    OPCOUNTER(ZDD_EXISTS),
    OPCOUNTER(ZDD_FORALL),
    OPCOUNTER(ZDD_UNIQUE),
    OPCOUNTER(ZDD_PROJECT),
    OPCOUNTER(ZDD_COUNT_U64),
    OPCOUNTER(ZDD_COUNT_GMP),
    OPCOUNTER(ZDD_ISOP),
    OPCOUNTER(ZDD_COVER_TO_BDD),
    OPCOUNTER(ZDD_WITHOUT_SUPERSETS),
    OPCOUNTER(ZDD_MINIMAL_SETS),

    /* Other counters */
    SYLVAN_ITERATOR_CREATED,
    SYLVAN_ITERATOR_ITEMS,
    SYLVAN_GC_COUNT,
    LLMSSET_LOOKUP,

    SYLVAN_COUNTER_COUNTER
} Sylvan_Counters;

#undef OPCOUNTER

typedef enum
{
    SYLVAN_GC,
    SYLVAN_TIMER_COUNTER
} Sylvan_Timers;

typedef struct
{
    uint64_t counters[SYLVAN_COUNTER_COUNTER];
    /* the timers are in ns */
    uint64_t timers[SYLVAN_TIMER_COUNTER];
    /* startstop is for internal use */
    uint64_t timers_startstop[SYLVAN_TIMER_COUNTER];
} sylvan_stats_t;

/**
 * Stable, named runtime diagnostics.
 *
 * Initialize <version> and <size> with SYLVAN_STATISTICS_INIT before calling
 * sylvan_statistics_snapshot. The versioned, sized structure may be extended
 * without making callers depend on the internal counter-array layout above.
 * Counter aggregation stops the workers. Table and cache occupancies are
 * exact when no concurrent external caller mutates them; otherwise they are
 * observational diagnostics. Statistics fields are zero when Sylvan was
 * built without statistics.
 */
#define SYLVAN_STATISTICS_VERSION UINT32_C(1)

typedef struct sylvan_statistics {
    uint32_t version;
    uint32_t size;
    uint64_t nodes_used;
    uint64_t nodes_capacity;
    uint64_t nodes_max_capacity;
    uint64_t cache_used;
    uint64_t cache_capacity;
    uint64_t cache_max_capacity;
    uint64_t gc_count;
    uint64_t gc_time_ns;
    uint32_t worker_count;
    uint32_t statistics_enabled;
} sylvan_statistics;

#define SYLVAN_STATISTICS_V1_SIZE \
    ((uint32_t)(offsetof(sylvan_statistics, statistics_enabled) + \
                sizeof(((sylvan_statistics*)0)->statistics_enabled)))

#define SYLVAN_STATISTICS_INIT \
    { SYLVAN_STATISTICS_VERSION, (uint32_t)sizeof(sylvan_statistics), \
      UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT64_C(0), \
      UINT64_C(0), UINT64_C(0), UINT64_C(0), UINT32_C(0), UINT32_C(0) }

typedef enum sylvan_statistic_kind {
    SYLVAN_STATISTIC_COUNTER = 0,
    SYLVAN_STATISTIC_OPERATION = 1,
    SYLVAN_STATISTIC_TIMER = 2
} sylvan_statistic_kind;

/**
 * One programmatic statistics record.
 *
 * <name> is a borrowed, process-lifetime label. For a counter, <value> is the
 * count. For an operation, <value> is the call count and the cache fields are
 * populated. For a timer, <value> is nanoseconds. Unused fields are zero.
 */
typedef struct sylvan_statistic {
    const char *name;
    sylvan_statistic_kind kind;
    uint32_t reserved;
    uint64_t value;
    uint64_t cache_hits;
    uint64_t cache_puts;
} sylvan_statistic;

/** Unique reachable decision nodes at one sparse variable level. */
typedef struct sylvan_level_statistic {
    uint32_t level;
    uint32_t reserved;
    uint64_t node_count;
} sylvan_level_statistic;

/**
 * Initialize stats system (done by sylvan_init_package)
 */
static inline void sylvan_stats_init(void);

/**
 * Reset all counters (for statistics)
 */
static inline void sylvan_stats_reset(void);

/**
 * Obtain current counts (this stops the world during counting)
 */
static inline void sylvan_stats_snapshot(sylvan_stats_t* target);

/**
 * Obtain a stable runtime-diagnostics snapshot.
 *
 * Returns SYLVAN_OK on success or SYLVAN_ERR_INVALID for a null target, an
 * unsupported version, or a structure smaller than version 1.
 */
static inline int sylvan_statistics_snapshot(sylvan_statistics *target);

/**
 * Obtain all named counters, operation counts, and timers.
 *
 * With <entries> NULL and <capacity> zero, writes the required number of
 * entries to <count>. If the supplied capacity is too small, writes the
 * required count and returns SYLVAN_ERR_OVERFLOW without modifying entries.
 */
static inline int sylvan_statistics_read(
    sylvan_statistic *entries, size_t capacity, size_t *count);

/**
 * Profile unique physical nodes reachable from several MTBDD/BDD roots.
 *
 * Entries are sorted by sparse variable level; terminals are excluded from
 * the entries and reported separately through <leaf_count>. Complemented
 * edges do not duplicate physical nodes. The caller must protect every root.
 *
 * With <entries> NULL and <capacity> zero, writes the required number of
 * levels to <count>. Insufficient capacity returns SYLVAN_ERR_OVERFLOW without
 * modifying entries. A successful empty-root profile has zero levels/leaves.
 */
static inline int mtbdd_level_statistics(
    const MTBDD *roots, size_t root_count,
    sylvan_level_statistic *entries, size_t capacity,
    size_t *count, uint64_t *leaf_count);

/**
 * ZDD counterpart of mtbdd_level_statistics.
 *
 * ZDD terminals are excluded from level entries and counted in <leaf_count>.
 * The caller must protect every root.
 */
static inline int zdd_level_statistics(
    const ZDD *roots, size_t root_count,
    sylvan_level_statistic *entries, size_t capacity,
    size_t *count, uint64_t *leaf_count);

/**
 * Write statistic report to file (stdout, stderr, etc)
 */
void sylvan_stats_report(FILE* target);

TASK(void, sylvan_stats_init)
TASK(void, sylvan_stats_reset)
TASK(void, sylvan_stats_snapshot, sylvan_stats_t*, target)
TASK(int, sylvan_statistics_snapshot, sylvan_statistics*, target)
TASK(int, sylvan_statistics_read, sylvan_statistic*, entries,
     size_t, capacity, size_t*, count)
TASK(int, mtbdd_level_statistics, const MTBDD*, roots, size_t, root_count,
     sylvan_level_statistic*, entries, size_t, capacity,
     size_t*, count, uint64_t*, leaf_count)
TASK(int, zdd_level_statistics, const ZDD*, roots, size_t, root_count,
     sylvan_level_statistic*, entries, size_t, capacity,
     size_t*, count, uint64_t*, leaf_count)

#if SYLVAN_STATS

#ifdef __MACH__
#define getabstime() mach_absolute_time()
#else
static uint64_t
getabstime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t t = ts.tv_sec;
    t *= 1000000000UL;
    t += ts.tv_nsec;
    return t;
}
#endif

#ifdef __ELF__
extern __thread sylvan_stats_t sylvan_stats;
#else
extern pthread_key_t sylvan_stats_key;
#endif

static inline void
sylvan_stats_count(size_t counter)
{
#ifdef __ELF__
    sylvan_stats.counters[counter]++;
#else
    sylvan_stats_t *sylvan_stats = (sylvan_stats_t*)pthread_getspecific(sylvan_stats_key);
    sylvan_stats->counters[counter]++;
#endif
}

static inline void
sylvan_stats_add(size_t counter, size_t amount)
{
#ifdef __ELF__
    sylvan_stats.counters[counter]+=amount;
#else
    sylvan_stats_t *sylvan_stats = (sylvan_stats_t*)pthread_getspecific(sylvan_stats_key);
    sylvan_stats->counters[counter]+=amount;
#endif
}

static inline void
sylvan_timer_start(size_t timer)
{
    uint64_t t = getabstime();

#ifdef __ELF__
    sylvan_stats.timers_startstop[timer] = t;
#else
    sylvan_stats_t *sylvan_stats = (sylvan_stats_t*)pthread_getspecific(sylvan_stats_key);
    sylvan_stats->timers_startstop[timer] = t;
#endif
}

static inline void
sylvan_timer_stop(size_t timer)
{
    uint64_t t = getabstime();

#ifdef __ELF__
    sylvan_stats.timers[timer] += (t - sylvan_stats.timers_startstop[timer]);
#else
    sylvan_stats_t *sylvan_stats = (sylvan_stats_t*)pthread_getspecific(sylvan_stats_key);
    sylvan_stats->timers[timer] += (t - sylvan_stats->timers_startstop[timer]);
#endif
}

#else

static inline void
sylvan_stats_count(size_t counter)
{
    (void)counter;
}

static inline void
sylvan_stats_add(size_t counter, size_t amount)
{
    (void)counter;
    (void)amount;
}

static inline void
sylvan_timer_start(size_t timer)
{
    (void)timer;
}

static inline void
sylvan_timer_stop(size_t timer)
{
    (void)timer;
}

#endif

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
