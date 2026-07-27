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

/*
 * Every timed sample starts with an empty operation cache. Expected results
 * remain protected, so the benchmark measures recursive operation and cache
 * behavior without mixing garbage collection into the timing.
 */

typedef struct benchmark_config {
    unsigned int workers;
    size_t levels;
    size_t width;
    size_t rounds;
} benchmark_config;

typedef enum benchmark_method {
    METHOD_AND_EXISTS,
    METHOD_GENERIC_AND_EXISTS,
    METHOD_SEPARATE_AND_EXISTS,
    METHOD_GENERIC_OR_FORALL,
    METHOD_SEPARATE_OR_FORALL,
    METHOD_GENERIC_XOR_UNIQUE,
    METHOD_SEPARATE_XOR_UNIQUE,
    METHOD_COUNT
} benchmark_method;

static const char *method_names[METHOD_COUNT] = {
    "and_exists",
    "generic_and_exists",
    "separate_and_exists",
    "generic_or_forall",
    "separate_or_forall",
    "generic_xor_unique",
    "separate_xor_unique"
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
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + 1e-9 * (double)time.tv_nsec;
#endif
}

static uint64_t
benchmark_random(uint64_t *state)
{
    uint64_t value = (*state += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static int
benchmark_double_compare(const void *left, const void *right)
{
    const double a = *(const double*)left;
    const double b = *(const double*)right;
    return (a > b) - (a < b);
}

static double
benchmark_median(double *values, size_t count)
{
    qsort(values, count, sizeof(*values), benchmark_double_compare);
    if ((count & 1) != 0) return values[count / 2];
    return 0.5 * (values[count / 2 - 1] + values[count / 2]);
}

static int
benchmark_parse_size(const char *text, size_t *result)
{
    char *end = NULL;
    errno = 0;
    const uintmax_t value = strtoumax(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        value == 0 || value > SIZE_MAX) {
        return 0;
    }
    *result = (size_t)value;
    return 1;
}

static int
benchmark_build_random(
    lace_worker *lace, BDD *destination, const uint32_t *levels,
    size_t level_count, size_t width, uint64_t seed)
{
    if (width > SIZE_MAX / sizeof(BDD)) return SYLVAN_ERR_OOM;
    BDD *current = malloc(width * sizeof(*current));
    BDD *next = malloc(width * sizeof(*next));
    if (current == NULL || next == NULL) {
        free(current);
        free(next);
        return SYLVAN_ERR_OOM;
    }

    for (size_t i = 0; i < width; i++) {
        current[i] = mtbdd_invalid;
        next[i] = (benchmark_random(&seed) & 1) ? bdd_true : bdd_false;
        mtbdd_refs_pushptr(&current[i]);
        mtbdd_refs_pushptr(&next[i]);
    }

    int status = SYLVAN_OK;
    for (size_t level_index = level_count; level_index > 0; level_index--) {
        const uint32_t level = levels[level_index - 1];
        for (size_t i = 0; i < width; i++) {
            BDD low = next[benchmark_random(&seed) % width];
            BDD high = next[benchmark_random(&seed) % width];
            if ((benchmark_random(&seed) & 3) == 0) low = bdd_not(low);
            if ((benchmark_random(&seed) & 3) == 0) high = bdd_not(high);
            status = _mtbdd_try_make_node(&current[i], level, low, high);
            if (status != SYLVAN_OK) goto done;
        }

        BDD *swap = current;
        current = next;
        next = swap;
    }

    *destination = next[benchmark_random(&seed) % width];

done:
    mtbdd_refs_popptr(2 * width);
    free(current);
    free(next);
    (void)lace;
    return status;
}

static int
benchmark_run_method(
    lace_worker *lace, BDD *destination, BDD a, BDD b, BDDSET variables,
    benchmark_method method)
{
    BDD intermediate = mtbdd_invalid;
    mtbdd_refs_pushptr(&intermediate);
    int status;

    switch (method) {
    case METHOD_AND_EXISTS:
        status = bdd_and_exists_CALL(lace, destination, a, b, variables);
        break;
    case METHOD_GENERIC_AND_EXISTS:
        status = bdd_apply_abstract_CALL(
            lace, destination, a, b, variables,
            BDD_APPLY_AND, BDD_ABSTRACT_EXISTS);
        break;
    case METHOD_SEPARATE_AND_EXISTS:
        status = bdd_and_CALL(lace, &intermediate, a, b);
        if (status == SYLVAN_OK) {
            status = bdd_exists_CALL(
                lace, destination, intermediate, variables);
        }
        break;
    case METHOD_GENERIC_OR_FORALL:
        status = bdd_apply_abstract_CALL(
            lace, destination, a, b, variables,
            BDD_APPLY_OR, BDD_ABSTRACT_FORALL);
        break;
    case METHOD_SEPARATE_OR_FORALL:
        status = bdd_or_CALL(lace, &intermediate, a, b);
        if (status == SYLVAN_OK) {
            status = bdd_forall_CALL(
                lace, destination, intermediate, variables);
        }
        break;
    case METHOD_GENERIC_XOR_UNIQUE:
        status = bdd_apply_abstract_CALL(
            lace, destination, a, b, variables,
            BDD_APPLY_XOR, BDD_ABSTRACT_UNIQUE);
        break;
    case METHOD_SEPARATE_XOR_UNIQUE:
        status = bdd_xor_CALL(lace, &intermediate, a, b);
        if (status == SYLVAN_OK) {
            status = bdd_unique_CALL(
                lace, destination, intermediate, variables);
        }
        break;
    default:
        status = SYLVAN_ERR_INVALID;
        break;
    }

    mtbdd_refs_popptr(1);
    return status;
}

static benchmark_method
benchmark_reference_method(benchmark_method method)
{
    switch (method) {
    case METHOD_AND_EXISTS:
    case METHOD_GENERIC_AND_EXISTS:
    case METHOD_SEPARATE_AND_EXISTS:
        return METHOD_AND_EXISTS;
    case METHOD_GENERIC_OR_FORALL:
    case METHOD_SEPARATE_OR_FORALL:
        return METHOD_SEPARATE_OR_FORALL;
    case METHOD_GENERIC_XOR_UNIQUE:
    case METHOD_SEPARATE_XOR_UNIQUE:
        return METHOD_SEPARATE_XOR_UNIQUE;
    default:
        return METHOD_COUNT;
    }
}

static int
benchmark_scenario(
    lace_worker *lace, const benchmark_config *config, const char *name,
    const uint32_t *a_levels, size_t a_level_count,
    const uint32_t *b_levels, size_t b_level_count,
    const uint32_t *abstract_levels, size_t abstract_count,
    const uint32_t *guard_levels, size_t guard_count, uint64_t seed)
{
    BDD a = mtbdd_invalid;
    BDD b = mtbdd_invalid;
    BDD guard = mtbdd_invalid;
    BDD guarded_b = mtbdd_invalid;
    BDD result = mtbdd_invalid;
    BDDSET variables = mtbdd_invalid;
    BDD references[3] = {
        mtbdd_invalid, mtbdd_invalid, mtbdd_invalid
    };
    mtbdd_refs_pushptr(&a);
    mtbdd_refs_pushptr(&b);
    mtbdd_refs_pushptr(&guard);
    mtbdd_refs_pushptr(&guarded_b);
    mtbdd_refs_pushptr(&result);
    mtbdd_refs_pushptr(&variables);
    for (size_t i = 0; i < 3; i++) mtbdd_refs_pushptr(&references[i]);

    int status = benchmark_build_random(
        lace, &a, a_levels, a_level_count, config->width, seed);
    if (status != SYLVAN_OK) goto done;
    status = benchmark_build_random(
        lace, &b, b_levels, b_level_count, config->width,
        seed ^ UINT64_C(0x9e3779b97f4a7c15));
    if (status != SYLVAN_OK) goto done;
    if (guard_count != 0) {
        size_t guard_width = config->width / 4;
        if (guard_width < 4) guard_width = 4;
        status = benchmark_build_random(
            lace, &guard, guard_levels, guard_count, guard_width,
            seed ^ UINT64_C(0x43f6a8885a308d31));
        if (status != SYLVAN_OK) goto done;
        status = bdd_and_CALL(lace, &guarded_b, b, guard);
        if (status != SYLVAN_OK) goto done;
        b = guarded_b;
    }
    status = bdd_set_from_array(
        &variables, abstract_levels, abstract_count);
    if (status != SYLVAN_OK) goto done;

    const benchmark_method reference_methods[3] = {
        METHOD_AND_EXISTS,
        METHOD_SEPARATE_OR_FORALL,
        METHOD_SEPARATE_XOR_UNIQUE
    };
    for (size_t i = 0; i < 3; i++) {
        status = benchmark_run_method(
            lace, &references[i], a, b, variables, reference_methods[i]);
        if (status != SYLVAN_OK) goto done;
    }
    const size_t a_nodes = mtbdd_node_count(a);
    const size_t b_nodes = mtbdd_node_count(b);
    const size_t reference_nodes[3] = {
        mtbdd_node_count(references[0]),
        mtbdd_node_count(references[1]),
        mtbdd_node_count(references[2])
    };

    if (config->rounds > SIZE_MAX / METHOD_COUNT / sizeof(double)) {
        status = SYLVAN_ERR_OOM;
        goto done;
    }
    double *samples = calloc(
        METHOD_COUNT * config->rounds, sizeof(*samples));
    if (samples == NULL) {
        status = SYLVAN_ERR_OOM;
        goto done;
    }

    for (size_t round = 0; round < config->rounds; round++) {
        for (size_t offset = 0; offset < METHOD_COUNT; offset++) {
            const benchmark_method method =
                (benchmark_method)((round + offset) % METHOD_COUNT);
            sylvan_clear_cache_CALL(lace);
            const double start = benchmark_now();
            status = benchmark_run_method(
                lace, &result, a, b, variables, method);
            const double elapsed = benchmark_now() - start;
            if (status != SYLVAN_OK) {
                free(samples);
                goto done;
            }

            const benchmark_method reference =
                benchmark_reference_method(method);
            const size_t reference_index =
                reference == METHOD_AND_EXISTS ? 0 :
                reference == METHOD_SEPARATE_OR_FORALL ? 1 : 2;
            if (result != references[reference_index]) {
                fprintf(stderr, "%s produced an incorrect result\n",
                        method_names[method]);
                free(samples);
                status = SYLVAN_ERR_INVALID;
                goto done;
            }
            samples[(size_t)method * config->rounds + round] =
                1000.0 * elapsed;
        }
    }

    double medians[METHOD_COUNT];
    for (size_t method = 0; method < METHOD_COUNT; method++) {
        medians[method] = benchmark_median(
            samples + method * config->rounds, config->rounds);
    }

    for (size_t method = 0; method < METHOD_COUNT; method++) {
        const double baseline =
            method <= METHOD_SEPARATE_AND_EXISTS
                ? medians[METHOD_AND_EXISTS]
                : method <= METHOD_SEPARATE_OR_FORALL
                    ? medians[METHOD_SEPARATE_OR_FORALL]
                    : medians[METHOD_SEPARATE_XOR_UNIQUE];
        const size_t reference_index =
            method <= METHOD_SEPARATE_AND_EXISTS ? 0 :
            method <= METHOD_SEPARATE_OR_FORALL ? 1 : 2;
        printf("%s,%u,%zu,%zu,%zu,%zu,%zu,%zu,%s,%.6f,%.3f\n",
               name, config->workers, config->levels, config->width,
               config->rounds, a_nodes, b_nodes,
               reference_nodes[reference_index], method_names[method],
               medians[method],
               medians[method] / baseline);
    }
    fflush(stdout);
    free(samples);

done:
    mtbdd_refs_popptr(9);
    return status;
}

TASK(int, benchmark_run, const benchmark_config*, config)
int
benchmark_run_CALL(lace_worker *lace, const benchmark_config *config)
{
    uint32_t *all_levels = malloc(config->levels * sizeof(*all_levels));
    uint32_t *even_levels = malloc(
        ((config->levels + 1) / 2) * sizeof(*even_levels));
    uint32_t *odd_levels = malloc(
        (config->levels / 2) * sizeof(*odd_levels));
    uint32_t *sparse_levels = malloc(
        ((config->levels + 3) / 4) * sizeof(*sparse_levels));
    uint32_t *non_sparse_levels = malloc(
        config->levels * sizeof(*non_sparse_levels));
    if (all_levels == NULL || even_levels == NULL || odd_levels == NULL ||
        sparse_levels == NULL || non_sparse_levels == NULL) {
        free(all_levels);
        free(even_levels);
        free(odd_levels);
        free(sparse_levels);
        free(non_sparse_levels);
        return SYLVAN_ERR_OOM;
    }

    size_t even_count = 0;
    size_t odd_count = 0;
    size_t sparse_count = 0;
    size_t non_sparse_count = 0;
    for (size_t i = 0; i < config->levels; i++) {
        all_levels[i] = (uint32_t)i;
        if ((i & 1) == 0) even_levels[even_count++] = (uint32_t)i;
        else odd_levels[odd_count++] = (uint32_t)i;
        if ((i & 3) == 0) {
            sparse_levels[sparse_count++] = (uint32_t)i;
        } else {
            non_sparse_levels[non_sparse_count++] = (uint32_t)i;
        }
    }

    printf("scenario,workers,levels,width,rounds,a_nodes,b_nodes,"
           "result_nodes,method,median_ms,ratio\n");
    int status = benchmark_scenario(
        lace, config, "shared-half", all_levels, config->levels,
        all_levels, config->levels, even_levels, even_count,
        NULL, 0, UINT64_C(0xa221f41d9326b84d));
    if (status == SYLVAN_OK) {
        status = benchmark_scenario(
            lace, config, "shared-sparse", all_levels, config->levels,
            all_levels, config->levels, sparse_levels, sparse_count,
            non_sparse_levels, non_sparse_count,
            UINT64_C(0x9b0e3a3f41c9d863));
    }
    if (status == SYLVAN_OK) {
        status = benchmark_scenario(
            lace, config, "image-shaped", even_levels, even_count,
            all_levels, config->levels, even_levels, even_count,
            odd_levels, odd_count, UINT64_C(0xb77c3f91480a62e5));
    }

    free(all_levels);
    free(even_levels);
    free(odd_levels);
    free(sparse_levels);
    free(non_sparse_levels);
    return status;
}

static void
benchmark_usage(const char *program)
{
    fprintf(stderr,
            "Usage: %s [--workers N] [--levels N] [--width N] "
            "[--rounds N]\n", program);
}

int
main(int argc, char **argv)
{
    benchmark_config config = { 1, 24, 64, 11 };
    for (int i = 1; i < argc; i++) {
        if (i + 1 >= argc) {
            benchmark_usage(argv[0]);
            return 2;
        }
        size_t value;
        if (!benchmark_parse_size(argv[++i], &value)) {
            benchmark_usage(argv[0]);
            return 2;
        }
        if (strcmp(argv[i - 1], "--workers") == 0 &&
            value <= UINT_MAX) {
            config.workers = (unsigned int)value;
        } else if (strcmp(argv[i - 1], "--levels") == 0 &&
                   value <= UINT32_MAX) {
            config.levels = value;
        } else if (strcmp(argv[i - 1], "--width") == 0) {
            config.width = value;
        } else if (strcmp(argv[i - 1], "--rounds") == 0) {
            config.rounds = value;
        } else {
            benchmark_usage(argv[0]);
            return 2;
        }
    }

    if (config.levels < 2) {
        benchmark_usage(argv[0]);
        return 2;
    }

    lace_start(config.workers, 0, 0);
    sylvan_set_sizes(
        (size_t)1 << 22, (size_t)1 << 22,
        (size_t)1 << 20, (size_t)1 << 20);
    sylvan_init_package();
    mtbdd_init();

    const int status = benchmark_run(&config);

    sylvan_quit();
    lace_stop();
    if (status != SYLVAN_OK) {
        fprintf(stderr, "benchmark failed with status %d\n", status);
        return 1;
    }
    return 0;
}
