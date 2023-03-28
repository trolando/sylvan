/**
 * N-queens example.
 * Based on work by Robert Meolic, released by him into the public domain.
 */

#include <getopt.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <sylvan.h>
#include <sylvan_table.h>

/* Configuration */
static int report_minterms = 0; // report minterms at every major step
static int report_minor = 0; // report minor steps
static int report_stats = 0; // report stats at end
static int workers = 0; // autodetect number of workers by default
static size_t size = 0; // will be set by caller

/* getopt configuration */

static void
print_usage()
{
    printf("Usage: nqueens [-h] [-w <workers>] [--workers <workers>] [--report-minor]\n");
    printf("            [--report-minterms] [--report-stats] [--help] [--usage] <size>\n");
}

static void
print_help()
{
    printf("Usage: nqueens [OPTION...] <size>\n\n");
    printf("  -w, --workers <workers>    Number of workers (default = 0: autodetect)\n");
    printf("      --report-minor         Report minor steps\n");
    printf("      --report-minterms      Report #minterms at every major step\n");
    printf("      --report-stats         Report statistics at end\n");
    printf("  -h, --help                 Give this help list\n");
    printf("      --usage                Give a short usage message\n");
}

static void
parse_args(int argc, char **argv)
{
    static const struct option longopts[] = {
        {.name = "workers", .val = 'w', .has_arg = required_argument},
        {.name = "report-minterms", .val = 1, .has_arg = no_argument},
        {.name = "report-minor", .val = 2, .has_arg = no_argument},
        {.name = "report-stats", .val = 3, .has_arg = no_argument},
        {.name = "usage", .val = 99, .has_arg = no_argument},
        {.name = "help", .val = 'h', .has_arg = no_argument},
        {},
    };
    int key = 0;
    int long_index = 0;
    while ((key = getopt_long(argc, argv, "w:h", longopts, &long_index)) != -1) {
        switch (key) {
        case 'w':
            workers = atoi(optarg);
            break;
        case 1:
            report_minterms = 1;
            break;
        case 2:
            report_minor = 1;
            break;
        case 3:
            report_stats = 1;
            break;
        case 99:
            print_usage();
            exit(0);
        case 'h':
            print_help();
            exit(0);
        }
    }
    if (optind >= argc) {
        printf("missing required parameter <size>\n");
        exit(-1);
    }
    size = atoi(argv[optind]);
}

/* Obtain current wallclock time */
static double
wctime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

static double t_start;
#define INFO(s, ...) fprintf(stdout, "[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(...) { fprintf(stderr, __VA_ARGS__); exit(-1); }

VOID_TASK_0(gc_start)
{
    if (report_minor) {
        printf("\n");
    }
    INFO("(GC) Starting garbage collection...\n");
}

VOID_TASK_0(gc_end)
{
    INFO("(GC) Garbage collection done.\n");
}

