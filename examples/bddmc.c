#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getrss.h>

#include <sylvan/internal/internal.h>

#include <common.h>

/* Configuration */
static int report_levels = 0; // report states at end of every level
static int report_table = 0; // report table size at end of every level
static int report_nodes = 0; // report number of nodes of BDDs
static int strategy = 2; // 0 = BFS, 1 = PAR, 2 = SAT, 3 = CHAINING
static int check_deadlocks = 0; // set to 1 to check for deadlocks on-the-fly (only bfs/par)
static int merge_relations = 0; // merge relations to 1 relation
static int print_transition_matrix = 0; // print transition relation matrix
static int workers = 0; // autodetect
static const char* model_filename = NULL; // filename of model

static void
print_usage()
{
    printf("Usage: bddmc [-h] [-s <bfs|par|sat|chaining>] [-w <workers>]\n");
    printf("        [--strategy=<bfs|par|sat|chaining>] [--workers=<workers>]\n");
    printf("        [--count-nodes] [--count-states] [--count-table] [--deadlocks]\n");
    printf("        [--merge-relations] [--print-matrix] [--help] [--usage] <model>\n");
}

static void
print_help()
{
    printf("Usage: bddmc [OPTION...] <model>\n\n");
    printf("  -s, --strategy=<bfs|par|sat|chaining>\n");
    printf("                             Strategy for reachability (default=sat)\n");
    printf("  -w, --workers=<workers>    Number of workers (default=0: autodetect)\n");
    printf("      --count-nodes          Report #nodes for BDDs\n");
    printf("      --count-states         Report #states at each level\n");
    printf("      --count-table          Report table usage at each level\n");
    printf("      --deadlocks            Check for deadlocks\n");
    printf("      --merge-relations      Merge transition relations into one transition relation\n");
    printf("      --print-matrix         Print transition matrix\n");
    printf("  -h, --help                 Give this help list\n");
    printf("      --usage                Give a short usage message\n");
}

static void
parse_args(int argc, const char **argv)
{
    static const struct optparse_long longopts[] = {
        {"workers", 'w', OPTPARSE_REQUIRED},
        {"strategy", 's', OPTPARSE_REQUIRED},
        {"deadlocks", 3, OPTPARSE_NONE},
        {"count-nodes", 5, OPTPARSE_NONE},
        {"count-states", 1, OPTPARSE_NONE},
        {"count-table", 2, OPTPARSE_NONE},
        {"merge-relations", 6, OPTPARSE_NONE},
        {"print-matrix", 4, OPTPARSE_NONE},
        {"help", 'h', OPTPARSE_NONE},
        {"usage", 'u', OPTPARSE_NONE},
        {},
    };
    int option = 0;
    struct optparse options;
    optparse_init(&options, argv);
    while ((option = optparse_long(&options, longopts, NULL)) != -1) {
        switch (option) {
            case 'w':
                workers = atoi(options.optarg);
                break;
            case 's':
                if (strcmp(options.optarg, "bfs")==0) strategy = 0;
                else if (strcmp(options.optarg, "par")==0) strategy = 1;
                else if (strcmp(options.optarg, "sat")==0) strategy = 2;
                else if (strcmp(options.optarg, "chaining")==0) strategy = 3;
                else {
                    print_usage();
                    exit(0);
                }
                break;
            case 4:
                print_transition_matrix = 1;
                break;
            case 3:
                check_deadlocks = 1;
                break;
            case 1:
                report_levels = 1;
                break;
            case 2:
                report_table = 1;
                break;
            case 5:
                report_nodes = 1;
                break;
            case 6:
                merge_relations = 1;
                break;
            case 'u':
                print_usage();
                exit(0);
            case 'h':
                print_help();
                exit(0);
        }
    }
    if (options.optind >= argc) {
        print_usage();
        exit(0);
    }
    model_filename = optparse_arg(&options);
}

