#include <argp.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <getrss.h>
#include <sylvan_int.h>
#include <sylvan_sl.h>

/**
 * TODO
 * - implement trace output and input
 * - variable selection heuristics: smallest #nodes, smallest #clauses, largest #clauses
 * - convert ZDD to CNF on the graph
 */

/* Configuration */
static int workers = 0; // autodetect
static int verbose = 0;
static int parsetobdd = 0;
static int tobdd = 0;
static int useisoc = 0;
static int qmc = -1;
static int memory = 2048;
static int bound = 0;
static int death_threshold = 100;
static int nodebound = 0;
static int clausebound = 0;
static int inflate = 0;
static char* cnf_filename = NULL; // filename of CNF
static char* out_filename = NULL; // filename of output
static char* dot_filename = NULL; // filename of DOT file
static char* trace_filename = NULL; // filename of trace file
static char* tracein_filename = NULL; // filename of trace file
static char* enc_filename = NULL; // re-encode CNF from ZDD

/* argp configuration */
static struct argp_option options[] =
{
    {0, 0, 0, 0, "Basic program options", 1},
    {"workers", 'w', "<workers>", 0, "Number of workers (default=0: autodetect)", 0},
    {"memory", 'm', "<megabytes>", 0, "How many MB memory for nodes table plus operation cache", 0},
    {"verbose", 'v', 0, 0, "Set output to verbose", 0},
    {0, 0, 0, 0, "Parsing and preprocessing", 2},
    {"parsetobdd", 1, 0, 0, "During parsing, construct BDD to compute SAT/UNSAT", 2},
    {"tobdd", 2, 0, 0, "After parsing, construct BDD to compute SAT/UNSAT", 2},
    {"dot", 'd', "<dot file>", 0, "After parsing, write DOT file of the clause ZDD", 2},
    {"inflate", 7, "<cycles>", 0, "After parsing, inflate the clause set for <cycles> cycles", 2},
    {0, 0, 0, 0, "Variable elimination", 3},
    {"isoc", 3, 0, 0, "Compute VE using the ISOC algorithm on the BDD of the variable environment", 3},
    {"qmc", 4, "<factor>", 0, "Use QMC-style resolution whenever the ZDD has grown by <factor>", 3},
    {"nodebound", 5, 0, 0, "Perform bounded variable elimination based on #nodes", 3},
    {"clausebound", 6, 0, 0, "Perform bounded variable elimination based on #clauses", 3},
    {0, 0, 0, 0, "Other options", 4},
    {"trace", 't', "<trace file>", 0, "Write action trace to file", 4},
    {"tracein", 'i', "<trace file>", 0, "Read action trace from file", 4},
    {"output", 'o', "<output file>", 0, "Write result to CNF", 4},
    {"reencode", 'r', "<reencode file>", 0, "After preprocessing, re-encode CNF from ZDD", 4},
    {0, 0, 0, 0, 0, 0}
};

static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case 'w':
        workers = atoi(arg);
        break;
    case 'v':
        verbose = 1;
        break;
    case 'o':
        out_filename = arg;
        break;
    case 'd':
        dot_filename = arg;
        break;
    case 'm':
        memory = atoi(arg);
        break;
    case 't':
        trace_filename = arg;
        break;
    case 'i':
        tracein_filename =arg;
        break;
    case 'r':
        enc_filename = arg;
        break;
    case 1:
        parsetobdd = 1;
        break;
    case 2:
        tobdd = 1;
        break;
    case 3:
        useisoc = 1;
        break;
    case 4:
        qmc = atoi(arg);
        break;
    case 5:
        nodebound = 1;
        break;
    case 6:
        clausebound = 1;
        break;
    case 7:
        inflate = atoi(arg);
        break;
    case ARGP_KEY_ARG:
        if (state->arg_num == 0) cnf_filename = arg;
        if (state->arg_num >= 2) argp_usage(state);
        break; 
    case ARGP_KEY_END:
        // if (state->arg_num < 1) argp_usage(state);
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = { options, parse_opt, "<cnf_file>", 0, 0, 0, 0 };

/* Obtain current wallclock time */
static double
wctime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

static double t_start;
#define INFO(s, ...) fprintf(stdout, "\rc [% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(s, ...) { fprintf(stderr, "\rc [% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__); exit(-1); }

#define print_clause_db(db, nvars) CALL(fprint_clause_db, stdout, db, nvars)
#define fprint_clause_db(f, db, nvars) CALL(fprint_clause_db, f, db, nvars)
VOID_TASK_DECL_3(fprint_clause_db, FILE*, ZDD, int);

/**
 * Comparator for comparing literals by variable.
 */
int literal_compare(const void *a, const void *b)
{
    int lita = *(int*)a;
    if (lita<0) lita=-lita;
    int litb = *(int*)b;
    if (litb<0) litb=-litb;
    return lita-litb;
}

int nvars, nclauses, nlits;

/**
 * Parser.
 * Read CNF file into a ZDD of clause database.
 */
