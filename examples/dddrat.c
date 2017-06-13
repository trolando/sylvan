#include <argp.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <sylvan_int.h>

/* Configuration */
static int workers = 0; // autodetect
static int verbose = 0;
static char* cnf_filename = NULL; // filename of CNF
static char* drat_filename = NULL; // filename of DRAT

/* argp configuration */
static struct argp_option options[] =
{
    {"workers", 'w', "<workers>", 0, "Number of workers (default=0: autodetect)", 0},
    {"verbose", 'v', 0, 0, "Set verbose", 0},
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
    case ARGP_KEY_ARG:
        if (state->arg_num == 0) cnf_filename = arg;
        if (state->arg_num == 1) drat_filename = arg;
        if (state->arg_num >= 2) argp_usage(state);
        break; 
    case ARGP_KEY_END:
        if (state->arg_num < 2) argp_usage(state);
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = { options, parse_opt, "<cnf_file> <drat_file>", 0, 0, 0, 0 };

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
    zdd_protect(&db);

    /* parse clauses */
    int read_clauses = 0;
    int last_literal = 0;
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
        }
        /* set last literal and continue loop */
        last_literal = lit;
    }

    // printf("c total number of literals: %d\n", nlits);
    free(lits);
    zdd_unprotect(&db);

    return db;
}

/**
 * Check if the empty clause is in the set.
 */
int
test_empty_clause(ZDD set)
{
    if (set == zdd_true) return 1;
    if (set & zdd_complement) return 0;
    if (set == zdd_false) return 1;
    if (test_empty_clause(zdd_gethigh(set)) == 0) return 0;
    return test_empty_clause(zdd_getlow(set));
}

#define test_rup(db, units, clause, clause_len) CALL(test_rup, db, units, clause, clause_len)
TASK_4(int, test_rup, ZDD*, db, ZDD*, units, int*, clause, int, clause_len)
{
    /**
     * Add all literals in the clause NEGATED as units
     * (reverse order because more efficient)
     */
    ZDD clause_units = zdd_true;
    for (int i=0; i<clause_len; i++) {
        int unit = -clause[clause_len-i-1];
        uint32_t unitvar = unit < 0 ? (-2*unit) : (2*unit + 1);
        zdd_refs_push(clause_units);
        clause_units = zdd_set_add(clause_units, unitvar);
        zdd_refs_pop(1);
    }

    /**
     * Now propagate units until contradiction or done
     */
    ZDD db_check = *db;
    ZDD all_units = *units;
    ZDD new_units = clause_units;
    while (new_units != zdd_true) {
        zdd_refs_push(db_check);
        zdd_refs_push(new_units);
        zdd_refs_push(all_units);
        all_units = zdd_set_union(all_units, new_units);
        zdd_refs_pop(1);
        zdd_refs_push(all_units);
        if (zdd_clause_units_contradict(all_units)) {
            zdd_refs_pop(3);
            return 1; // found contradiction
        }
        db_check = zdd_clause_up(db_check, new_units);
        zdd_refs_push(db_check);
        new_units = zdd_clause_units(db_check);
        zdd_refs_pop(4);
        if (new_units == zdd_false) return 1; // found empty clause
    }

    /**
     * Not RUP, set *db and *units for further processing (RAT checking)
     * ... and return 0
     */
    *db = db_check;
    *units = all_units;
    return 0;
}

#define test_multirup(db, units, clauses, pivot) CALL(test_multirup, db, units, clauses, pivot)
TASK_4(int, test_multirup, ZDD, db, ZDD, units, ZDD, clauses, int, pivot)
{
    if (0) {
        int rat = 1;
        int32_t arr[nvars+1];
        ZDD res = zdd_clause_enum_first(clauses, arr);
        while (res != zdd_false) {
            int len = 0;
            for (int i=0; i<nvars; i++) {
                if (arr[i] == 0) break;
                else len++;
            }
            int32_t lits[len];
            // remove -pivot
            int j=0;
            for (int i=0; i<len; i++) {
                if (arr[i] != -pivot) lits[j++] = arr[i];
            }
            lits[j] = 0;
            len = j; // (should be len = len - 1)
            // sort
            qsort(lits, len, sizeof(int32_t), literal_compare);
            if (!test_rup(&db, &units, lits, len)) {
                rat = 0;
                break;
            }
            res = zdd_clause_enum_next(clauses, arr);
        }
        return rat;
    }

    if (clauses == zdd_true) return 1; // empty clause
    if (clauses == zdd_false) return 1; // empty set

    // in parallel test others...
    zddnode_t clauses_node = ZDD_GETNODE(clauses);
    SPAWN(test_multirup, db, units, zddnode_low(clauses, clauses_node), pivot);

    uint32_t var = zddnode_getvariable(clauses_node);
    ZDD high = zddnode_high(clauses, clauses_node);
    int lit = var & 1 ? var/2 : -1*(var/2);
    int res = 0;
    if (lit == pivot) {
        // skip pivot
        res = test_multirup(db, units, high, pivot);
    } else {
        ZDD new_units = zdd_set_add(zdd_true, var^1);
        while (new_units != zdd_true) {
            zdd_refs_push(db);
            zdd_refs_push(new_units);
            zdd_refs_push(units);
            units = zdd_set_union(units, new_units);
            zdd_refs_pop(1);
            zdd_refs_push(units);
            if (zdd_clause_units_contradict(units)) {
                zdd_refs_pop(3);
                res = 1;
                break;
            }
            db = zdd_clause_up(db, new_units);
            zdd_refs_push(db);
            new_units = zdd_clause_units(db);
            zdd_refs_pop(4);
            if (new_units == zdd_false) {
                res = 1;
                break;
            }
        }     
        if (res == 0) {
            res = test_multirup(db, units, high, pivot);
        }
    }
    
    int otherres = SYNC(test_multirup);
    return (res && otherres) ? 1 : 0;
}

