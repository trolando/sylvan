/* Regression tests for universal MTBDD comparisons, including cache entries
 * left by parallel comparisons that stop early. Norm comparisons take only
 * double leaves; the ordering comparisons also take integers and fractions. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lace.h>
#include <sylvan.h>
#include <sylvan_cache.h>

#include "test_assert.h"

enum { ABS, REL, LEQ, LESS, GEQ, GREATER, OPERATIONS };
static const char *names[] = {"equal_norm_d", "equal_norm_rel_d", "leq", "less", "geq", "greater"};
static uint64_t random_state = UINT64_C(0x2545f4914f6cdd1d);

static uint32_t
random_u32(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return (uint32_t)random_state;
}

static MTBDD
compare(int op, MTBDD a, MTBDD b, double epsilon)
{
    switch (op) {
    case ABS: return mtbdd_equal_norm_d(a, b, epsilon);
    case REL: return mtbdd_equal_norm_rel_d(a, b, epsilon);
    case LEQ: return mtbdd_leq(a, b);
    case LESS: return mtbdd_less(a, b);
    case GEQ: return mtbdd_geq(a, b);
    default: return mtbdd_greater(a, b);
    }
}

static MTBDD
evaluate(MTBDD dd, uint64_t assignment)
{
    while (!mtbdd_isleaf(dd)) {
        dd = (assignment >> mtbdd_getvar(dd)) & 1
            ? mtbdd_gethigh(dd) : mtbdd_getlow(dd);
    }
    return dd;
}

static double
value(MTBDD leaf)
{
    switch (mtbdd_gettype(leaf)) {
    case 0: return (double)mtbdd_getint64(leaf);
    case 1: return mtbdd_getdouble(leaf);
    default: return (double)mtbdd_getnumer(leaf) / mtbdd_getdenom(leaf);
    }
}

/* Enumerate assignments independently of the recursive comparison algorithm.
 * Ordering ignores points missing from either domain. Equality requires the
 * domains to match, and regards two undefined values as equal. */
static MTBDD
oracle(int op, MTBDD a, MTBDD b, double epsilon, unsigned first, unsigned variables)
{
    for (uint64_t s = 0; s < (UINT64_C(1) << variables); s++) {
        MTBDD x = evaluate(a, s << first), y = evaluate(b, s << first);
        if (x == mtbdd_false || y == mtbdd_false) {
            if (op <= REL && x != y) return mtbdd_false;
            continue;
        }
        double va = value(x), vb = value(y);
        int holds;
        switch (op) {
        case ABS: holds = fabs(va - vb) < epsilon; break;
        case REL: holds = va == vb || (va != 0 && fabs((va - vb) / va) < epsilon); break;
        case LEQ: holds = va <= vb; break;
        case LESS: holds = va < vb; break;
        case GEQ: holds = va >= vb; break;
        default: holds = va > vb; break;
        }
        if (!holds) return mtbdd_false;
    }
    return mtbdd_true;
}

static int
check(int op, MTBDD a, MTBDD b, double epsilon, unsigned first, unsigned variables)
{
    MTBDD expected = oracle(op, a, b, epsilon, first, variables);
    MTBDD actual = compare(op, a, b, epsilon);
    if (actual != expected) {
        fprintf(stderr, "%s: oracle %s, comparison %s (first=%u, variables=%u)\n",
                names[op], expected == mtbdd_true ? "true" : "false",
                actual == mtbdd_true ? "true" : actual == mtbdd_false ? "false" : "invalid",
                first, variables);
        return 1;
    }
    return 0;
}

static MTBDD
number(int type, int n)
{
    if (type == 0) return mtbdd_int64(n);
    if (type == 1) return mtbdd_double(n / 4.0);
    return mtbdd_fraction(n, (uint64_t)(abs(n) % 7 + 1));
}

static MTBDD
random_dd(int type, unsigned var, unsigned end)
{
    if (var == end || random_u32() % 8 == 0) {
        if (random_u32() % 4 == 0) return mtbdd_false;
        return number(type, (int)(random_u32() % 65) - 32);
    }
    MTBDD low = random_dd(type, var + 1, end);
    MTBDD high = random_dd(type, var + 1, end);
    return mtbdd_makenode(var, low, high);
}