TASK_1(ZDD, parse_cnf_file, FILE*, file)
{
    /* parse header */
    while (1) {
        int ch = fgetc(file);
        if (ch == EOF) {
            Abort("unexpected end-of-file");
        } else if (ch == 'c') {
            while ((ch = fgetc(file)) != '\n') {
                if (ch == EOF) Abort("unexpected end-of-file");
            }
            continue;
        } else if (ch != 'p') {
            Abort("unexpected characters");
        }
        if ((fscanf (file, " cnf %d %d", &nvars, &nclauses)) != 2) {
            Abort("invalid header");
        }
        if (nvars < 0 || nclauses < 0) {
            Abort("invalid header");
        }
        break;
    }
    
    /* setup arrays */
    int lits_size = 64, lits_count = 0;
    int *lits = (int*)malloc(sizeof(int[lits_size]));
    if (lits == NULL) Abort("out of memory");

    /* start with empty clause database */
    ZDD db = zdd_false;
    zdd_refs_pushptr(&db);

    /* parse clauses */
    int read_clauses = 0;
    int last_literal = 0;
    nlits = 0;

    double last_report = 0;

    while (1) {
        int ch = fgetc(file);
        /* skip whitespace */
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
        /* skip comment lines */
        if (ch == 'c') {
            while ((ch = fgetc(file)) != '\n') {
                if (ch == EOF) Abort("unexpected end-of-file");
            }
            continue;
        }
        /* if EOF, check if good */
        if (ch == EOF) {
            if (last_literal != 0) Abort("zero missing");
            if (read_clauses < nclauses) Abort("clause(s) missing");
            break;
        }
        /* read next sign or digit */
        int sign;
        if (ch == '-') {
            sign = -1;
            ch = fgetc(file);
            if (!isdigit(ch)) Abort("expected digit after '-'");
        } else {
            sign = 1;
            if (!isdigit (ch)) Abort("expected digit or '-'");
        }
        int lit = ch - '0';
        /* parse rest of literal */
        while (isdigit(ch = fgetc(file))) lit = 10*lit + (ch - '0');
        if (lit > nvars) Abort("variable exceeds maximum");
        if (read_clauses >= nclauses) Abort("number of clauses more than expected");
        /* increase array size if needed */
        if (lits_count == lits_size) {
            lits_size += lits_size/2;
            lits = (int*)realloc(lits, sizeof(int[lits_size]));
            if (lits == NULL) Abort("out of memory");
        }
        if (lit != 0) {
            /* add literal to array */
            lits[lits_count++] = lit * sign;
            nlits++;
        } else {
            /* sort the literals by variable */
            qsort(lits, lits_count, sizeof(int), literal_compare);
            /* check for tautology or same variables */
            for (int i=0; i<lits_count-1; i++) {
                if (lits[i] == lits[i+1] || lits[i] == -lits[i+1]) Abort("variables twice in clause");
            }
            /* add clause to database */
            lits[lits_count] = 0;
            db = zdd_add_clause(db, lits);
            /* increase number of read clauses and reset literal count */
            read_clauses++;
            lits_count = 0;
            /* report? */
            if (verbose) {
                double perc = 100.0*read_clauses/nclauses;
                // every 5%
                if ((int)(perc/1) > (int)(last_report/1)) {
                    INFO("%.2f%% %zu nodes %d clauses\n", perc, zdd_nodecount(&db, 1), read_clauses);
                    last_report = perc;
                }
                (void)last_report;
            }
        }
        /* set last literal and continue loop */
        last_literal = lit;
    }

    INFO("Read %s, %d variables, %d clauses, %d literals.\n", cnf_filename, nvars, nclauses, nlits);

    // printf("c total number of literals: %d\n", nlits);
    free(lits);
    zdd_refs_popptr(1);
    return db;
}

/**
 * Helper function for parallel recursive CNF-to-ZDD conversion
 */
TASK_4(ZDD, arr_to_zdd, int*, lits, int*, indices, int, first, int, count)
{
    if (count <= 32) {
        ZDD db = zdd_false;
        zdd_refs_pushptr(&db);
        for (int i=0; i<count; i++) db = zdd_add_clause(db, lits+indices[first+i]);
        zdd_refs_popptr(1);
        return db;
    } else {
        zdd_refs_spawn(SPAWN(arr_to_zdd, lits, indices, first, count/2));
        ZDD db1 = zdd_refs_push(CALL(arr_to_zdd, lits, indices, first + count/2, count - count/2));
        ZDD db2 = zdd_refs_push(zdd_refs_sync(SYNC(arr_to_zdd)));
        db1 = zdd_or(db1, db2);
        zdd_refs_pop(2);
        return db1;
    }
}

/**
 * Parser.
 * First read CNF into int array.
 * Then convert int array to ZDD.
 */
