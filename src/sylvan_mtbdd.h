/*
 * Copyright 2011-2015 Formal Methods and Tools, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * This is an implementation of Multi-Terminal Binary Decision Diagrams.
 * They encode functions on Boolean variables to any domain.
 *
 * Three domains are supported by default: Boolean, Integer and Real.
 * Boolean MTBDDs are identical to BDDs (as supported by the bdd subpackage).
 * Integer MTBDDs are encoded using "uint64_t" terminals.
 * Real MTBDDs are encoded using "double" terminals.
 * Negative integers/reals are encoded using the complement edge.
 *
 * Labels of Boolean variables of MTBDD nodes are 24-bit integers.
 *
 * Custom terminals are supported. For notification when nodes are deleted in gc,
 * set a callback using sylvan_set_ondead and for each custom terminal node, call
 * the function mtbdd_notify_ondead.
 *
 * Terminal type "0" is the Integer type, type "1" is the Real type.
 * For non-Boolean MTBDDs, mtbdd_false is used for partial functions, i.e. mtbdd_false
 * indicates that the function is not defined for a certain input.
 */

/* Do not include this file directly. Instead, include sylvan.h */

#ifndef SYLVAN_MTBDD_H
#define SYLVAN_MTBDD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * An MTBDD is a 64-bit value. The low 40 bits are an index into the unique table.
 * The highest 1 bit is the complement edge, indicating negation.
 * For Boolean MTBDDs, this means "not X", for Integer and Real MTBDDs, this means "-X".
 */
typedef uint64_t MTBDD;

/**
 * mtbdd_true is only used in Boolean MTBDDs. mtbdd_false has multiple roles (see above).
 */
#define mtbdd_complement    ((MTBDD)0x8000000000000000LL)
#define mtbdd_false         ((MTBDD)0)
#define mtbdd_true          (mtbdd_false|mtbdd_complement)

/**
 * Initialize MTBDD functionality.
 * This initializes internal and external referencing datastructures,
 * and registers them in the garbage collection framework.
 */
void sylvan_init_mtbdd();

/**
 * Create a MTBDD terminal of type <type> and value <value>.
 * For custom types, the value could be a pointer to some external struct.
 */
MTBDD mtbdd_makeleaf(uint32_t type, uint64_t value);

/**
 * Create an internal MTBDD node of Boolean variable <var>, with low edge <low> and high edge <high>.
 * <var> is a 24-bit integer.
 */
MTBDD mtbdd_makenode(uint32_t var, MTBDD low, MTBDD high);

/**
 * Returns 1 is the MTBDD is a terminal, or 0 otherwise.
 */
int mtbdd_isleaf(MTBDD mtbdd);
#define mtbdd_isnode(mtbdd) (mtbdd_isleaf(mtbdd) ? 0 : 1)

/**
 * For MTBDD terminals, returns <type> and <value>
 */
uint32_t mtbdd_gettype(MTBDD terminal);
uint64_t mtbdd_getvalue(MTBDD terminal);

/**
 * For internal MTBDD nodes, returns <var>, <low> and <high>
 */
uint32_t mtbdd_getvar(MTBDD node);
MTBDD mtbdd_getlow(MTBDD node);
MTBDD mtbdd_gethigh(MTBDD node);

/**
 * Compute the negation of the MTBDD
 * For Boolean MTBDDs, this means "not X", for integer and reals, this means "-X".
 */
#define mtbdd_isnegated(dd) ((dd & mtbdd_complement) ? 1 : 0)
#define mtbdd_negate(dd) (dd ^ mtbdd_complement)
#define mtbdd_not(dd) (dd ^ mtbdd_complement)

/**
 * Create terminals representing uint64_t (type 0) and double (type 1) values
 */
MTBDD mtbdd_uint64(uint64_t value);
MTBDD mtbdd_double(double value);

/**
 * Get the value of a terminal (for Integer and Real terminals, types 0 and 1)
 */
#define mtbdd_getuint64(terminal) mtbdd_getvalue(terminal)
double mtbdd_getdouble(MTBDD terminal);

/**
 * Create the conjunction of variables in arr.
 * I.e. arr[0] \and arr[1] \and ... \and arr[length-1]
 */
MTBDD mtbdd_fromarray(uint32_t* arr, size_t length);

/**
 * Create a MTBDD cube representing the conjunction of variables in their positive or negative
 * form depending on whether the cube[idx] equals 0 (negative), 1 (positive) or 2 (any).
 * Use cube[idx]==3 for "s=s'" in interleaved variables (matches with next variable)
 * <variables> is the cube of variables (var1 \and var2 \and ... \and varn)
 */
MTBDD mtbdd_cube(MTBDD variables, uint8_t *cube, MTBDD terminal);

/**
 * Same as mtbdd_cube, but extends <mtbdd> with the assignment <cube> \to <terminal>.
 * If <mtbdd> already assigns a value to the cube, the new value <terminal> is taken.
 * Does not support cube[idx]==3.
 */