TASK_2(int, parse_drat_file, FILE*, file, ZDD, db)
{
    // TODO: protect db
    zdd_protect(&db);

    /* setup arrays */
    int lits_size = 64, lits_count = 0;
    int *lits = (int*)malloc(sizeof(int[lits_size]));
    if (lits == NULL) Abort("out of memory");

    /* parse clauses */
    int last_literal = 0;
    int deleting = 0;

    while (1) {
        int ch = fgetc(file);
        /* skip whitespace */
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
        /* check deletion */
        if (ch == 'd') {
            deleting = 1;
            continue;
        }
        /* if EOF, check if good */
        if (ch == EOF) {
            if (last_literal != 0) Abort("zero missing");
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
            int pivot = lits[0];
            /* sort the literals by variable */
            qsort(lits, lits_count, sizeof(int), literal_compare);
            /* check for tautology or same variables */
            for (int i=0; i<lits_count-1; i++) {
                if (lits[i] == lits[i+1] || lits[i] == -lits[i+1]) Abort("variables twice in clause");
            }
            /* end lits with 0 */
            lits[lits_count] = 0;
            /* if deleting, delete and continue */
            if (deleting) {
                // TODO zdd_remove_clause
                ZDD cl = zdd_clause(lits);
                db = zdd_diff(db, cl);
                deleting = 0;
            } else {
                // TODO if verbose
                if (verbose) {
                    INFO("checking lemma");
                    for (int i=0; i<lits_count; i++) {
                        printf(" %d", lits[i]);
                    }
                    printf("\n");
                }
                // check if RUP: add every literal (negated) as unit and propagate
                ZDD units = zdd_true;
                ZDD _db = db;
                if (test_rup(&_db, &units, lits, lits_count)) {
                    // INFO("Found RUP");
                } else {
                    // INFO("Found not RUP -- checking RAT with pivot %d", pivot);
                    /**
                     * Get all clauses with -pivot, then run RUP check on all (excluding -pivot)
                     */
                    ZDD neg_pivot = zdd_set_add(zdd_true, pivot < 0 ? -2*pivot+1 : 2*pivot);
                    ZDD db_env = zdd_clause_environment(db, neg_pivot);
                    // if (!test_empty_clause(db_env)) printf("oi\n");
                    if (!test_multirup(_db, units, db_env, pivot)) {
                        INFO("RAT check failed!\n");
                        return 0;
                    }
                }
                // add to database
                /* add clause to database */
                db = zdd_add_clause(db, lits);
            }
            /* reset literal count */
            lits_count = 0;
        }
        /* set last literal and continue loop */
        last_literal = lit;
    }

    free(lits);
    zdd_unprotect(&db);

    return 1;
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
        if (f == NULL) Abort("Cannot open file %s!", cnf_filename);

        INFO("Opened %s.\n", cnf_filename);

        db = CALL(parse_cnf_file, f);

        // Close file
        fclose(f);
    } else {
        INFO("Reading from stdin.\n");
        db = CALL(parse_cnf_file, stdin);
    }

    INFO("Read %s, %d variables, %d clauses, %d literals.\n", cnf_filename, nvars, nclauses, nlits);

    return db;
}


int
main(int argc, char **argv)
{
    t_start = wctime();
    argp_parse(&argp, argc, argv, 0, 0, 0);

    // Init Lace
    lace_init(workers, 1000000); // auto-detect number of workers, use a 1,000,000 size task queue
    lace_startup(0, NULL, NULL); // auto-detect program stack, do not use a callback for startup

    LACE_ME;

    // Init Sylvan
    // Give 2 GB memory
    sylvan_set_limits(2LL*1LL<<30, 0, 0);
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
    zdd_protect(&db);

    // Store the original set of clauses somewhere
    ZDD original_db = db;
    zdd_protect(&original_db);

    // Report the number of clauses and nodes
    {
        double n_clauses = zdd_satcount(db);
        size_t n_nodes = zdd_nodecount(&db, 1);
        INFO("After loading CNF: %.0lf clauses using %zu nodes.\n", n_clauses, n_nodes);
    }

    // Perform self subsumption on clause database
    db = zdd_clause_self_subsume(db);

    // Report the number of clauses and nodes
    {
        double n_clauses = zdd_satcount(db);
        size_t n_nodes = zdd_nodecount(&db, 1);
        INFO("After self-subsumption: %.0lf clauses using %zu nodes.\n", n_clauses, n_nodes);
    }

    // Perform DRAT check
    FILE *f = fopen(drat_filename, "r");
    if (f == NULL) Abort("Cannot open file %s!", drat_filename);

    INFO("Opened %s.\n", drat_filename);

    int success = CALL(parse_drat_file, f, db);

    // Close file
    fclose(f);
    
    if (success) {
        INFO("DRAT check good\n");
    } else {
        INFO("DRAT check bad\n");
    }

    // Report Sylvan statistics (if SYLVAN_STATS is set)
    if (verbose) sylvan_stats_report(stdout);

    return 0;
}
