#include <argp.h>
#include <inttypes.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

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
static BDD action_variables = mtbdd_true;
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
    BDD sets[2];
    if (mtbdd_reader_frombinary(f, sets, 2) != 0) Abort("Invalid input file!\n");

    size_t set_state_vars;
    if (fread(&set_state_vars, sizeof(size_t), 1, f) != 1) Abort("Invalid input file!\n");

    set_t set = (set_t)malloc(sizeof(struct set));
    set->bdd = sets[0];
    set->variables = sets[1];

    sylvan_protect(&set->bdd);
    sylvan_protect(&set->variables);

    return set;
}

/* Load a relation from file */
#define rel_load_proj(f) CALL(rel_load_proj, f)
TASK_1(rel_t, rel_load_proj, FILE*, f)
{
    int r_k, w_k;
    if (fread(&r_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");
    if (fread(&w_k, sizeof(int), 1, f) != 1) Abort("Invalid file format.");
    int r_proj[r_k], w_proj[w_k];
    if (fread(r_proj, sizeof(int), r_k, f) != (size_t)r_k) Abort("Invalid file format.");
    if (fread(w_proj, sizeof(int), w_k, f) != (size_t)w_k) Abort("Invalid file format.");

    /** create variables */
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

    /* Compute all_variables, which are all variables the transition relation is defined on */
    // printf("read: ");
    // for (int i=0; i<r_i; i++) printf("%d ", r_proj[i]);
    // printf("write: ");
    // for (int i=0; i<w_i; i++) printf("%d ", w_proj[i]);
    // printf("\n");

    /* Compute all_variables, which are all variables the transition relation is defined on */
    BDDVAR all_vars[bits_per_integer * a_i * 2];
    for (int i=0; i<a_i; i++) {
        for (int j=0; j<bits_per_integer; j++) {
            all_vars[2*(i*bits_per_integer+j)] = 2*(a_proj[i]*bits_per_integer+j);
            all_vars[2*(i*bits_per_integer+j)+1] = 2*(a_proj[i]*bits_per_integer+j)+1;
        }
    }
    BDD all_variables = sylvan_set_fromarray(all_vars, bits_per_integer * a_i * 2);

    rel_t rel = (rel_t)malloc(sizeof(struct relation));
    rel->bdd = mtbdd_false;
    rel->variables = mtbdd_false;
    sylvan_protect(&rel->bdd);
    sylvan_protect(&rel->variables);

    rel->variables = sylvan_set_addall(all_variables, action_variables);

    return rel;
}

#define rel_load(f, rel) CALL(rel_load, f, rel)
VOID_TASK_2(rel_load, FILE*, f, rel_t, rel)
{
    if (mtbdd_reader_frombinary(f, &rel->bdd, 1) != 0) Abort("Invalid file format.");
}

int rule = 0;
int comp = 0;

BDD
_makenode(uint32_t var, BDD low, BDD high)
{
    switch (comp) {
    case 0:
        break;
    case 1:
        if (MTBDD_HASMARK(low)) {
            return MTBDD_TOGGLEMARK(_makenode(var, MTBDD_TOGGLEMARK(low), high));
        }
        break;
    case 2:
        if (MTBDD_HASMARK(low)) {
            return MTBDD_TOGGLEMARK(_makenode(var, MTBDD_TOGGLEMARK(low), MTBDD_TOGGLEMARK(high)));
        }
        break;
    case 3:
        if (MTBDD_HASMARK(high)) {
            return MTBDD_TOGGLEMARK(_makenode(var, low, MTBDD_TOGGLEMARK(high)));
        }
        break;
    }

    switch (rule) {
    case 0:
        break;
    case 1:
        if (low == high) return low;
        break;
    case 2:
        if (high == mtbdd_false) return low;
        break;
    case 3:
        if (high == mtbdd_true || high == 1) return low;
        break;
    case 4:
        if (low == mtbdd_false) return high;
        break;
    case 5:
        if (low == mtbdd_true || low == 1) return high;
        break;
    case 6:
        if (low == MTBDD_TOGGLEMARK(high)) return low;
        break;
    }

    struct mtbddnode n;
    if (comp != 3) mtbddnode_makenode(&n, var, low, high);
    else mtbddnode_makenode(&n, var, high, low);

    int created;
    uint64_t index = llmsset_lookup(nodes, n.a, n.b, &created);
    if (index == 0) {
        LACE_ME;

        mtbdd_refs_push(low);
        mtbdd_refs_push(high);
        sylvan_gc();
        mtbdd_refs_pop(2);

        index = llmsset_lookup(nodes, n.a, n.b, &created);
        if (index == 0) {
            fprintf(stderr, "BDD Unique table full, %zu of %zu buckets filled!\n", llmsset_count_marked(nodes), llmsset_get_size(nodes));
            exit(1);
        }
    }

    MTBDD result = index;
    return result;
}

/**
 * Convert an MTBDD
 */
TASK_2(MTBDD, _from_mtbdd, MTBDD, dd, MTBDD, dom)
{
    /* Special treatment for True and False */
    if (dom == mtbdd_true) {
        if (dd == mtbdd_true) return 1;
        if (dd == mtbdd_false) return 0;
        Abort("MTBDD has more variables than expected!\n");
    }

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Check cache */
    MTBDD result;
    if (cache_get3(100LL<<40, dd, dom, rule, &result)) return result;

    /* Get variable and cofactors */
    mtbddnode_t dd_node = MTBDD_STRIPMARK(dd) > 1 ? MTBDD_GETNODE(dd) : NULL;
    uint32_t dd_var = dd_node != NULL ? mtbddnode_getvariable(dd_node) : 0xffffffff;

    mtbddnode_t dom_node = MTBDD_GETNODE(dom);
    uint32_t dom_var = mtbddnode_getvariable(dom_node);

    /* Get variable and cofactors */
    MTBDD dd0, dd1;
    if (dom_var == dd_var) {
        dd0 = mtbddnode_followlow(dd, dd_node);
        dd1 = mtbddnode_followhigh(dd, dd_node);
    } else {
        dd0 = dd1 = dd;
    }

    /* Recursive */
    MTBDD dom_next = mtbddnode_followhigh(dom, dom_node);
    mtbdd_refs_spawn(SPAWN(_from_mtbdd, dd1, dom_next));
    TBDD low = mtbdd_refs_push(CALL(_from_mtbdd, dd0, dom_next));
    TBDD high = mtbdd_refs_sync(SYNC(_from_mtbdd));
    mtbdd_refs_pop(1);
    result = _makenode(dom_var, low, high);

    /* Store in cache */
    cache_put3(100LL<<40, dd, dom, rule, result);

    return result;
}

#define reduce(dd) CALL(reduce, dd)
TASK_1(MTBDD, reduce, MTBDD, dd)
{
    /* Special treatment for True and False */
    if (dd == 0) return mtbdd_false;
    if (dd == 1) return comp ? mtbdd_true : 1;

    /* Maybe perform garbage collection */
    sylvan_gc_test();

    /* Check cache */
    MTBDD result;
    if (cache_get3(102LL<<40, dd, comp, rule, &result)) return result;

    /* Get variable and cofactors */
    mtbddnode_t dd_node = MTBDD_GETNODE(dd);
    uint32_t dd_var = mtbddnode_getvariable(dd_node);

    /* Get variable and cofactors */
    MTBDD dd0 = mtbddnode_followlow(dd, dd_node);
    MTBDD dd1 = mtbddnode_followhigh(dd, dd_node);

    /* Recursive */
    mtbdd_refs_spawn(SPAWN(reduce, dd1));
    TBDD low = mtbdd_refs_push(CALL(reduce, dd0));
    TBDD high = mtbdd_refs_sync(SYNC(reduce));
    mtbdd_refs_pop(1);
    result = _makenode(dd_var, low, high);

    /* Store in cache */
    cache_put3(102LL<<40, dd, comp, rule, result);

    return result;
}

#define check_support(dd, dom) CALL(check_support, dd, dom)
TASK_2(int, check_support, MTBDD, dd, MTBDD, dom)
{
    BDD supp = sylvan_support(dd);
    mtbdd_refs_push(supp);
    int good = sylvan_and(supp, dom) == dom ? 1 : 0;
    mtbdd_refs_pop(1);
    return good;
}

#define check_no_skip(dd, dom) CALL(check_no_skip, dd, dom)
TASK_2(int, check_no_skip, MTBDD, dd, MTBDD, dom)
{
    /* Special treatment for True and False */
    if (dd <= 1) return dom == mtbdd_true ? 1 : 0;
    if (dom == mtbdd_true) return 0;

    /* Get variable and cofactors */
    mtbddnode_t dd_node = MTBDD_GETNODE(dd);
    uint32_t dd_var = mtbddnode_getvariable(dd_node);

    mtbddnode_t dom_node = MTBDD_GETNODE(dom);
    uint32_t dom_var = mtbddnode_getvariable(dom_node);

    if (dd_var != dom_var) return 0;

    /* Check cache */
    uint64_t result;
    if (cache_get3(101LL<<40, dd, dom, 0, &result)) return result ? 1 : 0;

    /* Get variable and cofactors */
    MTBDD dd0 = mtbddnode_followlow(dd, dd_node);
    MTBDD dd1 = mtbddnode_followhigh(dd, dd_node);
    MTBDD dom_next = mtbddnode_followhigh(dom, dom_node);

    /* Recursive */
    SPAWN(check_no_skip, dd1, dom_next);
    int low = CALL(check_no_skip, dd0, dom_next);
    int high = SYNC(check_no_skip);
    result = (low && high) ? 1 : 0;

    /* Store in cache */
    cache_put3(101LL<<40, dd, dom, 0, result);

    return result ? 1 : 0;
}

static void
print_matrix(BDD vars)
{
    for (int i=0; i<vector_size; i++) {
        if (sylvan_set_isempty(vars)) {
            fprintf(stdout, "-");
        } else {
            BDDVAR next_s = 2*((i+1)*bits_per_integer);
            if (sylvan_set_first(vars) < next_s) {
                fprintf(stdout, "+");
                for (;;) {
                    vars = sylvan_set_next(vars);
                    if (sylvan_set_isempty(vars)) break;
                    if (sylvan_set_first(vars) >= next_s) break;
                }
            } else {
                fprintf(stdout, "-");
            }
        }
    }
}

VOID_TASK_1(bdd_unmark_rec, BDD, dd)
{
    if (dd <= 1) return;
    mtbddnode_t dd_node = MTBDD_GETNODE(dd);
    if (!mtbddnode_getmark(dd_node)) return;
    mtbddnode_setmark(dd_node, 0);
    SPAWN(bdd_unmark_rec, mtbddnode_getlow(dd_node));
    CALL(bdd_unmark_rec, mtbddnode_gethigh(dd_node));
    SYNC(bdd_unmark_rec);
}

VOID_TASK_1(bdd_mark_rec, BDD, dd)
{
    if (dd <= 1) return;
    mtbddnode_t dd_node = MTBDD_GETNODE(dd);
    if (mtbddnode_getmark(dd_node)) return;
    mtbddnode_setmark(dd_node, 1);
    SPAWN(bdd_mark_rec, mtbddnode_getlow(dd_node));
    CALL(bdd_mark_rec, mtbddnode_gethigh(dd_node));
    SYNC(bdd_mark_rec);
}

size_t
count_marked_nodes()
{
    size_t total = 0;
    size_t n = llmsset_get_size(nodes);
    for (size_t idx=2; idx<n; idx++) {
        if (llmsset_is_marked(nodes, idx)) {
            mtbddnode_t node = (mtbddnode_t)llmsset_index_to_ptr(nodes, idx);
            if (!mtbddnode_getmark(node)) continue;
            total++;
        }
    }
    return total;
}

VOID_TASK_0(gc_start)
{
    // INFO("(GC) Starting garbage collection...\n");
}

VOID_TASK_0(gc_end)
{
    // INFO("(GC) Garbage collection done.\n");
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

    /* Compute action_variables */
    sylvan_protect(&action_variables);
    for (int i=0; i<actionbits; i++) {
        action_variables = mtbdd_makenode(1000000+(actionbits-i-1), mtbdd_false, action_variables);
    }

    /* Read initial state */
    set_t initial = set_load(f);

    // Read number of transitions
    if (fread(&next_count, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    next = (rel_t*)malloc(sizeof(rel_t) * next_count);

    for (int i=0; i<next_count; i++) next[i] = rel_load_proj(f);
    for (int i=0; i<next_count; i++) rel_load(f, next[i]);

    /* Read visited set */
    int save_reachable;
    if (fread(&save_reachable, sizeof(int), 1, f) != 1) Abort("Invalid input file!\n");
    if (save_reachable != 1) Abort("Invalid input file!\n");

    set_t visited = set_load(f);

    /* Done */
    fclose(f);

    /* Check supports */
    // printf("%d\n", check_support(initial->bdd, initial->variables));
    // printf("%d\n", check_support(visited->bdd, visited->variables));
    // for (int i=0; i<next_count; i++) printf("%d\n", check_support(next[i]->bdd, next[i]->variables));

    // Report statistics
    INFO("Read file '%s'\n", model_filename);
    INFO("%d integers per state, %d bits per integer, %d transition groups\n", vector_size, bits_per_integer, next_count);

    for (int i=0; i<next_count; i++) {
        INFO("");
        print_matrix(next[i]->variables);
        fprintf(stdout, "\n");
    }

    size_t count_as_bdd;
    {
        BDD all_bdd[2+next_count];
        all_bdd[0] = initial->bdd;
        all_bdd[1] = visited->bdd;
        for (int i=0; i<next_count; i++) all_bdd[2+i] = next[i]->bdd;
        count_as_bdd = mtbdd_nodecount_more(all_bdd, 2+next_count);
    }

    TBDD all_tbdd[2+next_count];

    INFO("BDD nodes:\n");
    {
        all_tbdd[0] = tbdd_from_mtbdd(initial->bdd, initial->variables);
        size_t before = sylvan_nodecount(initial->bdd);
        initial->bdd = CALL(_from_mtbdd, initial->bdd, initial->variables);
        // if (!check_no_skip(initial->bdd, initial->variables)) Abort("Failure (initial)\n");
        size_t after = sylvan_nodecount(initial->bdd);
        INFO("Initial states: %zu => %zu nodes\n", before, after);
    }
    {
        all_tbdd[1] = tbdd_from_mtbdd(visited->bdd, visited->variables);
        size_t before = sylvan_nodecount(visited->bdd);
        visited->bdd = CALL(_from_mtbdd, visited->bdd, visited->variables);
        // if (!check_no_skip(initial->bdd, initial->variables)) Abort("Failure (visited)\n");
        size_t after = sylvan_nodecount(visited->bdd);
        INFO("Visited states: %zu => %zu nodes\n", before, after);
    }
    for (int i=0; i<next_count; i++) {
        all_tbdd[2+i] = tbdd_from_mtbdd(next[i]->bdd, next[i]->variables);
        size_t before = sylvan_nodecount(next[i]->bdd);
        next[i]->bdd = CALL(_from_mtbdd, next[i]->bdd, next[i]->variables);
        // if (!check_no_skip(initial->bdd, initial->variables)) Abort("Failure (next[%d])\n", i);
        size_t after = sylvan_nodecount(next[i]->bdd);
        INFO("Transition %d: %zu => %zu nodes\n", i, before, after);
    }

    /* compute total node counts */
    {
        BDD all_dd[2+next_count];
        all_dd[0] = initial->bdd;
        all_dd[1] = visited->bdd;
        for (int i=0; i<next_count; i++) all_dd[2+i] = next[i]->bdd;
        INFO("All BDDs: %'zu nodes\n", count_as_bdd);
        INFO("All DDs: %'zu nodes\n", mtbdd_nodecount_more(all_dd, 2+next_count));
        INFO("All TBDDs: %'zu nodes\n", tbdd_nodecount_more(all_tbdd, 2+next_count));
    }

    size_t t_as_tbdd = tbdd_nodecount_more(&all_tbdd[2], next_count);
    size_t v_as_tbdd = tbdd_nodecount_more(&all_tbdd[1], 1);

    {
        size_t tresults[8][4];
        size_t vresults[8][4];
        for (int r = 0; r < 6; r++) {
            rule = r;
            for (int c = 0; c < 1; c++) {
                comp = c;
                BDD all_dd[2+next_count];

                /* get rid of all other nodes */
                sylvan_gc();
                sylvan_gc_disable();
                for (int i=0; i<next_count; i++) all_dd[i] = reduce(next[i]->bdd);
                sylvan_gc_enable();

                /* count nodes */
                for (int i=0; i<next_count; i++) CALL(bdd_mark_rec, all_dd[i]);
                tresults[r][c] = count_marked_nodes();
                for (int i=0; i<next_count; i++) CALL(bdd_unmark_rec, all_dd[i]);

                /* get rid of all other nodes */
                sylvan_gc();
                sylvan_gc_disable();
                all_dd[0] = reduce(visited->bdd);
                sylvan_gc_enable();

                /* count nodes */
                CALL(bdd_mark_rec, all_dd[0]);
                vresults[r][c] = count_marked_nodes();
                CALL(bdd_unmark_rec, all_dd[0]);
    
                INFO("rule %d comp %d: visited: %'zu, transitions: %'zu nodes\n", r, c, vresults[r][c], tresults[r][c]);
            }
        }

        /**
         * dump to output file
         */
        char fn[strlen(model_filename)+12];
        FILE *f;

        snprintf(fn, strlen(model_filename)+10, "%s.trans", model_filename);
        f = fopen(fn, "w");
        fprintf(f, "[");
        for (int r=0; r<6; r++) {
            fprintf(f, "[");
            for (int c=0; c<1; c++) {
                fprintf(f, "%zu,", tresults[r][c]);
            }
            fprintf(f, "],");
        }
        fprintf(f, "%zu,", t_as_tbdd);
        fprintf(f, "]\n");
        fclose(f);

        snprintf(fn, strlen(model_filename)+10, "%s.visited", model_filename);
        f = fopen(fn, "w");
        fprintf(f, "[");
        for (int r=0; r<6; r++) {
            fprintf(f, "[");
            for (int c=0; c<1; c++) {
                fprintf(f, "%zu,", vresults[r][c]);
            }
            fprintf(f, "],");
        }
        fprintf(f, "%zu,", v_as_tbdd);
        fprintf(f, "]\n");
        fclose(f);
     }

    /* analyse */
    /*
    CALL(bdd_mark_rec, initial->bdd);
    CALL(bdd_mark_rec, visited->bdd);
    for (int i=0; i<next_count; i++) CALL(bdd_mark_rec, next[i]->bdd);

    size_t s = 0, t = 0, total = 0;
    size_t n = llmsset_get_size(nodes);
    for (size_t idx=2; idx<n; idx++) {
        if (llmsset_is_marked(nodes, idx)) {
            mtbddnode_t node = (mtbddnode_t)llmsset_index_to_ptr(nodes, idx);
            if (!mtbddnode_getmark(node)) continue;

            MTBDD low = mtbddnode_getlow(node);
            MTBDD high = mtbddnode_gethigh(node);

            total++;
            if (low == high) s++;
            if (high == mtbdd_false) t++;
        }
    }
    printf("%zu, %zu, %zu\n", s, t, total);
    */

    // sylvan_stats_report(stdout);

    return 0;
}
