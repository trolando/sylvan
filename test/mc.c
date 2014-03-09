#include <stdio.h>
#include <stdlib.h>
#include <sylvan.h>
#include <inttypes.h>
#include <assert.h>
#include <sys/time.h>
#include <llmsset.h>

static int report = 0;
static int report_table = 0;
static int run_par = 1;

double wctime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

typedef struct vector_domain *vdom_t;
typedef struct vector_set *vset_t;
typedef struct vector_relation *vrel_t;

struct vector_domain
{
    size_t vector_size;
    size_t bits_per_integer;
    BDDVAR *vec_to_bddvar;      // Translation of bit to BDDVAR for X
    BDDVAR *prime_vec_to_bddvar;// Translation of bit to BDDVAR for X'

    // Generated based on vec_to_bddvar and prime_vec_to_bddvar
    BDD universe;               // Every BDDVAR used for X
    BDD prime_universe;         // Every BDDVAR used for X'
};

struct vector_set
{
    vdom_t dom;

    BDD bdd;                    // Represented BDD
    size_t vector_size;         // How long is the vector in integers
    BDDVAR *vec_to_bddvar;      // Translation of bit to BDDVAR

    // Generated based on vec_to_bddvar and vector_size
    BDD projection;             // Universe \ X (for projection)
    BDD variables;              // X (for satcount etc)
};

struct vector_relation
{
    vdom_t dom;

    BDD bdd;                    // Represented BDD
    size_t vector_size;         // How long is the vector in integers
    BDDVAR *vec_to_bddvar;      // Translation of bit to BDDVAR for X
    BDDVAR *prime_vec_to_bddvar;// Translation of bit to BDDVAR for X'

    // Generated based on vec_to_bddvar and vector_size
    BDD variables;              // X
    BDD prime_variables;        // X'
    BDD all_variables;          // X U X'
};

static vset_t
set_load(FILE* f, vdom_t dom)
{
    vset_t set = (vset_t)malloc(sizeof(struct vector_set));
    set->dom = dom;

    sylvan_serialize_fromfile(f);

    size_t bdd;
    assert(fread(&bdd, sizeof(size_t), 1, f) == 1);
    set->bdd = sylvan_ref(sylvan_serialize_get_reversed(bdd));

    assert(fread(&set->vector_size, sizeof(size_t), 1, f) == 1);
    set->vec_to_bddvar = (BDDVAR*)malloc(sizeof(BDDVAR) * dom->bits_per_integer * set->vector_size);
    assert(fread(set->vec_to_bddvar, sizeof(BDDVAR), dom->bits_per_integer * set->vector_size, f) == dom->bits_per_integer * set->vector_size);

    sylvan_gc_disable();
    set->variables = sylvan_ref(sylvan_set_fromarray(set->vec_to_bddvar, dom->bits_per_integer * set->vector_size));
    sylvan_gc_enable();

    return set;
}

static vrel_t
rel_load(FILE* f, vdom_t dom)
{
    vrel_t rel = (vrel_t)malloc(sizeof(struct vector_relation));
    rel->dom = dom;

    sylvan_serialize_fromfile(f);

    size_t bdd;
    assert(fread(&bdd, sizeof(size_t), 1, f) == 1);
    rel->bdd = sylvan_ref(sylvan_serialize_get_reversed(bdd));

    assert(fread(&rel->vector_size, sizeof(size_t), 1, f) == 1);
    rel->vec_to_bddvar = (BDDVAR*)malloc(sizeof(BDDVAR) * dom->bits_per_integer * rel->vector_size);
    rel->prime_vec_to_bddvar = (BDDVAR*)malloc(sizeof(BDDVAR) * dom->bits_per_integer * rel->vector_size);
    assert(fread(rel->vec_to_bddvar, sizeof(BDDVAR), rel->vector_size*dom->bits_per_integer, f) == rel->vector_size * dom->bits_per_integer);
    assert(fread(rel->prime_vec_to_bddvar, sizeof(BDDVAR), rel->vector_size*dom->bits_per_integer, f) == rel->vector_size * dom->bits_per_integer);

    sylvan_gc_disable();
    rel->variables = sylvan_ref(sylvan_set_fromarray(rel->vec_to_bddvar, dom->bits_per_integer * rel->vector_size));
    rel->prime_variables = sylvan_ref(sylvan_set_fromarray(rel->prime_vec_to_bddvar, dom->bits_per_integer * rel->vector_size));
    rel->all_variables = sylvan_ref(sylvan_set_addall(rel->prime_variables, rel->variables));
    sylvan_gc_enable();

    return rel;
}

