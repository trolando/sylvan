/**
 * SCC
 * Symbolic scc detection
 * Based on the Lockstep algorithm by Bloem, Gabow, Somenzi 2006
 *
 * TODO: perhaps use saturation algorithm to obtain the F and B sets??
 */

#include <argp.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifdef HAVE_PROFILER
#include <gperftools/profiler.h>
#endif

#include <sylvan.h>
#include <sylvan_common.h>
#include <llmsset.h>

/* Configuration */
// static int report_levels = 0; // report states at end of every level
// static int report_table = 0; // report table size at end of every level
static int report_nodes = 0; // report number of nodes of BDDs
// static int strategy = 1; // set to 1 = use PAR strategy; set to 0 = use BFS strategy
// static int check_deadlocks = 0; // set to 1 to check for deadlocks
static int merge_relations = 0; // merge relations to 1 relation
static int print_transition_matrix = 0; // print transition relation matrix
static int workers = 0; // autodetect
static char* model_filename = NULL; // filename of model
#ifdef HAVE_PROFILER
static char* profile_filename = NULL; // filename for profiling
#endif

/* argp configuration */
static struct argp_option options[] =
{
    {"workers", 'w', "<workers>", 0, "Number of workers (default=0: autodetect)", 0},
//    {"strategy", 's', "<bfs|par|sat>", 0, "Strategy for eeachability (default=par)", 0},
#ifdef HAVE_PROFILER
    {"profiler", 'p', "<filename>", 0, "Filename for profiling", 0},
#endif
//    {"deadlocks", 3, 0, 0, "Check for deadlocks", 1},
//    {"count-nodes", 5, 0, 0, "Report #nodes for BDDs", 1},
//    {"count-states", 1, 0, 0, "Report #states at each level", 1},
//    {"count-table", 2, 0, 0, "Report table usage at each level", 1},
    {"merge-relations", 6, 0, 0, "Merge transition relations into one transition relation", 1},
    {"print-matrix", 4, 0, 0, "Print transition matrix", 1},
    {0, 0, 0, 0, 0, 0}
};
static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case 'w':
        workers = atoi(arg);
        break;
/*    case 's':
        if (strcmp(arg, "bfs")==0) strategy = 0;
        else if (strcmp(arg, "par")==0) strategy = 1;
        else if (strcmp(arg, "sat")==0) strategy = 2;
        else argp_usage(state);
        break;
*/    case 4:
        print_transition_matrix = 1;
        break;
/*    case 3:
        check_deadlocks = 1;
        break;
    case 1:
        report_levels = 1;
        break;
    case 2:
        report_table = 1;
        break;
*/    case 6:
        merge_relations = 1;
        break;
#ifdef HAVE_PROFILER
    case 'p':
        profile_filename = arg;
        break;
#endif
    case ARGP_KEY_ARG:
        if (state->arg_num >= 1) argp_usage(state);
        model_filename = arg;
        break;
    case ARGP_KEY_END:
        if (state->arg_num < 1) argp_usage(state);
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}
static struct argp argp = { options, parse_opt, "<model>", 0, 0, 0, 0 };

/* Globals */
typedef struct set
{
    BDD bdd;
    BDD variables; // all variables in the set (used by satcount)
} *set_t;

typedef struct relation
{
    BDD bdd;
    BDD variables; // all variables in the relation (used by relprod)
} *rel_t;

static int vector_size; // size of vector
static int statebits, actionbits; // number of bits for state, number of bits for action
static int bits_per_integer; // number of bits per integer in the vector
static int next_count; // number of partitions of the transition relation
static BDD state_variables, prime_variables, action_variables;
static set_t initial, reachable; // initial and reachable states
static rel_t *next; // each partition of the transition relation

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

/* Load a set from file */
#define set_load(f) CALL(set_load, f)
TASK_1(set_t, set_load, FILE*, f)
{
    sylvan_serialize_fromfile(f);

    size_t set_bdd, set_vector_size, set_state_vars;
    if ((fread(&set_bdd, sizeof(size_t), 1, f) != 1) ||
        (fread(&set_vector_size, sizeof(size_t), 1, f) != 1) ||
        (fread(&set_state_vars, sizeof(size_t), 1, f) != 1)) {
        Abort("Invalid input file!\n");
    }

    set_t set = (set_t)malloc(sizeof(struct set));
    set->bdd = sylvan_serialize_get_reversed(set_bdd);
    set->variables = sylvan_support(sylvan_serialize_get_reversed(set_state_vars));

    sylvan_protect(&set->bdd);
    sylvan_protect(&set->variables);

    return set;
}