TASK_1(ZDD, parse_cnf_file_2, FILE*, file)
{
    /* parse header */
    while (1) {
        int ch = fgetc(file);
        if (ch == EOF) {
            Abort("unexpected end-of-file");
        } else if (ch == 'c') {
            while ((ch = fgetc(file)) != '\n') {
                if (ch == EOF) Abort("unexpected end-of-file");
            }
            continue;
        } else if (ch != 'p') {
            Abort("unexpected characters");
        }
        if ((fscanf (file, " cnf %d %d", &nvars, &nclauses)) != 2) {
            Abort("invalid header");
        }
        if (nvars < 0 || nclauses < 0) {
            Abort("invalid header");
        }
        break;
    }
    
    /* setup arrays */
    int lits_size = 64, lits_count = 0;
    int *lits = (int*)malloc(sizeof(int[lits_size]));
    if (lits == NULL) Abort("out of memory");
    int *indices = (int*)malloc(sizeof(int[nclauses]));

    /* parse clauses */
    int read_clauses = 0;
    int last_literal = 0;
    int lits_first = 0;
    nlits = 0;

    while (1) {
        int ch = fgetc(file);
        /* skip whitespace */
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
        /* skip comment lines */
        if (ch == 'c') {
            while ((ch = fgetc(file)) != '\n') {
                if (ch == EOF) Abort("unexpected end-of-file");
            }
            continue;
        }
        /* if EOF, check if good */
        if (ch == EOF) {
            if (last_literal != 0) Abort("zero missing");
            if (read_clauses < nclauses) Abort("clause(s) missing");
            break;
        }
        /* read next sign or digit */
        int sign;
        if (ch == '-') {
            sign = -1;
            ch = fgetc(file);
            if (!isdigit(ch)) Abort("expected digit after '-'");
        } else {
            sign = 1;
            if (!isdigit (ch)) Abort("expected digit or '-'");
        }
        int lit = ch - '0';
        /* parse rest of literal */
        while (isdigit(ch = fgetc(file))) lit = 10*lit + (ch - '0');
        if (lit > nvars) Abort("variable exceeds maximum");
        if (read_clauses >= nclauses) Abort("number of clauses more than expected");
        /* increase array size if needed */
        if (lits_count == lits_size) {
            lits_size += lits_size/2; // grow by factor 150%
            lits = (int*)realloc(lits, sizeof(int[lits_size]));
            if (lits == NULL) Abort("out of memory");
        }
        if (lit != 0) {
            /* add literal to array */
            lits[lits_count++] = lit * sign;
            nlits++;
        } else {
            /* sort the literals by variable */
            qsort(lits+lits_first, lits_count-lits_first, sizeof(int), literal_compare);
            /* check for tautology or same variables */
            for (int i=lits_first; i<lits_count-1; i++) {
                if (lits[i] == lits[i+1] || lits[i] == -lits[i+1]) Abort("variables twice in clause");
            }
            /* add clause to database */
            lits[lits_count++] = 0;
            indices[read_clauses++] = lits_first;
            lits_first = lits_count;
        }
        /* set last literal and continue loop */
        last_literal = lit;
    }

    //if (verbose) INFO("Read literals read %d clauses read %d expected %d\n", nlits, read_clauses, nclauses);
    INFO("Read %s, %d variables, %d clauses, %d literals.\n", cnf_filename, nvars, nclauses, nlits);


    /* start with empty clause database */
    
    ZDD db = CALL(arr_to_zdd, lits, indices, 0, read_clauses);

    free(lits);
    return db;
}

/**
 * Parser.
 * Read CNF file into a BDD of satisfying assignments.
 */
TASK_1(ZDD, parse_cnf_file_bdd, FILE*, file)
{
    /* parse header */
    while (1) {
        int ch = fgetc(file);
        if (ch == EOF) {
            Abort("unexpected end-of-file");
        } else if (ch == 'c') {
            while ((ch = fgetc(file)) != '\n') {
                if (ch == EOF) Abort("unexpected end-of-file");
            }
            continue;
        } else if (ch != 'p') {
            Abort("unexpected characters");
        }
        if ((fscanf (file, " cnf %d %d", &nvars, &nclauses)) != 2) {
            Abort("invalid header");
        }
        if (nvars < 0 || nclauses < 0) {
            Abort("invalid header");
        }
        break;
    }
    
    /* start with unconstrained set */
    MTBDD sat = mtbdd_true;
    mtbdd_refs_pushptr(&sat);

    MTBDD vars = mtbdd_true;
    mtbdd_refs_pushptr(&vars);
    for (int i=nvars; i>=1; i--) vars = sylvan_set_add(vars, i);

    /* parse clauses */
    uint8_t *cube_arr = (uint8_t*)malloc(sizeof(int[nvars]));
    for (int i=0; i<nvars; i++) cube_arr[i] = 2;
    int read_clauses = 0;
    int last_literal = 0;
    nlits = 0;

    double last_report = 0;

    while (1) {
        int ch = fgetc(file);
        /* skip whitespace */
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
        /* skip comment lines */
        if (ch == 'c') {
            while ((ch = fgetc(file)) != '\n') {
                if (ch == EOF) Abort("unexpected end-of-file");
            }
            continue;
        }
        /* if EOF, check if good */
        if (ch == EOF) {
            if (last_literal != 0) Abort("zero missing");
            if (read_clauses < nclauses) Abort("clause(s) missing");
            break;
        }
        /* read next sign or digit */
        int sign;
        if (ch == '-') {
            sign = -1;
            ch = fgetc(file);
            if (!isdigit(ch)) Abort("expected digit after '-'");
        } else {
            sign = 1;
            if (!isdigit (ch)) Abort("expected digit or '-'");
        }
        int lit = ch - '0';
        /* parse rest of literal */
        while (isdigit(ch = fgetc(file))) lit = 10*lit + (ch - '0');
        if (lit > nvars) Abort("variable exceeds maximum");
        if (read_clauses >= nclauses) Abort("number of clauses more than expected");
        /* increase array size if needed */
        if (lit != 0) {
            /* add literal to array */
            if (cube_arr[lit-1] != 2) Abort("variables twice in clause");
            cube_arr[lit-1] = sign == -1 ? 0 : 1;
            nlits++;
        } else {
            /* add clause to database */
            MTBDD cl = mtbdd_false;
            mtbdd_refs_pushptr(&cl);
            for (int i=nvars-1; i>=0; i--) {
                if (cube_arr[i] == 0) cl = sylvan_makenode(i+1, sylvan_true, cl);
                if (cube_arr[i] == 1) cl = sylvan_makenode(i+1, cl, sylvan_true);
            }
            sat = sylvan_and(sat, cl);
            mtbdd_refs_popptr(1);
            /* increase number of read clauses and reset literal count */
            read_clauses++;
            for (int i=0; i<nvars; i++) cube_arr[i] = 2;
            /* report? */
            if (verbose) {
                double perc = 100.0*read_clauses/nclauses;
                // every 5%
                //if ((int)(perc/1) > (int)(last_report/1)) {
                    INFO("%.2f%% %zu nodes %d clauses\n", perc, mtbdd_nodecount(sat), read_clauses);
                    last_report = perc;
                //}
                (void)last_report;
            }
            if (sat == mtbdd_false) {
                if (verbose) sylvan_stats_report(stdout);
                INFO("Empty sat after %d clauses\n", read_clauses);
                Abort("UNSAT\n");
            }
        }
        /* set last literal and continue loop */
        last_literal = lit;
    }

    Abort("?\n");

    // printf("c total number of literals: %d\n", nlits);
    free(cube_arr);
    mtbdd_refs_popptr(2);
    return sat;
}

