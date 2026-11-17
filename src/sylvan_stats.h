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
    OPCOUNTER(BDD_PROJECT),
    OPCOUNTER(BDD_AND_EXISTS),
    OPCOUNTER(BDD_AND_PROJECT),
    OPCOUNTER(BDD_RELNEXT),
    OPCOUNTER(BDD_RELPREV),
    OPCOUNTER(BDD_SATCOUNT),
    OPCOUNTER(BDD_COMPOSE),
    OPCOUNTER(BDD_RESTRICT),
    OPCOUNTER(BDD_CONSTRAIN),
    OPCOUNTER(BDD_CLOSURE),
    OPCOUNTER(BDD_ISBDD),
    OPCOUNTER(BDD_SUPPORT),
    OPCOUNTER(BDD_PATHCOUNT),
    OPCOUNTER(BDD_DISJOINT),

    /* MTBDD operations */
    OPCOUNTER(MTBDD_APPLY),
    OPCOUNTER(MTBDD_UAPPLY),
    OPCOUNTER(MTBDD_ABSTRACT),
    OPCOUNTER(MTBDD_ITE),
    OPCOUNTER(MTBDD_EQUAL_NORM),
    OPCOUNTER(MTBDD_EQUAL_NORM_REL),
    OPCOUNTER(MTBDD_LEQ),
    OPCOUNTER(MTBDD_LESS),
    OPCOUNTER(MTBDD_GEQ),
    OPCOUNTER(MTBDD_GREATER),
    OPCOUNTER(MTBDD_AND_ABSTRACT_PLUS),
    OPCOUNTER(MTBDD_AND_ABSTRACT_MAX),
    OPCOUNTER(MTBDD_COMPOSE),
    OPCOUNTER(MTBDD_MINIMUM),
    OPCOUNTER(MTBDD_MAXIMUM),
    OPCOUNTER(MTBDD_EVAL_COMPOSE),

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
    OPCOUNTER(ZDD_ITE),
    OPCOUNTER(ZDD_NOT),
    OPCOUNTER(ZDD_DIFF),
    OPCOUNTER(ZDD_EXISTS),
    OPCOUNTER(ZDD_PROJECT),
    OPCOUNTER(ZDD_ISOP),
    OPCOUNTER(ZDD_COVER_TO_BDD),

    /* Other counters */
    SYLVAN_GC_COUNT,
    LLMSSET_LOOKUP,

    /* Exact counts, evaluation and checked cofactor */
    OPCOUNTER(BDD_SATCOUNT_U64),
    OPCOUNTER(BDD_SATCOUNT_GMP),
    OPCOUNTER(MTBDD_SATCOUNT_U64),
    OPCOUNTER(MTBDD_SATCOUNT_GMP),
    OPCOUNTER(LDD_SATCOUNT_U64),
    OPCOUNTER(LDD_SATCOUNT_GMP),
    OPCOUNTER(ZDD_SATCOUNT_U64),
    OPCOUNTER(ZDD_SATCOUNT_GMP),
    BDD_EVAL,
    MTBDD_EVAL,
    BDD_COFACTOR,

    OPCOUNTER(BDD_UNIQUE),
    OPCOUNTER(BDD_INTERSECTION_WITNESS),
    OPCOUNTER(BDD_PICK_REPRESENTATIVES),
    OPCOUNTER(BDD_CARDINALITY),
    /* Boolean ZDD completion */
    OPCOUNTER(ZDD_XOR),
    OPCOUNTER(ZDD_EQUIV),
    OPCOUNTER(ZDD_NAND),
    OPCOUNTER(ZDD_NOR),
    OPCOUNTER(ZDD_IMP),
    OPCOUNTER(ZDD_DISJOINT),
    OPCOUNTER(ZDD_SUBSET),
    OPCOUNTER(ZDD_FORALL),
    OPCOUNTER(ZDD_UNIQUE),
    OPCOUNTER(ZDD_MINIMAL_SETS),
    OPCOUNTER(ZDD_WITHOUT_SUPERSETS),
    OPCOUNTER(ZDD_BOOLEAN_CHECK),
    ZDD_EVAL_ASSIGNMENT,

    /* Append operations to preserve public counter values. */
    OPCOUNTER(BDD_REACH),
    OPCOUNTER(LDD_REACH),
    OPCOUNTER(LDD_EXTEND_REL),
    OPCOUNTER(MTBDD_MAP),
    OPCOUNTER(MTBDD_MAP_REDUCE),
    OPCOUNTER(MTBDD_COMPARE_LEQ),
    OPCOUNTER(MTBDD_COMPARE_LT),
    OPCOUNTER(MTBDD_COMPARE_GEQ),
    OPCOUNTER(MTBDD_COMPARE_GT),
    OPCOUNTER(MTBDD_COMPARE_EQUAL_ABS),
    OPCOUNTER(MTBDD_COMPARE_EQUAL_REL),
    OPCOUNTER(MTBDD_ANY_LEQ),
    OPCOUNTER(MTBDD_ANY_LT),
    OPCOUNTER(MTBDD_ANY_GEQ),
    OPCOUNTER(MTBDD_ANY_GT),
    OPCOUNTER(MTBDD_ANY_EQUAL_ABS),
    OPCOUNTER(MTBDD_ANY_EQUAL_REL),
    OPCOUNTER(MTBDD_ALL_EQUAL_ABS),
    OPCOUNTER(MTBDD_ALL_EQUAL_REL),
    OPCOUNTER(MTBDD_ALL_LEQ),
    OPCOUNTER(MTBDD_ALL_LT),
    OPCOUNTER(MTBDD_ALL_GEQ),
    OPCOUNTER(MTBDD_ALL_GT),
    OPCOUNTER(BDD_PROBABILITY),

    SYLVAN_ITERATOR_CREATED,
    SYLVAN_ITERATOR_ITEMS,

    MTBDD_LEVEL_STATISTICS,
    ZDD_LEVEL_STATISTICS,

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
 * Runtime diagnostics independent of the size of sylvan_stats_t.
 * Initialize with SYLVAN_STATISTICS_INIT before taking a snapshot. Version 1
 * and its minimum size are fixed; future extensions may only append fields.
 * <size> is the caller's writable byte capacity, including version and size.
 * Nodes used counts allocated slots (including unreachable nodes until GC
 * and reserved slots 0 and 1, which do not occupy hash entries). It may
 * exceed nodes_capacity by two when the node arena exceeds the hash size;
 * capacities are table slots, not the node-index bound after shrinking GC.
 * Counts and timers are zero with SYLVAN_STATS disabled; occupancy, capacities
 * and worker_count remain available. All timers in this API are nanoseconds.
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