/* Load a relation from file */
#define rel_load(f) CALL(rel_load, f)
TASK_1(rel_t, rel_load, FILE*, f)
{
    sylvan_serialize_fromfile(f);

    size_t rel_bdd, rel_vars;
    if ((fread(&rel_bdd, sizeof(size_t), 1, f) != 1) ||
        (fread(&rel_vars, sizeof(size_t), 1, f) != 1)) {
        Abort("Invalid input file!\n");
    }

    rel_t rel = (rel_t)malloc(sizeof(struct relation));
    rel->bdd = sylvan_serialize_get_reversed(rel_bdd);
    rel->variables = sylvan_support(sylvan_serialize_get_reversed(rel_vars));

    sylvan_protect(&rel->bdd);
    sylvan_protect(&rel->variables);

    return rel;
}

#define print_example(example, variables) CALL(print_example, example, variables)
VOID_TASK_2(print_example, BDD, example, BDDSET, variables)
{
    uint8_t str[vector_size * bits_per_integer];

    if (example != sylvan_false) {
        sylvan_sat_one(example, variables, str);
        printf("[");
        for (int i=0; i<vector_size; i++) {
            uint32_t res = 0;
            for (int j=0; j<bits_per_integer; j++) {
                if (str[bits_per_integer*i+j] == 1) res++;
                res<<=1;
            }
            if (i>0) printf(",");
            printf("%" PRIu32, res);
        }
        printf("]");
    }
}

/**
 * Extend a transition relation to a larger domain (using s=s')
 */
#define extend_relation(rel, vars) CALL(extend_relation, rel, vars)
TASK_2(BDD, extend_relation, BDD, relation, BDDSET, variables)
{
    /* first determine which state BDD variables are in rel */
    int has[statebits];
    for (int i=0; i<statebits; i++) has[i] = 0;
    BDDSET s = variables;
    while (!sylvan_set_isempty(s)) {
        BDDVAR v = sylvan_set_var(s);
        if (v/2 >= (unsigned)statebits) break; // action labels
        has[v/2] = 1;
        s = sylvan_set_next(s);
    }

    /* create "s=s'" for all variables not in rel */
    BDD eq = sylvan_true;
    for (int i=statebits-1; i>=0; i--) {
        if (has[i]) continue;
        BDD low = sylvan_makenode(2*i+1, eq, sylvan_false);
        bdd_refs_push(low);
        BDD high = sylvan_makenode(2*i+1, sylvan_false, eq);
        bdd_refs_pop(1);
        eq = sylvan_makenode(2*i, low, high);
    }

    bdd_refs_push(eq);
    BDD result = sylvan_and(relation, eq);
    bdd_refs_pop(1);

    return result;
}

/**
 * Compute \BigUnion ( sets[i] )
 * This is used to merge extended transition relations
 */
#define big_union(first, count) CALL(big_union, first, count)
TASK_2(BDD, big_union, int, first, int, count)
{
    if (count == 1) return next[first]->bdd;

    bdd_refs_spawn(SPAWN(big_union, first, count/2));
    BDD right = bdd_refs_push(CALL(big_union, first+count/2, count-count/2));
    BDD left = bdd_refs_push(bdd_refs_sync(SYNC(big_union)));
    BDD result = sylvan_or(left, right);
    bdd_refs_pop(2);
    return result;
}

static void
print_matrix(BDD vars)
{
    for (int i=0; i<vector_size; i++) {
        if (sylvan_set_isempty(vars)) {
            fprintf(stdout, "-");
        } else {
            BDDVAR next_s = 2*((i+1)*bits_per_integer);
            if (sylvan_set_var(vars) < next_s) {
                fprintf(stdout, "+");
                for (;;) {
                    vars = sylvan_set_next(vars);
                    if (sylvan_set_isempty(vars)) break;
                    if (sylvan_set_var(vars) >= next_s) break;
                }
            } else {
                fprintf(stdout, "-");
            }
        }
    }
}