static vdom_t domain;
static int nGrps;
static vrel_t *next;

TASK_4(BDD, go_par, BDD, set, BDD, all, size_t, from, size_t, len)
{
    if (len == 1) {
        BDD succ = sylvan_relprods(set, next[from]->bdd, next[from]->all_variables);
        sylvan_ref(succ);
        BDD result = sylvan_diff(succ, all);
        sylvan_deref(succ);
        return result;
    }

    BDD left, right;
    SPAWN(go_par, set, all, from, (len+1)/2);
    right = sylvan_ref(CALL(go_par, set, all, from+(len+1)/2, len/2));
    left = sylvan_ref(SYNC(go_par));
    BDD result = CALL(sylvan_ite, left, sylvan_true, right, 0);
    sylvan_deref(left);
    sylvan_deref(right);
    return result;
}

static void
par(vset_t set)
{
    BDD states = set->bdd;
    BDD new = sylvan_ref(states);
    size_t counter = 1;
    do {
        printf("Level %zu... ", counter++);
        if (report) {
            printf("%zu states\n", (size_t)sylvan_satcount(states, set->variables));
        }
        BDD cur = new;
        new = sylvan_ref(CALL(go_par, cur, states, 0, nGrps));
        sylvan_deref(cur);
        // states = states + new
        BDD temp = sylvan_ref(sylvan_or(states, new));
        sylvan_deref(states);
        states = temp;
        if (report_table) {
            llmsset_t __sylvan_get_internal_data();
            llmsset_t tbl = __sylvan_get_internal_data();
            size_t filled = llmsset_get_filled(tbl);
            size_t total = llmsset_get_size(tbl);
            printf("done, table: %0.1f%% full (%zu nodes).\n", 100.0*(double)filled/total, filled);
        } else {
            printf("done.\n");
        }
    } while (new != sylvan_false);
    sylvan_deref(new);
    set->bdd = states;
}

static void
bfs(vset_t set)
{
    BDD states = set->bdd;
    BDD new = sylvan_ref(states);
    size_t counter = 1;
    do {
        printf("Level %zu... ", counter++);
        if (report) {
            printf("%zu states\n", (size_t)sylvan_satcount(states, set->variables));
        }
        BDD cur = new;
        new = sylvan_false;
        size_t i;
        for (i=0; i<(size_t)nGrps; i++) {
            /*if (!report) {
                printf("%zu, ", i);
                fflush(stdout);
            }*/
            // a = RelProdS(cur, next)
            BDD a = sylvan_ref(sylvan_relprods(cur, next[i]->bdd, next[i]->all_variables));
            // b = a - states
            BDD b = sylvan_ref(sylvan_diff(a, states));
            // report
            if (report) {
                printf("Transition %zu, next has %zu BDD nodes, new has %zu BDD nodes\n", i, sylvan_nodecount(a), sylvan_nodecount(b));
            }
            sylvan_deref(a);
            // new = new + b
            BDD c = sylvan_ref(sylvan_or(b, new));
            sylvan_deref(b);
            sylvan_deref(new);
            new = c;
        }
        sylvan_deref(cur);
        // states = states + new
        BDD temp = sylvan_ref(sylvan_or(states, new));
        sylvan_deref(states);
        states = temp;
        if (report_table) {
            llmsset_t __sylvan_get_internal_data();
            llmsset_t tbl = __sylvan_get_internal_data();
            size_t filled = llmsset_get_filled(tbl);
            size_t total = llmsset_get_size(tbl);
            printf("done, table: %0.1f%% full (%zu nodes).\n", 100.0*(double)filled/total, filled);
        } else {
            printf("done.\n");
        }
    } while (new != sylvan_false);
    sylvan_deref(new);
    set->bdd = states;
}

