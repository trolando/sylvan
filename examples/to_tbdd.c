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
#include <sylvan_int.h>

/* Configuration */
static int workers = 0; // autodetect
static char* model_filename = NULL; // filename of model

/* argp configuration */
static struct argp_option options[] =
{
    {"workers", 'w', "<workers>", 0, "Number of workers (default=0: autodetect)", 0},
    {0, 0, 0, 0, 0, 0}
};
static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case 'w':
        workers = atoi(arg);
        break;
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

VOID_TASK_0(gc_start)
{
    INFO("(GC) Starting garbage collection...\n");
}

VOID_TASK_0(gc_end)
{
    INFO("(GC) Garbage collection done.\n");
}

int
main(int argc, char **argv)
{
    argp_parse(&argp, argc, argv, 0, 0, 0);
    setlocale(LC_NUMERIC, "en_US.utf-8");
    t_start = wctime();

    FILE *f = fopen(model_filename, "r");
    if (f == NULL) {
        fprintf(stderr, "Cannot open file '%s'!\n", model_filename);
        return -1;
    }

    // Init Lace
    lace_init(workers, 1000000); // auto-detect number of workers, use a 1,000,000 size task queue
    lace_startup(0, NULL, NULL); // auto-detect program stack, do not use a callback for startup

    LACE_ME;

    // Init Sylvan
    // Nodes table size: 24 bytes * 2**N_nodes
    // Cache table size: 36 bytes * 2**N_cache
    // With: N_nodes=25, N_cache=24: 1.3 GB memory
    sylvan_init_package(1LL<<21, 1LL<<27, 1LL<<20, 1LL<<26);
    sylvan_init_bdd();
    sylvan_init_tbdd();
    sylvan_gc_hook_pregc(TASK(gc_start));
    sylvan_gc_hook_postgc(TASK(gc_end));

    /* Load domain information */
    if ((fread(&vector_size, sizeof(int), 1, f) != 1) ||
        (fread(&statebits, sizeof(int), 1, f) != 1) ||
        (fread(&actionbits, sizeof(int), 1, f) != 1)) {
        Abort("Invalid input file!\n");
    }

    bits_per_integer = statebits;
    statebits *= vector_size;

    // Read initial state
    set_t states = set_load(f);

    // Read transitions
    if (fread(&next_count, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    next = (rel_t*)malloc(sizeof(rel_t) * next_count);

    int i;
    for (i=0; i<next_count; i++) {
        next[i] = rel_load(f);
    }

    /* Done */
    fclose(f);

    // Report statistics
    INFO("Read file '%s'\n", model_filename);
    INFO("%d integers per state, %d bits per integer, %d transition groups\n", vector_size, bits_per_integer, next_count);

    INFO("BDD nodes:\n");
    INFO("Initial states: %zu BDD nodes\n", sylvan_nodecount(states->bdd));
    for (i=0; i<next_count; i++) {
        INFO("Transition %d: %zu BDD nodes\n", i, sylvan_nodecount(next[i]->bdd));
    }

    /* compute total node counts */
    BDD all_dd[1+next_count];
    all_dd[0] = states->bdd;
    for (int i=0; i<next_count; i++) all_dd[1+i] = next[i]->bdd;
    INFO("All DDs: %'zu nodes\n", mtbdd_nodecount_more(all_dd, 1+next_count));

    /* add action bits to variables */
    BDD action_variables = mtbdd_true;
    sylvan_protect(&action_variables);
    for (int i=0; i<actionbits; i++) {
        action_variables = mtbdd_makenode(1000000+(actionbits-i-1), mtbdd_false, action_variables);
    }

    /* dump every transition to .dot */
    for (int i=0; i<next_count; i++) {
        char filename[64];
        sprintf(filename, "trans-%03d.dot", i);
        FILE *f = fopen(filename, "w");
        mtbdd_fprintdot(f, next[i]->bdd);
        fclose(f);
    }

    /* convert to TBDD */
    states->bdd = tbdd_from_mtbdd(states->bdd, states->variables);
    for (i=0; i<next_count; i++) {
        MTBDD domain = sylvan_and(action_variables, next[i]->variables);
        next[i]->bdd = tbdd_from_mtbdd(next[i]->bdd, domain);
    }

    /* dump every transition to .dot */
    for (int i=0; i<next_count; i++) {
        char filename[64];
        sprintf(filename, "trans-%03d-tbdd.dot", i);
        FILE *f = fopen(filename, "w");
        tbdd_fprintdot(f, next[i]->bdd);
        fclose(f);
    }

    INFO("TBDD nodes:\n");
    INFO("Initial states: %zu TBDD nodes\n", tbdd_nodecount(states->bdd));
    for (i=0; i<next_count; i++) {
        INFO("Transition %d: %zu TBDD nodes\n", i, tbdd_nodecount(next[i]->bdd));
    }

    /* compute total node counts */
    all_dd[0] = states->bdd;
    for (int i=0; i<next_count; i++) all_dd[1+i] = next[i]->bdd;
    INFO("All DDs: %'zu nodes\n", tbdd_nodecount_more(all_dd, 1+next_count));

    sylvan_stats_report(stdout);

    return 0;
}
