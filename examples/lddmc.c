#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getrss.h>

#include <sylvan/internal.h>

#include <common.h>

/* Configuration */
static int report_levels = 0; // report states at start of every level
static int report_table = 0; // report table size at end of every level
static int report_nodes = 0; // report number of nodes of LDDs
static int strategy = 2; // 0 = BFS, 1 = PAR, 2 = SAT, 3 = CHAINING
static int check_deadlocks = 0; // set to 1 to check for deadlocks on-the-fly
static int print_transition_matrix = 0; // print transition relation matrix
static int workers = 0; // autodetect
static const char* model_filename = NULL; // filename of model
static const char* out_filename = NULL; // filename of output

static void
print_usage()
{
    printf("Usage: lddmc [-h] [-s <bfs|par|sat|chaining>] [-w <workers>]\n");
    printf("            [--strategy=<bfs|par|sat|chaining>] [--workers=<workers>]\n");
    printf("            [--count-nodes] [--count-states] [--count-table] [--deadlocks]\n");
    printf("            [--print-matrix] [--help] [--usage] <model> [<output-bdd>]\n");
}

static void
print_help()
{
    printf("Usage: lddmc [OPTION...] <model> [<output-bdd>]\n\n");
    printf("  -s, --strategy=<bfs|par|sat|chaining>\n");
    printf("                             Strategy for reachability (default=par)\n");
    printf("  -w, --workers=<workers>    Number of workers (default=0: autodetect)\n");
    printf("      --count-nodes          Report #nodes for LDDs\n");
    printf("      --count-states         Report #states at each level\n");
    printf("      --count-table          Report table usage at each level\n");
    printf("      --deadlocks            Check for deadlocks\n");
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
    if (options.optind < argc) out_filename = optparse_arg(&options);
}

/**
 * Types (set and relation)
 */
typedef struct set
{
    LISTDD dd;
} *set_t;

typedef struct relation
{
    LISTDD dd;
    LISTDD meta; // for relprod
    int r_k, w_k, *r_proj, *w_proj;
    int firstvar; // for saturation/chaining
    LISTDD topmeta; // for saturation
} *rel_t;

static int vector_size; // size of vector in integers
static int next_count; // number of partitions of the transition relation
static rel_t *next; // each partition of the transition relation

static double t_start;
#define INFO(s, ...) fprintf(stdout, "[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(...) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "Abort at line %d!\n", __LINE__); exit(-1); }

static LISTDD
listdd_union_or_abort_CALL(lace_worker *lace, LISTDD a, LISTDD b)
{
    LISTDD result = listdd_invalid;
    if (listdd_union_CALL(lace, &result, a, b) != SYLVAN_OK) Abort("ListDD union failed.\n");
    return result;
}

static LISTDD
listdd_diff_or_abort_CALL(lace_worker *lace, LISTDD a, LISTDD b)
{
    LISTDD result = listdd_invalid;
    if (listdd_diff_CALL(lace, &result, a, b) != SYLVAN_OK) Abort("ListDD difference failed.\n");
    return result;
}

static LISTDD
listdd_intersection_or_abort_CALL(lace_worker *lace, LISTDD a, LISTDD b)
{
    LISTDD result = listdd_invalid;
    if (listdd_intersection_CALL(lace, &result, a, b) != SYLVAN_OK) Abort("ListDD intersection failed.\n");
    return result;
}

static LISTDD
listdd_rel_next_or_abort_CALL(lace_worker *lace, LISTDD set, LISTDD relation, LISTDD meta)
{
    LISTDD result = listdd_invalid;
    if (listdd_rel_next_CALL(lace, &result, set, relation, meta) != SYLVAN_OK) Abort("ListDD relational product failed.\n");
    return result;
}

static LISTDD
listdd_rel_next_union_or_abort_CALL(lace_worker *lace, LISTDD set, LISTDD relation, LISTDD meta, LISTDD un)
{
    LISTDD result = listdd_invalid;
    if (listdd_rel_next_union_CALL(lace, &result, set, relation, meta, un) != SYLVAN_OK) {
        Abort("ListDD relational product failed.\n");
    }
    return result;
}

/**
 * Load a set from file
 */