/** The meaning of a named statistics record's value and cache fields. */
typedef enum sylvan_statistic_kind {
    SYLVAN_STATISTIC_COUNTER = 0,
    SYLVAN_STATISTIC_OPERATION = 1,
    SYLVAN_STATISTIC_TIMER = 2
} sylvan_statistic_kind;

/**
 * One named statistics record, with a fixed layout independent of the number
 * of counters. <name> is a borrowed, process-lifetime, case-sensitive label.
 * For a counter, <value> is the count. For an operation, it is the recursive
 * call count, with cache hits and puts in the corresponding fields. For a
 * timer it is elapsed nanoseconds. Unused fields, including reserved, are zero.
 * This record's size will not grow; extensions require a separate API.
 */
typedef struct sylvan_statistic {
    const char *name;
    sylvan_statistic_kind kind;
    uint32_t reserved;
    uint64_t value;
    uint64_t cache_hits;
    uint64_t cache_puts;
} sylvan_statistic;

/**
 * Unique reachable physical decision nodes at one variable level.
 * <level> is the variable number, <node_count> excludes leaves, and reserved
 * is zero. Results contain only occupied levels, sorted in increasing order.
 * This record has a fixed layout independent of the number of levels.
 */
typedef struct sylvan_level_statistic {
    uint32_t level;
    uint32_t reserved;
    uint64_t node_count;
} sylvan_level_statistic;

