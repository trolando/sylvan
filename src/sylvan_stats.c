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

#include <sylvan_int.h>
#include <sylvan_platform.h>

#include <errno.h>  // for errno
#include <string.h> // memset
#include <inttypes.h>

/**
 * Instructions for sylvan_stats_report
 */
static const struct
{
    int type; /* 0 for print line, 1 for simple counter, 2 for operation with CACHED and CACHEDPUT */
              /* 3 for timer, 4 for report table data */
    int id;
    const char *key;
} sylvan_report_info[] =
{
    {0, 0, "Tables"},
    {1, BDD_NODES_CREATED, "MTBDD nodes created"},
    {1, BDD_NODES_REUSED, "MTBDD nodes reused"},
    {1, ZDD_NODES_CREATED, "ZDD nodes created"},
    {1, ZDD_NODES_REUSED, "ZDD nodes reused"},
    {1, LDD_NODES_CREATED, "LDD nodes created"},
    {1, LDD_NODES_REUSED, "LDD nodes reused"},
    {1, LLMSSET_LOOKUP, "Lookup iterations"},
    {4, 0, NULL}, /* trigger to report unique nodes and operation cache */

    {0, 0, "Operation            Count            Cache get        Cache put"},
    {2, BDD_AND, "BDD and"},
    {2, BDD_XOR, "BDD xor"},
    {2, BDD_ITE, "BDD ite"},
    {2, BDD_EXISTS, "BDD exists"},
    {2, BDD_UNIQUE, "BDD unique"},
    {2, BDD_INTERSECTION_WITNESS, "BDD intersection witness"},
    {2, BDD_PICK_REPRESENTATIVES, "BDD pick representatives"},
    {2, BDD_CARDINALITY, "BDD cardinality"},
    {2, BDD_PROJECT, "BDD project"},
    {2, BDD_AND_EXISTS, "BDD andexists"},
    {2, BDD_AND_PROJECT, "BDD andproject"},
    {2, BDD_RELNEXT, "BDD relnext"},
    {2, BDD_RELPREV, "BDD relprev"},
    {2, BDD_CLOSURE, "BDD closure"},
    {2, BDD_COMPOSE, "BDD compose"},
    {2, BDD_RESTRICT, "BDD restrict"},
    {2, BDD_CONSTRAIN, "BDD constrain"},
    {2, BDD_SUPPORT, "BDD support"},
    {2, BDD_SATCOUNT, "BDD satcount"},
    {2, BDD_PROBABILITY, "BDD probability"},
    {2, BDD_PATHCOUNT, "BDD pathcount"},
    {2, BDD_ISBDD, "BDD isbdd"},
    {2, BDD_DISJOINT, "BDD disjoint"},

    {2, MTBDD_APPLY, "MTBDD binary apply"},
    {2, MTBDD_UAPPLY, "MTBDD unary apply"},
    {2, MTBDD_MAP, "MTBDD map"},
    {2, MTBDD_MAP_REDUCE, "MTBDD map_reduce"},
    {2, MTBDD_COMPARE_LEQ, "MTBDD compare_leq"},
    {2, MTBDD_COMPARE_LT, "MTBDD compare_lt"},
    {2, MTBDD_COMPARE_GEQ, "MTBDD compare_geq"},
    {2, MTBDD_COMPARE_GT, "MTBDD compare_gt"},
    {2, MTBDD_COMPARE_EQUAL_ABS, "MTBDD compare_equal_abs"},
    {2, MTBDD_COMPARE_EQUAL_REL, "MTBDD compare_equal_rel"},
    {2, MTBDD_ANY_LEQ, "MTBDD any_leq"},
    {2, MTBDD_ANY_LT, "MTBDD any_lt"},
    {2, MTBDD_ANY_GEQ, "MTBDD any_geq"},
    {2, MTBDD_ANY_GT, "MTBDD any_gt"},
    {2, MTBDD_ANY_EQUAL_ABS, "MTBDD any_equal_abs"},
    {2, MTBDD_ANY_EQUAL_REL, "MTBDD any_equal_rel"},
    {2, MTBDD_ALL_EQUAL_ABS, "MTBDD all_equal_abs"},
    {2, MTBDD_ALL_EQUAL_REL, "MTBDD all_equal_rel"},
    {2, MTBDD_ALL_LEQ, "MTBDD all_leq"},
    {2, MTBDD_ALL_LT, "MTBDD all_lt"},
    {2, MTBDD_ALL_GEQ, "MTBDD all_geq"},
    {2, MTBDD_ALL_GT, "MTBDD all_gt"},
    {2, MTBDD_ABSTRACT, "MTBDD abstract"},
    {2, MTBDD_ITE, "MTBDD ite"},
    {2, MTBDD_EQUAL_NORM, "MTBDD eq norm"},
    {2, MTBDD_EQUAL_NORM_REL, "MTBDD eq norm rel"},
    {2, MTBDD_LEQ, "MTBDD leq"},
    {2, MTBDD_LESS, "MTBDD less"},
    {2, MTBDD_GEQ, "MTBDD geq"},
    {2, MTBDD_GREATER, "MTBDD greater"},
    {2, MTBDD_AND_ABSTRACT_PLUS, "MTBDD and_abs_plus"},
    {2, MTBDD_AND_ABSTRACT_MAX, "MTBDD and_abs_max"},
    {2, MTBDD_COMPOSE, "MTBDD compose"},
    {2, MTBDD_MINIMUM, "MTBDD minimum"},
    {2, MTBDD_MAXIMUM, "MTBDD maximum"},
    {2, MTBDD_EVAL_COMPOSE, "MTBDD eval_compose"},

    {2, LDD_UNION, "LDD union"},
    {2, LDD_MINUS, "LDD minus"},
    {2, LDD_INTERSECT, "LDD intersect"},
    {2, LDD_RELPROD, "LDD relprod"},
    {2, LDD_RELPREV, "LDD relprev"},
    {2, LDD_PROJECT, "LDD project"},
    {2, LDD_JOIN, "LDD join"},
    {2, LDD_MATCH, "LDD match"},
    {2, LDD_SATCOUNT, "LDD satcount"},
    {2, LDD_SATCOUNTL, "LDD satcountl"},
    {2, LDD_ZIP, "LDD zip"},
    {2, LDD_RELPROD_UNION, "LDD relprod_union"},
    {2, LDD_PROJECT_MINUS, "LDD project_minus"},

    {2, ZDD_FROM_MTBDD, "ZDD from_mtbdd"},
    {2, ZDD_TO_MTBDD, "ZDD to_mtbdd"},
    {2, ZDD_UNION_CUBE, "ZDD union_cube"},
    {2, ZDD_EXTEND_DOMAIN, "ZDD ext_domain"},
    {2, ZDD_SUPPORT, "ZDD support" },
    {2, ZDD_PATHCOUNT, "ZDD pathcount" },
    {2, ZDD_AND, "ZDD and" },
    {2, ZDD_OR, "ZDD or" },
    {2, ZDD_ITE, "ZDD ite" },
    {2, ZDD_NOT, "ZDD not" },
    {2, ZDD_DIFF, "ZDD diff" },
    {2, ZDD_EXISTS, "ZDD exists" },
    {2, ZDD_PROJECT, "ZDD project" },
    {2, ZDD_ISOP, "zdd isop"},
    {2, ZDD_COVER_TO_BDD, "zdd cover_to_bdd"},
    {2, ZDD_XOR, "ZDD xor"},
    {2, ZDD_EQUIV, "ZDD equiv"},
    {2, ZDD_NAND, "ZDD nand"},
    {2, ZDD_NOR, "ZDD nor"},
    {2, ZDD_IMP, "ZDD imp"},
    {2, ZDD_DISJOINT, "ZDD disjoint"},
    {2, ZDD_SUBSET, "ZDD subset"},
    {2, ZDD_FORALL, "ZDD forall"},
    {2, ZDD_UNIQUE, "ZDD unique"},
    {2, ZDD_MINIMAL_SETS, "ZDD minimal_sets"},
    {2, ZDD_WITHOUT_SUPERSETS, "ZDD without_supersets"},
    {2, ZDD_BOOLEAN_CHECK, "ZDD boolean_check"},
    {1, ZDD_EVAL_ASSIGNMENT, "ZDD eval_assignment"},

    {2, BDD_SATCOUNT_U64, "BDD satcount u64"},
    {2, BDD_SATCOUNT_GMP, "BDD satcount gmp"},
    {2, MTBDD_SATCOUNT_U64, "MTBDD satcount u64"},
    {2, MTBDD_SATCOUNT_GMP, "MTBDD satcount gmp"},
    {2, LDD_SATCOUNT_U64, "LDD satcount u64"},
    {2, LDD_SATCOUNT_GMP, "LDD satcount gmp"},
    {2, ZDD_SATCOUNT_U64, "ZDD satcount u64"},
    {2, ZDD_SATCOUNT_GMP, "ZDD satcount gmp"},
    {1, BDD_EVAL, "BDD eval"},
    {1, MTBDD_EVAL, "MTBDD eval"},
    {1, BDD_COFACTOR, "BDD cofactor"},

    {2, BDD_REACH, "BDD reach"},
    {2, LDD_REACH, "LDD reach"},
    {2, LDD_EXTEND_REL, "LDD extend_rel"},

    {1, SYLVAN_ITERATOR_CREATED, "Sylvan iterators created"},
    {1, SYLVAN_ITERATOR_ITEMS, "Sylvan iterator items"},

    {1, MTBDD_LEVEL_STATISTICS, "MTBDD level statistics"},
    {1, ZDD_LEVEL_STATISTICS, "ZDD level statistics"},

    {0, 0, "Garbage collection"},
    {1, SYLVAN_GC_COUNT, "GC executions"},
    {3, SYLVAN_GC, "Total time spent"},

    {-1, -1, NULL},
};