static int
test_domains(void)
{
    /* Absolute norm must be symmetric even if a nonterminal was allocated
     * after the leaf on the other side. Its cofactors need not be within
     * epsilon of each other, only of the other operand. */
    MTBDD middle = mtbdd_double(2.0);
    MTBDD spread = mtbdd_makenode(0, mtbdd_double(1.0), mtbdd_double(3.0));
    if (check(ABS, spread, middle, 1.5, 0, 1)) return 1;
    if (check(ABS, middle, spread, 1.5, 0, 1)) return 1;

    for (int type = 0; type < 3; type++) {
        MTBDD x = number(type, 4), y = number(type, 5);
        MTBDD cases[] = {
            mtbdd_false, x, y,
            mtbdd_makenode(0, mtbdd_false, x),
            mtbdd_makenode(0, mtbdd_false, y),
            mtbdd_makenode(0, x, mtbdd_false),
            mtbdd_makenode(2, mtbdd_false, y)
        };
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            for (unsigned j = 0; j < sizeof(cases) / sizeof(cases[0]); j++) {
                for (int op = type == 1 ? ABS : LEQ; op < OPERATIONS; op++) {
                    if (check(op, cases[i], cases[j], 0.3, 0, 3)) return 1;
                }
            }
        }
        for (unsigned trial = 0; trial < 80; trial++) {
            MTBDD a = random_dd(type, 0, 6), b = random_dd(type, 0, 6);
            /* Include equal diagrams and shared undefined cofactors, as well
             * as independent domains. Reverse every pair too. */
            if (trial % 4 == 0) b = a;
            if (trial % 4 == 1) {
                a = mtbdd_makenode(0, mtbdd_false, random_dd(type, 1, 6));
                b = mtbdd_makenode(0, mtbdd_false, random_dd(type, 1, 6));
            }
            for (int op = type == 1 ? ABS : LEQ; op < OPERATIONS; op++) {
                double epsilon = trial % 2 ? 0.3 : 8.0;
                if (check(op, a, b, epsilon, 0, 6)) return 1;
                if (check(op, b, a, epsilon, 0, 6)) return 1;
            }
        }
    }
    return 0;
}

enum { SPINE = 16, DEPTH = 10, PAIRS = (1 << DEPTH) - 1, ROUNDS = 32 };
typedef struct { MTBDD a, b; unsigned first, variables; } pair;
static pair pairs[PAIRS];
static unsigned pair_count;

/* Every assignment satisfies a < b and both norm tests. Different values
 * keep the comparisons from collapsing to identical handles or tiny DAGs. */
static pair
true_pair(unsigned var, unsigned remaining)
{
    pair p = {mtbdd_false, mtbdd_false, var, remaining};
    if (remaining == 0) {
        double v = 128.0 + (random_u32() % 65536) / 8.0;
        p.a = mtbdd_double(v);
        p.b = mtbdd_double(v + 0.03125);
    } else {
        pair low = true_pair(var + 1, remaining - 1);
        pair high = true_pair(var + 1, remaining - 1);
        p.a = mtbdd_makenode(var, low.a, high.a);
        p.b = mtbdd_makenode(var, low.b, high.b);
        pairs[pair_count++] = p;
    }
    return p;
}

static int
test_cache(void)
{
    unsigned failures[OPERATIONS] = {0};
    for (unsigned round = 0; round < ROUNDS; round++) {
        pair_count = 0;
        pair good = true_pair(SPINE, DEPTH);
        test_assert(pair_count == PAIRS);
        for (int op = 0; op < OPERATIONS; op++) {
            /* Clear before the false comparison, never between that call and
             * the follow-up queries. A long low spine lets workers steal the
             * large true high branches before the low leaf stops the call. */
            cache_clear();
            int reverse = op == GEQ || op == GREATER;
            MTBDD ga = reverse ? good.b : good.a;
            MTBDD gb = reverse ? good.a : good.b;
            MTBDD a = mtbdd_double(reverse ? 1.0 : 100.0);
            MTBDD b = mtbdd_double(reverse ? 100.0 : 1.0);
            for (unsigned v = SPINE; v-- > 0;) {
                a = mtbdd_makenode(v, a, ga);
                b = mtbdd_makenode(v, b, gb);
            }
            test_assert(check(op, a, b, 0.125, 0, SPINE + DEPTH) == 0);
            /* Check the root first, then every internal subdiagram pair.
             * Oracle enumeration for each pair also checks the generator. */
            unsigned before = failures[op];
            if (check(op, ga, gb, 0.125, SPINE, DEPTH)) failures[op]++;
            for (unsigned i = 0; i < pair_count; i++) {
                pair p = pairs[i];
                if (check(op, reverse ? p.b : p.a, reverse ? p.a : p.b,
                          0.125, p.first, p.variables)) {
                    failures[op]++;
                    break;
                }
            }
            /* Count affected rounds, rather than individual poisoned pairs. */
            failures[op] = before + (failures[op] != before);
        }
    }
    unsigned total = 0;
    for (int op = 0; op < OPERATIONS; op++) {
        printf("%s: %u/%u cache regression rounds failed\n", names[op], failures[op], ROUNDS);
        total += failures[op];
    }
    return total != 0;
}

TASK_1(int, runtests, int, cache_only)
{
    /* This test isolates comparisons from GC; no unprotected construction
     * temporaries can disappear, and cache contents survive until queried. */
    sylvan_gc_disable();
    if (!cache_only && test_domains()) return 1;
    return test_cache();
}

int
main(int argc, char **argv)
{
    unsigned workers = argc > 1 ? (unsigned)atoi(argv[1]) : 4;
    if (workers == 0) workers = 1;
    int cache_only = argc > 2 && strcmp(argv[2], "cache") == 0;
    lace_start(workers, 0);
    sylvan_set_sizes(1LL << 20, 1LL << 20, 1LL << 18, 1LL << 18);
    sylvan_init_package();
    sylvan_init_mtbdd();
    int status = RUN(runtests, cache_only);
    sylvan_quit();
    lace_stop();
    return status;
}