VOID_TASK_IMPL_3(fprint_clause_db, FILE*, f, ZDD, db, int, nvars)
{
    // Get number of clauses
    int n_clauses = zdd_satcount(db);
    fprintf(f, "p cnf %d %d\n", nvars, n_clauses);

    int32_t arr[nvars+1];
    ZDD res = zdd_clause_enum_first(db, arr);
    while (res != zdd_false) {
        for (int i=0; i<nvars; i++) {
            if (arr[i] == 0) {
                fprintf(f, "0\n");
                break;
            } else {
                fprintf(f, "%d ", arr[i]);
            }
        }
        res = zdd_clause_enum_next(db, arr);
    }
}

char*
to_h(double size, char *buf) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    int i = 0;
    for (;size>1024;size/=1024) i++;
    sprintf(buf, "%.*f %s", i, size, units[i]);
    return buf;
}

static void
print_memory_usage(void)
{
    char buf[32];
    to_h(getCurrentRSS(), buf);
    INFO("Memory usage: %s\n", buf);
}

void
print_set(ZDD set)
{
    if (set == zdd_true) return;
    printf("%d", zdd_getvar(set));
    if (zdd_gethigh(set) != zdd_true) printf(", ");
    print_set(zdd_gethigh(set));
}

void
print_units(ZDD set)
{
    if (set == zdd_true) return;
    uint32_t var = zdd_getvar(set);
    printf("%s%d", var&1?"":"-", var/2);
    if (zdd_gethigh(set) != zdd_true) printf(",");
    print_units(zdd_gethigh(set));
}

MTBDD
units_to_bdd(ZDD set)
{
    if (set == zdd_true) return mtbdd_true;
    MTBDD sub = units_to_bdd(zdd_gethigh(set));
    uint32_t var = zdd_getvar(set);
    if (var&1) {
        // positive
        return mtbdd_makenode(var/2, sylvan_false, sub);
    } else {
        // negative
        return mtbdd_makenode(var/2, sub, sylvan_false);
    }
}

ZDD
units_to_zdd(ZDD set)
{
    if (set == zdd_true) return zdd_false; // empty set
    ZDD sub = units_to_zdd(zdd_gethigh(set));
    uint32_t var = zdd_getvar(set);
    return zdd_makenode(var, sub, zdd_true);
}

VOID_TASK_0(gc_start)
{
    INFO("Starting garbage collection\n");
}

VOID_TASK_0(gc_end)
{
    INFO("Garbage collection done\n");
}

TASK_0(ZDD, read_input_cnf)
{
    ZDD db;

    if (cnf_filename != NULL) {
        // Open file
        FILE *f = fopen(cnf_filename, "r");
        if (f == NULL) Abort("Cannot open file %s!\n", cnf_filename);

        INFO("Opened %s.\n", cnf_filename);

        if (parsetobdd) {
            MTBDD sat = CALL(parse_cnf_file_bdd, f);
            if (sat == mtbdd_false) {
                INFO("UNSAT\n");
                if (verbose) sylvan_stats_report(stdout);
                exit(-1);
            } else {
                INFO("SAT\n");
                if (verbose) sylvan_stats_report(stdout);
                exit(0);
            }
        }

        db = CALL(parse_cnf_file_2, f);

        // Close file
        fclose(f);
    } else {
        INFO("Reading from stdin.\n");
        if (parsetobdd) {
            MTBDD sat = CALL(parse_cnf_file_bdd, stdin);
            if (sat == mtbdd_false) {
                INFO("UNSAT\n");
                if (verbose) sylvan_stats_report(stdout);
                exit(-1);
            } else {
                INFO("SAT\n");
                if (verbose) sylvan_stats_report(stdout);
                exit(0);
            }
        }
        db = CALL(parse_cnf_file_2, stdin);
    }

    return db;
}

