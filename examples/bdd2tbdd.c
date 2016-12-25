#include <argp.h>
#include <assert.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <getrss.h>

#include <sylvan.h>
#include <sylvan_int.h>

/* Configuration */
static int workers = 0; // autodetect
static char* bdd_filename = NULL; // filename of input BDD
static char* tbdd_filename = NULL; // filename of output TBDD
static int verbose = 0;

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
        if (state->arg_num == 0) bdd_filename = arg;
        if (state->arg_num == 1) tbdd_filename = arg;
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
static struct argp argp = { options, parse_opt, "<infile> <outfile>", 0, 0, 0, 0 };

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
    int r_k, w_k, *r_proj, *w_proj;
} *rel_t;

static int vectorsize; // size of vector in integers
static int *statebits; // number of bits for each state integer
static int actionbits; // number of bits for action label
static int totalbits;  // total number of bits
static int next_count; // number of partitions of the transition relation
static rel_t *next;    // each partition of the transition relation

/**
 * Obtain current wallclock time
 */
static double
wctime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

static double t_start;
#define INFO(s, ...) fprintf(stdout, "[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "Abort at line %d!\n", __LINE__); exit(-1); }

static char*
to_h(double size, char *buf)
{   
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

/* Load a set from file */
#define set_load(f) CALL(set_load, f)
TASK_1(set_t, set_load, FILE*, f)
{
    // allocate set
    set_t set = (set_t)malloc(sizeof(struct set));
    set->bdd = sylvan_false;
    set->variables = sylvan_true;
    sylvan_protect(&set->bdd);
    sylvan_protect(&set->variables);

    // read k
    int k;
    if (fread(&k, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");

    if (k == -1) {
        // create variables for a full state vector
        uint32_t vars[totalbits];
        for (int i=0; i<totalbits; i++) vars[i] = 2*i;
        set->variables = sylvan_set_fromarray(vars, totalbits);
    } else {
        // read proj
        int proj[k];
        if (fread(proj, sizeof(int), k, f) != (size_t)k) Abort("Invalid input file!\n");
        // create variables for a short/projected state vector
        uint32_t vars[totalbits];
        uint32_t cv = 0;
        int j = 0, n = 0;
        for (int i=0; i<vectorsize && j<k; i++) {
            if (i == proj[j]) {
                for (int x=0; x<statebits[i]; x++) vars[n++] = (cv += 2) - 2;
                j++;
            } else {
                cv += 2 * statebits[i];
            }
        }
        set->variables = sylvan_set_fromarray(vars, n);
    }

    // read bdd
    if (mtbdd_reader_frombinary(f, &set->bdd, 1) != 0) Abort("Invalid input file!\n");

    return set;
}

/**
 * Load a relation from file
 * This part just reads the r_k, w_k, r_proj and w_proj variables.
 */
#define rel_load_proj(f) CALL(rel_load_proj, f)
TASK_1(rel_t, rel_load_proj, FILE*, f)
{
    rel_t rel = (rel_t)malloc(sizeof(struct relation));
    int r_k, w_k;
    if (fread(&r_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");
    if (fread(&w_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");
    rel->r_k = r_k;
    rel->w_k = w_k;
    int *r_proj = (int*)malloc(sizeof(int[r_k]));
    int *w_proj = (int*)malloc(sizeof(int[w_k]));
    if (fread(r_proj, sizeof(int), r_k, f) != (size_t)r_k) Abort("Invalid file format.");
    if (fread(w_proj, sizeof(int), w_k, f) != (size_t)w_k) Abort("Invalid file format.");
    rel->r_proj = r_proj;
    rel->w_proj = w_proj;

    rel->bdd = sylvan_false;
    sylvan_protect(&rel->bdd);

    /* Compute a_proj the union of r_proj and w_proj, and a_k the length of a_proj */
    int a_proj[r_k+w_k];
    int r_i = 0, w_i = 0, a_i = 0;
    for (;r_i < r_k || w_i < w_k;) {
        if (r_i < r_k && w_i < w_k) {
            if (r_proj[r_i] < w_proj[w_i]) {
                a_proj[a_i++] = r_proj[r_i++];
            } else if (r_proj[r_i] > w_proj[w_i]) {
                a_proj[a_i++] = w_proj[w_i++];
            } else /* r_proj[r_i] == w_proj[w_i] */ {
                a_proj[a_i++] = w_proj[w_i++];
                r_i++;
            }
        } else if (r_i < r_k) {
            a_proj[a_i++] = r_proj[r_i++];
        } else if (w_i < w_k) {
            a_proj[a_i++] = w_proj[w_i++];
        }
    }
    const int a_k = a_i;

    /* Compute all_variables, which are all variables the transition relation is defined on */
    uint32_t all_vars[totalbits * 2];
    uint32_t curvar = 0; // start with variable 0
    int i=0, j=0, n=0;
    for (; i<vectorsize && j<a_k; i++) {
        if (i == a_proj[j]) {
            for (int k=0; k<statebits[i]; k++) {
                all_vars[n++] = curvar;
                all_vars[n++] = curvar + 1;
                curvar += 2;
            }
            j++;
        } else {
            curvar += 2 * statebits[i];
        }
    }
    rel->variables = sylvan_set_fromarray(all_vars, n);
    sylvan_protect(&rel->variables);

    return rel;
}

/**
 * Load a relation from file
 * This part just reads the bdd of the relation
 */
#define rel_load(rel, f) CALL(rel_load, rel, f)
VOID_TASK_2(rel_load, rel_t, rel, FILE*, f)
{
    if (mtbdd_reader_frombinary(f, &rel->bdd, 1) != 0) Abort("Invalid file format!\n");
}

VOID_TASK_0(gc_start)
{
    char buf[32];
    to_h(getCurrentRSS(), buf);
    INFO("(GC) Starting garbage collection... (rss: %s)\n", buf);
}

VOID_TASK_0(gc_end)
{
    char buf[32];
    to_h(getCurrentRSS(), buf);
    INFO("(GC) Garbage collection done.       (rss: %s)\n", buf);
}

int
main(int argc, char **argv)
{
    /**
     * Parse command line, set locale, set startup time for INFO messages.
     */
    argp_parse(&argp, argc, argv, 0, 0, 0);
    setlocale(LC_NUMERIC, "en_US.utf-8");
    t_start = wctime();

    /**
     * Initialize Lace.
     *
     * First: setup with given number of workers (0 for autodetect) and some large size task queue.
     * Second: start all worker threads with default settings.
     * Third: setup local variables using the LACE_ME macro.
     */
    lace_init(workers, 1000000);
    lace_startup(0, NULL, NULL);
    LACE_ME;

    /**
     * Initialize Sylvan.
     *
     * First: set memory limits
     * - 2 GB memory, nodes table twice as big as cache, initial size halved 6x
     *   (that means it takes 6 garbage collections to get to the maximum nodes&cache size)
     * Second: initialize package and subpackages
     * Third: add hooks to report garbage collection
     */
    sylvan_set_limits(2LL<<30, 1, 6);
    sylvan_init_package();
    sylvan_init_bdd();
    sylvan_init_tbdd();
    sylvan_gc_hook_pregc(TASK(gc_start));
    sylvan_gc_hook_postgc(TASK(gc_end));

    /**
     * Read the model from file
     */

    /* Open the file */
    FILE *f = fopen(bdd_filename, "r");
    if (f == NULL) Abort("Cannot open file '%s'!\n", bdd_filename);

    /* Read domain data */
    if (fread(&vectorsize, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    statebits = (int*)malloc(sizeof(int[vectorsize]));
    if (fread(statebits, sizeof(int), vectorsize, f) != (size_t)vectorsize) Abort("Invalid input file!\n");
    if (fread(&actionbits, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    totalbits = 0;
    for (int i=0; i<vectorsize; i++) totalbits += statebits[i];

    /* Read initial state */
    set_t states = set_load(f);

    /* Read number of transition relations */
    if (fread(&next_count, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    next = (rel_t*)malloc(sizeof(rel_t) * next_count);

    /* Read transition relations */
    for (int i=0; i<next_count; i++) next[i] = rel_load_proj(f);
    for (int i=0; i<next_count; i++) rel_load(next[i], f);

    /* We ignore the reachable states and action labels that are stored after the relations */

    /* Close the file */
    fclose(f);

    /**
     * Some statistics reporting
     */

    INFO("Read file '%s'\n", bdd_filename);
    INFO("%d integers per state, %d bits per state, %d transition groups\n", vectorsize, totalbits, next_count);

    /* compute total node counts */
    BDD all_dd[1+next_count];
    all_dd[0] = states->bdd;
    for (int i=0; i<next_count; i++) all_dd[1+i] = next[i]->bdd;
    size_t count_before = mtbdd_nodecount_more(all_dd, 1+next_count);

    /* add action bits to variables */
    BDD action_variables = mtbdd_true;
    sylvan_protect(&action_variables);
    for (int i=0; i<actionbits; i++) {
        action_variables = mtbdd_makenode(1000000+(actionbits-i-1), mtbdd_false, action_variables);
    }

    /* convert BDDs to TBDDs */
    states->bdd = tbdd_from_mtbdd(states->bdd, states->variables);
    for (int i=0; i<next_count; i++) {
        MTBDD domain = sylvan_and(action_variables, next[i]->variables);
        next[i]->bdd = tbdd_from_mtbdd(next[i]->bdd, domain);
    }

    /* compute total node counts */
    all_dd[0] = states->bdd;
    for (int i=0; i<next_count; i++) all_dd[1+i] = next[i]->bdd;
    size_t count_after = tbdd_nodecount_more(all_dd, 1+next_count);
    INFO("#Nodes from %'zu to %'zu.\n", count_before, count_after);

    /**
     * Write the result to file
     */

    // Create TBDD file
    f = fopen(tbdd_filename, "w");
    if (f == NULL) Abort("Cannot open file '%s'!\n", bdd_filename);

    // Write domain...
    fwrite(&vectorsize, sizeof(int), 1, f);
    fwrite(statebits, sizeof(int), vectorsize, f);
    fwrite(&actionbits, sizeof(int), 1, f);

    // Write initial state...
    int k = -1;
    fwrite(&k, sizeof(int), 1, f);
    tbdd_writer_tobinary(f, &states->bdd, 1);

    // Write number of transitions
    fwrite(&next_count, sizeof(int), 1, f);

    // Write meta for each transition
    for (int i=0; i<next_count; i++) {
        fwrite(&next[i]->r_k, sizeof(int), 1, f);
        fwrite(&next[i]->w_k, sizeof(int), 1, f);
        fwrite(next[i]->r_proj, sizeof(int), next[i]->r_k, f);
        fwrite(next[i]->w_proj, sizeof(int), next[i]->w_k, f);
    }

    // Write BDD for each transition
    for (int i=0; i<next_count; i++) tbdd_writer_tobinary(f, &next[i]->bdd, 1);

    // No reachable states or action labels (maybe later)
    int dummy = 0;
    fwrite(&dummy, sizeof(int), 1, f);
    fwrite(&dummy, sizeof(int), 1, f);

    // Close the file
    fclose(f);

    // Report to the user
    INFO("Written file %s.\n", tbdd_filename);

    print_memory_usage();
    sylvan_stats_report(stdout);

    return 0;
}