/**
 * Types (set and relation)
 */
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
static int totalbits; // total number of bits
static int next_count; // number of partitions of the transition relation
static rel_t *next; // each partition of the transition relation

static double t_start;
#define INFO(s, ...) fprintf(stdout, "[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "Abort at line %d!\n", __LINE__); exit(-1); }

static char*
to_h(double size, char *buf)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    int i = 0;
    for (;size>1024;size/=1024) i++;
    snprintf(buf, 32, "%.*f %s", i, size, units[i]);
    return buf;
}

static void
print_memory_usage(void)
{
    char buf[32];
    to_h((double)getCurrentRSS(), buf);
    INFO("Memory usage: %s\n", buf);
}

/**
 * Load a set from file
 * The expected binary format:
 * - int k : projection size, or -1 for full state
 * - int[k] proj : k integers specifying the variables of the projection
 * - MTBDD[1] BDD (mtbdd binary format)
 */
TASK(set_t, set_load, FILE*, f)

set_t set_load_CALL(lace_worker* lace, FILE* f)
{
    // allocate set
    set_t set = (set_t)malloc(sizeof(struct set));
    set->bdd = mtbdd_false;
    set->variables = mtbdd_true;
    mtbdd_protect(&set->bdd);
    mtbdd_protect(&set->variables);

    // read k
    int k;
    if (fread(&k, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");

    if (k == -1) {
        // create variables for a full state vector
        uint32_t* vars = SYLVAN_ALLOCA(uint32_t, totalbits);
        for (int i=0; i<totalbits; i++) vars[i] = 2*i;
        set->variables = mtbdd_set_from_array(vars, totalbits);
    } else {
        // read proj
        int* proj = SYLVAN_ALLOCA(int, k);
        if (fread(proj, sizeof(int), k, f) != (size_t)k) Abort("Invalid input file!\n");
        // create variables for a short/projected state vector
        uint32_t* vars = SYLVAN_ALLOCA(uint32_t, totalbits);
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
        set->variables = mtbdd_set_from_array(vars, n);
    }

    // read bdd
    if (mtbdd_reader_frombinary(f, &set->bdd, 1) != 0) Abort("Invalid input file!\n");

    return set;
    (void)lace;
}

void set_free(set_t set)
{
    free(set);
}

/**
 * Load a relation from file
 * This part just reads the r_k, w_k, r_proj and w_proj variables.
 */
TASK(rel_t, rel_load_proj, FILE*, f)
rel_t rel_load_proj_CALL(lace_worker* lace, FILE* f)
{
    rel_t rel = (rel_t)malloc(sizeof(struct relation));
    int r_k, w_k;
    if (fread(&r_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");
    if (fread(&w_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");
    rel->r_k = r_k;
    rel->w_k = w_k;
    int *r_proj = (int*)malloc(r_k * sizeof(int));
    int *w_proj = (int*)malloc(w_k * sizeof(int));
    if (fread(r_proj, sizeof(int), r_k, f) != (size_t)r_k) Abort("Invalid file format.");
    if (fread(w_proj, sizeof(int), w_k, f) != (size_t)w_k) Abort("Invalid file format.");
    rel->r_proj = r_proj;
    rel->w_proj = w_proj;

    rel->bdd = mtbdd_false;
    mtbdd_protect(&rel->bdd);

    /* Compute a_proj the union of r_proj and w_proj, and a_k the length of a_proj */
    int* a_proj = SYLVAN_ALLOCA(int, r_k+w_k);
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
    uint32_t* all_vars = SYLVAN_ALLOCA(uint32_t, totalbits * 2);
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
    rel->variables = mtbdd_set_from_array(all_vars, n);
    mtbdd_protect(&rel->variables);

    return rel;
    (void)lace;
}

/**
 * Load a relation from file
 * This part just reads the bdd of the relation
 */
TASK(void, rel_load, rel_t, rel, FILE*, f)
void rel_load_CALL(lace_worker* lace, rel_t rel, FILE* f)
{
    if (mtbdd_reader_frombinary(f, &rel->bdd, 1) != 0) Abort("Invalid file format!\n");
    return;
    (void)lace;
}

/**
 * Print a single example of a set to stdout
 * Assumption: the example is a full vector and variables contains all state variables...
 */
TASK(void, print_example, BDD, example, BDDSET, variables)
void print_example_CALL(lace_worker* lace, BDD example, BDDSET variables)
{
    uint8_t* str = SYLVAN_ALLOCA(uint8_t, totalbits);

    if (example != mtbdd_false) {
        bdd_sat_one(example, variables, str);
        int x=0;
        printf("[");
        for (int i=0; i<vectorsize; i++) {
            uint32_t res = 0;
            for (int j=0; j<statebits[i]; j++) {
                if (str[x++] == 1) res++;
                res <<= 1;
            }
            if (i>0) printf(",");
            printf("%" PRIu32, res);
        }
        printf("]");
    }
    return;
    (void)lace;
}

/**
 * Implementation of (parallel) saturation
 * (assumes relations are ordered on first variable)
 */
TASK(BDD, go_sat, BDD, set, int, idx)
BDD go_sat_CALL(lace_worker* lace, BDD set, int idx)
{
    /* Terminal cases */
    if (set == mtbdd_false) return mtbdd_false;
    if (idx == next_count) return set;

    /* Consult the cache */
    BDD result;
    const BDD _set = set;
    if (cache_get3(200LL<<40, _set, idx, 0, &result)) return result;
    mtbdd_refs_pushptr(&_set);

    /**
     * Possible improvement: cache more things (like intermediate results?)
     *   and chain-apply more of the current level before going deeper?
     */

    /* Check if the relation should be applied */
    const uint32_t var = mtbdd_getvar(next[idx]->variables);
    if (set == mtbdd_true || var <= mtbdd_getvar(set)) {
        /* Count the number of relations starting here */
        int count = idx+1;
        while (count < next_count && var == mtbdd_getvar(next[count]->variables)) count++;
        count -= idx;
        /*
         * Compute until fixpoint:
         * - SAT deeper
         * - chain-apply all current level once
         */
        BDD prev = mtbdd_false;
        BDD step = mtbdd_false;
        mtbdd_refs_pushptr(&set);
        mtbdd_refs_pushptr(&prev);
        mtbdd_refs_pushptr(&step);
        while (prev != set) {
            prev = set;
            // SAT deeper
            set = go_sat_CALL(lace, set, idx+count);
            // chain-apply all current level once
            for (int i=0;i<count;i++) {
                step = bdd_relnext_CALL(lace, set, next[idx+i]->bdd, next[idx+i]->variables);
                set = bdd_not(bdd_and_CALL(lace, bdd_not(set), bdd_not(step)));
                step = mtbdd_false; // unset, for gc
            }
        }
        mtbdd_refs_popptr(3);
        result = set;
    } else {
        /* Recursive computation */
        mtbdd_refs_spawn(go_sat_SPAWN(lace, mtbdd_getlow(set), idx));
        BDD high = mtbdd_refs_push(go_sat_CALL(lace, mtbdd_gethigh(set), idx));
        BDD low = mtbdd_refs_sync(go_sat_SYNC(lace));
        mtbdd_refs_pop(1);
        result = mtbdd_makenode(mtbdd_getvar(set), low, high);
    }

    /* Store in cache */
    cache_put3(200LL<<40, _set, idx, 0, result);
    mtbdd_refs_popptr(1);
    return result;
}

/**
 * Wrapper for the Saturation strategy
 */
TASK(void, sat, set_t, set)
void sat_CALL(lace_worker* lace, set_t set)
{
    set->bdd = go_sat_CALL(lace, set->bdd, 0);
}

/**
 * Implement parallel strategy (that performs the relnext operations in parallel)
 * This function does one level...
 */
TASK(BDD, go_par, BDD, cur, BDD, visited, size_t, from, size_t, len, BDD*, deadlocks)
BDD go_par_CALL(lace_worker* lace, BDD cur, BDD visited, size_t from, size_t len, BDD* deadlocks)
{
    if (len == 1) {
        // Calculate NEW successors (not in visited)
        BDD succ = bdd_relnext_CALL(lace, cur, next[from]->bdd, next[from]->variables);
        mtbdd_refs_push(succ);
        if (deadlocks) {
            // check which BDDs in deadlocks do not have a successor in this relation
            BDD anc = bdd_relprev_CALL(lace, next[from]->bdd, succ, next[from]->variables);
            mtbdd_refs_push(anc);
            *deadlocks = bdd_diff(*deadlocks, anc); //FIXME use a CALL
            mtbdd_refs_pop(1);
        }
        BDD result = bdd_diff(succ, visited);
        mtbdd_refs_pop(1);
        return result;
    } else {
        BDD deadlocks_left;
        BDD deadlocks_right;
        if (deadlocks) {
            deadlocks_left = *deadlocks;
            deadlocks_right = *deadlocks;
            mtbdd_protect(&deadlocks_left);
            mtbdd_protect(&deadlocks_right);
        }

        // Recursively calculate left+right
        mtbdd_refs_spawn(go_par_SPAWN(lace, cur, visited, from, (len+1)/2, deadlocks ? &deadlocks_left: NULL));
        BDD right = mtbdd_refs_push(go_par_CALL(lace, cur, visited, from+(len+1)/2, len/2, deadlocks ? &deadlocks_right : NULL));
        BDD left = mtbdd_refs_push(mtbdd_refs_sync(go_par_SYNC(lace)));

        // Merge results of left+right
        BDD result = bdd_or(left, right);
        mtbdd_refs_pop(2);

        if (deadlocks) {
            mtbdd_refs_push(result);
            *deadlocks = bdd_and_CALL(lace, deadlocks_left, deadlocks_right);
            mtbdd_unprotect(&deadlocks_left);
            mtbdd_unprotect(&deadlocks_right);
            mtbdd_refs_pop(1);
        }

        return result;
    }
}

/**
 * Implementation of the PAR strategy
 */
TASK(void, par, set_t, set)
void par_CALL(lace_worker* lace, set_t set)
{
    BDD visited = set->bdd;
    BDD next_level = visited;
    BDD cur_level = mtbdd_false;
    BDD deadlocks = mtbdd_false;

    mtbdd_protect(&visited);
    mtbdd_protect(&next_level);
    mtbdd_protect(&cur_level);
    mtbdd_protect(&deadlocks);

    int iteration = 1;
    do {
        // calculate successors in parallel
        cur_level = next_level;
        deadlocks = cur_level;

        next_level = go_par_CALL(lace, cur_level, visited, 0, next_count, check_deadlocks ? &deadlocks : NULL);

        if (check_deadlocks && deadlocks != mtbdd_false) {
            INFO("Found %0.0f deadlock states... ", bdd_satcount_CALL(lace, deadlocks, set->variables));
            if (deadlocks != mtbdd_false) {
                printf("example: ");
                print_example(deadlocks, set->variables);
                check_deadlocks = 0;
            }
            printf("\n");
        }

        // visited = visited + new
        visited = bdd_or(visited, next_level);

        if (report_table && report_levels) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, %0.0f states explored, table: %0.1f%% full (%zu nodes)\n",
                iteration, bdd_satcount_CALL(lace, visited, set->variables),
                100.0*(double)filled/total, filled);
        } else if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, table: %0.1f%% full (%zu nodes)\n",
                iteration,
                100.0*(double)filled/total, filled);
        } else if (report_levels) {
            INFO("Level %d done, %0.0f states explored\n", iteration, bdd_satcount_CALL(lace, visited, set->variables));
        } else {
            INFO("Level %d done\n", iteration);
        }
        iteration++;
    } while (next_level != mtbdd_false);

    set->bdd = visited;

    mtbdd_unprotect(&visited);
    mtbdd_unprotect(&next_level);
    mtbdd_unprotect(&cur_level);
    mtbdd_unprotect(&deadlocks);
}

/**
 * Implement sequential strategy (that performs the relnext operations one by one)
 * This function does one level...
 */
TASK(BDD, go_bfs, BDD, cur, BDD, visited, size_t, from, size_t, len, BDD*, deadlocks)
BDD go_bfs_CALL(lace_worker* lace, BDD cur, BDD visited, size_t from, size_t len, BDD* deadlocks)
{
    if (len == 1) {
        // Calculate NEW successors (not in visited)
        BDD succ = bdd_relnext_CALL(lace, cur, next[from]->bdd, next[from]->variables);
        mtbdd_refs_push(succ);
        if (deadlocks) {
            // check which BDDs in deadlocks do not have a successor in this relation
            BDD anc = bdd_relprev_CALL(lace, next[from]->bdd, succ, next[from]->variables);
            mtbdd_refs_push(anc);
            *deadlocks = bdd_diff(*deadlocks, anc); // FIXME make it a CALL
            mtbdd_refs_pop(1);
        }
        BDD result = bdd_diff(succ, visited);
        mtbdd_refs_pop(1);
        return result;
    } else {
        BDD deadlocks_left;
        BDD deadlocks_right;
        if (deadlocks) {
            deadlocks_left = *deadlocks;
            deadlocks_right = *deadlocks;
            mtbdd_protect(&deadlocks_left);
            mtbdd_protect(&deadlocks_right);
        }

        // Recursively calculate left+right
        BDD left = go_bfs_CALL(lace, cur, visited, from, (len+1)/2, deadlocks ? &deadlocks_left : NULL);
        mtbdd_refs_push(left);
        BDD right = go_bfs_CALL(lace, cur, visited, from+(len+1)/2, len/2, deadlocks ? &deadlocks_right : NULL);
        mtbdd_refs_push(right);

        // Merge results of left+right
        BDD result = bdd_or(left, right);
        mtbdd_refs_pop(2);

        if (deadlocks) {
            mtbdd_refs_push(result);
            *deadlocks = bdd_and_CALL(lace, deadlocks_left, deadlocks_right);
            mtbdd_unprotect(&deadlocks_left);
            mtbdd_unprotect(&deadlocks_right);
            mtbdd_refs_pop(1);
        }

        return result;
    }
}

/**
 * Implementation of the BFS strategy
 */
TASK(void, bfs, set_t, set)
void bfs_CALL(lace_worker* lace, set_t set)
{
    BDD visited = set->bdd;
    BDD next_level = visited;
    BDD cur_level = mtbdd_false;
    BDD deadlocks = mtbdd_false;

    mtbdd_protect(&visited);
    mtbdd_protect(&next_level);
    mtbdd_protect(&cur_level);
    mtbdd_protect(&deadlocks);

    int iteration = 1;
    do {
        // calculate successors in parallel
        cur_level = next_level;
        deadlocks = cur_level;

        next_level = go_bfs_CALL(lace, cur_level, visited, 0, next_count, check_deadlocks ? &deadlocks : NULL);

        if (check_deadlocks && deadlocks != mtbdd_false) {
            INFO("Found %0.0f deadlock states... ", bdd_satcount_CALL(lace, deadlocks, set->variables));
            if (deadlocks != mtbdd_false) {
                printf("example: ");
                print_example(deadlocks, set->variables);
                check_deadlocks = 0;
            }
            printf("\n");
        }

        // visited = visited + new
        visited = bdd_or(visited, next_level);

        if (report_table && report_levels) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, %0.0f states explored, table: %0.1f%% full (%zu nodes)\n",
                iteration, bdd_satcount_CALL(lace, visited, set->variables),
                100.0*(double)filled/total, filled);
        } else if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, table: %0.1f%% full (%zu nodes)\n",
                iteration,
                100.0*(double)filled/total, filled);
        } else if (report_levels) {
            INFO("Level %d done, %0.0f states explored\n", iteration, bdd_satcount_CALL(lace, visited, set->variables));
        } else {
            INFO("Level %d done\n", iteration);
        }
        iteration++;
    } while (next_level != mtbdd_false);

    set->bdd = visited;

    mtbdd_unprotect(&visited);
    mtbdd_unprotect(&next_level);
    mtbdd_unprotect(&cur_level);
    mtbdd_unprotect(&deadlocks);
}

