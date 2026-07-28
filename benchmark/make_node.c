/*
 * Copyright 2026 Tom van Dijk, Formal Methods and Tools,
 * University of Twente
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

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <lace.h>
#include <sylvan/internal.h>

typedef enum benchmark_method {
    METHOD_INLINE,
    METHOD_TRY,
    METHOD_TASK_CALL,
    METHOD_TASK_ENTRY,
    METHOD_BATCH_ENTRY
} benchmark_method;

typedef enum benchmark_scenario {
    SCENARIO_INSERT,
    SCENARIO_REUSE,
    SCENARIO_REDUCTION,
    SCENARIO_COMPLEMENT
} benchmark_scenario;

typedef enum benchmark_context {
    CONTEXT_WORKER,
    CONTEXT_EXTERNAL
} benchmark_context;

typedef struct benchmark_config {
    benchmark_method method;
    benchmark_scenario scenario;
    benchmark_context context;
    unsigned int workers;
    unsigned int table_bits;
    size_t operations;
    size_t batch_size;
    unsigned int occupancy_percent;
    uint32_t first_level;
} benchmark_config;

static const char *method_names[] = {
    "inline",
    "try",
    "task_call",
    "task_entry",
    "batch_entry"
};

static const char *scenario_names[] = {
    "insert",
    "reuse",
    "reduction",
    "complement"
};

static const char *context_names[] = {
    "worker",
    "external"
};

static double
benchmark_now(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    static int initialized;
    LARGE_INTEGER counter;
    if (!initialized) {
        QueryPerformanceFrequency(&frequency);
        initialized = 1;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
}

static int
benchmark_parse_size(const char *text, size_t *result)
{
    char *end = NULL;
    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > SIZE_MAX) {
        return 0;
    }
    *result = (size_t)value;
    return 1;
}

static int
benchmark_arguments(
    const benchmark_config *config, size_t index,
    uint32_t *level, MTBDD *low, MTBDD *high)
{
    switch (config->scenario) {
    case SCENARIO_INSERT:
        *level = config->first_level + (uint32_t)index;
        *low = bdd_false;
        *high = bdd_true;
        return SYLVAN_OK;
    case SCENARIO_REUSE:
        *level = config->first_level;
        *low = bdd_false;
        *high = bdd_true;
        return SYLVAN_OK;
    case SCENARIO_REDUCTION:
        *level = config->first_level;
        *low = bdd_true;
        *high = bdd_true;
        return SYLVAN_OK;
    case SCENARIO_COMPLEMENT:
        *level = config->first_level + (uint32_t)index;
        *low = bdd_true;
        *high = bdd_false;
        return SYLVAN_OK;
    }
    return SYLVAN_ERR_INVALID;
}

TASK(int, benchmark_safe_make_node,
     MTBDD*, destination, uint32_t, level, MTBDD, low, MTBDD, high)
int
benchmark_safe_make_node_CALL(
    lace_worker *lace, MTBDD *destination,
    uint32_t level, MTBDD low, MTBDD high)
{
    (void)lace;
    return _mtbdd_try_make_node(destination, level, low, high);
}

static int
benchmark_make_one(
    lace_worker *lace, const benchmark_config *config,
    MTBDD *destination, size_t index)
{
    uint32_t level;
    MTBDD low;
    MTBDD high;
    int status = benchmark_arguments(config, index, &level, &low, &high);
    if (status != SYLVAN_OK) return status;

    switch (config->method) {
    case METHOD_INLINE:
        *destination = mtbdd_make_node(level, low, high);
        return SYLVAN_OK;
    case METHOD_TRY:
        return _mtbdd_try_make_node(destination, level, low, high);
    case METHOD_TASK_CALL:
        return benchmark_safe_make_node_CALL(
            lace, destination, level, low, high);
    case METHOD_TASK_ENTRY:
        return benchmark_safe_make_node(destination, level, low, high);
    case METHOD_BATCH_ENTRY:
        break;
    }
    return SYLVAN_ERR_INVALID;
}

TASK(int, benchmark_safe_make_batch,
     const benchmark_config*, config, MTBDD*, destinations,
     size_t, first, size_t, count)
int
benchmark_safe_make_batch_CALL(
    lace_worker *lace, const benchmark_config *config,
    MTBDD *destinations, size_t first, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t level;
        MTBDD low;
        MTBDD high;
        int status = benchmark_arguments(
            config, first + i, &level, &low, &high);
        if (status != SYLVAN_OK) return status;
        status = _mtbdd_try_make_node(
            &destinations[first + i], level, low, high);
        if (status != SYLVAN_OK) return status;
    }
    (void)lace;
    return SYLVAN_OK;
}

TASK(int, benchmark_make_range,
     const benchmark_config*, config, MTBDD*, destinations,
     size_t, first, size_t, count)
int
benchmark_make_range_CALL(
    lace_worker *lace, const benchmark_config *config,
    MTBDD *destinations, size_t first, size_t count)
{
    if (count > 4096 && config->workers > 1) {
        const size_t left_count = count / 2;
        benchmark_make_range_SPAWN(
            lace, config, destinations, first, left_count);
        const int right_status = benchmark_make_range_CALL(
            lace, config, destinations, first + left_count,
            count - left_count);
        const int left_status = benchmark_make_range_SYNC(lace);
        return right_status != SYLVAN_OK ? right_status : left_status;
    }

    if (config->method == METHOD_BATCH_ENTRY) {
        size_t done = 0;
        while (done < count) {
            size_t batch = count - done;
            if (batch > config->batch_size) batch = config->batch_size;
            const int status = benchmark_safe_make_batch(
                config, destinations, first + done, batch);
            if (status != SYLVAN_OK) return status;
            done += batch;
        }
        return SYLVAN_OK;
    }

    for (size_t i = 0; i < count; i++) {
        const int status = benchmark_make_one(
            lace, config, &destinations[first + i], first + i);
        if (status != SYLVAN_OK) return status;
    }
    return SYLVAN_OK;
}

TASK(int, benchmark_prefill,
     const benchmark_config*, config, size_t, target)
int
benchmark_prefill_CALL(
    lace_worker *lace, const benchmark_config *config, size_t target)
{
    (void)lace;
    const size_t filled = nodes_count_nodes(nodes);
    uint32_t level = 0;
    for (size_t i = filled; i < target; i++) {
        MTBDD result;
        const int status = _mtbdd_try_make_node(
            &result, level++, bdd_false, bdd_true);
        if (status != SYLVAN_OK) return status;
    }
    if (config->scenario == SCENARIO_REUSE) {
        MTBDD result;
        return _mtbdd_try_make_node(
            &result, config->first_level, bdd_false, bdd_true);
    }
    return SYLVAN_OK;
}

static int
benchmark_run_external(
    const benchmark_config *config, MTBDD *destinations)
{
    if (config->method == METHOD_TASK_ENTRY) {
        for (size_t i = 0; i < config->operations; i++) {
            uint32_t level;
            MTBDD low;
            MTBDD high;
            int status = benchmark_arguments(
                config, i, &level, &low, &high);
            if (status != SYLVAN_OK) return status;
            status = benchmark_safe_make_node(
                &destinations[i], level, low, high);
            if (status != SYLVAN_OK) return status;
        }
        return SYLVAN_OK;
    }

    if (config->method == METHOD_BATCH_ENTRY) {
        size_t done = 0;
        while (done < config->operations) {
            size_t batch = config->operations - done;
            if (batch > config->batch_size) batch = config->batch_size;
            const int status = benchmark_safe_make_batch(
                config, destinations, done, batch);
            if (status != SYLVAN_OK) return status;
            done += batch;
        }
        return SYLVAN_OK;
    }

    return SYLVAN_ERR_INVALID;
}

static void
benchmark_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s --method inline|try|task-call|task-entry|batch-entry "
            "--scenario insert|reuse|reduction|complement "
            "[--context worker|external] [--workers N] [--operations N] "
            "[--batch-size N] [--table-bits N] [--occupancy N]\n",
            program);
}

static int
benchmark_parse_method(const char *text, benchmark_method *method)
{
    for (size_t i = 0; i < sizeof(method_names) / sizeof(method_names[0]); i++) {
        char normalized[32];
        size_t length = strlen(method_names[i]);
        if (length >= sizeof(normalized)) return 0;
        memcpy(normalized, method_names[i], length + 1);
        for (size_t j = 0; j < length; j++) {
            if (normalized[j] == '_') normalized[j] = '-';
        }
        if (strcmp(text, normalized) == 0) {
            *method = (benchmark_method)i;
            return 1;
        }
    }
    return 0;
}

static int
benchmark_parse_scenario(
    const char *text, benchmark_scenario *scenario)
{
    for (size_t i = 0;
         i < sizeof(scenario_names) / sizeof(scenario_names[0]); i++) {
        if (strcmp(text, scenario_names[i]) == 0) {
            *scenario = (benchmark_scenario)i;
            return 1;
        }
    }
    return 0;
}

static int
benchmark_parse_context(const char *text, benchmark_context *context)
{
    for (size_t i = 0;
         i < sizeof(context_names) / sizeof(context_names[0]); i++) {
        if (strcmp(text, context_names[i]) == 0) {
            *context = (benchmark_context)i;
            return 1;
        }
    }
    return 0;
}

int
main(int argc, char **argv)
{
    benchmark_config config = {
        METHOD_TRY, SCENARIO_REUSE, CONTEXT_WORKER,
        1, 22, 1000000, 256, 0, 0
    };
    int have_method = 0;
    int have_scenario = 0;

    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) {
            benchmark_usage(argv[0]);
            return 2;
        }
        const char *option = argv[i++];
        const char *value = argv[i];
        size_t parsed;
        if (strcmp(option, "--method") == 0) {
            have_method = benchmark_parse_method(value, &config.method);
            if (!have_method) goto invalid;
        } else if (strcmp(option, "--scenario") == 0) {
            have_scenario =
                benchmark_parse_scenario(value, &config.scenario);
            if (!have_scenario) goto invalid;
        } else if (strcmp(option, "--context") == 0) {
            if (!benchmark_parse_context(value, &config.context)) goto invalid;
        } else if (!benchmark_parse_size(value, &parsed)) {
            goto invalid;
        } else if (strcmp(option, "--workers") == 0 &&
                   parsed > 0 && parsed <= UINT_MAX) {
            config.workers = (unsigned int)parsed;
        } else if (strcmp(option, "--operations") == 0 && parsed > 0) {
            config.operations = parsed;
        } else if (strcmp(option, "--batch-size") == 0 && parsed > 0) {
            config.batch_size = parsed;
        } else if (strcmp(option, "--table-bits") == 0 &&
                   parsed >= 10 && parsed < sizeof(size_t) * CHAR_BIT) {
            config.table_bits = (unsigned int)parsed;
        } else if (strcmp(option, "--occupancy") == 0 && parsed <= 80) {
            config.occupancy_percent = (unsigned int)parsed;
        } else {
            goto invalid;
        }
    }

    if (!have_method || !have_scenario) goto invalid;
    if (config.context == CONTEXT_EXTERNAL &&
        config.method != METHOD_TASK_ENTRY &&
        config.method != METHOD_BATCH_ENTRY) {
        fprintf(stderr,
                "external context requires task-entry or batch-entry\n");
        return 2;
    }
    if ((config.scenario == SCENARIO_INSERT ||
         config.scenario == SCENARIO_COMPLEMENT) &&
        config.operations > UINT32_C(0x00ffffff)) {
        fprintf(stderr, "too many operations for 24-bit levels\n");
        return 2;
    }
    if (config.operations > SIZE_MAX / sizeof(MTBDD)) {
        fprintf(stderr, "result array is too large\n");
        return 2;
    }

    MTBDD *destinations =
        malloc(config.operations * sizeof(*destinations));
    if (destinations == NULL) {
        fprintf(stderr, "could not allocate result array\n");
        return 1;
    }

    const size_t table_size = (size_t)1 << config.table_bits;
    lace_start(config.workers, 0, 0);
    sylvan_set_sizes(
        table_size, table_size, (size_t)1 << 16, (size_t)1 << 16);
    sylvan_init_package();
    mtbdd_init();

    size_t filled_before;
    size_t total;
    sylvan_table_usage(&filled_before, &total);
    const size_t target =
        total / 100 * config.occupancy_percent +
        total % 100 * config.occupancy_percent / 100;
    config.first_level = (uint32_t)(target + 1);

    int status = benchmark_prefill(&config, target);
    sylvan_table_usage(&filled_before, &total);
    if (status != SYLVAN_OK) {
        fprintf(stderr, "prefill failed with status %d\n", status);
        goto done;
    }
    if ((config.scenario == SCENARIO_INSERT ||
         config.scenario == SCENARIO_COMPLEMENT) &&
        (uint64_t)config.first_level + config.operations >
            UINT32_C(0x00ffffff)) {
        fprintf(stderr, "occupancy and operation count exceed 24-bit levels\n");
        status = SYLVAN_ERR_INVALID;
        goto done;
    }

    const double start = benchmark_now();
    if (config.context == CONTEXT_WORKER) {
        status = benchmark_make_range(
            &config, destinations, 0, config.operations);
    } else {
        status = benchmark_run_external(&config, destinations);
    }
    const double elapsed = benchmark_now() - start;

    size_t filled_after;
    sylvan_table_usage(&filled_after, &total);
    uint64_t checksum = 0;
    for (size_t i = 0; i < config.operations; i++) {
        checksum ^= destinations[i] + UINT64_C(0x9e3779b97f4a7c15) * i;
    }

    if (status == SYLVAN_OK) {
        printf("method,scenario,context,workers,operations,batch_size,"
               "table_size,requested_occupancy,filled_before,filled_after,"
               "elapsed_ms,ns_per_operation,checksum\n");
        printf("%s,%s,%s,%u,%zu,%zu,%zu,%u,%zu,%zu,%.6f,%.3f,"
               "0x%016" PRIx64 "\n",
               method_names[config.method],
               scenario_names[config.scenario],
               context_names[config.context],
               config.workers, config.operations, config.batch_size,
               total, config.occupancy_percent,
               filled_before, filled_after, 1000.0 * elapsed,
               1000000000.0 * elapsed / (double)config.operations,
               checksum);
    }

done:
    sylvan_quit();
    lace_stop();
    free(destinations);
    if (status != SYLVAN_OK) {
        fprintf(stderr, "benchmark failed with status %d\n", status);
        return 1;
    }
    return 0;

invalid:
    benchmark_usage(argv[0]);
    return 2;
}