#define mtbdd_union_cube(mtbdd, variables, cube, terminal) CALL(mtbdd_union_cube, mtbdd, variables, cube, terminal)
TASK_DECL_4(BDD, mtbdd_union_cube, MTBDD, MTBDD, uint8_t*, MTBDD);

/**
 * Count the number of MTBDD nodes and terminals (excluding mtbdd_false and mtbdd_true) in a MTBDD)
 */
size_t mtbdd_nodecount(MTBDD mtbdd);

/**
 * Write a DOT representation of a MTBDD
 * The callback function is required for custom terminals.
 */
typedef void (*print_terminal_label_cb)(FILE *out, uint32_t type, uint64_t value);
void mtbdd_fprintdot(FILE *out, MTBDD mtbdd, print_terminal_label_cb cb);
#define mtbdd_printdot(mtbdd, cb) mtbdd_fprintdot(stdout, mtbdd, cb)

/**
 * Garbage collection
 * Sylvan supplies two default methods to handle references to nodes, but the user
 * is encouraged to implement custom handling. Simply add a handler using sylvan_gc_add_mark
 * and let the handler call mtbdd_gc_mark_rec for every MTBDD that should be saved
 * during garbage collection.
 */

/**
 * Call mtbdd_gc_mark_rec for every mtbdd you want to keep in your custom mark functions.
 */
VOID_TASK_DECL_1(mtbdd_gc_mark_rec, MTBDD);
#define mtbdd_gc_mark_rec(mtbdd) CALL(mtbdd_gc_mark_rec, mtbdd)

/**
 * Default external referencing. During garbage collection, MTBDDs marked with mtbdd_ref will
 * be kept in the forest.
 * It is recommended to prefer mtbdd_protect and mtbdd_unprotect.
 */
MTBDD mtbdd_ref(MTBDD a);
void mtbdd_deref(MTBDD a);
size_t mtbdd_count_refs();

/**
 * Default external pointer referencing. During garbage collection, the pointers are followed and the MTBDD
 * that they refer to are kept in the forest.
 */
void mtbdd_protect(MTBDD* ptr);
void mtbdd_unprotect(MTBDD* ptr);
size_t mtbdd_count_protected();

/**
 * If sylvan_set_ondead is set to a callback, then this function marks MTBDDs (terminals).
 * When they are dead after the mark phase in garbage collection, the callback is called for marked MTBDDs.
 * The ondead callback can either perform cleanup or resurrect dead terminals.
 */
#define mtbdd_notify_ondead(dd) llmsset_notify_ondead(nodes, dd&~mtbdd_complement)

/**
 * Infrastructure for internal references (per-thread, e.g. during MTBDD operations)
 * Use mtbdd_refs_push and mtbdd_refs_pop to put MTBDDs on a thread-local reference stack.
 * Use mtbdd_refs_spawn and mtbdd_refs_sync around SPAWN and SYNC operations when the result
 * of the spawned Task is a MTBDD that must be kept during garbage collection.
 */
typedef struct mtbdd_refs_internal
{
    size_t r_size, r_count;
    size_t s_size, s_count;
    MTBDD *results;
    Task **spawns;
} *mtbdd_refs_internal_t;

extern DECLARE_THREAD_LOCAL(mtbdd_refs_key, mtbdd_refs_internal_t);

static inline MTBDD
mtbdd_refs_push(MTBDD mtbdd)
{
    LOCALIZE_THREAD_LOCAL(mtbdd_refs_key, mtbdd_refs_internal_t);
    if (mtbdd_refs_key->r_count >= mtbdd_refs_key->r_size) {
        mtbdd_refs_key->r_size *= 2;
        mtbdd_refs_key->results = (MTBDD*)realloc(mtbdd_refs_key->results, sizeof(MTBDD) * mtbdd_refs_key->r_size);
    }
    mtbdd_refs_key->results[mtbdd_refs_key->r_count++] = mtbdd;
    return mtbdd;
}

static inline void
mtbdd_refs_pop(int amount)
{
    LOCALIZE_THREAD_LOCAL(mtbdd_refs_key, mtbdd_refs_internal_t);
    mtbdd_refs_key->r_count-=amount;
}

static inline void
mtbdd_refs_spawn(Task *t)
{
    LOCALIZE_THREAD_LOCAL(mtbdd_refs_key, mtbdd_refs_internal_t);
    if (mtbdd_refs_key->s_count >= mtbdd_refs_key->s_size) {
        mtbdd_refs_key->s_size *= 2;
        mtbdd_refs_key->spawns = (Task**)realloc(mtbdd_refs_key->spawns, sizeof(Task*) * mtbdd_refs_key->s_size);
    }
    mtbdd_refs_key->spawns[mtbdd_refs_key->s_count++] = t;
}

static inline MTBDD
mtbdd_refs_sync(MTBDD result)
{
    LOCALIZE_THREAD_LOCAL(mtbdd_refs_key, mtbdd_refs_internal_t);
    mtbdd_refs_key->s_count--;
    return result;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
