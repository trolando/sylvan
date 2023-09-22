#include <getopt.h>

#include <sys/time.h>
#include <sys/mman.h>
#include <boost/graph/sloan_ordering.hpp>
#include <boost/graph/graph_traits.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <string>
#include <sys/stat.h>
#include <fcntl.h>
#include <span>

#include <sylvan_int.h>
#include "aag.h"


using namespace sylvan;

typedef struct safety_game
{
    MTBDD *gates;           // and gates
    MTBDD c_inputs;         // controllable inputs
    MTBDD u_inputs;         // uncontrollable inputs
    int *level_to_order;    // mapping from variable level to static variable order
} safety_game_t;

double t_start;
#define INFO(s, ...) fprintf(stdout, "\r[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__)
#define Abort(s, ...) { fprintf(stderr, "\r[% 8.2f] " s, wctime()-t_start, ##__VA_ARGS__); exit(-1); }


/* Configuration */
static int workers = 1;
static int verbose = 0;
static char *filename = nullptr; // filename of the aag file
static int static_reorder = 0;
static int dynamic_reorder = 0;
static int sloan_w1 = 1;
static int sloan_w2 = 8;

//static FILE *log_file = nullptr;

/* Global variables */
static aag_file_t aag{
        .header = {
                .m = 0,
                .i = 0,
                .l = 0,
                .o = 0,
                .a = 0,
                .b = 0,
                .c = 0,
                .j = 0,
                .f = 0
        },
        .inputs = nullptr,
        .outputs = nullptr,
        .latches = nullptr,
        .l_next = nullptr,
        .lookup = nullptr,
        .gatelhs = nullptr,
        .gatelft = nullptr,
        .gatergt = nullptr
};
static aag_buffer_t aag_buffer{
        .content = nullptr,
        .size = 0,
        .pos = 0,
        .file_descriptor = -1,
        .filestat = {}
};
static safety_game_t game{
        .gates = nullptr,
        .c_inputs = sylvan_set_empty(),
        .u_inputs = sylvan_set_empty(),
        .level_to_order = nullptr
};

/* Obtain current wallclock time */
static double
wctime()
{
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    return (tv.tv_sec + 1E-6 * tv.tv_usec);
}

static void
print_usage()
{
    printf("Usage: aigsynt [-w <workers>] [-d --dynamic-reordering] [-s --static-reordering]\n");
    printf("               [-v --verbose] [--help] [--usage] <model> [<output-bdd>]\n");
}

static void
print_help()
{
    printf("Usage: aigsynt [OPTION...] <model> [<output-bdd>]\n\n");
    printf("                             Strategy for reachability (default=par)\n");
    printf("  -d,                        Dynamic variable ordering\n");
    printf("  -w, --workers=<workers>    Number of workers (default=0: autodetect)\n");
    printf("  -v,                        Dynamic variable ordering\n");
    printf("  -s,                        Reorder with Sloan\n");
    printf("  -h, --help                 Give this help list\n");
    printf("      --usage                Give a short usage message\n");
}

static void
parse_args(int argc, char **argv)
{
    static const option longopts[] = {
            {"workers",            required_argument, (int *) 'w', 1},
            {"dynamic-reordering", no_argument,       nullptr,     'd'},
            {"static-reordering",  no_argument,       nullptr,     's'},
            {"verbose",            no_argument,       nullptr,     'v'},
            {"help",               no_argument,       nullptr,     'h'},
            {"usage",              no_argument,       nullptr,     99},
            {nullptr,              no_argument,       nullptr,     0},
    };
    int key = 0;
    int long_index = 0;
    while ((key = getopt_long(argc, argv, "w:s:h", longopts, &long_index)) != -1) {
        switch (key) {
            case 'w':
                workers = atoi(optarg);
                break;
            case 's':
                static_reorder = 1;
                break;
            case 'd':
                dynamic_reorder = 1;
                break;
            case 'v':
                verbose = 1;
                break;
            case 99:
                print_usage();
                exit(0);
            case 'h':
                print_help();
                exit(0);
        }
    }
    if (optind >= argc) {
        print_usage();
        exit(0);
    }
    filename = argv[optind];
}

VOID_TASK_0(gc_start)
{
    size_t used, total;
    sylvan_table_usage(&used, &total);
    printf("\n");
    INFO("GC: str: %zu/%zu size\n", used, total);
}