#define load_model() CALL(load_model)
VOID_TASK_0(load_model)
{
    FILE *f = fopen(model_filename, "r");
    if (f == NULL) {
        Abort("Cannot open file '%s'!\n", model_filename);
    }

    /* Load domain information */
    if ((fread(&vector_size, sizeof(int), 1, f) != 1) ||
        (fread(&statebits, sizeof(int), 1, f) != 1) ||
        (fread(&actionbits, sizeof(int), 1, f) != 1)) {
        Abort("Invalid input file!\n");
    }

    bits_per_integer = statebits;
    statebits *= vector_size;

    // Create state_variables, prime_variables, action_variables
    sylvan_protect(&state_variables);
    sylvan_protect(&prime_variables);
    sylvan_protect(&action_variables);

    state_variables = sylvan_set_empty();
    prime_variables = sylvan_set_empty();
    action_variables = sylvan_set_empty();

    for (int i=statebits-1; i>=0; i--) {
        state_variables = sylvan_set_add(state_variables, i*2);
        prime_variables = sylvan_set_add(prime_variables, i*2+1);
    }

    for (int i=actionbits-1; i>=0; i--) {
        action_variables = sylvan_set_add(action_variables, 1000000+i);
    }

    // Read initial state
    initial = set_load(f);

    // Read transitions
    if (fread(&next_count, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    next = (rel_t*)malloc(sizeof(rel_t) * next_count);

    int i;
    for (i=0; i<next_count; i++) {
        next[i] = rel_load(f);
    }

    // We also need the reachable state space
    int save_reachable;
    if (fread(&save_reachable, sizeof(int), 1, f) != 1) Abort("Invalid input file (old version)!\n");
    if (save_reachable == 0) Abort("Input file does not contain reachable states!\n");
    reachable = set_load(f);

    /* Done */
    fclose(f);

    // Report statistics
    INFO("Read file '%s'\n", model_filename);
    INFO("%d integers per state, %d bits per integer, %d transition groups\n", vector_size, bits_per_integer, next_count);
}

#define merge_transition_relations() CALL(merge_transition_relations)
VOID_TASK_0(merge_transition_relations)
{
    INFO("Extending transition relations to full domain.\n");
    for (int i=0; i<next_count; i++) {
        next[i]->bdd = extend_relation(next[i]->bdd, next[i]->variables);
        next[i]->variables = prime_variables;
    }

    INFO("Taking union of all transition relations.\n");
    next[0]->bdd = big_union(0, next_count);
    next_count = 1;
}

VOID_TASK_0(gc_start)
{
    INFO("(GC) Starting garbage collection...\n");
}

VOID_TASK_0(gc_end)
{
    INFO("(GC) Garbage collection done.\n");
}

static uint64_t three_and_opid;

#define three_and(a, b, c) CALL(three_and, a, b, c)
TASK_3(BDD, three_and, BDD, a, BDD, b, BDD, c)
{
    if (a == sylvan_false || b == sylvan_false || c == sylvan_false) return sylvan_false;

    if (a == sylvan_true) return sylvan_and(b, c);
    if (b == sylvan_true) return sylvan_and(a, c);
    if (c == sylvan_true) return sylvan_and(a, b);

    BDD result;
    if (cache_get(a|three_and_opid, b, c, &result)) return result;

    sylvan_gc_test();

    BDDVAR a_var = sylvan_var(a);
    BDDVAR b_var = sylvan_var(b);
    BDDVAR c_var = sylvan_var(c);

    BDD var = a_var;
    if (var > b_var) var = b_var;
    if (var > c_var) var = c_var;

    BDD a_low, a_high;
    if (var == a_var) {
        a_low = sylvan_low(a);
        a_high = sylvan_high(a);
    } else {
        a_low = a_high = a;
    }

    BDD b_low, b_high;
    if (var == b_var) {
        b_low = sylvan_low(b);
        b_high = sylvan_high(b);
    } else {
        b_low = b_high = b;
    }

    BDD c_low, c_high;
    if (var == c_var) {
        c_low = sylvan_low(c);
        c_high = sylvan_high(c);
    } else {
        c_low = c_high = c;
    }

    bdd_refs_spawn(SPAWN(three_and, a_low, b_low, c_low));
    BDD high = bdd_refs_push(CALL(three_and, a_high, b_high, c_high));
    BDD low = bdd_refs_sync(SYNC(three_and));
    result = sylvan_makenode(var, low, high);
    bdd_refs_pop(1);

    cache_put(a|three_and_opid, b, c, result);
    return result;
}

/**
 * Compute image in parallel for the given transition relations
 */
TASK_4(BDD, parnext, BDD, cur, BDD, visited, size_t, from, size_t, len)
{
    if (len == 1) {
        // compute image of cur
        return sylvan_relnext(cur, next[from]->bdd, next[from]->variables);
    } else {
        // Recursively calculate left+right
        bdd_refs_spawn(SPAWN(parnext, cur, visited, from, len/2));
        BDD right = bdd_refs_push(CALL(parnext, cur, visited, from+len/2, len-len/2));
        BDD left = bdd_refs_push(bdd_refs_sync(SYNC(parnext)));

        // Merge results of left+right
        BDD result = sylvan_or(left, right);
        bdd_refs_pop(2);

        return result;
    }
}

/**
 * Compute preimage in parallel for the given transition relations
 */
TASK_4(BDD, parprev, BDD, cur, BDD, visited, size_t, from, size_t, len)
{
    if (len == 1) {
        // compute pre-image of cur
        return sylvan_relprev(next[from]->bdd, cur, next[from]->variables);
    } else {
        // Recursively calculate left+right
        bdd_refs_spawn(SPAWN(parprev, cur, visited, from, len/2));
        BDD right = bdd_refs_push(CALL(parprev, cur, visited, from+len/2, len-len/2));
        BDD left = bdd_refs_push(bdd_refs_sync(SYNC(parprev)));

        // Merge results of left+right
        BDD result = sylvan_or(left, right);
        bdd_refs_pop(2);

        return result;
    }
}

size_t scc_count = 0;

#define report(scc) CALL(report, scc)
VOID_TASK_1(report, BDD, scc)
{
    // do nothing for now
    // INFO("Reporting SCC containing %.0f states\n", sylvan_satcount(scc, state_variables));
    size_t current = __sync_add_and_fetch(&scc_count, 1);
    if (current % 1000 == 0) INFO("Number of SCCs: %zu\n", scc_count);
    (void)scc;
}

VOID_TASK_3(helper_next, BDD*, f, BDD*, ffront, BDD, states)
{
    *ffront = CALL(parnext, *ffront, *f, 0, next_count);
    *ffront = three_and(*ffront, sylvan_not(*f), states);
    *f = sylvan_or(*f, *ffront);
}

VOID_TASK_3(helper_prev, BDD*, b, BDD*, bfront, BDD, states)
{
    *bfront = CALL(parprev, *bfront, *b, 0, next_count);
    *bfront = three_and(*bfront, sylvan_not(*b), states);
    *b = sylvan_or(*b, *bfront);
}

TASK_1(BDD, scc, BDD, states)
{
    // INFO("enter scc detection for state space size %.0f\n", sylvan_satcount(states, state_variables));

    // Compute all SCCs in the set "states"
    if (states == sylvan_false) return sylvan_false;

    // Pick a pivot state (random)
    BDD v = sylvan_pick_single_cube(states, state_variables);
    bdd_refs_push(v);

    // Sanity check (did we get exactly 1 pivot state?)
    assert(sylvan_satcount(v, state_variables)==1.0);

    // INFO("pivot picked\n");
    // print_example(v, state_variables);

    // Initialize for first lockstep search
    BDD f=v, ffront=v, b=v, bfront=v;

    sylvan_protect(&f);
    sylvan_protect(&b);
    sylvan_protect(&ffront);
    sylvan_protect(&bfront);

    // int count = 1;
    while (ffront != sylvan_false && bfront != sylvan_false) {
        // perform image and preimage computations in parallel
        // INFO("Lockstep phase 1: %d\n", count++);
        SPAWN(helper_next, &f, &ffront, states);
        CALL(helper_prev, &b, &bfront, states);
        SYNC(helper_next);
        // INFO("f has %.0f states\n", sylvan_satcount(f, state_variables));
        // INFO("ffront has %.0f states\n", sylvan_satcount(ffront, state_variables));
        // INFO("b has %.0f states\n", sylvan_satcount(b, state_variables));
        // INFO("bfront has %.0f states\n", sylvan_satcount(bfront, state_variables));
    }

    if (ffront == sylvan_false) {
        // INFO("f converged with %.0f states\n", sylvan_satcount(f, state_variables));
        // f is converged first!
        // we now converge b but avoid states not in f
        // update the set b with states not in f... so the front ignores them

        // we now continue computing backward reachability, but restricted to states in F
        b = sylvan_or(b, sylvan_not(f));
        while (bfront != sylvan_false) {
            CALL(helper_prev, &b, &bfront, states);    
        }
        // not correct btw INFO("b converged with %.0f states\n", sylvan_satcount(b, state_variables));

        // now b contains states in B and not F, states in B and F, and states in not F
        // intersecting b and f results in states in B and F
        BDD c = sylvan_and(f, b);
        report(c);

        // parallel: F minus C, states minus F
        sylvan_unprotect(&ffront);
        sylvan_unprotect(&bfront);
        b = sylvan_and(states, sylvan_not(f));
        f = sylvan_and(f, sylvan_not(c));
        SPAWN(scc, f);
        CALL(scc, b);
        SYNC(scc);
        sylvan_unprotect(&f);
        sylvan_unprotect(&b);
    } else {
        // INFO("b converged with %.0f states\n", sylvan_satcount(b, state_variables));
        // b is converged first!
        // we now converge f but avoid states not in b
        // update the set f with states not in b... so the front ignores them

        // we now continue computing forwards reachability, but restricted to states in B
        f = sylvan_or(f, sylvan_not(b));
        while (ffront != sylvan_false) {
            // INFO("Lockstep phase 2: %d\n", count++);
            CALL(helper_next, &f, &ffront, states);    
        }
        // INFO("f converged with %.0f states\n", sylvan_satcount(f, state_variables));

        BDD c = sylvan_and(f, b);
        report(c);

        // parallel: B minus C, states minus B
        sylvan_unprotect(&ffront);
        sylvan_unprotect(&bfront);
        f = sylvan_and(states, sylvan_not(b));
        b = sylvan_and(b, sylvan_not(c));
        SPAWN(scc, f);
        CALL(scc, b);
        SYNC(scc);
        sylvan_unprotect(&f);
        sylvan_unprotect(&b);
    }

    return sylvan_invalid; // not yet implemented
}

int
main(int argc, char **argv)
{
    argp_parse(&argp, argc, argv, 0, 0, 0);
    setlocale(LC_NUMERIC, "en_US.utf-8");

    t_start = wctime();

    // Initialize Lace with default values
    lace_init(workers, 0);
    lace_startup(0, NULL, NULL);
    LACE_ME;

    // Initialize Sylvan
    // Nodes table size: 24 bytes * 2**N_nodes
    // Cache table size: 36 bytes * 2**N_cache
    // With: N_nodes=25, N_cache=24: 1.3 GB memory
    // With: N_nodes=26, N_cache=25: 2.6 GB memory
    // With: N_nodes=27, N_cache=26: 5.2 GB memory
    sylvan_init_package(1LL<<23, 1LL<<26, 1LL<<22, 1LL<<25);
    sylvan_init_bdd(6); // granularity 6 is decent default value - 1 means "use cache for every operation"
    sylvan_gc_add_mark(0, TASK(gc_start));
    sylvan_gc_add_mark(40, TASK(gc_end));
    three_and_opid = cache_next_opid();

    load_model();

    if (print_transition_matrix) {
        for (int i=0; i<next_count; i++) {
            INFO("");
            print_matrix(next[i]->variables);
            fprintf(stdout, "\n");
        }
    }

    if (merge_relations) merge_transition_relations();

    if (report_nodes) {
        INFO("BDD nodes:\n");
        INFO("Initial states: %zu BDD nodes\n", sylvan_nodecount(initial->bdd));
        INFO("Reachable states: %zu BDD nodes\n", sylvan_nodecount(reachable->bdd));
        for (int i=0; i<next_count; i++) {
            INFO("Transition %d: %zu BDD nodes\n", i, sylvan_nodecount(next[i]->bdd));
        }
    }

#ifdef HAVE_PROFILER
    if (profile_filename != NULL) ProfilerStart(profile_filename);
#endif

    double t1 = wctime();
    CALL(scc, reachable->bdd);
    double t2 = wctime();
    INFO("SCC detection time: %f\n", t2-t1);
    INFO("Discovered %zu SCCs!\n", scc_count);

#ifdef HAVE_PROFILER
    if (profile_filename != NULL) ProfilerStop();
#endif

    // Now we just have states
    // REPORT something, todo

    sylvan_stats_report(stdout, 1);

    return 0;
}