int
main(int argc, char **argv)
{
    t_start = wctime();
    // Parse arguments
    argp_parse(&argp, argc, argv, 0, 0, 0);

    // Init Lace
    lace_init(workers, 1000000); // auto-detect number of workers, use a 1,000,000 size task queue
    lace_startup(0, NULL, NULL); // auto-detect program stack, do not use a callback for startup

    LACE_ME;

    // Init Sylvan
    // Give 2 GB memory
    sylvan_set_limits((size_t)memory*1LL<<20, 0, 10);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_zdd();

    // Set hooks for logging garbage collection
    if (verbose) {
        sylvan_gc_hook_pregc(TASK(gc_start));
        sylvan_gc_hook_postgc(TASK(gc_end));
    }
    
    // Read the input CNF
    ZDD db = CALL(read_input_cnf);
    zdd_refs_pushptr(&db);

    // Report the number of clauses and nodes
    if (1) {
        double n_clauses = zdd_satcount(db);
        size_t n_nodes = zdd_nodecount(&db, 1);
        INFO("After converting CNF to ZDD: %.0lf clauses using %zu nodes.\n", n_clauses, n_nodes);
    }

    if (tobdd) {
        if (zdd_clause_sat(db, mtbdd_true) == mtbdd_false) {
            INFO("UNSAT\n");
            if (verbose) sylvan_stats_report(stdout);
            exit(-1);
        } else {
            INFO("SAT\n");
            if (verbose) sylvan_stats_report(stdout);
            exit(0);
        }
    }

    // Perform self subsumption on clause database
    db = zdd_clause_self_subsume(db);

    // Report the number of clauses and nodes
    if (1) {
        double n_clauses = zdd_satcount(db);
        size_t n_nodes = zdd_nodecount(&db, 1);
        INFO("After performing self-subsumption: %.0lf clauses using %zu nodes.\n", n_clauses, n_nodes);
    }

    if (dot_filename != NULL) {
        FILE *fdot = fopen(dot_filename,"w");
        zdd_fprintdot(fdot, db);
        fclose(fdot);
    }

    // The ZDD "all_units" will store all unit clauses found during analysis...
    // Initialize with zdd_true, i.e., the empty set.
    ZDD all_units = zdd_true;
    zdd_refs_pushptr(&all_units);

    // The ZDD for temporary units...
    ZDD units = zdd_true;
    zdd_refs_pushptr(&units);

    // Find all unit clauses in the clause database
    units = zdd_clause_units(db);
    if (units == zdd_false) {
        Abort("The empty clause has been found!\n");
    }

    // Process all the units
    if (units != zdd_true) {
        // Repeat until no more new units are found
        while (units != zdd_true) {
            if (verbose) INFO("Found %zu new units!\n", zdd_set_count(units));
            // Add the new units to all_units and check for contradictions
            all_units = zdd_set_union(all_units, units);
            if (zdd_clause_units_contradict(all_units)) {
                Abort("Units contradict! Aborting.\n");
            }
            // Then perform unit propagation of the new units
            db = zdd_clause_up(db, units);
            // See if there are new units now
            units = zdd_clause_units(db);
            if (units == zdd_false) {
                Abort("The empty clause has been found! Aborting.\n");
            }
        }
    }

    // Report the number of clauses and nodes
    if (1) {
        double n_clauses = zdd_satcount(db);
        size_t n_nodes = zdd_nodecount(&db, 1);
        INFO("After initial unit propagation: %.0lf clauses using %zu nodes.\n", n_clauses, n_nodes);
    }

    ZDD reduced = zdd_false;
    zdd_refs_pushptr(&reduced);

    ZDD old_db = zdd_false;
    zdd_refs_pushptr(&old_db);

    double db_clauses = zdd_satcount(db);
    size_t db_nodes = zdd_nodecount(&db, 1);

    const double orig_db_clauses = db_clauses;
    const size_t orig_db_nodes = db_nodes;

    /**
     * This is of course a totally strange thing
     */

    if (inflate > 0) {
        INFO("Inflating %d times!\n", inflate);
    }

    for (int ii=0; ii<inflate; ii++) {
        ZDD bucket = zdd_false; // empty
        zdd_refs_pushptr(&bucket);

        for (int i=1; i<=nvars; i++) {
            /**
             * Compute the environment of the current variable
             */
            ZDD lits = zdd_set_from_array((uint32_t[]){2*i, 2*i+1}, 2);
            zdd_refs_push(lits);
            ZDD env = zdd_clause_environment(db, lits);
            zdd_refs_push(env);

            if (env == zdd_false) {
                zdd_refs_pop(2);
                continue;
            }

            if (useisoc) {
                /**
                 * Compute the satisfying assignments for the current environment
                 */
                MTBDD sat = zdd_clause_sat(env, mtbdd_true);
                mtbdd_refs_push(sat);

                /**
                 * Eliminate variable
                 */
                MTBDD quantified = sylvan_exists(sat, sylvan_ithvar(i));
                mtbdd_refs_push(quantified);

                MTBDD q_check;
                reduced = zdd_clause_isoc(quantified, quantified, &q_check);

                assert(q_check == quantified);
                mtbdd_refs_pop(2);
            } else {
                ZDD cof_n = zdd_clause_cof(env, 2*i);
                zdd_refs_push(cof_n);
                ZDD cof_p = zdd_clause_cof(env, 2*i+1);
                zdd_refs_push(cof_p);
                reduced = zdd_clause_distribution(cof_n, cof_p);
                zdd_refs_pop(2);
            }

            /**
             * Add to bucket
             */
            bucket = zdd_clause_union(bucket, reduced);
            zdd_refs_pop(2); // lits, env

            /**
             * Report
             */
            if (verbose) {
                double bucket_clauses = zdd_satcount(bucket);
                size_t bucket_nodes = zdd_nodecount(&bucket, 1);
                INFO("\033[1;36mUpdated\033[m var %d/%d %.0f clauses (%zd nodes)\n", i, nvars, bucket_clauses, bucket_nodes);
            }
        }

        /**
         * Report
         */
        double bucket_clauses = zdd_satcount(bucket);
        size_t bucket_nodes = zdd_nodecount(&bucket, 1);
        INFO("After sweep: %.0f clauses (%zd nodes)\n", bucket_clauses, bucket_nodes);

        /**
         * Make Quine Free
         */

        db = zdd_clause_union(db, bucket);
        zdd_refs_popptr(1);
        INFO("After union: %.0f clauses (%zd nodes)\n", zdd_satcount(db), zdd_nodecount(&db, 1));

        db = zdd_clause_qmc(db);
        INFO("After QMC-style resolution: %.0f clauses (%zd nodes)\n", zdd_satcount(db), zdd_nodecount(&db, 1));
    }
    
    /**
     * After inflation, maybe write the ZDD-CNF encoding?
     * There are <nvars> (1..2..n)
     * Encode "false" with variable n+1
     * Encode "true" with variable n+2
     */
    if (enc_filename) {
        FILE* f = fopen(enc_filename, "w");
        // get number of nodes...
        size_t n_nodes = zdd_nodecount(&db, 1);
        sylvan_skiplist_t sl = sylvan_skiplist_alloc(n_nodes);
        zdd_writer_add(sl, db);
        size_t sl_count = sylvan_skiplist_count(sl);
        // number of clauses is 3 + 2*sl_count
        // number of variables is nvars + sl_count + 2
        fprintf(f, "p cnf %zu %zu\n", nvars+sl_count+2, 3+2*sl_count);
        // encode root
        fprintf(f, "%d 0\n", nvars + 2 + (int)sylvan_skiplist_get(sl, db));
        // encode false as positive <nvars+1>
        fprintf(f, "%d 0\n", nvars + 1);
        // encode true as negative <nvars+2>
        fprintf(f, "%d 0\n", -(nvars + 2));
        // encode each node
        // THEN as -<node> <lit> <thennode>
        // ELSE as -<node> <elsenode>
        for (size_t i=1; i<=sl_count; i++) {
            ZDD dd = sylvan_skiplist_getr(sl, i);
            int nodevar = nvars + 2 + i;
            int cnfvar = zdd_getvar(dd);
            cnfvar = (cnfvar&1) ? cnfvar/2+1 : -1*(cnfvar/2+1);
            ZDD dd1 = zdd_gethigh(dd);
            ZDD dd0 = zdd_getlow(dd);
            int thennodevar = dd1 == zdd_false ? nvars+1 : dd1 == zdd_true ? nvars+2 : nvars + 2 + (int)sylvan_skiplist_get(sl, dd1);
            int elsenodevar = dd0 == zdd_false ? nvars+1 : dd0 == zdd_true ? nvars+2 : nvars + 2 + (int)sylvan_skiplist_get(sl, dd0);
            fprintf(f, "%d %d %d 0\n", -nodevar, cnfvar, thennodevar);
            fprintf(f, "%d %d 0\n", -nodevar, elsenodevar);
        }
        sylvan_skiplist_free(sl);
        fclose(f);
        INFO("Re-encoded ZDD to CNF %s\n", enc_filename);
        return 0;
    }

    size_t last_qmc_size = db_nodes;

    int elim_total = 0;
    int i = 1;

    if (tracein_filename != 0) {
        FILE *fin = fopen(tracein_filename, "r");
        if (fin == NULL) Abort("cannot read tracein file");
        while (!feof(fin)) {
            int i = 0;
            int ch;
            while ((ch = fgetc(fin)) != EOF) {
                if (ch != ' ' && ch != '\n') break;
            }
            if (ch == EOF) break;
            if (ch == 'q') {
                db = zdd_clause_qmc(db);
                db_clauses = zdd_satcount(db);
                db_nodes = zdd_nodecount(&db, 1);

                INFO("After \033[1;32mQMC-style resolution\033[m %g clauses (%zd nodes)\n", db_clauses, db_nodes);
                continue;
            }
            if (ch != 'e') Abort("tracefile error\n");

            if (fscanf(fin, "%d", &i) != 1) break;

            /**
             * Compute the environment of the current variable
             */
            ZDD lits = zdd_set_from_array((uint32_t[]){2*i, 2*i+1}, 2);
            zdd_refs_push(lits);
            ZDD env = zdd_clause_environment(db, lits);
            zdd_refs_push(env);

            if (env == zdd_false) continue; // nothing to do

            if (useisoc) {
                /**
                 * Compute the satisfying assignments for the current environment
                 */
                MTBDD sat = zdd_clause_sat(env, mtbdd_true);
                mtbdd_refs_push(sat);

                /**
                 * Eliminate variable
                 */
                MTBDD quantified = sylvan_exists(sat, sylvan_ithvar(i));
                mtbdd_refs_push(quantified);

                /**
                 * Run ISOC algorithm
                 */
                MTBDD q_check;
                reduced = zdd_clause_isoc(quantified, quantified, &q_check);

                mtbdd_refs_pop(2);
            } else {
                ZDD cof_n = zdd_clause_cof(env, 2*i);
                zdd_refs_push(cof_n);
                ZDD cof_p = zdd_clause_cof(env, 2*i+1);
                zdd_refs_push(cof_p);
                reduced = zdd_clause_distribution(cof_n, cof_p);
                zdd_refs_pop(2);
            }

            /**
             * Replace clauses
             */
            old_db = db;
            db = zdd_diff(db, env);
            db = zdd_clause_union(db, reduced);

            zdd_refs_pop(2); // lits, env

            // Find all unit clauses in the clause database
            units = zdd_clause_units(db);
            if (units == zdd_false) {
                Abort("The empty clause has been found!\n");
            }

            /**
             * Propagate units
             */
            if (units != zdd_true) {
                // Repeat until no more new units are found
                while (units != zdd_true) {
                    if (verbose) INFO("Found %zu new units!\n", zdd_set_count(units));
                    // Add the new units to all_units and check for contradictions
                    all_units = zdd_set_union(all_units, units);
                    if (zdd_clause_units_contradict(all_units)) {
                        Abort("Units contradict! Aborting. (UNSAT)\n");
                    }
                    // Then perform unit propagation of the new units
                    db = zdd_clause_up(db, units);
                    // See if there are new units now
                    units = zdd_clause_units(db);
                    if (units == zdd_false) {
                        Abort("The empty clause has been found! Aborting. (UNSAT)\n");
                    }
                }
            }

            db_clauses = zdd_satcount(db);
            db_nodes = zdd_nodecount(&db, 1);

            //INFO("After \033[1;32mvariable elimination\033[m of %d %g clauses (%zd nodes)\n", i, db_clauses, db_nodes);
            INFO("After \033[1;32mvariable elimination\033[m %g clauses (%zd nodes)\n", db_clauses, db_nodes);
        }

        if (db == zdd_true) {
            Abort("Empty clause! (UNSAT)\n");
        } else if (db == zdd_false) {
            Abort("No clause! (SAT)\n");
        }
    }

    FILE* tracefile = (trace_filename != 0) ? fopen(trace_filename, "w") : NULL;

    for (;;) {
        int minbound = -1;
        int mini = 1;
        int elim=0;

        if (bound != 0) {
            if (clausebound) INFO("Running loop with clause bound=%d\n", bound);
            else if (nodebound) INFO("Running loop with node bound=%d\n", bound);
        }

        for (; i<=nvars; i++) {
            /**
             * Compute the environment of the current variable
             */
            ZDD lits = zdd_set_from_array((uint32_t[]){2*i, 2*i+1}, 2);
            zdd_refs_push(lits);
            ZDD env = zdd_clause_environment(db, lits);
            zdd_refs_push(env);

            if (env == zdd_false) continue; // TODO: not ideal, remember

            if (useisoc) {
                /**
                 * Compute the satisfying assignments for the current environment
                 */
                MTBDD sat = zdd_clause_sat(env, mtbdd_true);
                mtbdd_refs_push(sat);

                /**
                 * Eliminate variable
                 */
                MTBDD quantified = sylvan_exists(sat, sylvan_ithvar(i));
                mtbdd_refs_push(quantified);

                MTBDD q_check;
                reduced = zdd_clause_isoc(quantified, quantified, &q_check);

                if (q_check != quantified) {
                    MTBDD what = zdd_clause_sat(reduced, mtbdd_true);
                    ZDD env_vars = zdd_clause_support(env);
                    zdd_refs_push(env_vars);
                    int n_env_vars = zdd_set_count(env_vars);
                    printf("uh oh %zx %zx %zx\n", q_check, what, quantified);
                    printf("qcheck/what has %f, quantified %f minterms", mtbdd_satcount(what, n_env_vars), mtbdd_satcount(quantified, n_env_vars));
                    assert(q_check == quantified);
                }
                mtbdd_refs_pop(2);
            } else {
                ZDD cof_n = zdd_clause_cof(env, 2*i);
                zdd_refs_push(cof_n);
                ZDD cof_p = zdd_clause_cof(env, 2*i+1);
                zdd_refs_push(cof_p);
                reduced = zdd_clause_distribution(cof_n, cof_p);
                zdd_refs_pop(2);

                if (verbose) {
                    //double env_before = zdd_satcount(env);
                    //double env_after = (useisoc) ? zdd_satcount(reduced) : zdd_satcount(reduced);
                    // INFO("From %f (%f X %f) to %f\n.", env_before, zdd_satcount(cof_n), zdd_satcount(cof_p), env_after);
                }
            }

            /**
             * Now replace clauses but only if within bound
             */
            old_db = db;
            db = zdd_diff(db, env);
            //db = zdd_or(db, reduced);
            db = zdd_clause_union(db, reduced);

            zdd_refs_pop(2); // lits, env

            // Find all unit clauses in the clause database
            units = zdd_clause_units(db);
            if (units == zdd_false) {
                Abort("The empty clause has been found!\n");
            }

            /**
             * Propagate units
             */
            if (units != zdd_true) {
                // Repeat until no more new units are found
                while (units != zdd_true) {
                    if (verbose) INFO("Found %zu new units!\n", zdd_set_count(units));
                    // Add the new units to all_units and check for contradictions
                    all_units = zdd_set_union(all_units, units);
                    if (zdd_clause_units_contradict(all_units)) {
                        Abort("Units contradict! Aborting. (UNSAT)\n");
                    }
                    // Then perform unit propagation of the new units
                    db = zdd_clause_up(db, units);
                    // See if there are new units now
                    units = zdd_clause_units(db);
                    if (units == zdd_false) {
                        Abort("The empty clause has been found! Aborting. (UNSAT)\n");
                    }
                }
            }

            /**
             * Make Quine Free
             */

            double new_db_clauses = 0;
            size_t new_db_nodes = 0;
            int did_qmc = 0;

            if (qmc != -1 && (last_qmc_size*qmc < db_nodes)) {
                ZDD older_db = db;
                db = zdd_clause_qmc(db);
                new_db_clauses = zdd_satcount(db);
                new_db_nodes = zdd_nodecount(&db, 1);

                if (db_nodes < new_db_nodes ){
                    if (verbose) INFO("Skip QMC-style resolution (%.0f to %.0f clauses, %zd to %zd nodes).\n", db_clauses, new_db_clauses, db_nodes, new_db_nodes);
                    db = older_db;
                } else if (db_clauses != new_db_clauses || db_nodes != new_db_nodes) {
                    if (verbose) INFO("After QMC-style resolution: from %.0f to %.0f clauses (%zd to %zd nodes)\n", db_clauses, new_db_clauses, db_nodes, new_db_nodes);
                    did_qmc = 1;
                } else {
                    if (verbose) INFO("Skip QMC-style resolution (no change).\n");
                    db = older_db;
                }

                last_qmc_size = db_nodes;
            }

            double old_db_clauses = db_clauses;
            size_t old_db_nodes = db_nodes;
            db_clauses = zdd_satcount(db);
            db_nodes = zdd_nodecount(&db, 1);

            /**
             * Test bound
             */
            if (clausebound && (db_clauses > old_db_clauses && (int)(db_clauses - old_db_clauses) > bound)) {
                db = old_db;
                if (verbose) INFO("Skipped (bound=%d) % 4d of % 4d from %.0f to %.0f clauses (%zd to %zd nodes)\n", bound, i, nvars, old_db_clauses, db_clauses, old_db_nodes, db_nodes);
                if (minbound == -1 || minbound > (int)(db_clauses-old_db_clauses)) {
                    minbound = db_clauses-old_db_clauses;
                    mini = i;
                }
                old_db = zdd_false; // deref
                db_clauses = old_db_clauses;
                db_nodes = old_db_nodes;
                continue;
            } else if (nodebound && (db_nodes > old_db_nodes && (int)(db_nodes - old_db_nodes) > bound)) {
                db = old_db;
                if (verbose) INFO("Skipped (bound=%d) % 4d of % 4d from %.0f to %.0f clauses (%zd to %zd nodes)\n", bound, i, nvars, old_db_clauses, db_clauses, old_db_nodes, db_nodes);
                if (minbound == -1 || minbound > (int)(db_nodes-old_db_nodes)) {
                    minbound = db_nodes-old_db_nodes;
                    mini = i;
                }
                old_db = zdd_false; // deref
                db_clauses = old_db_clauses;
                db_nodes = old_db_nodes;
                continue;
            } else {
                if (tracefile != NULL) fprintf(tracefile, "e %d\n", i);
                if (did_qmc) {
                    if (tracefile != NULL) fprintf(tracefile, "q\n");
                    INFO("\033[1;32mEliminated\033[m var %d (%d/%d) from %g to %g to %g clauses (%zd to %zd to %zd nodes)\n", i, elim_total+1, nvars, old_db_clauses, new_db_clauses, db_clauses, old_db_nodes, new_db_nodes, db_nodes);
                } else {
                    INFO("\033[1;32mEliminated\033[m var %d (%d/%d) from %g to %g clauses (%zd to %zd nodes)\n", i, elim_total+1, nvars, old_db_clauses, db_clauses, old_db_nodes, db_nodes);
                }
                old_db = zdd_false; // deref
                elim++;
                elim_total++;
            }

            if (verbose) print_memory_usage();

            /**
             * Check death condition
             */
            // if (orig_db_clauses * death_threshold < db_clauses) Abort("Reached %zu > %d*%zu (total elim=%d)\n", db_clauses, death_threshold, orig_db_clauses, elim_total);
            (void)death_threshold;

            if (1) break;
        }

        if (db == zdd_true) {
            INFO("Empty clause!\n");
            break;
        }

        assert(elim != 0 || minbound != -1);

        if (!elim) {
            bound = minbound;
            i = mini;
        } else {
            bound = 0;
            i = 1;
        }

        assert(bound >= 0);
    }

    // Report Sylvan statistics (if SYLVAN_STATS is set)
    if (verbose) sylvan_stats_report(stdout);

    zdd_refs_popptr(5);

    (void)orig_db_nodes;
    (void)orig_db_clauses;

    return 0;
}