VOID_TASK_0(gc_end)
{
    size_t used, total;
    sylvan_table_usage(&used, &total);
    INFO("GC: end: %zu/%zu size\n\n", used, total);
}

VOID_TASK_0(reordering_start) {
    printf("\r[% 8.2f] RE: from %zu to ... ", wctime()-t_start, llmsset_count_marked(nodes));
}

VOID_TASK_0(reordering_end) {
    printf("%zu nodes in %f\n", llmsset_count_marked(nodes), wctime() - reorder_db->config.t_start_sifting);
}

void order_statically()
{
    int *matrix = new int[aag.header.m * aag.header.m];
    for (unsigned m = 0; m < aag.header.m * aag.header.m; m++) matrix[m] = 0;
    for (unsigned m = 0; m < aag.header.m; m++) matrix[m * aag.header.m + m] = 1;

    for (uint64_t i = 0; i < aag.header.i; i++) {
        int v = (int) aag.inputs[i] / 2 - 1;
        matrix[v * aag.header.m + v] = 1;
    }

    for (uint64_t l = 0; l < aag.header.l; l++) {
        int v = (int) aag.latches[l] / 2 - 1;
        int n = (int) aag.l_next[l] / 2 - 1;
        matrix[v * aag.header.m + v] = 1; // l -> l
        if (n >= 0) {
            matrix[v * aag.header.m + n] = 1; // l -> n
            matrix[n * aag.header.m + v] = 1; // make symmetric
        }
    }

    for (uint64_t a = 0; a < aag.header.a; a++) {
        int v = (int) aag.gatelhs[a] / 2 - 1;
        int x = (int) aag.gatelft[a] / 2 - 1;
        int y = (int) aag.gatergt[a] / 2 - 1;
        matrix[v * aag.header.m + v] = 1;
        if (x >= 0) {
            matrix[v * aag.header.m + x] = 1;
            matrix[x * aag.header.m + v] = 1;
        }
        if (y >= 0) {
            matrix[v * aag.header.m + y] = 1;
            matrix[y * aag.header.m + v] = 1;
        }
    }

    typedef boost::adjacency_list<boost::setS, boost::vecS, boost::undirectedS,
            boost::property<boost::vertex_color_t, boost::default_color_type,
                    boost::property<boost::vertex_degree_t, int,
                            boost::property<boost::vertex_priority_t, double>>>> Graph;

    typedef boost::graph_traits<Graph>::vertex_descriptor Vertex;

    Graph g = Graph(aag.header.m);

    for (unsigned row = 0; row < aag.header.m; row++) {
        for (unsigned col = 0; col < aag.header.m; col++) {
            if (matrix[row * aag.header.m + col]) boost::add_edge(row, col, g);
        }
    }

    std::vector<Vertex> inv_perm(boost::num_vertices(g));

    boost::sloan_ordering(g, inv_perm.begin(), boost::get(boost::vertex_color, g), boost::make_degree_map(g),
                          boost::get(boost::vertex_priority, g), sloan_w1, sloan_w2);

    std::vector<int> level_to_var;

    for (uint64_t i = 0; i <= aag.header.m; i++) level_to_var[i] = -1;

    int r = 0;

    r = 0;
    for (unsigned long &i: inv_perm) {
        uint64_t j = i + 1;
        if (level_to_var[j] != -1) {
            printf("ERROR: level_to_var of %zu is already %d (%d)\n", (size_t) j, level_to_var[j], r);
            for (uint64_t k = 1; k <= aag.header.m; k++) {
                if (level_to_var[k] == -1) printf("%zu is still -1\n", (size_t) k);
                level_to_var[k] = r++;
            }
        } else {
            level_to_var[j] = r++;
        }
    }

    printf("r=%d M=%d\n", r, (int) aag.header.m);
#if 1
    for (unsigned m = 0; m < aag.header.m * aag.header.m; m++) matrix[m] = 0;

    for (uint64_t i = 0; i < aag.header.i; i++) {
        int v = level_to_var[aag.inputs[i] / 2];
        matrix[v * aag.header.m + v] = 1;
    }

    for (uint64_t l = 0; l < aag.header.l; l++) {
        int v = level_to_var[aag.latches[l] / 2];
        int n = level_to_var[aag.l_next[l] / 2];
        matrix[v * aag.header.m + v] = 1; // l -> l
        if (n >= 0) {
            matrix[v * aag.header.m + n] = 1; // l -> n
        }
    }

    for (uint64_t a = 0; a < aag.header.a; a++) {
        int v = level_to_var[aag.gatelhs[a] / 2];
        int x = level_to_var[aag.gatelft[a] / 2];
        int y = level_to_var[aag.gatergt[a] / 2];
        matrix[v * aag.header.m + v] = 1;
        if (x >= 0) {
            matrix[v * aag.header.m + x] = 1;
        }
        if (y >= 0) {
            matrix[v * aag.header.m + y] = 1;
        }
    }

    printf("Matrix\n");
    for (unsigned row = 0; row < aag.header.m; row++) {
        for (unsigned col = 0; col < aag.header.m; col++) {
            printf("%c", matrix[row * aag.header.m + col] ? '+' : '-');
        }
        printf("\n");
    }
#endif
}