#if SYLVAN_STATS

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
        sylvan_stats = sylvan_alloc_aligned(sizeof(sylvan_stats_t));
        if (sylvan_stats == 0) {
            fprintf(stderr, "sylvan_stats: Unable to allocate memory: %s!\n", strerror(errno));
            exit(1);
        }
    }
    pthread_setspecific(sylvan_stats_key, sylvan_stats);
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

VOID_TASK_1(sylvan_stats_sum, sylvan_stats_t*, target)
{
#ifdef __ELF__
    for (int i=0; i<SYLVAN_COUNTER_COUNTER; i++) {
        atomic_fetch_add((_Atomic(uint64_t)*)target->counters + i, sylvan_stats.counters[i]);
    }
    for (int i=0; i<SYLVAN_TIMER_COUNTER; i++) {
        atomic_fetch_add((_Atomic(uint64_t)*)target->timers + i, sylvan_stats.timers[i]);
    }
#else
    sylvan_stats_t *sylvan_stats = pthread_getspecific(sylvan_stats_key);
    if (sylvan_stats != NULL) {
        for (int i=0; i<SYLVAN_COUNTER_COUNTER; i++) {
            atomic_fetch_add((_Atomic(uint64_t)*)target->counters + i, sylvan_stats->counters[i]);
        }
        for (int i=0; i<SYLVAN_TIMER_COUNTER; i++) {
            atomic_fetch_add((_Atomic(uint64_t)*)target->timers + i, sylvan_stats->timers[i]);
        }
    }
#endif
}

