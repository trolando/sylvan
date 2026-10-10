/* See probs.h. Plain C99, no dependencies. */

#include "probs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ the file */

static char *
slurp(const char *path, char *err, size_t err_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) { snprintf(err, err_len, "cannot open %s", path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); snprintf(err, err_len, "cannot read %s", path); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); snprintf(err, err_len, "cannot read %s", path); return NULL; }
    rewind(f);
    char *text = (char*)malloc((size_t)n + 1);
    if (text == NULL) { fclose(f); snprintf(err, err_len, "out of memory"); return NULL; }
    size_t got = fread(text, 1, (size_t)n, f);
    fclose(f);
    text[got] = '\0';
    return text;
}

static int
is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

int
probs_read(const blif_t *b, const char *path, double *p, char *err, size_t err_len)
{
    char *text = slurp(path, err, err_len);
    if (text == NULL) return -1;
    int rc = -1, i = 0, line_no = 0;
    char *line = text;
    while (line != NULL && *line != '\0') {
        char *next = strchr(line, '\n');
        if (next != NULL) *next++ = '\0';
        line_no++;
        char *s = line;
        line = next;
        while (is_space(*s)) s++;
        if (*s == '\0' || *s == '#') continue;
        char *name = s;
        while (*s != '\0' && !is_space(*s)) s++;
        if (*s == '\0') {
            snprintf(err, err_len, "%s:%d: no probability after %s", path, line_no, name);
            goto done;
        }
        *s++ = '\0';
        errno = 0;
        char *end;
        const double v = strtod(s, &end);
        while (is_space(*end)) end++;
        if (end == s || *end != '\0' || errno != 0 || !(v >= 0.0 && v <= 1.0)) {
            snprintf(err, err_len, "%s:%d: '%s' is not a probability", path, line_no, s);
            goto done;
        }
        if (i >= b->n_var) {
            snprintf(err, err_len, "%s:%d: more lines than the circuit's %d variables",
                     path, line_no, b->n_var);
            goto done;
        }
        const char *want = b->sig[b->var_sig[i]].name;
        if (strcmp(name, want) != 0) {
            snprintf(err, err_len, "%s:%d: %s, where variable %d is %s", path, line_no,
                     name, i, want);
            goto done;
        }
        p[i++] = v;
    }
    if (i != b->n_var) {
        snprintf(err, err_len, "%s: %d probabilities for %d variables", path, i, b->n_var);
        goto done;
    }
    rc = 0;
done:
    free(text);
    return rc;
}

/* ------------------------------------------------------------------ the memo */

static size_t
slot_of(uint64_t key, size_t mask)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    return (size_t)key & mask;
}

int
probs_memo_init(probs_memo *m)
{
    const size_t cap = 1u << 12;
    m->key = (uint64_t*)calloc(cap, sizeof(uint64_t));
    m->val = (double*)malloc(cap * sizeof(double));
    m->mask = cap - 1;
    m->used = 0;
    m->failed = m->key == NULL || m->val == NULL;
    return m->failed ? -1 : 0;
}

int
probs_memo_get(const probs_memo *m, uint64_t key, double *val)
{
    if (m->key == NULL) return 0;
    for (size_t h = slot_of(key, m->mask); m->key[h] != 0; h = (h + 1) & m->mask) {
        if (m->key[h] == key) { *val = m->val[h]; return 1; }
    }
    return 0;
}

static void
insert(uint64_t *keys, double *vals, size_t mask, uint64_t key, double val)
{
    size_t h = slot_of(key, mask);
    while (keys[h] != 0 && keys[h] != key) h = (h + 1) & mask;
    keys[h] = key;
    vals[h] = val;
}

void
probs_memo_put(probs_memo *m, uint64_t key, double val)
{
    if (m->failed || key == 0) { m->failed = 1; return; }
    if (2 * (m->used + 1) > m->mask + 1) {
        const size_t cap = 2 * (m->mask + 1);
        uint64_t *k = (uint64_t*)calloc(cap, sizeof(uint64_t));
        double *v = (double*)malloc(cap * sizeof(double));
        if (k == NULL || v == NULL) { free(k); free(v); m->failed = 1; return; }
        for (size_t h = 0; h <= m->mask; h++) {
            if (m->key[h] != 0) insert(k, v, cap - 1, m->key[h], m->val[h]);
        }
        free(m->key);
        free(m->val);
        m->key = k;
        m->val = v;
        m->mask = cap - 1;
    }
    insert(m->key, m->val, m->mask, key, val);
    m->used++;
}

void
probs_memo_free(probs_memo *m)
{
    free(m->key);
    free(m->val);
    m->key = NULL;
    m->val = NULL;
}

/* ------------------------------------------------------------------ the row */

void
probs_print(double before, double after)
{
    printf(",\"prob_before\":");
    if (before >= 0.0) printf("%.17g", before); else printf("null");
    printf(",\"prob_after\":");
    if (after >= 0.0) printf("%.17g", after); else printf("null");
}