#define make_gate(gate) CALL(make_gate, gate)
VOID_TASK_1(make_gate, int, gate)
{
    if (game.gates[gate] != sylvan_invalid) return;
    int lft = (int) aag.gatelft[gate] / 2;
    int rgt = (int) aag.gatergt[gate] / 2;

    MTBDD l, r;
    if (lft == 0) {
        l = sylvan_false;
    } else if (aag.lookup[lft] != -1) {
        make_gate(aag.lookup[lft]);
        l = game.gates[aag.lookup[lft]];
    } else {
        l = sylvan_ithvar(game.level_to_order[lft]); // always use even variables (prime is odd)
    }
    if (rgt == 0) {
        r = sylvan_false;
    } else if (aag.lookup[rgt] != -1) {
        make_gate(aag.lookup[rgt]);
        r = game.gates[aag.lookup[rgt]];
    } else {
        r = sylvan_ithvar(game.level_to_order[rgt]); // always use even variables (prime is odd)
    }
    if (aag.gatelft[gate] & 1) l = sylvan_not(l);
    if (aag.gatergt[gate] & 1) r = sylvan_not(r);
    game.gates[gate] = sylvan_and(l, r);
    mtbdd_protect(&game.gates[gate]);
}

#define solve_game() RUN(solve_game)
TASK_0(int, solve_game)
{
    game.level_to_order = (int *) calloc(aag.header.m + 1, sizeof(int));

    if (static_reorder) {
        order_statically();
    } else {
        for (int i = 0; i <= (int) aag.header.m; i++) game.level_to_order[i] = i;
    }

    INFO("Making the gate BDDs...\n");

    game.gates = new MTBDD[aag.header.a];
    for (uint64_t a = 0; a < aag.header.a; a++) game.gates[a] = sylvan_invalid;
    for (uint64_t gate = 0; gate < aag.header.a; gate++) {
        make_gate(gate);
        if (dynamic_reorder) {
            sylvan_test_reduce_heap();
        }
    }

    sylvan_test_reduce_heap();
    if (verbose) INFO("Gates have size %zu\n", mtbdd_nodecount_more(game.gates, aag.header.a));

    game.c_inputs = sylvan_set_empty();
    game.u_inputs = sylvan_set_empty();
    mtbdd_protect(&game.c_inputs);
    mtbdd_protect(&game.u_inputs);

    // Now read the [[optional]] labels to find controllable vars
    while (true) {
        int c = aag_buffer_peek(&aag_buffer);
        if (c != 'l' and c != 'i' and c != 'o') break;
        aag_buffer_skip(&aag_buffer);
        int pos = (int) aag_buffer_read_uint(&aag_buffer);
        aag_buffer_read_token(" ", &aag_buffer);
        std::string s;
        aag_buffer_read_string(s, &aag_buffer);
        aag_buffer_read_wsnl(&aag_buffer);
        if (c == 'i') {
            if (strncmp(s.c_str(), "controllable_", 13) == 0) {
                game.c_inputs = sylvan_set_add(game.c_inputs, game.level_to_order[aag.inputs[pos] / 2]);
            } else {
                game.u_inputs = sylvan_set_add(game.u_inputs, game.level_to_order[aag.inputs[pos] / 2]);
            }
        }
    }
    INFO("There are %zu controllable and %zu uncontrollable inputs.\n", sylvan_set_count(game.c_inputs), sylvan_set_count(game.u_inputs));

    // Actually just make the compose vector
    MTBDD CV = sylvan_map_empty();
    mtbdd_protect(&CV);

    for (uint64_t l = 0; l < aag.header.l; l++) {
        MTBDD nxt;
        if (aag.lookup[aag.l_next[l] / 2] == -1) {
            nxt = sylvan_ithvar(game.level_to_order[aag.l_next[l] / 2]);
        } else {
            nxt = game.gates[aag.lookup[aag.l_next[l] / 2]];
        }
        if (aag.l_next[l] & 1) nxt = sylvan_not(nxt);
        CV = sylvan_map_add(CV, game.level_to_order[aag.latches[l] / 2], nxt);
    }

    // now make output
    INFO("output is %zu (lookup: %d)\n", (size_t) aag.outputs[0], aag.lookup[aag.outputs[0] / 2]);
    MTBDD Unsafe;
    mtbdd_protect(&Unsafe);
    if (aag.lookup[aag.outputs[0] / 2] == -1) {
        Unsafe = sylvan_ithvar(aag.outputs[0] / 2);
    } else {
        Unsafe = game.gates[aag.lookup[aag.outputs[0] / 2]];
    }
    if (aag.outputs[0] & 1) Unsafe = sylvan_not(Unsafe);
    Unsafe = sylvan_forall(Unsafe, game.c_inputs);
    Unsafe = sylvan_exists(Unsafe, game.u_inputs);

    MTBDD OldUnsafe = sylvan_false; // empty set
    MTBDD Step = sylvan_false;
    mtbdd_protect(&OldUnsafe);
    mtbdd_protect(&Step);

    while (Unsafe != OldUnsafe) {
        OldUnsafe = Unsafe;

        Step = sylvan_compose(Unsafe, CV);
        Step = sylvan_forall(Step, game.c_inputs);
        Step = sylvan_exists(Step, game.u_inputs);

        // check if initial state in Step (all 0)
        MTBDD Check = Step;
        while (Check != sylvan_false) {
            if (Check == sylvan_true) {
                return 0;
            } else {
                Check = sylvan_low(Check);
            }
        }

        Unsafe = sylvan_or(Unsafe, Step);
    }
    return 1;
}

