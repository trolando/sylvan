/* See blif.h. Plain C99, no dependencies. */

#include "blif.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- symbols */

typedef struct {
    int   *slot;      /* signal index + 1, 0 when empty */
    size_t mask;
    size_t used;
} symtab;

static size_t
sym_hash(const char *s)
{
    size_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    return h;
}

static int
sym_init(symtab *t)
{
    t->mask = 1023;
    t->used = 0;
    t->slot = (int*)calloc(t->mask + 1, sizeof(int));
    return t->slot == NULL ? -1 : 0;
}

static int sym_grow(symtab *t, const blif_signal *sig);

/* Look up <name>, appending a new signal when it is not there yet. */
static int
sym_intern(symtab *t, blif_signal **sig, int *n_sig, int *cap_sig,
           const char *name)
{
    if ((t->used + 1) * 10 > (t->mask + 1) * 7) {
        if (sym_grow(t, *sig) != 0) return -1;
    }
    size_t i = sym_hash(name) & t->mask;
    while (t->slot[i] != 0) {
        if (strcmp((*sig)[t->slot[i] - 1].name, name) == 0) {
            return t->slot[i] - 1;
        }
        i = (i + 1) & t->mask;
    }
    if (*n_sig == *cap_sig) {
        int cap = *cap_sig < 64 ? 64 : *cap_sig * 2;
        blif_signal *grown =
            (blif_signal*)realloc(*sig, (size_t)cap * sizeof(blif_signal));
        if (grown == NULL) return -1;
        *sig = grown;
        *cap_sig = cap;
    }
    blif_signal *s = &(*sig)[*n_sig];
    s->name = strdup(name);
    if (s->name == NULL) return -1;
    s->kind = BLIF_UNDEF;
    s->var = -1;
    s->gate = -1;
    t->slot[i] = *n_sig + 1;
    t->used++;
    return (*n_sig)++;
}

static int
sym_grow(symtab *t, const blif_signal *sig)
{
    size_t mask = t->mask * 2 + 1;
    int *slot = (int*)calloc(mask + 1, sizeof(int));
    if (slot == NULL) return -1;
    for (size_t i = 0; i <= t->mask; i++) {
        if (t->slot[i] == 0) continue;
        size_t j = sym_hash(sig[t->slot[i] - 1].name) & mask;
        while (slot[j] != 0) j = (j + 1) & mask;
        slot[j] = t->slot[i];
    }
    free(t->slot);
    t->slot = slot;
    t->mask = mask;
    return 0;
}

/* ------------------------------------------------------------------ input */

