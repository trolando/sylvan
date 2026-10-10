/*
 * --probs: the probability of the first output when every input is an
 * independent event with a given probability -- the top-event probability of
 * a fault tree. Shared by both drivers, so that both read the same file the
 * same way and compute in the same arithmetic.
 *
 * THE FILE (tools/mef2blif.py writes one next to every BLIF it writes):
 * '#' comment lines and blank lines, then one line per name of the circuit's
 * variables, in the order blif.c numbers them:
 *
 *     NAME PROBABILITY
 *
 * Each name must be that variable's name, so that a file written for another
 * circuit, or for the same tree under another order, is refused rather than
 * misread.
 *
 * THE COMPUTATION is one pass over the diagram, memoised on the edge, so a
 * complemented edge is a function of its own:
 *
 *     P(f) = (1 - p_v) P(f|v=0) + p_v P(f|v=1),  P(true) = 1, P(false) = 0,
 *
 * v the variable labelling f's root. Every term is a product of probabilities
 * and every sum is of non-negative terms, so there is no cancellation: a
 * probability of 1e-13 comes out to a relative 1e-16 or so per level, never
 * as 1 - (1 - 1e-13). The value does not depend on the order, but the order
 * decides in which order the terms are added, so before and after a sift the
 * two values agree to a relative n_var ulps (about 1e-13), not to the bit.
 */

#ifndef SIFT_BENCH_PROBS_H
#define SIFT_BENCH_PROBS_H

#include <stddef.h>
#include <stdint.h>

#include "blif.h"

/*
 * Read PATH for circuit B into p[0 .. B->n_var - 1], p[i] for the circuit's
 * variable i. Returns 0, or -1 with a message in err.
 */
int probs_read(const blif_t *b, const char *path, double *p, char *err, size_t err_len);

/* The memo of a pass: edge -> probability, open addressing, growing at half
   full. Key 0 is the empty slot; neither driver has a non-terminal edge 0. */
typedef struct {
    uint64_t *key;
    double   *val;
    size_t    mask;
    size_t    used;
    int       failed;   /* an allocation failed: the pass is not to be trusted */
} probs_memo;

int  probs_memo_init(probs_memo *m);
int  probs_memo_get(const probs_memo *m, uint64_t key, double *val);
void probs_memo_put(probs_memo *m, uint64_t key, double val);
void probs_memo_free(probs_memo *m);

/* The two RESULT fields, prob_before and prob_after, with their leading
   comma; null for a value that could not be computed (negative). */
void probs_print(double before, double after);

#endif