static set_t
set_load(FILE* f)
{
    set_t set = (set_t)malloc(sizeof(struct set));

    /* read projection (actually we don't support projection) */
    int k;
    if (fread(&k, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    if (k != -1) Abort("Invalid input file!\n"); // only support full vector

    /* read dd */
    listdd_serialize_fromfile_old(f);
    size_t dd;
    if (fread(&dd, sizeof(size_t), 1, f) != 1) Abort("Invalid input file!\n");
    set->dd = listdd_serialize_get_reversed(dd);
    listdd_protect(&set->dd);

    return set;
}

/**
 * Save a set to file
 */
static void
set_save(FILE* f, set_t set)
{
    int k = -1;
    fwrite(&k, sizeof(int), 1, f);
    size_t dd = listdd_serialize_add(set->dd);
    listdd_serialize_tofile(f);
    fwrite(&dd, sizeof(size_t), 1, f);
}

/**
 * Free the memory associated with a set
 */
static void
set_free(set_t set)
{
    free(set);
}

/**
 * Load a relation from file
 */
TASK(rel_t, rel_load_proj, FILE*, f)
rel_t rel_load_proj_CALL(lace_worker* lace, FILE* f)
{
    int r_k, w_k;
    if (fread(&r_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");
    if (fread(&w_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");

    rel_t rel = (rel_t)malloc(sizeof(struct relation));
    rel->r_k = r_k;
    rel->w_k = w_k;
    rel->r_proj = (int*)malloc((size_t)rel->r_k * sizeof(*rel->r_proj));
    rel->w_proj = (int*)malloc((size_t)rel->w_k * sizeof(*rel->w_proj));

    if (fread(rel->r_proj, sizeof(int), rel->r_k, f) != (size_t)rel->r_k) Abort("Invalid file format.");
    if (fread(rel->w_proj, sizeof(int), rel->w_k, f) != (size_t)rel->w_k) Abort("Invalid file format.");

    int *r_proj = rel->r_proj;
    int *w_proj = rel->w_proj;

    rel->firstvar = -1;

    /* Compute the meta */
    uint32_t* meta = (uint32_t*)malloc((size_t)(vector_size*2+2) * sizeof(*meta));
    memset(meta, 0, (size_t)(vector_size*2+2) * sizeof(*meta));
    int r_i=0, w_i=0, i=0, j=0;
    for (;;) {
        int type = 0;
        if (r_i < r_k && r_proj[r_i] == i) {
            r_i++;
            type += 1; // read
        }
        if (w_i < w_k && w_proj[w_i] == i) {
            w_i++;
            type += 2; // write
        }
        if (type == 0) meta[j++] = 0;
        else if (type == 1) { meta[j++] = 3; }
        else if (type == 2) { meta[j++] = 4; }
        else if (type == 3) { meta[j++] = 1; meta[j++] = 2; }
        if (type != 0 && rel->firstvar == -1) rel->firstvar = i;
        if (r_i == r_k && w_i == w_k) {
            meta[j++] = 5; // action label
            meta[j++] = (uint32_t)-1;
            break;
        }
        i++;
    }

    rel->meta = listdd_invalid;
    listdd_protect(&rel->meta);
    if (listdd_singleton(&rel->meta, meta, j) != SYLVAN_OK) Abort("Cannot create relation metadata.\n");
    if (rel->firstvar != -1) {
        rel->topmeta = listdd_invalid;
        listdd_protect(&rel->topmeta);
        if (listdd_singleton(&rel->topmeta, meta+rel->firstvar, j-rel->firstvar) != SYLVAN_OK) {
            Abort("Cannot create relation metadata.\n");
        }
    }
    rel->dd = listdd_empty;
    listdd_protect(&rel->dd);

    free(meta);
    return rel;
    (void)lace;
}

TASK(void, rel_load, FILE*, f, rel_t, rel)
void rel_load_CALL(lace_worker* lace, FILE* f, rel_t rel)
{
    listdd_serialize_fromfile_old(f);
    size_t dd;
    if (fread(&dd, sizeof(size_t), 1, f) != 1) Abort("Invalid input file!");
    rel->dd = listdd_serialize_get_reversed(dd);
    (void)lace;
}

/**
 * Save a relation to file
 */
static void
rel_save_proj(FILE* f, rel_t rel)
{
    fwrite(&rel->r_k, sizeof(int), 1, f);
    fwrite(&rel->w_k, sizeof(int), 1, f);
    fwrite(rel->r_proj, sizeof(int), rel->r_k, f);
    fwrite(rel->w_proj, sizeof(int), rel->w_k, f);
}

static void
rel_save(FILE* f, rel_t rel)
{
    size_t dd = listdd_serialize_add(rel->dd);
    listdd_serialize_tofile(f);
    fwrite(&dd, sizeof(size_t), 1, f);
}

/**
 * Clone a set
 */
static set_t
set_clone(set_t source)
{
    set_t set = (set_t)malloc(sizeof(struct set));
    set->dd = source->dd;
    listdd_protect(&set->dd);
    return set;
}

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
 * Get the first variable of the transition relation
 */
static int
get_first(LISTDD meta)
{
    uint32_t val = listdd_node_value(meta);
    if (val != 0) return 0;
    return 1+get_first(listdd_follow(meta, val));
}

/**
 * Print a single example of a set to stdout
 */
static void
print_example(LISTDD example)
{
    if (example != listdd_empty) {
        uint32_t* vec = (uint32_t*)malloc((size_t)vector_size * sizeof(*vec));
        listdd_pick_values(example, vec, vector_size);

        printf("[");
        for (int i=0; i<vector_size; i++) {
            if (i>0) printf(",");
            printf("%" PRIu32, vec[i]);
        }
        printf("]");
        free(vec);
    }
}

static void
print_matrix(size_t size, LISTDD meta)
{
    if (size == 0) return;
    uint32_t val = listdd_node_value(meta);
    if (val == 1) {
        printf("+");
        print_matrix(size-1, listdd_follow(listdd_follow(meta, 1), 2));
    } else {
        if (val == (uint32_t)-1) printf("-");
        else if (val == 0) printf("-");
        else if (val == 3) printf("r");
        else if (val == 4) printf("w");
        print_matrix(size-1, listdd_follow(meta, val));
    }
}

/**
 * Implement parallel strategy (that performs the relprod operations in parallel)
 */
TASK(int, go_par, LISTDD*, result, LISTDD, cur, LISTDD, visited, size_t, from, size_t, len, LISTDD*, deadlocks)
int go_par_CALL(lace_worker* lace, LISTDD *destination, LISTDD cur, LISTDD visited, size_t from, size_t len, LISTDD* deadlocks)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;
    if (len == 1) {
        // Calculate NEW successors (not in visited)
        LISTDD succ = listdd_invalid;
        LISTDD anc = listdd_invalid;
        LISTDD computed = listdd_invalid;
        LISTDD updated_deadlocks = listdd_invalid;
        listdd_refs_pushptr(&succ);
        listdd_refs_pushptr(&anc);
        listdd_refs_pushptr(&computed);
        listdd_refs_pushptr(&updated_deadlocks);
        int status = listdd_rel_next_CALL(lace, &succ, cur, next[from]->dd, next[from]->meta);
        if (deadlocks) {
            // check which MDDs in deadlocks do not have a successor in this relation
            if (status == SYLVAN_OK) {
                status = listdd_rel_prev_CALL(lace, &anc, succ, next[from]->dd, next[from]->meta, cur);
            }
            if (status == SYLVAN_OK) {
                status = listdd_diff_CALL(lace, &updated_deadlocks, *deadlocks, anc);
            }
        }
        if (status == SYLVAN_OK) status = listdd_diff_CALL(lace, &computed, succ, visited);
        if (status == SYLVAN_OK) {
            if (deadlocks) *deadlocks = updated_deadlocks;
            *destination = computed;
        }
        listdd_refs_popptr(4);
        return status;
    } else if (deadlocks != NULL) {
        LISTDD deadlocks_left = *deadlocks;
        LISTDD deadlocks_right = *deadlocks;
        LISTDD left = listdd_invalid;
        LISTDD right = listdd_invalid;
        LISTDD computed = listdd_invalid;
        LISTDD updated_deadlocks = listdd_invalid;
        listdd_refs_pushptr(&deadlocks_left);
        listdd_refs_pushptr(&deadlocks_right);
        listdd_refs_pushptr(&left);
        listdd_refs_pushptr(&right);
        listdd_refs_pushptr(&computed);
        listdd_refs_pushptr(&updated_deadlocks);

        // Recursively compute left+right
        go_par_SPAWN(lace, &left, cur, visited, from, len/2, &deadlocks_left);
        int status = go_par_CALL(lace, &right, cur, visited, from+len/2, len-len/2, &deadlocks_right);
        int left_status = go_par_SYNC(lace);
        if (status == SYLVAN_OK) status = left_status;

        // Merge results of left+right
        if (status == SYLVAN_OK) status = listdd_union_CALL(lace, &computed, left, right);

        // Intersect deadlock sets
        if (status == SYLVAN_OK) {
            status = listdd_intersection_CALL(lace, &updated_deadlocks, deadlocks_left, deadlocks_right);
        }
        if (status == SYLVAN_OK) {
            *deadlocks = updated_deadlocks;
            *destination = computed;
        }
        listdd_refs_popptr(6);
        return status;
    } else {
        LISTDD left = listdd_invalid;
        LISTDD right = listdd_invalid;
        LISTDD computed = listdd_invalid;
        listdd_refs_pushptr(&left);
        listdd_refs_pushptr(&right);
        listdd_refs_pushptr(&computed);

        // Recursively compute left+right
        go_par_SPAWN(lace, &left, cur, visited, from, len/2, NULL);
        int status = go_par_CALL(lace, &right, cur, visited, from+len/2, len-len/2, NULL);
        int left_status = go_par_SYNC(lace);
        if (status == SYLVAN_OK) status = left_status;

        // Merge results of left+right
        if (status == SYLVAN_OK) status = listdd_union_CALL(lace, &computed, left, right);
        if (status == SYLVAN_OK) *destination = computed;
        listdd_refs_popptr(3);
        return status;
    }
}

/**
 * Implementation of the PAR strategy
 */
TASK(void, par, set_t, set)
void par_CALL(lace_worker* lace, set_t set)
{
    /* Prepare variables */
    LISTDD visited = set->dd;
    LISTDD front = visited;
    listdd_refs_pushptr(&visited);
    listdd_refs_pushptr(&front);

    int iteration = 1;
    do {
        if (check_deadlocks) {
            // compute successors in parallel
            LISTDD deadlocks = front;
            listdd_refs_pushptr(&deadlocks);
            if (go_par_CALL(lace, &front, front, visited, 0, next_count, &deadlocks) != SYLVAN_OK) {
                Abort("Parallel ListDD exploration failed.\n");
            }
            listdd_refs_popptr(1);

            if (deadlocks != listdd_empty) {
                INFO("Found %0.0Lf deadlock states... ", listdd_count_CALL(lace, deadlocks));
                printf("example: ");
                print_example(deadlocks);
                printf("\n");
                check_deadlocks = 0;
            }
        } else {
            // compute successors in parallel
            if (go_par_CALL(lace, &front, front, visited, 0, next_count, NULL) != SYLVAN_OK) {
                Abort("Parallel ListDD exploration failed.\n");
            }
        }

        // visited = visited + front
        visited = listdd_union_or_abort_CALL(lace, visited, front);

        INFO("Level %d done", iteration);
        if (report_levels) {
            printf(", %0.0Lf states explored", listdd_count_CALL(lace, visited));
        }
        if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            printf(", table: %0.1f%% full (%zu nodes)", 100.0*(double)filled/total, filled);
        }
        char buf[32];
        to_h((double)getCurrentRSS(), buf);
        printf(", rss=%s.\n", buf);
        iteration++;
    } while (front != listdd_empty);

    set->dd = visited;
    listdd_refs_popptr(2);
}

/**
 * Implement sequential strategy (that performs the relprod operations one by one)
 */
TASK(int, go_bfs, LISTDD*, result, LISTDD, cur, LISTDD, visited, size_t, from, size_t, len, LISTDD*, deadlocks)
int go_bfs_CALL(lace_worker* lace, LISTDD *destination, LISTDD cur, LISTDD visited, size_t from, size_t len, LISTDD* deadlocks)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;
    if (len == 1) {
        // Calculate NEW successors (not in visited)
        LISTDD succ = listdd_invalid;
        LISTDD anc = listdd_invalid;
        LISTDD computed = listdd_invalid;
        LISTDD updated_deadlocks = listdd_invalid;
        listdd_refs_pushptr(&succ);
        listdd_refs_pushptr(&anc);
        listdd_refs_pushptr(&computed);
        listdd_refs_pushptr(&updated_deadlocks);
        int status = listdd_rel_next_CALL(lace, &succ, cur, next[from]->dd, next[from]->meta);
        if (deadlocks) {
            // check which MDDs in deadlocks do not have a successor in this relation
            if (status == SYLVAN_OK) {
                status = listdd_rel_prev_CALL(lace, &anc, succ, next[from]->dd, next[from]->meta, cur);
            }
            if (status == SYLVAN_OK) {
                status = listdd_diff_CALL(lace, &updated_deadlocks, *deadlocks, anc);
            }
        }
        if (status == SYLVAN_OK) status = listdd_diff_CALL(lace, &computed, succ, visited);
        if (status == SYLVAN_OK) {
            if (deadlocks) *deadlocks = updated_deadlocks;
            *destination = computed;
        }
        listdd_refs_popptr(4);
        return status;
    } else if (deadlocks != NULL) {
        LISTDD deadlocks_left = *deadlocks;
        LISTDD deadlocks_right = *deadlocks;
        LISTDD left = listdd_invalid;
        LISTDD right = listdd_invalid;
        LISTDD computed = listdd_invalid;
        LISTDD updated_deadlocks = listdd_invalid;
        listdd_refs_pushptr(&deadlocks_left);
        listdd_refs_pushptr(&deadlocks_right);
        listdd_refs_pushptr(&left);
        listdd_refs_pushptr(&right);
        listdd_refs_pushptr(&computed);
        listdd_refs_pushptr(&updated_deadlocks);

        // Recursively compute left+right
        int status = go_par_CALL(lace, &left, cur, visited, from, len/2, &deadlocks_left);
        if (status == SYLVAN_OK) {
            status = go_par_CALL(lace, &right, cur, visited, from+len/2, len-len/2, &deadlocks_right);
        }

        // Merge results of left+right
        if (status == SYLVAN_OK) status = listdd_union_CALL(lace, &computed, left, right);

        // Intersect deadlock sets
        if (status == SYLVAN_OK) {
            status = listdd_intersection_CALL(lace, &updated_deadlocks, deadlocks_left, deadlocks_right);
        }
        if (status == SYLVAN_OK) {
            *deadlocks = updated_deadlocks;
            *destination = computed;
        }
        listdd_refs_popptr(6);
        return status;
    } else {
        LISTDD left = listdd_invalid;
        LISTDD right = listdd_invalid;
        LISTDD computed = listdd_invalid;
        listdd_refs_pushptr(&left);
        listdd_refs_pushptr(&right);
        listdd_refs_pushptr(&computed);

        // Recursively compute left+right
        int status = go_par_CALL(lace, &left, cur, visited, from, len/2, NULL);
        if (status == SYLVAN_OK) {
            status = go_par_CALL(lace, &right, cur, visited, from+len/2, len-len/2, NULL);
        }

        // Merge results of left+right
        if (status == SYLVAN_OK) status = listdd_union_CALL(lace, &computed, left, right);
        if (status == SYLVAN_OK) *destination = computed;
        listdd_refs_popptr(3);
        return status;
    }
}

/* BFS strategy, sequential strategy (but operations are parallelized by Sylvan) */
TASK(void, bfs, set_t, set)
void bfs_CALL(lace_worker* lace, set_t set)
{
    /* Prepare variables */
    LISTDD visited = set->dd;
    LISTDD front = visited;
    listdd_refs_pushptr(&visited);
    listdd_refs_pushptr(&front);

    int iteration = 1;
    do {
        if (check_deadlocks) {
            // compute successors
            LISTDD deadlocks = front;
            listdd_refs_pushptr(&deadlocks);
            if (go_bfs_CALL(lace, &front, front, visited, 0, next_count, &deadlocks) != SYLVAN_OK) {
                Abort("Sequential ListDD exploration failed.\n");
            }
            listdd_refs_popptr(1);

            if (deadlocks != listdd_empty) {
                INFO("Found %0.0Lf deadlock states... ", listdd_count_CALL(lace, deadlocks));
                printf("example: ");
                print_example(deadlocks);
                printf("\n");
                check_deadlocks = 0;
            }
        } else {
            // compute successors
            if (go_bfs_CALL(lace, &front, front, visited, 0, next_count, NULL) != SYLVAN_OK) {
                Abort("Sequential ListDD exploration failed.\n");
            }
        }

        // visited = visited + front
        visited = listdd_union_or_abort_CALL(lace, visited, front);

        INFO("Level %d done", iteration);
        if (report_levels) {
            printf(", %0.0Lf states explored", listdd_count_CALL(lace, visited));
        }
        if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            printf(", table: %0.1f%% full (%zu nodes)", 100.0*(double)filled/total, filled);
        }
        char buf[32];
        to_h((double)getCurrentRSS(), buf);
        printf(", rss=%s.\n", buf);
        iteration++;
    } while (front != listdd_empty);

    set->dd = visited;
    listdd_refs_popptr(2);
}

/**
 * Implementation of (parallel) saturation
 * (assumes relations are ordered on first variable)
 */
TASK(int, go_sat, LISTDD*, result, LISTDD, set, int, idx, int, depth)
int go_sat_CALL(lace_worker* lace, LISTDD *destination, LISTDD set, int idx, int depth)
{
    if (destination == NULL) return SYLVAN_ERR_INVALID;
    /* Terminal cases */
    if (set == listdd_empty || idx == next_count) {
        *destination = set;
        return SYLVAN_OK;
    }

    /* Consult the cache */
    LISTDD computed = listdd_invalid;
    const LISTDD _set = set;
    if (cache_get3(201LL<<40, _set, idx, 0, &computed)) {
        *destination = computed;
        return SYLVAN_OK;
    }
    listdd_refs_pushptr(&_set);
    listdd_refs_pushptr(&computed);

    /**
     * Possible improvement: cache more things (like intermediate results?)
     *   and chain-apply more of the current level before going deeper?
     */

    /* Check if the relation should be applied */
    const int var = next[idx]->firstvar;
    assert(depth <= var);
    if (depth == var) {
        /* Count the number of relations starting here */
        int n = 1;
        while ((idx + n) < next_count && var == next[idx + n]->firstvar) n++;
        /*
         * Compute until fixpoint:
         * - SAT deeper
         * - chain-apply all current level once
         */
        LISTDD prev = listdd_empty;
        listdd_refs_pushptr(&set);
        listdd_refs_pushptr(&prev);
        int status = SYLVAN_OK;
        while (prev != set) {
            prev = set;
            // SAT deeper
            status = go_sat_CALL(lace, &set, set, idx + n, depth);
            if (status != SYLVAN_OK) break;
            // chain-apply all current level once
            for (int i=0; i<n; i++) {
                set = listdd_rel_next_union_or_abort_CALL(lace, set, next[idx+i]->dd, next[idx+i]->topmeta, set);
            }
        }
        listdd_refs_popptr(2);
        if (status != SYLVAN_OK) { listdd_refs_popptr(2); return status; }
        computed = set;
    } else {
        /* Recursive computation */
        LISTDD down = listdd_invalid;
        LISTDD right = listdd_invalid;
        listdd_refs_pushptr(&down);
        listdd_refs_pushptr(&right);
        go_sat_SPAWN(lace, &right, listdd_node_right(set), idx, depth);
        int status = go_sat_CALL(lace, &down, listdd_node_down(set), idx, depth+1);
        int right_status = go_sat_SYNC(lace);
        if (status == SYLVAN_OK) status = right_status;
        if (status == SYLVAN_OK) {
            status = _listdd_try_make_node(&computed, listdd_node_value(set), down, right);
        }
        listdd_refs_popptr(2);
        if (status != SYLVAN_OK) { listdd_refs_popptr(2); return status; }
    }

    /* Store in cache */
    cache_put3(201LL<<40, _set, idx, 0, computed);
    *destination = computed;
    listdd_refs_popptr(2);
    return SYLVAN_OK;
}

/**
 * Wrapper for the Saturation strategy
 */
TASK(void, sat, set_t, set)
void sat_CALL(lace_worker* lace, set_t set)
{
    if (go_sat_CALL(lace, &set->dd, set->dd, 0, 0) != SYLVAN_OK) {
        Abort("ListDD saturation failed.\n");
    }
}

/**
 * Implementation of the Chaining strategy (does not support deadlock detection)
 */
TASK(void, chaining, set_t, set)
void chaining_CALL(lace_worker* lace, set_t set)
{
    LISTDD visited = set->dd;
    LISTDD front = visited;
    LISTDD succ = listdd_empty;

    listdd_refs_pushptr(&visited);
    listdd_refs_pushptr(&front);
    listdd_refs_pushptr(&succ);

    int iteration = 1;
    do {
        // calculate successors in parallel
        for (int i=0; i<next_count; i++) {
            succ = listdd_rel_next_or_abort_CALL(lace, front, next[i]->dd, next[i]->meta);
            front = listdd_union_or_abort_CALL(lace, front, succ);
            succ = listdd_empty; // reset, for gc
        }

        // front = front - visited
        // visited = visited + front
        front = listdd_diff_or_abort_CALL(lace, front, visited);
        visited = listdd_union_or_abort_CALL(lace, visited, front);

        INFO("Level %d done", iteration);
        if (report_levels) {
            printf(", %0.0Lf states explored", listdd_count(visited));
        }
        if (report_table) {
            size_t filled, total;
            sylvan_table_usage(&filled, &total);
            printf(", table: %0.1f%% full (%zu nodes)", 100.0*(double)filled/total, filled);
        }
        char buf[32];
        to_h((double)getCurrentRSS(), buf);
        printf(", rss=%s.\n", buf);
        iteration++;
    } while (front != listdd_empty);

    set->dd = visited;
    listdd_refs_popptr(3);
    (void)lace;
}

TASK(void, gc_start)
void gc_start_CALL(lace_worker* lace)
{
    char buf[32];
    to_h((double)getCurrentRSS(), buf);
    INFO("(GC) Starting garbage collection... (rss: %s)\n", buf);
    (void)lace;
}

TASK(void, gc_end)
void gc_end_CALL(lace_worker* lace)
{
    char buf[32];
    to_h((double)getCurrentRSS(), buf);
    INFO("(GC) Garbage collection done.       (rss: %s)\n", buf);
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

TASK(int, run)
int run_CALL(lace_worker* lace)
{
    /**
     * Read the model from file
     */

    FILE *f = fopen(model_filename, "rb");
    if (f == NULL) {
        Abort("Cannot open file '%s'!\n", model_filename);
        return -1;
    }

    /* Read domain data */
    if (fread(&vector_size, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");

    /* Read initial state */
    set_t initial = set_load(f);

    /* Read number of transition relations */
    if (fread(&next_count, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    next = (rel_t*)malloc(sizeof(rel_t) * next_count);

    /* Read transition relations */
    for (int i=0; i<next_count; i++) next[i] = rel_load_proj(f);
    for (int i=0; i<next_count; i++) rel_load(f, next[i]);

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
            if ((*q)->firstvar > (*p)->firstvar) {
                t = *q;
                *q = *p;
                *p = t;
                if (--i) continue;
            }
            i = j++;
        }
    }

    INFO("Read file '%s'\n", model_filename);
    INFO("%d integers per state, %d transition groups\n", vector_size, next_count);

    if (print_transition_matrix) {
        for (int i=0; i<next_count; i++) {
            INFO("");
            print_matrix(vector_size, next[i]->meta);
            printf(" (%d)\n", get_first(next[i]->meta));
        }
    }

    set_t states = set_clone(initial);

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
    INFO("Final states: %0.0Lf states\n", listdd_count(states->dd));
    if (report_nodes) {
        INFO("Final states: %zu LISTDD nodes\n", listdd_node_count(states->dd));
    }

    if (out_filename != NULL) {
        INFO("Writing to %s.\n", out_filename);

        // Create LDD file
        FILE *f = fopen(out_filename, "w");
        listdd_serialize_reset();

        // Write domain...
        fwrite(&vector_size, sizeof(int), 1, f);

        // Write initial state...
        set_save(f, initial);

        // Write number of transitions
        fwrite(&next_count, sizeof(int), 1, f);

        // Write transitions
        for (int i=0; i<next_count; i++) rel_save_proj(f, next[i]);
        for (int i=0; i<next_count; i++) rel_save(f, next[i]);

        // Write reachable states
        int has_reachable = 1;
        fwrite(&has_reachable, sizeof(int), 1, f);
        set_save(f, states);

        // Write action labels
        fclose(f);
    }

    set_free(states);
    set_free(initial);

    return 0;
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

    sylvan_set_limits(max, 1, 16);
    sylvan_init_package();
    listdd_init();
    sylvan_gc_hook_pregc(gc_start_CALL);
    sylvan_gc_hook_postgc(gc_end_CALL);

    run();

    print_memory_usage();
    sylvan_stats_report(stdout);

    sylvan_quit();
    lace_stop();
}
