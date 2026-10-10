/*
 * Minimal BLIF reader, shared by the Sylvan and the CUDD driver.
 *
 * It exists so that both tools see byte-for-byte the same circuit, the same
 * variable order, and the same gate construction sequence: the only thing that
 * differs between the two runs is the decision diagram library underneath.
 *
 * Supported: .model .inputs .outputs .latch .names .end, '#' comments and
 * backslash line continuations. That is everything the picotrav inputs use.
 *
 * Conventions, all deliberate and all applied identically to both tools:
 *
 *  - VARIABLE ORDER is the file's own order: the names on .inputs first, in
 *    the order they appear, then the output signal of each .latch in the order
 *    the latches appear. This is "the file's default order".
 *
 *  - LATCHES are cut. A latch output is a free variable and a latch input is
 *    just another signal. So a sequential circuit is read as one combinational
 *    frame, which is what building a BDD per .outputs signal means here.
 *
 *  - A .names block is the sum of its cubes, negated when the output column is
 *    '0'. A block with no cubes is constant false.
 */

#ifndef BLIF_H
#define BLIF_H

#include <stddef.h>

typedef struct {
    int  *fanin;     /* signal indices, in the order given on .names        */
    int   n_fanin;
    char **cube;     /* n_cube strings of n_fanin chars from "01-"          */
    int   n_cube;
    int   polarity;  /* 1: cubes are the on-set. 0: cubes are the off-set.  */
} blif_gate;

typedef enum {
    BLIF_UNDEF = 0,  /* mentioned but never driven                          */
    BLIF_VAR,        /* primary input or latch output: a BDD variable       */
    BLIF_GATE        /* driven by a .names block                            */
} blif_kind;

typedef struct {
    char     *name;
    blif_kind kind;
    int       var;   /* variable index, when kind == BLIF_VAR               */
    int       gate;  /* gate index,     when kind == BLIF_GATE              */
} blif_signal;

typedef struct {
    char        *model;

    blif_signal *sig;
    int          n_sig;

    blif_gate   *gate;
    int          n_gate;

    int         *var_sig;   /* variable index -> signal index               */
    int          n_var;
    int          n_latch;   /* how many of the variables came from latches  */

    int         *out_sig;   /* output index -> signal index                 */
    int          n_out;

    /* Gate indices in an order in which every gate follows its fanin gates,
       covering exactly the gates reachable from the outputs. Both drivers
       build in this order, so the two constructions are the same sequence. */
    int         *topo;
    int          n_topo;
} blif_t;

/* Returns 0 on success; on failure returns -1 and writes a message to err. */
int  blif_read(blif_t *b, const char *path, char *err, size_t err_len);
void blif_free(blif_t *b);

/*
 * Choose the starting variable order: perm[i] is the decision-diagram variable
 * that the circuit's i-th input becomes.
 *
 *   "file"    perm[i] = i, the file's own order
 *   "reverse" perm[i] = n-1-i
 *   "random"  a seeded shuffle
 *
 * This lives here, rather than in either driver, so that both tools provably
 * start from the same order: they call the same function with the same seed and
 * it uses its own generator rather than rand(), which the drivers also use.
 * Returns 0 on success, -1 on an unknown mode.
 */
int blif_var_perm(int n, const char *mode, unsigned seed, int *perm);

#endif