/**
 * Implementation of the Chaining strategy (does not support deadlock detection)
 */
TASK(void, chaining, set_t, set)
void chaining_CALL(lace_worker* lace, set_t set)
{
    BDD visited = set->bdd;
    BDD next_level = visited;
    BDD succ = mtbdd_false;

    mtbdd_refs_pushptr(&visited);
    mtbdd_refs_pushptr(&next_level);
    mtbdd_refs_pushptr(&succ);

    int iteration = 1;
    do {
        // calculate successors in parallel
        for (int i=0; i<next_count; i++) {
            succ = bdd_relnext_CALL(lace, next_level, next[i]->bdd, next[i]->variables);
            next_level = bdd_or(next_level, succ);
            succ = mtbdd_false; // reset, for gc
        }

        // new = new - visited
        // visited = visited + new
        next_level = bdd_diff(next_level, visited);
        visited = bdd_or(visited, next_level);

        if (report_table && report_levels) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, %0.0f states explored, table: %0.1f%% full (%zu nodes)\n",
                iteration, bdd_satcount_CALL(lace, visited, set->variables),
                100.0*(double)filled/total, filled);
        } else if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            INFO("Level %d done, table: %0.1f%% full (%zu nodes)\n",
                iteration,
                100.0*(double)filled/total, filled);
        } else if (report_levels) {
            INFO("Level %d done, %0.0f states explored\n", iteration, bdd_satcount_CALL(lace, visited, set->variables));
        } else {
            INFO("Level %d done\n", iteration);
        }
        iteration++;
    } while (next_level != mtbdd_false);

    set->bdd = visited;
    mtbdd_refs_popptr(3);
}

