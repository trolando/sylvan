#include <argp.h>
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdint.h>

#include <sylvan_int.h>

#include <string>

#include <boost/config.hpp>
#include <boost/graph/sloan_ordering.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/adjacency_list.hpp>

using namespace sylvan;

/**************************************************

TODO
====

Use Boost to provide automatic .bz2 and .gz pipes.
Use dynamic reordering and/or static orders

**************************************************/

/* Configuration */
static int workers = 1; // autodetect
static int verbose = 1;
static char* aag_filename = NULL; // filename of DOT file
static int reorder = 0;

static int sloan_w1 = 1;
static int sloan_w2 = 8;

/* argp configuration */
static struct argp_option options[] =
{
    {"workers", 'w', "<workers>", 0, "Number of workers (default=0: autodetect)", 0},
    {"verbose", 'v', 0, 0, "Set verbose", 0},
    {"reorder", 'r', 0, 0, "Reorder with Sloan", 0},
    {0, 0, 0, 0, 0, 0}
};

static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    switch (key) {
    case 'w':
        workers = atoi(arg);
        break;
    case 'r':
        reorder = 1;
        break;
    case 'v':
        verbose = 1;
        break;
    case ARGP_KEY_ARG:
        if (state->arg_num == 0) aag_filename = arg;
        if (state->arg_num >= 1) argp_usage(state);
        break; 
    case ARGP_KEY_END:
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

static struct argp argp = { options, parse_opt, "<aag_file>", 0, 0, 0, 0 };

/* Obtain current wallclock time */
static double
wctime()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

static double t_start;
#define INFO(s, ...) fprintf(stdout, "\r[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(s, ...) { fprintf(stderr, "\r[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__); exit(-1); }

/**
 * Global stuff
 */

uint8_t* buf;
size_t pos, size;

int parser_peek()
{
    if (pos == size) return EOF;
    return (int)buf[pos];
}

int parser_read()
{
    if (pos == size) return EOF;
    return (int)buf[pos++];
}

void parser_skip()
{
    pos++;
}

void err()
{
    Abort("File read error.");
}

void read_wsnl()
{
    while (1) {
        int c = parser_peek();
        if (c != ' ' && c != '\n' && c != '\t') return;
        parser_skip();
    }
}

void read_ws()
{
    while (1) {
        int c = parser_peek();
        if (c != ' ' && c != '\t') return;
        parser_skip();
    }
}

void read_token(const char *str)
{
    while (*str != 0) if (parser_read() != (int)(uint8_t)(*str++)) err();
}

uint64_t read_uint()
{
    uint64_t r = 0;
    while (1) {
        int c = parser_peek();
        if (c < '0' || c > '9') return r;
        r *= 10;
        r += c-'0';
        parser_skip();
    }
}

int64_t read_int()
{
    if (parser_peek() == '-') {
        parser_skip();
        return -(int64_t)read_uint();
    } else {
        return read_uint();
    }
}

void read_string(std::string &s)
{
    s = "";
    while (1) {
        int c = parser_peek();
        if (c == EOF || c == '\n') return;
        s += (char)c;
        parser_skip();
    }
}

int *level_to_var;

void make_gate(int a, MTBDD* gates, int* gatelhs, int* gatelft, int* gatergt, int* lookup)
{
    LACE_ME;
    if (gates[a] != sylvan_invalid) return;
    int lft = gatelft[a]/2;
    int rgt = gatergt[a]/2;
    /*
    if (verbose) {
        INFO("Going to make gate %d with lhs %d (%d) and rhs %d (%d)\n", a, lft, lookup[lft], rgt, lookup[rgt]);
    }
    */
    MTBDD l, r;
    if (lft == 0) {
        l = sylvan_false;
    } else if (lookup[lft] != -1) {
        make_gate(lookup[lft], gates, gatelhs, gatelft, gatergt, lookup);
        l = gates[lookup[lft]];
    } else {
        l = sylvan_ithvar(level_to_var[lft]);
    }
    if (rgt == 0) {
        r = sylvan_false;
    } else if (lookup[rgt] != -1) {
        make_gate(lookup[rgt], gates, gatelhs, gatelft, gatergt, lookup);
        r = gates[lookup[rgt]];
    } else {
        r = sylvan_ithvar(level_to_var[rgt]);
    }
    if (gatelft[a]&1) l = sylvan_not(l);
    if (gatergt[a]&1) r = sylvan_not(r);
    gates[a] = sylvan_and(l, r);
    mtbdd_protect(&gates[a]);
}

VOID_TASK_0(parse)
{
    read_wsnl();
    read_token("aag");
    read_ws();
    uint64_t M = read_uint(); // maximum variable index
    read_ws();
    uint64_t I = read_uint(); // number of inputs
    read_ws();
    uint64_t L = read_uint(); // number of latches
    read_ws();
    uint64_t O = read_uint(); // number of outputs
    read_ws();
    uint64_t A = read_uint(); // number of AND gates
    read_ws();
    uint64_t B=0, C=0, J=0, F=0; // optional
    read_ws();
    if (parser_peek() != '\n') {
        B = read_uint(); // number of bad state properties
        read_ws();
    }
    if (parser_peek() != '\n') {
        C = read_uint(); // number of invariant constraints
        read_ws();
    }
    if (parser_peek() != '\n') {
        J = read_uint(); // number of justice properties
        read_ws();
    }
    if (parser_peek() != '\n') {
        F = read_uint(); // number of fairness constraints
    }
    read_wsnl();

    // we expect one output
    if (O != 1) Abort("expecting 1 output\n");
    if (B != 0 or C != 0 or J != 0 or F != 0) Abort("no support for new format\n");

    (void)M; // we don't actually need to know how many variables there are

    INFO("Preparing %zu inputs, %zu latches and %zu AND-gates\n", I, L, A);

    // INFO("Now reading %zu inputs\n", I);

    int inputs[I];
    int outputs[O];
    int latches[L];
    int l_next[L];

    for (uint64_t i=0; i<I; i++) {
        inputs[i] = read_uint();
        read_wsnl();
    }

    // INFO("Now reading %zu latches\n", L);

    for (uint64_t l=0; l<L; l++) {
        latches[l] = read_uint();
        read_ws();
        l_next[l] = read_uint();
        read_wsnl();
    }

    // INFO("Now reading %zu outputs\n", O);

    for (uint64_t o=0; o<O; o++) {
        outputs[o] = read_uint();
        read_wsnl();
    }

    // INFO("Now reading %zu and-gates\n", A);

    int gatelhs[A];
    int gatelft[A];
    int gatergt[A];

    int lookup[M+1];
    for (uint64_t i=0; i<=M; i++) lookup[i] = -1; // not an and-gate

    for (uint64_t a=0; a<A; a++) {
        gatelhs[a] = read_uint();
        lookup[gatelhs[a]/2] = a;
        read_ws();
        gatelft[a] = read_uint();
        read_ws();
        gatergt[a] = read_uint();
        read_wsnl();
    }

    if (reorder) {
        int *matrix = new int[M*M];
        for (unsigned m=0; m<M*M; m++) matrix[m] = 0;
        for (unsigned m=0; m<M; m++) matrix[m*M+m] = 1;

        for (uint64_t i=0; i<I; i++) {
            int v = inputs[i]/2-1;
            matrix[v*M+v] = 1;
        }

        for (uint64_t l=0; l<L; l++) {
            int v = latches[l]/2-1;
            int n = l_next[l]/2-1;
            matrix[v*M+v] = 1; // l -> l
            if (n >= 0) {
                matrix[v*M+n] = 1; // l -> n
                matrix[n*M+v] = 1; // make symmetric
            }  
        }

        for (uint64_t a=0; a<A; a++) {
            int v = gatelhs[a]/2-1;
            int x = gatelft[a]/2-1;
            int y = gatergt[a]/2-1;
            matrix[v*M+v] = 1;
            if (x >= 0) {
                matrix[v*M+x] = 1;
                matrix[x*M+v] = 1;
            }
            if (y >= 0) {
                matrix[v*M+y] = 1;
                matrix[y*M+v] = 1;
            }
        }

        if (0) {
            printf("Matrix\n");
            for (unsigned row=0; row<M; row++) {
                for (unsigned col=0; col<M; col++) {
                    printf("%c", matrix[row*M+col] ? '+' : '-');
                }
                printf("\n");
            }
        }

        typedef boost::adjacency_list<boost::setS, boost::vecS, boost::undirectedS,
                boost::property<boost::vertex_color_t, boost::default_color_type,
                boost::property<boost::vertex_degree_t, int,
                boost::property<boost::vertex_priority_t, double>>>> Graph;

        typedef boost::graph_traits<Graph>::vertex_descriptor Vertex;

        Graph g = Graph(M);

        for (unsigned row=0; row<M; row++) {
            for (unsigned col=0; col<M; col++) {
                if (matrix[row*M+col]) boost::add_edge(row, col, g);
            }
        }

        boost::property_map<Graph, boost::vertex_index_t>::type index_map = boost::get(boost::vertex_index, g);
        std::vector<Vertex> inv_perm(boost::num_vertices(g));

        boost::sloan_ordering(g, inv_perm.begin(), boost::get(boost::vertex_color, g), boost::make_degree_map(g), boost::get(boost::vertex_priority, g), sloan_w1, sloan_w2);

        level_to_var = new int[M+1];
        for (unsigned int i=0; i<=M; i++) level_to_var[i] = -1;
        
        int r = 0;
        for (typename std::vector<Vertex>::const_iterator i=inv_perm.begin(); i != inv_perm.end(); ++i) {
            int j = index_map[*i];
            printf("%d %d\n", r++, j);
        }
    
        r = 0;
        for (typename std::vector<Vertex>::const_iterator i=inv_perm.begin(); i != inv_perm.end(); ++i) {
            int j = (*i)+1;
            if (level_to_var[j] != -1) {
                printf("ERROR: level_to_var of %d is already %d (%d)\n", j, level_to_var[j], r);
                for (unsigned int i=1; i<=M; i++) {
                    if (level_to_var[i] == -1) printf("%d is still -1\n", i);
                    level_to_var[i] = r++;
                }
            } else {
                level_to_var[j] = r++;
            }
            //assert(level_to_var[j] == -1);
        }

        // printf("r=%d M=%d\n", r, (int)M);

        if (0) {
            for (unsigned m=0; m<M*M; m++) matrix[m] = 0;

            for (uint64_t i=0; i<I; i++) {
                int v = level_to_var[inputs[i]/2];
                matrix[v*M+v] = 1;
            }

            for (uint64_t l=0; l<L; l++) {
                int v = level_to_var[latches[l]/2];
                int n = level_to_var[l_next[l]/2];
                matrix[v*M+v] = 1; // l -> l
                if (n >= 0) {
                    matrix[v*M+n] = 1; // l -> n
                }
            }

            for (uint64_t a=0; a<A; a++) {
                int v = level_to_var[gatelhs[a]/2];
                int x = level_to_var[gatelft[a]/2];
                int y = level_to_var[gatergt[a]/2];
                matrix[v*M+v] = 1;
                if (x >= 0) {
                    matrix[v*M+x] = 1;
                }
                if (y >= 0) {
                    matrix[v*M+y] = 1;
                }
            }

            printf("Matrix\n");
            for (unsigned row=0; row<M; row++) {
                for (unsigned col=0; col<M; col++) {
                    printf("%c", matrix[row*M+col] ? '+' : '-');
                }
                printf("\n");
            }
        }

        delete[] matrix;
    } else {
        level_to_var = new int[M+1];
        for (unsigned int i=0; i<=M; i++) level_to_var[i] = i-1;
    }

/*
    // add edges for the bipartite graph
    for (int i = 0; i < dm_nrows(m); i++) {
            for (int j = 0; j < dm_ncols(m); j++) {
                    if (dm_is_set(m, i, j)) add_edge(i, dm_nrows(m) + j, g);
            }
    }*/


    /*
    for (uint64_t a=0; a<A; a++) {
        MTBDD lhs = sylvan_ithvar(read_uint()/2);
        mtbdd_refs_push(lhs);
        read_ws();
        int left = read_uint();
        read_ws();
        int right = read_uint();
        read_wsnl();
        MTBDD lft = left&1 ? sylvan_nithvar(left/2) : sylvan_ithvar(left/2);
        mtbdd_refs_push(lft);
        MTBDD rgt = right&1 ? sylvan_nithvar(right/2) : sylvan_ithvar(right/2);
        mtbdd_refs_push(rgt);
        MTBDD rhs = sylvan_and(lft, rgt);
        mtbdd_refs_push(rhs);
        gates[a] = sylvan_equiv(lhs, rhs);
        mtbdd_ref(gates[a]);
    }
    */

    MTBDD Xc = sylvan_set_empty(), Xu = sylvan_set_empty();
    mtbdd_protect(&Xc);
    mtbdd_protect(&Xu);

    // Now read the [[optional]] labels to find controllable vars 
    while (1) {
        int c = parser_peek();
        if (c != 'l' and c != 'i' and c != 'o') break;
        parser_skip();
        int pos = read_uint();
        read_token(" ");
        std::string s;
        read_string(s);
        read_wsnl();
        if (c == 'i') {
            if (strncmp(s.c_str(), "controllable_", 13) == 0) {
                Xc = sylvan_set_add(Xc, level_to_var[inputs[pos]/2]);
            } else {
                Xu = sylvan_set_add(Xu, level_to_var[inputs[pos]/2]);
            }
        }
    }

#if 0
    sylvan_print(Xc);
    printf("\n");
    sylvan_print(Xu);
    printf("\n");
#endif

    INFO("There are %zu controllable and %zu uncontrollable inputs.\n",
        sylvan_set_count(Xc), sylvan_set_count(Xu));

    // sylvan_stats_report(stdout);

    INFO("Making the gate BDDs...\n");

    MTBDD gates[A];
    for (uint64_t a=0; a<A; a++) gates[a] = sylvan_invalid;
    for (uint64_t a=0; a<A; a++) make_gate(a, gates, gatelhs, gatelft, gatergt, lookup);

    if (verbose) {
        INFO("Gates have size %zu\n", mtbdd_nodecount_more(gates, A));
    }

#if 0
    for (uint64_t g=0; g<A; g++) {
        INFO("gate %d has size %zu\n", (int)g, sylvan_nodecount(gates[g]));
    }
#endif

    sylvan_stats_report(stdout);

#if 0
    for (uint64_t g=0; g<A; g++) {
        MTBDD supp = sylvan_support(gates[g]);
        while (supp != sylvan_set_empty()) {
            printf("%d ", sylvan_set_first(supp));
            supp = sylvan_set_next(supp);
        }
        printf("\n");
    }
#endif

#if 0
    MTBDD lnext[L];
    for (uint64_t l=0; l<L; l++) {
        MTBDD nxt;
        if (lookup[l_next[l]/2] == -1) {
            nxt = sylvan_ithvar(l_next[l]&1);
        } else {
            nxt = gates[lookup[l_next[l]]];
        }
        if (l_next[l]&1) nxt = sylvan_not(nxt);
        lnext[l] = sylvan_equiv(sylvan_ithvar(latches[l]+1), nxt);
    }
    INFO("done making latches\n");
#endif

    // And we don't care about comments.

    MTBDD Lvars = sylvan_set_empty();
    mtbdd_protect(&Lvars);

    for (uint64_t l=0; l<L; l++) {
        Lvars = sylvan_set_add(Lvars, level_to_var[latches[l]/2]);
    }

#if 0
    MTBDD LtoPrime = sylvan_map_empty();
    for (uint64_t l=0; l<L; l++) {
        LtoPrime = sylvan_map_add(LtoPrime, latches[l], latches[l]+1);
    }
#endif

    // Actually just make the compose vector
    MTBDD CV = sylvan_map_empty();
    mtbdd_protect(&CV);

    for (uint64_t l=0; l<L; l++) {
        MTBDD nxt;
        if (lookup[l_next[l]/2] == -1) {
            nxt = sylvan_ithvar(level_to_var[l_next[l]/2]);
        } else {
            nxt = gates[lookup[l_next[l]/2]];
        }
        if (l_next[l]&1) nxt = sylvan_not(nxt);
        CV = sylvan_map_add(CV, level_to_var[latches[l]/2], nxt);
    }

    // now make output
    INFO("output is %d (lookup: %d)\n", outputs[0], lookup[outputs[0]/2]);
    MTBDD Unsafe;
    mtbdd_protect(&Unsafe);
    if (lookup[outputs[0]/2] == -1) {
        Unsafe = sylvan_ithvar(level_to_var[outputs[0]/2]);
    } else {
        Unsafe = gates[lookup[outputs[0]/2]];
    }
    if (outputs[0]&1) Unsafe = sylvan_not(Unsafe);
    Unsafe = sylvan_forall(Unsafe, Xc);
    Unsafe = sylvan_exists(Unsafe, Xu);

#if 0
    MTBDD supp = sylvan_support(Unsafe);
    while (supp != sylvan_set_empty()) {
        printf("%d ", sylvan_set_first(supp));
        supp = sylvan_set_next(supp);
    }
    printf("\n");
    INFO("exactly %.0f states are bad\n", sylvan_satcount(Unsafe, Lvars));
#endif

    delete[] level_to_var;

    MTBDD OldUnsafe = sylvan_false; // empty set
    MTBDD Step = sylvan_false;
    mtbdd_protect(&OldUnsafe);
    mtbdd_protect(&Step);

    int iteration = 0;

    while (Unsafe != OldUnsafe) {
        OldUnsafe = Unsafe;
        iteration++;
        if (verbose) {
            //INFO("Iteration %d.\n", iteration);
            INFO("Iteration %d (%.0f unsafe states)...\n", iteration, sylvan_satcount(Unsafe, Lvars));
        }
        // INFO("Unsafe has %zu size\n", sylvan_nodecount(Unsafe));
        // INFO("exactly %.0f states are bad\n", sylvan_satcount(Unsafe, Lvars));
        Step = sylvan_compose(Unsafe, CV);
        // INFO("Hello we are %zu size\n", sylvan_nodecount(Step));
        Step = sylvan_forall(Step, Xc);
        // INFO("Hello we are %zu size\n", sylvan_nodecount(Step));
        Step = sylvan_exists(Step, Xu);
        // INFO("Hello we are %zu size\n", sylvan_nodecount(Step));

        /*
        MTBDD supp = sylvan_support(Step);
        while (supp != sylvan_set_empty()) {
            printf("%d ", sylvan_set_first(supp));
            supp = sylvan_set_next(supp);
        }
        printf("\n");
        sylvan_print(Step);
        printf("\n");
        */

        // check if initial state in Step (all 0)
        MTBDD Check = Step;
        while (Check != sylvan_false) {
            if (Check == sylvan_true) {
                INFO("initial state is Unsafe!\n");
                return;
            } else {
                Check = sylvan_low(Check);
            }
        }

        // INFO("Sizes: %zu and %zu\n", sylvan_nodecount(Unsafe), sylvan_nodecount(Step));
        // INFO("Time to OR\n");
        Unsafe = sylvan_or(Unsafe, Step);
        // INFO("Welcome baque\n");
    }

    INFO("Thank you for using me. I realize that.\n");

    // suppress a bunch of errors
    (void)M;
    (void)B;
    (void)C;
    (void)J;
    (void)F;
}

VOID_TASK_0(gc_mark)
{
}

VOID_TASK_0(gc_start)
{
    size_t used, total;
    sylvan_table_usage(&used, &total);
    INFO("Starting garbage collection of %zu/%zu size\n", used, total);
}

VOID_TASK_0(gc_end)
{
    size_t used, total;
    sylvan_table_usage(&used, &total);
    INFO("Garbage collection done of %zu/%zu size\n", used, total);
}

int
main(int argc, char **argv)
{
    // Load start time (for the output)
    t_start = wctime();

    // Parse arguments
    argp_parse(&argp, argc, argv, 0, 0, 0);

    // Init Lace
    lace_init(workers, 1000000); // auto-detect number of workers, use a 1,000,000 size task queue
    lace_startup(0, NULL, NULL); // auto-detect program stack, do not use a callback for startup
    LACE_ME;

    // Init Sylvan
    // Give 2 GB memory
    sylvan_set_limits(2LL*1LL<<30, 1, 15);
    sylvan_init_package();
    sylvan_init_mtbdd();

    // Set hooks for logging garbage collection
    if (verbose) {
        sylvan_gc_hook_pregc(TASK(gc_start));
        sylvan_gc_hook_postgc(TASK(gc_end));
    }
    sylvan_gc_hook_pregc(TASK(gc_mark));

    if (aag_filename == NULL) {
        Abort("stream not yet supported\n");
    }

    int fd = open(aag_filename, O_RDONLY);

    struct stat filestat;
    if(fstat(fd, &filestat) != 0) Abort("cannot stat file\n");
    size = filestat.st_size;

    buf = (uint8_t*)mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    if(buf == MAP_FAILED) Abort("mmap failed\n");

    CALL(parse);
    
    // Report Sylvan statistics (if SYLVAN_STATS is set)
    if (verbose) sylvan_stats_report(stdout);

    return 0;
}