VOID_TASK_IMPL_1(sylvan_stats_snapshot, sylvan_stats_t*, target)
{
    memset(target, 0, sizeof(sylvan_stats_t));
    TOGETHER(sylvan_stats_sum, target);
}

#define BLACK "\33[22;30m"
#define GRAY "\33[1;30m"
#define RED "\33[22;31m"
#define LRED "\33[1;31m"
#define GREEN "\33[22;32m"
#define LGREEN "\33[1;32m"
#define BROWN "\33[22;33m"
#define YELLOW "\33[1;33m"
#define BLUE "\33[22;34m"
#define LBLUE "\33[1;34m"
#define MAGENTA "\33[22;35m"
#define LMAGENTA "\33[1;35m"
#define CYAN "\33[22;36m"
#define LCYAN "\33[1;36m"
#define LGRAY "\33[22;37m"
#define WHITE "\33[1;37m"
#define NC "\33[m"
#define BOLD "\33[1m"
#define ULINE "\33[4m"
#define PINK "\33[38;5;200m"

static char*
to_h(double size, char *buf)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    int i = 0;
    for (;size>1024;size/=1024) i++;
    sprintf(buf, "%.*f %s", i, size, units[i]);
    return buf;
}

void
sylvan_stats_report(FILE *target)
{
    sylvan_stats_t totals;
    sylvan_stats_snapshot(&totals);

    // fix timers for MACH
#ifdef __MACH__
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    uint64_t c = timebase.numer/timebase.denom;
    for (int i=0;i<SYLVAN_TIMER_COUNTER;i++) totals.timers[i]*=c;
#endif

    int color = isatty(fileno(target)) ? 1 : 0;
    if (color) fprintf(target, ULINE WHITE "Sylvan statistics\n" NC);
    else fprintf(target, "Sylvan statistics\n");

    int i=0;
    for (;;) {
        if (sylvan_report_info[i].id == -1) break;
        int id = sylvan_report_info[i].id;
        int type = sylvan_report_info[i].type;
        if (type == 0) {
            if (color) fprintf(target, WHITE "\n%s\n" NC, sylvan_report_info[i].key);
            else fprintf(target, "\n%s\n", sylvan_report_info[i].key);
        } else if (type == 1) {
            if (totals.counters[id] > 0) {
                fprintf(target, "%-20s %'-16"PRIu64"\n", sylvan_report_info[i].key, totals.counters[id]);
            }
        } else if (type == 2) {
            if (totals.counters[id] > 0) {
                fprintf(target, "%-20s %'-16"PRIu64 " %'-16"PRIu64" %'-16"PRIu64 "\n", sylvan_report_info[i].key, totals.counters[id], totals.counters[id+1], totals.counters[id+2]);
            }
        } else if (type == 3) {
            if (totals.timers[id] > 0) {
                fprintf(target, "%-20s %'.6Lf sec.\n", sylvan_report_info[i].key, (long double)totals.timers[id]/1000000000);
            }
        } else if (type == 4) {
            fprintf(target, "%-20s %'zu of %'zu buckets filled.\n", "Unique nodes table", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            fprintf(target, "%-20s %'zu of %'zu buckets filled.\n", "Operation cache", cache_getused(), cache_getsize());
            char buf[64], buf2[64];
            to_h(24ULL * llmsset_get_size(nodes), buf);
            to_h(24ULL * llmsset_get_max_size(nodes), buf2);
            fprintf(target, "%-20s %s (max real) of %s (allocated virtual memory).\n", "Memory (nodes)", buf, buf2);
            to_h(36ULL * cache_getsize(), buf);
            to_h(36ULL * cache_getmaxsize(), buf2);
            fprintf(target, "%-20s %s (max real) of %s (allocated virtual memory).\n", "Memory (cache)", buf, buf2);
        }
        i++;
    }
}

#else

VOID_TASK_IMPL_0(sylvan_stats_init)
{
}

VOID_TASK_IMPL_0(sylvan_stats_reset)
{
}

VOID_TASK_IMPL_1(sylvan_stats_snapshot, sylvan_stats_t*, target)
{
    memset(target, 0, sizeof(sylvan_stats_t));
}

void
sylvan_stats_report(FILE* target)
{
    (void)target;
}

#endif

/* Legacy snapshots retain their historical timer representation on Mach.
 * Convert only the new API's private totals, without changing that behavior. */
static uint64_t
sylvan_statistics_time_ns(uint64_t ticks)
{
#ifdef __MACH__
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return (ticks / timebase.denom) * timebase.numer +
        ((ticks % timebase.denom) * timebase.numer) / timebase.denom;
#else
    return ticks;
#endif
}

/* cache_getused reads the legacy cache status words non-atomically. Hold
 * every worker in this task until sampling ends: a NEWFRAME steal loop, or
 * an early return from TOGETHER, could accept external RUN submissions. */
VOID_TASK_1(sylvan_statistics_tables, sylvan_statistics*, target)
{
    if (lace_get_worker()->worker == 0) {
        target->nodes_used = (uint64_t)llmsset_count_marked_no_yield(nodes);
        target->nodes_capacity = (uint64_t)llmsset_get_size(nodes);
        target->nodes_max_capacity = (uint64_t)llmsset_get_max_size(nodes);
        target->cache_used = (uint64_t)cache_getused();
        target->cache_capacity = (uint64_t)cache_getsize();
        target->cache_max_capacity = (uint64_t)cache_getmaxsize();
    }
    lace_barrier();
}

TASK_IMPL_1(int, sylvan_statistics_snapshot, sylvan_statistics*, target)
{
    if (target == NULL || target->version != SYLVAN_STATISTICS_VERSION ||
        target->size < SYLVAN_STATISTICS_V1_SIZE) return 0;

    sylvan_stats_t totals;
    CALL(sylvan_stats_snapshot, &totals);
    sylvan_statistics result = SYLVAN_STATISTICS_INIT;
    result.size = target->size;
    TOGETHER(sylvan_statistics_tables, &result);
    result.gc_count = totals.counters[SYLVAN_GC_COUNT];
    result.gc_time_ns = sylvan_statistics_time_ns(totals.timers[SYLVAN_GC]);
    result.worker_count = lace_workers();
    result.statistics_enabled = SYLVAN_STATS ? 1u : 0u;
    size_t writable = target->size < sizeof(result) ? target->size : sizeof(result);
    memcpy(target, &result, writable);
    return 1;
}

static void
sylvan_statistics_entry(sylvan_statistic *entry, size_t index, const sylvan_stats_t *totals)
{
    const int type = sylvan_report_info[index].type;
    const int id = sylvan_report_info[index].id;
    entry->name = sylvan_report_info[index].key;
    entry->reserved = 0;
    entry->value = type == 3 ? sylvan_statistics_time_ns(totals->timers[id]) : totals->counters[id];
    entry->cache_hits = type == 2 ? totals->counters[id+2] : 0;
    entry->cache_puts = type == 2 ? totals->counters[id+1] : 0;
    entry->kind = type == 1 ? SYLVAN_STATISTIC_COUNTER :
        type == 2 ? SYLVAN_STATISTIC_OPERATION : SYLVAN_STATISTIC_TIMER;
}

TASK_IMPL_3(int, sylvan_statistics_read, sylvan_statistic*, entries, size_t, capacity, size_t*, count)
{
    if (count == NULL || (entries == NULL && capacity != 0)) return 0;
    size_t required = 0;
    for (size_t i=0; sylvan_report_info[i].id != -1; i++) {
        const int type = sylvan_report_info[i].type;
        if (type >= 1 && type <= 3) required++;
    }
    *count = required;
    if (entries == NULL) return 1;
    if (capacity < required) return 0;

    sylvan_stats_t totals;
    CALL(sylvan_stats_snapshot, &totals);
    size_t output = 0;
    for (size_t i=0; sylvan_report_info[i].id != -1; i++) {
        const int type = sylvan_report_info[i].type;
        if (type >= 1 && type <= 3) sylvan_statistics_entry(entries + output++, i, &totals);
    }
    return 1;
}

TASK_IMPL_2(int, sylvan_statistics_read_counter, size_t, id, uint64_t*, value)
{
    if (id >= SYLVAN_COUNTER_COUNTER || value == NULL) return 0;
    sylvan_stats_t totals;
    CALL(sylvan_stats_snapshot, &totals);
    *value = totals.counters[id];
    return 1;
}

TASK_IMPL_2(int, sylvan_statistics_read_name, const char*, name, sylvan_statistic*, entry)
{
    if (name == NULL || entry == NULL) return 0;
    for (size_t i=0; sylvan_report_info[i].id != -1; i++) {
        const int type = sylvan_report_info[i].type;
        if (type >= 1 && type <= 3 && strcmp(name, sylvan_report_info[i].key) == 0) {
            sylvan_stats_t totals;
            CALL(sylvan_stats_snapshot, &totals);
            sylvan_statistics_entry(entry, i, &totals);
            return 1;
        }
    }
    return 0;
}

struct sylvan_index_set {
    uint64_t *slots;
    size_t capacity;
    size_t count;
};

struct sylvan_level_table {
    uint32_t *levels;
    uint64_t *counts;
    size_t capacity;
    size_t count;
};

struct sylvan_handle_stack {
    uint64_t *handles;
    size_t capacity;
    size_t count;
};

static uint64_t
sylvan_statistics_hash(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static int
sylvan_index_set_grow(struct sylvan_index_set *set)
{
    const size_t old_capacity = set->capacity;
    const size_t new_capacity = old_capacity == 0 ? 16 : old_capacity * 2;
    if (new_capacity < old_capacity ||
        new_capacity > SIZE_MAX / sizeof(*set->slots)) {
        return 0;
    }
    uint64_t *slots = calloc(new_capacity, sizeof(*slots));
    if (slots == NULL) return 0;
    for (size_t i = 0; i < old_capacity; i++) {
        const uint64_t stored = set->slots[i];
        if (stored == 0) continue;
        size_t position =
            (size_t)sylvan_statistics_hash(stored - 1) &
            (new_capacity - 1);
        while (slots[position] != 0) {
            position = (position + 1) & (new_capacity - 1);
        }
        slots[position] = stored;
    }
    free(set->slots);
    set->slots = slots;
    set->capacity = new_capacity;
    return 1;
}

static int
sylvan_index_set_insert(
    struct sylvan_index_set *set, uint64_t index, int *inserted)
{
    if (index == UINT64_MAX) return 0;
    if (set->capacity == 0 ||
        set->count + 1 > set->capacity / 2) {
        int status = sylvan_index_set_grow(set);
        if (!status) return status;
    }
    const uint64_t stored = index + 1;
    size_t position =
        (size_t)sylvan_statistics_hash(index) & (set->capacity - 1);
    while (set->slots[position] != 0) {
        if (set->slots[position] == stored) {
            *inserted = 0;
            return 1;
        }
        position = (position + 1) & (set->capacity - 1);
    }
    set->slots[position] = stored;
    set->count++;
    *inserted = 1;
    return 1;
}

static int
sylvan_level_table_grow(struct sylvan_level_table *table)
{
    const size_t old_capacity = table->capacity;
    const size_t new_capacity = old_capacity == 0 ? 16 : old_capacity * 2;
    if (new_capacity < old_capacity ||
        new_capacity > SIZE_MAX / sizeof(*table->levels) ||
        new_capacity > SIZE_MAX / sizeof(*table->counts)) {
        return 0;
    }
    uint32_t *levels = calloc(new_capacity, sizeof(*levels));
    uint64_t *counts = calloc(new_capacity, sizeof(*counts));
    if (levels == NULL || counts == NULL) {
        free(counts);
        free(levels);
        return 0;
    }
    for (size_t i = 0; i < old_capacity; i++) {
        const uint32_t stored = table->levels[i];
        if (stored == 0) continue;
        const uint32_t level = stored - 1;
        size_t position =
            (size_t)sylvan_statistics_hash(level) & (new_capacity - 1);
        while (levels[position] != 0) {
            position = (position + 1) & (new_capacity - 1);
        }
        levels[position] = stored;
        counts[position] = table->counts[i];
    }
    free(table->levels);
    free(table->counts);
    table->levels = levels;
    table->counts = counts;
    table->capacity = new_capacity;
    return 1;
}

static int
sylvan_level_table_increment(
    struct sylvan_level_table *table, uint32_t level)
{
    if (level == UINT32_MAX) return 0;
    if (table->capacity == 0 ||
        table->count + 1 > table->capacity / 2) {
        int status = sylvan_level_table_grow(table);
        if (!status) return status;
    }
    const uint32_t stored = level + 1;
    size_t position =
        (size_t)sylvan_statistics_hash(level) & (table->capacity - 1);
    while (table->levels[position] != 0) {
        if (table->levels[position] == stored) {
            if (table->counts[position] == UINT64_MAX) {
                return 0;
            }
            table->counts[position]++;
            return 1;
        }
        position = (position + 1) & (table->capacity - 1);
    }
    table->levels[position] = stored;
    table->counts[position] = 1;
    table->count++;
    return 1;
}

static int
sylvan_handle_stack_push(
    struct sylvan_handle_stack *stack, uint64_t handle)
{
    if (stack->count == stack->capacity) {
        const size_t old_capacity = stack->capacity;
        const size_t new_capacity =
            old_capacity == 0 ? 64 : old_capacity * 2;
        if (new_capacity < old_capacity ||
            new_capacity > SIZE_MAX / sizeof(*stack->handles)) {
            return 0;
        }
        uint64_t *handles = realloc(
            stack->handles, new_capacity * sizeof(*handles));
        if (handles == NULL) return 0;
        stack->handles = handles;
        stack->capacity = new_capacity;
    }
    stack->handles[stack->count++] = handle;
    return 1;
}

static int
sylvan_level_statistic_compare(const void *left, const void *right)
{
    const sylvan_level_statistic *a = left;
    const sylvan_level_statistic *b = right;
    return a->level < b->level ? -1 : a->level != b->level;
}

/* Check the node arena rather than the hash size, which can shrink below
 * live node indices. Family and liveness remain the caller's preconditions. */
static int
sylvan_statistics_valid_handle(uint64_t handle, int zdd)
{
    if (zdd && (handle == zdd_false || handle == zdd_true)) return 1;
    const uint64_t index = MTBDD_GETINDEX(handle);
    return (handle & ~(mtbdd_complement | UINT64_C(0x000000ffffffffff))) == 0 &&
        (index == 0 || (index >= 2 && index < llmsset_get_data_size(nodes)));
}

struct sylvan_level_output {
    sylvan_level_statistic *entries;
    size_t capacity;
    size_t *count;
    uint64_t *leaf_count;
};

TASK_4(int, sylvan_level_statistics_collect, const uint64_t*, roots, size_t, root_count,
       struct sylvan_level_output*, output, int, zdd)
{
    sylvan_level_statistic *entries = output->entries;
    const size_t capacity = output->capacity;
    size_t *count = output->count;
    uint64_t *leaf_count = output->leaf_count;
    if (count == NULL || leaf_count == NULL ||
        (root_count != 0 && roots == NULL) || (entries == NULL && capacity != 0)) return 0;
    for (size_t i=0; i<root_count; i++) {
        if (!sylvan_statistics_valid_handle(roots[i], zdd)) return 0;
    }
    if (!(zdd ? zdd_refs_reserve(root_count) : mtbdd_refs_reserve(root_count))) return 0;
    for (size_t i=0; i<root_count; i++) {
        if (zdd) zdd_refs_push(roots[i]); else mtbdd_refs_push(roots[i]);
    }
    sylvan_stats_count(zdd ? ZDD_LEVEL_STATISTICS : MTBDD_LEVEL_STATISTICS);
    sylvan_gc_test();

    int success = 1;
    struct sylvan_index_set visited = {0};
    struct sylvan_level_table levels = {0};
    struct sylvan_handle_stack stack = {0};
    uint64_t leaves = 0;
    for (size_t i=0; i<root_count && success; i++) {
        success = sylvan_handle_stack_push(&stack, roots[i]);
    }

    size_t yielded_at = 0;
    while (stack.count != 0 && success) {
        /* All descendants remain alive through the protected roots; the
         * private visited set and stack contain indices, never node pointers. */
        if (++yielded_at == 1024) {
            sylvan_gc_test();
            yielded_at = 0;
        }
        const uint64_t handle = stack.handles[--stack.count];
        if (!sylvan_statistics_valid_handle(handle, zdd)) { success = 0; break; }
        const uint64_t index = MTBDD_GETINDEX(handle);
        int inserted;
        success = sylvan_index_set_insert(&visited, index, &inserted);
        if (!success || !inserted) continue;
        if ((zdd && zdd_isleaf(handle)) || (!zdd && mtbdd_isleaf(handle))) {
            if (leaves == UINT64_MAX) success = 0; else leaves++;
            continue;
        }
        if (mtbddnode_ismapnode(MTBDD_GETNODE(handle)) ||
            zddnode_ismapnode(ZDD_GETNODE(handle))) { success = 0; break; }

        uint32_t level;
        uint64_t low, high;
        if (zdd) {
            zddnode_t node = ZDD_GETNODE(handle);
            level = zddnode_getvariable(node);
            low = zddnode_getlow(node);
            high = zddnode_gethigh(node);
        } else {
            mtbddnode_t node = MTBDD_GETNODE(handle);
            level = mtbddnode_getvariable(node);
            low = mtbddnode_getlow(node);
            high = mtbddnode_gethigh(node);
        }
        success = sylvan_level_table_increment(&levels, level);
        if (success) success = sylvan_handle_stack_push(&stack, low);
        if (success) success = sylvan_handle_stack_push(&stack, high);
    }

    if (success) {
        *count = levels.count;
        *leaf_count = leaves;
        if (entries != NULL && capacity < levels.count) {
            success = 0;
        } else if (entries != NULL) {
            size_t output = 0;
            for (size_t i=0; i<levels.capacity; i++) {
                if (levels.levels[i] == 0) continue;
                entries[output].level = levels.levels[i] - 1;
                entries[output].reserved = 0;
                entries[output].node_count = levels.counts[i];
                output++;
            }
            qsort(entries, levels.count, sizeof(*entries), sylvan_level_statistic_compare);
        }
    }
    free(stack.handles);
    free(levels.counts);
    free(levels.levels);
    free(visited.slots);
    if (zdd) zdd_refs_pop(root_count); else mtbdd_refs_pop(root_count);
    return success;
}

TASK_IMPL_6(int, mtbdd_level_statistics, const MTBDD*, roots, size_t, root_count,
            sylvan_level_statistic*, entries, size_t, capacity, size_t*, count, uint64_t*, leaf_count)
{
    struct sylvan_level_output output = {entries, capacity, count, leaf_count};
    return CALL(sylvan_level_statistics_collect, roots, root_count, &output, 0);
}

TASK_IMPL_6(int, zdd_level_statistics, const ZDD*, roots, size_t, root_count,
            sylvan_level_statistic*, entries, size_t, capacity, size_t*, count, uint64_t*, leaf_count)
{
    struct sylvan_level_output output = {entries, capacity, count, leaf_count};
    return CALL(sylvan_level_statistics_collect, roots, root_count, &output, 1);
}