/**
 * Initialize stats system (done by sylvan_init_package)
 */
VOID_TASK_DECL_0(sylvan_stats_init);
#define sylvan_stats_init() RUN(sylvan_stats_init)

/**
 * Reset all counters (for statistics)
 */
VOID_TASK_DECL_0(sylvan_stats_reset);
#define sylvan_stats_reset() RUN(sylvan_stats_reset)

/**
 * Obtain current counts (this stops the world during counting).
 * The caller must supply the full sylvan_stats_t of the linked library. Its
 * size and timer offsets change when counters are appended: an older header
 * is not sufficient. This legacy function cannot detect a smaller buffer.
 * New code should prefer sylvan_statistics_snapshot and sylvan_statistics_read
 * or sylvan_statistics_read_counter, which do not expose this growing layout.
 */
VOID_TASK_DECL_1(sylvan_stats_snapshot, sylvan_stats_t*);
#define sylvan_stats_snapshot(target) RUN(sylvan_stats_snapshot, target)

/**
 * Obtain runtime diagnostics after sylvan_init_package and before sylvan_quit.
 * Counter aggregation and table/cache sampling stop the workers in separate
 * phases. Fields need not describe a single instant under concurrent activity.
 * Returns 1 on success, or 0 for NULL, an unsupported version, or size smaller
 * than SYLVAN_STATISTICS_V1_SIZE, without modification.
 * Writes at most min(target->size, sizeof(sylvan_statistics)) bytes, preserving
 * version, size and any caller bytes beyond the library's known structure.
 */
TASK_DECL_1(int, sylvan_statistics_snapshot, sylvan_statistics*)
#define sylvan_statistics_snapshot(target) RUN(sylvan_statistics_snapshot, target)

/**
 * Read all named counters, operations (including cache counts), and timers.
 * <count> must not be NULL. With entries == NULL and capacity == 0, returns 1
 * and sets *count to the required number of records without sampling counters.
 * Otherwise capacity is the number of writable fixed-size records in entries.
 * If insufficient, returns 0 and sets *count to the required capacity, leaving
 * entries untouched. On success returns 1, sets *count and fills that many
 * entries. Record order is unspecified; names match sylvan_stats_report labels.
 * Returns 0 without modification for NULL count or NULL entries with nonzero
 * capacity. Values are aggregated across all workers, or zero when statistics
 * are disabled. Requires an initialized package, like the snapshot operation.
 */
TASK_DECL_3(int, sylvan_statistics_read, sylvan_statistic*, size_t, size_t*)
#define sylvan_statistics_read(entries, capacity, count) RUN(sylvan_statistics_read, entries, capacity, count)

/**
 * Read one raw counter by its Sylvan_Counters id, including _CACHED and
 * _CACHEDPUT ids. Returns 1 and writes the sum across workers (zero with
 * statistics disabled), or 0 for an out-of-range id or NULL value, leaving
 * the output unchanged. Timer ids belong to a different enum and are not
 * accepted as timer requests here. Requires an initialized package.
 */
TASK_DECL_2(int, sylvan_statistics_read_counter, size_t, uint64_t*)
#define sylvan_statistics_read_counter(id, value) RUN(sylvan_statistics_read_counter, id, value)

/**
 * Read one named record, with the same semantics as sylvan_statistics_read.
 * Names are matched exactly, including case. Returns 1 on success, or 0 for
 * an unknown name or NULL argument without modifying entry. Requires an
 * initialized package; labels returned by sylvan_statistics_read can be used
 * directly. For example, "BDD and" names an operation and "GC executions"
 * names a counter. Timer records, such as "Total time spent", use nanoseconds.
 */
TASK_DECL_2(int, sylvan_statistics_read_name, const char*, sylvan_statistic*)
#define sylvan_statistics_read_name(name, entry) RUN(sylvan_statistics_read_name, name, entry)

/**
 * Write statistic report to file (stdout, stderr, etc)
 */
void sylvan_stats_report(FILE* target);

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