int main(int argc, char **argv)
{
    t_start = wctime();
    setlocale(LC_NUMERIC, "en_US.utf-8");
    parse_args(argc, argv);
    INFO("Model: %s\n", filename);
    if (filename == nullptr) {
        Abort("Invalid file name.\n");
    }

    aag_buffer_open(&aag_buffer, filename, O_RDONLY);
    aag_file_read(&aag, &aag_buffer);

    if (verbose) {
        INFO("----------header----------\n");
        INFO("# of variables            \t %lu\n", aag.header.m);
        INFO("# of inputs               \t %lu\n", aag.header.i);
        INFO("# of latches              \t %lu\n", aag.header.l);
        INFO("# of outputs              \t %lu\n", aag.header.o);
        INFO("# of AND gates            \t %lu\n", aag.header.a);
        INFO("# of bad state properties \t %lu\n", aag.header.b);
        INFO("# of invariant constraints\t %lu\n", aag.header.c);
        INFO("# of justice properties   \t %lu\n", aag.header.j);
        INFO("# of fairness constraints \t %lu\n", aag.header.f);
        INFO("--------------------------\n");
    }

    lace_start(workers, 0);

    // 1LL<<19: 8192 nodes (minimum)
    // 1LL<<20: 16384 nodes
    // 1LL<<21: 32768 nodes
    // 1LL<<22: 65536 nodes
    // 1LL<<23: 131072 nodes
    // 1LL<<24: 262144 nodes
    // 1LL<<25: 524288 nodes
    sylvan_set_limits(1LL << 24, 1, 0);
    sylvan_init_package();
    sylvan_init_mtbdd();
    sylvan_init_reorder();
    sylvan_gc_disable();

    sylvan_set_reorder_type(SYLVAN_REORDER_BOUNDED_SIFT);

    // Set hooks for logging garbage collection & dynamic variable reordering
    if (verbose) {
        sylvan_re_hook_prere(TASK(reordering_start));
        sylvan_re_hook_postre(TASK(reordering_end));
        sylvan_gc_hook_pregc(TASK(gc_start));
        sylvan_gc_hook_postgc(TASK(gc_end));
    }

    int is_realizable = solve_game();
    if (is_realizable) {
        INFO("REALIZABLE\n");
    } else {
        INFO("UNREALIZABLE\n");
    }

    // Report Sylvan statistics (if SYLVAN_STATS is set)
    if (verbose) sylvan_stats_report(stdout);

    aag_buffer_close(&aag_buffer);
    sylvan_quit();
    lace_stop();

    return 0;
}