VOID_TASK_0(run)
{
    double t1 = wctime();

    BDD zero = sylvan_false;
    BDD one = sylvan_true;

    // Variables 0 ... (SIZE*SIZE-1)

    BDD board[size*size];
    for (size_t i=0; i<size*size; i++) {
        board[i] = sylvan_ithvar(i);
        sylvan_protect(board+i);
    }

    BDD res = one, temp = one;

    // we use sylvan's "protect" marking mechanism...
    // that means we hardly need to do manual ref/deref when the variables change
    sylvan_protect(&res);
    sylvan_protect(&temp);

    // Old satcount function still requires a silly variables cube
    BDD vars = one;
    sylvan_protect(&vars);
    for (size_t i=0; i<size*size; i++) vars = sylvan_and(vars, board[i]);

    INFO("Initialisation complete!\n");

    if (report_minor) {
        INFO("Encoding rows... ");
    } else {
        INFO("Encoding rows...\n");
    }

    for (size_t i=0; i<size; i++) {
        if (report_minor) {
            printf("%zu... ", i);
            fflush(stdout);
        }

        for (size_t j=0; j<size; j++) {
            // compute "\BigAnd (!board[i][k]) \or !board[i][j]" with k != j
            temp = one;
            for (size_t k=0; k<size; k++) {
                if (j==k) continue;
                temp = sylvan_and(temp, sylvan_not(board[i*size+k]));
            }
            temp = sylvan_or(temp, sylvan_not(board[i*size+j]));
            // add cube to "res"
            res = sylvan_and(res, temp);
        }
    }

    if (report_minor) {
        printf("\n");
    }
    if (report_minterms) {
        INFO("We have %.0f minterms\n", sylvan_satcount(res, vars));
    }
    if (report_minor) {
        INFO("Encoding columns... ");
    } else {
        INFO("Encoding columns...\n");
    }

    for (size_t j=0; j<size; j++) {
        if (report_minor) {
            printf("%zu... ", j);
            fflush(stdout);
        }

        for (size_t i=0; i<size; i++) {
            // compute "\BigAnd (!board[k][j]) \or !board[i][j]" with k != i
            temp = one;
            for (size_t k=0; k<size; k++) {
                if (i==k) continue;
                temp = sylvan_and(temp, sylvan_not(board[k*size+j]));
            }
            temp = sylvan_or(temp, sylvan_not(board[i*size+j]));
            // add cube to "res"
            res = sylvan_and(res, temp);
        }
    }

    if (report_minor) {
        printf("\n");
    }
    if (report_minterms) {
        INFO("We have %.0f minterms\n", sylvan_satcount(res, vars));
    }
    if (report_minor) {
        INFO("Encoding rising diagonals... ");
    } else {
        INFO("Encoding rising diagonals...\n");
    }

    for (size_t i=0; i<size; i++) {
        if (report_minor) {
            printf("%zu... ", i);
            fflush(stdout);
        }

        for (size_t j=0; j<size; j++) {
            temp = one;
            for (size_t k=0; k<size; k++) {
                // if (j+k-i >= 0 && j+k-i < size && k != i)
                if (j+k >= i && j+k < size+i && k != i) {
                    temp = sylvan_and(temp, sylvan_not(board[k*size + (j+k-i)]));
                }
            }
            temp = sylvan_or(temp, sylvan_not(board[i*size+j]));
            // add cube to "res"
            res = sylvan_and(res, temp);
        }
    }

    if (report_minor) {
        printf("\n");
    }
    if (report_minterms) {
        INFO("We have %.0f minterms\n", sylvan_satcount(res, vars));
    }
    if (report_minor) {
        INFO("Encoding falling diagonals... ");
    } else {
        INFO("Encoding falling diagonals...\n");
    }

    for (size_t i=0; i<size; i++) {
        if (report_minor) {
            printf("%zu... ", i);
            fflush(stdout);
        }

        for (size_t j=0; j<size; j++) {
            temp = one;
            for (size_t k=0; k<size; k++) {
                // if (j+i-k >= 0 && j+i-k < size && k != i)
                if (j+i >= k && j+i < size+k && k != i) {
                    temp = sylvan_and(temp, sylvan_not(board[k*size + (j+i-k)]));
                }
            }
            temp = sylvan_or(temp, sylvan_not(board[i*size + j]));
            // add cube to "res"
            res = sylvan_and(res, temp);
        }
    }

    if (report_minor) {
        printf("\n");
    }
    if (report_minterms) {
        INFO("We have %.0f minterms\n", sylvan_satcount(res, vars));
    }
    if (report_minor) {
        INFO("Final computation to place a queen on every row... ");
    } else {
        INFO("Final computation to place a queen on every row...\n");
    }

    for (size_t i=0; i<size; i++) {
        if (report_minor) {
            printf("%zu... ", i);
            fflush(stdout);
        }

        temp = zero;
        for (size_t j=0; j<size; j++) {
            temp = sylvan_or(temp, board[i*size+j]);
        }
        res = sylvan_and(res, temp);
    }

    if (report_minor) {
        printf("\n");
    }

    double t2 = wctime();

    INFO("Result: NQueens(%zu) has %.0f solutions.\n", size, sylvan_satcount(res, vars));
    INFO("Result BDD has %zu nodes.\n", sylvan_nodecount(res));
    INFO("Computation time: %f sec.\n", t2-t1);
}

int
main(int argc, char** argv)
{
    parse_args(argc, argv);
    setlocale(LC_NUMERIC, "en_US.utf-8");
    t_start = wctime();

    // Init Lace
    lace_start(workers, 1000000); // auto-detect number of workers, use a 1,000,000 size task queue

    // Lace is initialized, now set local variables

    // Init Sylvan
    // Nodes table size of 1LL<<20 is 1048576 entries
    // Cache size of 1LL<<18 is 262144 entries
    // Nodes table size: 24 bytes * nodes
    // Cache table size: 36 bytes * cache entries
    // With 2^20 nodes and 2^18 cache entries, that's 33 MB
    // With 2^24 nodes and 2^22 cache entries, that's 528 MB
    sylvan_set_sizes(1LL<<20, 1LL<<24, 1LL<<18, 1LL<<22);
    sylvan_init_package();
    sylvan_set_granularity(3); // granularity 3 is decent value for this small problem - 1 means "use cache for every operation"
    sylvan_init_bdd();

    // Before and after garbage collection, call gc_start and gc_end
    sylvan_gc_hook_pregc(TASK(gc_start));
    sylvan_gc_hook_postgc(TASK(gc_end));

    RUN(run);

    if (report_stats) {
        sylvan_stats_report(stdout);
    }

    sylvan_quit();
    lace_stop();
}