int
main(int argc, char **argv)
{
    // Filename in argv[0]
    if (argc == 1) {
        fprintf(stderr, "Usage: mc <filename>\n");
        return -1;
    }

    FILE *f = fopen(argv[1], "r");
    if (f == NULL) {
        fprintf(stderr, "Cannot open file '%s'!\n", argv[1]);
        return -1;
    }

    // Init Lace and Sylvan
    // Reasonable defaults: datasize of 26 (2048 MB), cachesize of 24 (576 MB), granularity of 4-16
    // 26: 2GB
    // 30: 32GB
    // 31: 64GB
    // 32: 128GB
    lace_init(4, 100000, 0); // 4 workers
    sylvan_init(25, 24, 4); // 2GB memory

    // Create domain
    domain = (vdom_t)malloc(sizeof(struct vector_domain));

    // Read domain info
    assert(fread(&domain->vector_size, sizeof(size_t), 1, f)==1);
    assert(fread(&domain->bits_per_integer, sizeof(size_t), 1, f)==1);

    printf("Vector size: %zu\n", domain->vector_size);
    printf("Bits per integer: %zu\n", domain->bits_per_integer);

    // Create universe
    domain->vec_to_bddvar = (BDDVAR*)malloc(sizeof(BDDVAR) * domain->bits_per_integer * domain->vector_size);
    domain->prime_vec_to_bddvar = (BDDVAR*)malloc(sizeof(BDDVAR) * domain->bits_per_integer * domain->vector_size);

    assert(fread(domain->vec_to_bddvar, sizeof(BDDVAR), domain->vector_size * domain->bits_per_integer, f) == domain->vector_size * domain->bits_per_integer);
    assert(fread(domain->prime_vec_to_bddvar, sizeof(BDDVAR), domain->vector_size * domain->bits_per_integer, f) == domain->vector_size * domain->bits_per_integer);

    sylvan_gc_disable();
    domain->universe = sylvan_ref(sylvan_set_fromarray(domain->vec_to_bddvar, domain->bits_per_integer * domain->vector_size));
    domain->prime_universe = sylvan_ref(sylvan_set_fromarray(domain->prime_vec_to_bddvar, domain->bits_per_integer * domain->vector_size));
    sylvan_gc_enable();

    printf("Loading initial state... ");
    fflush(stdout);
    vset_t initial = set_load(f, domain);
    printf("done\n");

    // Read transitions
    assert(fread(&nGrps, sizeof(int), 1, f) == 1);
    next = (vrel_t*)malloc(sizeof(vrel_t) * nGrps);

    printf("Loading transition relations... ");
    fflush(stdout);
    size_t i;
    for (i=0; i<(size_t)nGrps; i++) {
        next[i] = rel_load(f, domain);
        printf("%zu, ", i);
        fflush(stdout);
    }
    fclose(f);
    printf("done\n");

    // Report statistics
    printf("Read file '%s'\n", argv[1]);
    printf("%zu integers per state, %zu bits per integer, %d transition groups\n", domain->vector_size, domain->bits_per_integer, nGrps);
    printf("BDD nodes:\n");
    printf("Initial states: %zu BDD nodes\n", sylvan_nodecount(initial->bdd));
    for (i=0; i<(size_t)nGrps; i++) {
        printf("Transition %zu: %zu BDD nodes\n", i, sylvan_nodecount(next[i]->bdd));
    }

    sylvan_gc();

    if (run_par) {
        double t1 = wctime();
        par(initial);
        double t2 = wctime();
        printf("PAR Time: %f\n", t2-t1);
    } else {
        double t1 = wctime();
        bfs(initial);
        double t2 = wctime();
        printf("BFS Time: %f\n", t2-t1);
    }

    // Now we just have states
    BDD states = initial->bdd;
    printf("Final states: %zu states\n", (size_t)sylvan_satcount(states, initial->variables));
    printf("Final states: %zu BDD nodes\n", sylvan_nodecount(states));

    return 0;
}