/* Read the whole file and splice backslash continuations into one line. */
static char *
slurp(const char *path, char *err, size_t err_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { snprintf(err, err_len, "cannot open %s", path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *text = (char*)malloc((size_t)n + 1);
    if (text == NULL) { fclose(f); return NULL; }
    size_t got = fread(text, 1, (size_t)n, f);
    fclose(f);
    text[got] = '\0';

    /* A backslash at end of line, and the newline after it, become spaces. */
    char *w = text;
    for (char *r = text; *r; r++) {
        if (*r == '\\') {
            char *p = r + 1;
            while (*p == ' ' || *p == '\t' || *p == '\r') p++;
            if (*p == '\n') { *w++ = ' '; r = p; continue; }
        }
        if (*r == '\r') continue;
        *w++ = *r;
    }
    *w = '\0';
    return text;
}

/* --------------------------------------------------------------- topology */

/* Depth-first from every output, iteratively, so a deep circuit cannot
   overflow the C stack. Fills b->topo with the reachable gates, fanin first. */
static int
topo_build(blif_t *b, char *err, size_t err_len)
{
    char *mark = (char*)calloc((size_t)b->n_gate, 1); /* 0 new 1 open 2 done */
    int  *stack = (int*)malloc((size_t)(b->n_gate + 1) * sizeof(int) * 2);
    b->topo = (int*)malloc((size_t)b->n_gate * sizeof(int));
    if (mark == NULL || stack == NULL || b->topo == NULL) {
        free(mark); free(stack);
        snprintf(err, err_len, "out of memory building topological order");
        return -1;
    }
    b->n_topo = 0;

    for (int o = 0; o < b->n_out; o++) {
        const blif_signal *s = &b->sig[b->out_sig[o]];
        if (s->kind != BLIF_GATE) continue;

        int top = 0;
        stack[top * 2] = s->gate;
        stack[top * 2 + 1] = 0;
        top++;
        while (top > 0) {
            int g = stack[(top - 1) * 2];
            int i = stack[(top - 1) * 2 + 1];
            if (mark[g] == 2) { top--; continue; }
            mark[g] = 1;
            if (i < b->gate[g].n_fanin) {
                stack[(top - 1) * 2 + 1] = i + 1;
                const blif_signal *in = &b->sig[b->gate[g].fanin[i]];
                if (in->kind == BLIF_GATE && mark[in->gate] == 0) {
                    stack[top * 2] = in->gate;
                    stack[top * 2 + 1] = 0;
                    top++;
                } else if (in->kind == BLIF_GATE && mark[in->gate] == 1) {
                    snprintf(err, err_len, "combinational cycle at signal %s",
                             in->name);
                    free(mark); free(stack);
                    return -1;
                }
                continue;
            }
            mark[g] = 2;
            b->topo[b->n_topo++] = g;
            top--;
        }
    }
    free(mark);
    free(stack);
    return 0;
}

/* ------------------------------------------------------------------- read */

int
blif_read(blif_t *b, const char *path, char *err, size_t err_len)
{
    memset(b, 0, sizeof(*b));
    err[0] = '\0';

    char *text = slurp(path, err, err_len);
    if (text == NULL) {
        if (err[0] == '\0') snprintf(err, err_len, "cannot read %s", path);
        return -1;
    }

    symtab tab;
    if (sym_init(&tab) != 0) { free(text); return -1; }

    int cap_sig = 0, cap_gate = 0;
    int cap_var = 0, cap_out = 0;
    int current = -1;                 /* gate whose cubes are being read */
    int rc = -1;

    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line != NULL;
         line = strtok_r(NULL, "\n", &save)) {

        char *hash = strchr(line, '#');
        if (hash != NULL) *hash = '\0';
        while (*line == ' ' || *line == '\t') line++;
        if (*line == '\0') continue;

        if (*line != '.') {
            /* A cube of the current .names block. */
            if (current < 0) {
                snprintf(err, err_len, "cube outside a .names block: %s", line);
                goto done;
            }
            blif_gate *g = &b->gate[current];
            char *sp = NULL;
            char *t1 = strtok_r(line, " \t", &sp);
            char *t2 = strtok_r(NULL, " \t", &sp);
            const char *in = g->n_fanin > 0 ? t1 : "";
            const char *out = g->n_fanin > 0 ? t2 : t1;
            if (out == NULL) {
                snprintf(err, err_len, "malformed cube in .names for signal");
                goto done;
            }
            if ((int)strlen(in) != g->n_fanin) {
                snprintf(err, err_len,
                         "cube '%s' has %d literals, gate has %d inputs",
                         in, (int)strlen(in), g->n_fanin);
                goto done;
            }
            if (*out != '0' && *out != '1') {
                snprintf(err, err_len, "bad output column '%s'", out);
                goto done;
            }
            if (g->n_cube > 0 && g->polarity != (*out == '1')) {
                snprintf(err, err_len, "mixed output polarity in one .names");
                goto done;
            }
            g->polarity = (*out == '1');
            char **grown = (char**)realloc(g->cube,
                                           (size_t)(g->n_cube + 1) * sizeof(char*));
            if (grown == NULL) goto done;
            g->cube = grown;
            g->cube[g->n_cube] = strdup(in);
            if (g->cube[g->n_cube] == NULL) goto done;
            g->n_cube++;
            continue;
        }

        current = -1;
        char *sp = NULL;
        char *dir = strtok_r(line, " \t", &sp);

        if (strcmp(dir, ".model") == 0) {
            char *name = strtok_r(NULL, " \t", &sp);
            b->model = strdup(name == NULL ? "" : name);

        } else if (strcmp(dir, ".inputs") == 0 || strcmp(dir, ".outputs") == 0) {
            const int is_in = strcmp(dir, ".inputs") == 0;
            for (char *t = strtok_r(NULL, " \t", &sp); t != NULL;
                 t = strtok_r(NULL, " \t", &sp)) {
                int s = sym_intern(&tab, &b->sig, &b->n_sig, &cap_sig, t);
                if (s < 0) goto done;
                if (is_in) {
                    if (b->n_var == cap_var) {
                        cap_var = cap_var < 64 ? 64 : cap_var * 2;
                        int *g2 = (int*)realloc(b->var_sig,
                                                (size_t)cap_var * sizeof(int));
                        if (g2 == NULL) goto done;
                        b->var_sig = g2;
                    }
                    b->sig[s].kind = BLIF_VAR;
                    b->sig[s].var = b->n_var;
                    b->var_sig[b->n_var++] = s;
                } else {
                    if (b->n_out == cap_out) {
                        cap_out = cap_out < 64 ? 64 : cap_out * 2;
                        int *g2 = (int*)realloc(b->out_sig,
                                                (size_t)cap_out * sizeof(int));
                        if (g2 == NULL) goto done;
                        b->out_sig = g2;
                    }
                    b->out_sig[b->n_out++] = s;
                }
            }

        } else if (strcmp(dir, ".latch") == 0) {
            /* .latch <input> <output> [<type> <control>] [<init>]
               Only the output matters here: it becomes a free variable. */
            char *tok[6];
            int n = 0;
            for (char *t = strtok_r(NULL, " \t", &sp); t != NULL && n < 6;
                 t = strtok_r(NULL, " \t", &sp)) {
                tok[n++] = t;
            }
            if (n < 2) {
                snprintf(err, err_len, "malformed .latch");
                goto done;
            }
            int s = sym_intern(&tab, &b->sig, &b->n_sig, &cap_sig, tok[1]);
            if (s < 0) goto done;
            if (b->n_var == cap_var) {
                cap_var = cap_var < 64 ? 64 : cap_var * 2;
                int *g2 = (int*)realloc(b->var_sig,
                                        (size_t)cap_var * sizeof(int));
                if (g2 == NULL) goto done;
                b->var_sig = g2;
            }
            b->sig[s].kind = BLIF_VAR;
            b->sig[s].var = b->n_var;
            b->var_sig[b->n_var++] = s;
            b->n_latch++;

        } else if (strcmp(dir, ".names") == 0) {
            char *tok[256];
            int n = 0;
            for (char *t = strtok_r(NULL, " \t", &sp); t != NULL && n < 256;
                 t = strtok_r(NULL, " \t", &sp)) {
                tok[n++] = t;
            }
            if (n < 1) { snprintf(err, err_len, "empty .names"); goto done; }

            if (b->n_gate == cap_gate) {
                cap_gate = cap_gate < 64 ? 64 : cap_gate * 2;
                blif_gate *g2 = (blif_gate*)realloc(
                    b->gate, (size_t)cap_gate * sizeof(blif_gate));
                if (g2 == NULL) goto done;
                b->gate = g2;
            }
            blif_gate *g = &b->gate[b->n_gate];
            memset(g, 0, sizeof(*g));
            g->n_fanin = n - 1;
            g->polarity = 1;
            if (g->n_fanin > 0) {
                g->fanin = (int*)malloc((size_t)g->n_fanin * sizeof(int));
                if (g->fanin == NULL) goto done;
                for (int i = 0; i < g->n_fanin; i++) {
                    int s = sym_intern(&tab, &b->sig, &b->n_sig, &cap_sig, tok[i]);
                    if (s < 0) goto done;
                    g->fanin[i] = s;
                }
            }
            int out = sym_intern(&tab, &b->sig, &b->n_sig, &cap_sig, tok[n - 1]);
            if (out < 0) goto done;
            if (b->sig[out].kind == BLIF_GATE) {
                snprintf(err, err_len, "signal %s driven twice", tok[n - 1]);
                goto done;
            }
            if (b->sig[out].kind == BLIF_VAR) {
                snprintf(err, err_len, "signal %s is both a variable and a gate",
                         tok[n - 1]);
                goto done;
            }
            b->sig[out].kind = BLIF_GATE;
            b->sig[out].gate = b->n_gate;
            current = b->n_gate;
            b->n_gate++;

        } else if (strcmp(dir, ".end") == 0) {
            break;

        } else {
            snprintf(err, err_len, "unsupported directive %s", dir);
            goto done;
        }
    }

    for (int i = 0; i < b->n_sig; i++) {
        if (b->sig[i].kind == BLIF_UNDEF) {
            snprintf(err, err_len, "signal %s is used but never driven",
                     b->sig[i].name);
            goto done;
        }
    }
    if (b->n_out == 0) { snprintf(err, err_len, "no outputs"); goto done; }
    if (b->n_var == 0) { snprintf(err, err_len, "no variables"); goto done; }

    if (topo_build(b, err, err_len) != 0) goto done;
    rc = 0;

done:
    free(tab.slot);
    free(text);
    if (rc != 0) blif_free(b);
    return rc;
}

int
blif_var_perm(int n, const char *mode, unsigned seed, int *perm)
{
    for (int i = 0; i < n; i++) perm[i] = i;

    if (strcmp(mode, "file") == 0) return 0;

    if (strcmp(mode, "reverse") == 0) {
        for (int i = 0; i < n; i++) perm[i] = n - 1 - i;
        return 0;
    }

    if (strcmp(mode, "random") == 0) {
        /* Fisher-Yates over a self-contained 64-bit LCG, so the permutation
           depends on nothing but n and the seed. */
        uint64_t s = (uint64_t)seed * 6364136223846793005ull + 1442695040888963407ull;
        for (int i = n - 1; i > 0; i--) {
            s = s * 6364136223846793005ull + 1442695040888963407ull;
            const int j = (int)((s >> 33) % (uint64_t)(i + 1));
            const int t = perm[i]; perm[i] = perm[j]; perm[j] = t;
        }
        return 0;
    }

    return -1;
}

void
blif_free(blif_t *b)
{
    for (int i = 0; i < b->n_sig; i++) free(b->sig[i].name);
    free(b->sig);
    for (int i = 0; i < b->n_gate; i++) {
        free(b->gate[i].fanin);
        for (int c = 0; c < b->gate[i].n_cube; c++) free(b->gate[i].cube[c]);
        free(b->gate[i].cube);
    }
    free(b->gate);
    free(b->var_sig);
    free(b->out_sig);
    free(b->topo);
    free(b->model);
    memset(b, 0, sizeof(*b));
}