/**
 * Extend a transition relation to a larger domain (using s=s')
 */
TASK(BDD, extend_relation, MTBDD, relation, MTBDD, variables)
BDD extend_relation_CALL(lace_worker* lace, MTBDD relation, MTBDD variables)
{
    /* first determine which state BDD variables are in rel */
    int* has = SYLVAN_ALLOCA(int, totalbits);
    for (int i=0; i<totalbits; i++) has[i] = 0;
    MTBDD s = variables;
    while (!mtbdd_set_isempty(s)) {
        uint32_t v = mtbdd_set_first(s);
        if (v/2 >= (unsigned)totalbits) break; // action labels
        has[v/2] = 1;
        s = mtbdd_set_next(s);
    }

    /* create "s=s'" for all variables not in rel */
    BDD eq = mtbdd_true;
    for (int i=totalbits-1; i>=0; i--) {
        if (has[i]) continue;
        BDD low = mtbdd_makenode(2*i+1, eq, mtbdd_false);
        mtbdd_refs_push(low);
        BDD high = mtbdd_makenode(2*i+1, mtbdd_false, eq);
        mtbdd_refs_pop(1);
        eq = mtbdd_makenode(2*i, low, high);
    }

    mtbdd_refs_push(eq);
    BDD result = bdd_and_CALL(lace, relation, eq);
    mtbdd_refs_pop(1);

    return result;
}

/**
 * Compute \BigUnion ( sets[i] )
 */
TASK(BDD, big_union, int, first, int, count)
BDD big_union_CALL(lace_worker* lace, int first, int count)
{
    if (count == 1) return next[first]->bdd;

    mtbdd_refs_spawn(big_union_SPAWN(lace, first, count/2));
    BDD right = mtbdd_refs_push(big_union_CALL(lace, first+count/2, count-count/2));
    BDD left = mtbdd_refs_push(mtbdd_refs_sync(big_union_SYNC(lace)));
    BDD result = bdd_or(left, right);
    mtbdd_refs_pop(2);
    return result;
}

/**
 * Print one row of the transition matrix (for vars)
 */
static void
print_matrix_row(rel_t rel)
{
    int r_i = 0, w_i = 0;
    for (int i=0; i<vectorsize; i++) {
        int s = 0;
        if (r_i < rel->r_k && rel->r_proj[r_i] == i) {
            s |= 1;
            r_i++;
        }
        if (w_i < rel->w_k && rel->w_proj[w_i] == i) {
            s |= 2;
            w_i++;
        }
        if (s == 0) fprintf(stdout, "-");
        else if (s == 1) fprintf(stdout, "r");
        else if (s == 2) fprintf(stdout, "w");
        else if (s == 3) fprintf(stdout, "+");
    }
}

TASK(void, gc_start)
void gc_start_CALL(lace_worker* lace)
{
    char buf[32];
    to_h((double)getCurrentRSS(), buf);
    INFO("(GC) Starting garbage collection... (rss: %s)\n", buf);
    return;
    (void)lace;
}

TASK(void, gc_end)
void gc_end_CALL(lace_worker* lace)
{
    char buf[32];
    to_h((double)getCurrentRSS(), buf);
    INFO("(GC) Garbage collection done.       (rss: %s)\n", buf);
    return;
    (void)lace;
}

void
print_h(double size)
{
    const char* units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    int i = 0;
    for (;size>1024;size/=1024) i++;
    printf("%.*f %s", i, size, units[i]);
}

TASK(void, run)
void run_CALL(lace_worker* lace)
{
    /**
     * Read the model from file
     */

    /* Open the file */
    FILE *f = fopen(model_filename, "rb");
    if (f == NULL) Abort("Cannot open file '%s'!\n", model_filename);

    /* Read domain data */
    if (fread(&vectorsize, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    statebits = (int*)malloc(vectorsize * sizeof(int));
    if (fread(statebits, sizeof(int), vectorsize, f) != (size_t)vectorsize) Abort("Invalid input file!\n");
    if (fread(&actionbits, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    totalbits = 0;
    for (int i=0; i<vectorsize; i++) totalbits += statebits[i];

    /* Read initial state */
    set_t states = set_load(f);

    /* Read number of transition relations */
    if (fread(&next_count, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    next = (rel_t*)malloc(next_count * sizeof(rel_t));

    /* Read transition relations */
    for (int i=0; i<next_count; i++) next[i] = rel_load_proj(f);
    for (int i=0; i<next_count; i++) rel_load(next[i], f);

    /* We ignore the reachable states and action labels that are stored after the relations */

    /* Close the file */
    fclose(f);

    /**
     * Pre-processing and some statistics reporting
     */

    if (strategy == 2 || strategy == 3) {
        // for SAT and CHAINING, sort the transition relations (gnome sort because I like gnomes)
        int i = 1, j = 2;
        rel_t t;
        while (i < next_count) {
            rel_t *p = &next[i], *q = p-1;
            if (mtbdd_getvar((*q)->variables) > mtbdd_getvar((*p)->variables)) {
                t = *q;
                *q = *p;
                *p = t;
                if (--i) continue;
            }
            i = j++;
        }
    }

    INFO("Read file '%s'\n", model_filename);
    INFO("%d integers per state, %d bits per state, %d transition groups\n", vectorsize, totalbits, next_count);

    /* if requested, print the transition matrix */
    if (print_transition_matrix) {
        for (int i=0; i<next_count; i++) {
            INFO(""); // print time prefix
            print_matrix_row(next[i]); // print row
            fprintf(stdout, "\n"); // print newline
        }
    }

    /* merge all relations to one big transition relation if requested */
    if (merge_relations) {
        BDD newvars = mtbdd_set_empty();
        mtbdd_refs_pushptr(&newvars);
        for (int i=totalbits-1; i>=0; i--) {
            newvars = mtbdd_set_add(newvars, i*2+1);
            newvars = mtbdd_set_add(newvars, i*2);
        }

        INFO("Extending transition relations to full domain.\n");
        for (int i=0; i<next_count; i++) {
            next[i]->bdd = extend_relation(next[i]->bdd, next[i]->variables);
            next[i]->variables = newvars;
        }

        mtbdd_refs_popptr(1);

        INFO("Taking union of all transition relations.\n");
        next[0]->bdd = big_union(0, next_count);

        for (int i=1; i<next_count; i++) {
            next[i]->bdd = mtbdd_false;
            next[i]->variables = mtbdd_true;
        }
        next_count = 1;
    }

    if (report_nodes) {
        INFO("BDD nodes:\n");
        INFO("Initial states: %zu BDD nodes\n", mtbdd_nodecount(states->bdd));
        for (int i=0; i<next_count; i++) {
            INFO("Transition %d: %zu BDD nodes\n", i, mtbdd_nodecount(next[i]->bdd));
        }
    }

    print_memory_usage();

    if (strategy == 0) {
        double t1 = wctime();
        bfs_CALL(lace, states);
        double t2 = wctime();
        INFO("BFS Time: %f\n", t2-t1);
    } else if (strategy == 1) {
        double t1 = wctime();
        par_CALL(lace, states);
        double t2 = wctime();
        INFO("PAR Time: %f\n", t2-t1);
    } else if (strategy == 2) {
        double t1 = wctime();
        sat_CALL(lace, states);
        double t2 = wctime();
        INFO("SAT Time: %f\n", t2-t1);
    } else if (strategy == 3) {
        double t1 = wctime();
        chaining_CALL(lace, states);
        double t2 = wctime();
        INFO("CHAINING Time: %f\n", t2-t1);
    } else {
        Abort("Invalid strategy set?!\n");
    }

    // Now we just have states
    INFO("Final states: %0.0f states\n", bdd_satcount_CALL(lace, states->bdd, states->variables));
    if (report_nodes) {
        INFO("Final states: %zu BDD nodes\n", mtbdd_nodecount(states->bdd));
    }

    set_free(states);
}

int
main(int argc, const char **argv)
{
    /**
     * Parse command line, set locale, set startup time for INFO messages.
     */
    parse_args(argc, argv);
    setlocale(LC_NUMERIC, "en_US.utf-8");
    t_start = wctime();

    /**
     * Initialize Lace.
     *
     * First: setup with given number of workers (0 for autodetect) and some large size task queue.
     * Second: start all worker threads with default settings.
     * Third: setup local variables using the LACE_ME macro.
     */
    lace_start(workers, 1000000, 0);

    /**
     * Initialize Sylvan.
     *
     * First: set memory limits
     * - 2 GB memory, nodes table twice as big as cache, initial size halved 6x
     *   (that means it takes 6 garbage collections to get to the maximum nodes&cache size)
     * Second: initialize package and subpackages
     * Third: add hooks to report garbage collection
     */
    size_t max = 16LL<<30;
    if (max > getMaxMemory()) max = getMaxMemory()/10*9;
    printf("Setting Sylvan main tables memory to ");
    print_h((double)max);
    printf(" max.\n");

    mtbdd_set_limits(max, 1, 6);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_gc_hook_pregc(gc_start_CALL);
    sylvan_gc_hook_postgc(gc_end_CALL);

    run();

    print_memory_usage();

    sylvan_stats_report(stdout);

    sylvan_quit();
    lace_stop();
}
