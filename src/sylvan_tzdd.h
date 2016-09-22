/*
 * Copyright 2016 Tom van Dijk, Johannes Kepler University Linz
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
 * This is an implementation of Hybrid Multi-Terminal Zero-Suppressed Binary Decision Diagrams.
 */

/* Do not include this file directly. Instead, include sylvan.h */

#ifndef SYLVAN_TZDD_H
#define SYLVAN_TZDD_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * An TZDD is a 64-bit value. The low 32 bits are an index into the unique table.
 */
typedef uint64_t TZDD;
typedef TZDD TZDDMAP;

/**
 * Use 0 and 1 for false/true, the nodes table reserves these values
 */
#define tzdd_false         ((TZDD)0x0000000000000000LL)
#define tzdd_true          ((TZDD)0x0000000000000001LL)
#define tzdd_invalid       ((TZDD)0xffffffffffffffffLL)

/**
 * Initialize TZDD functionality.
 * This initializes internal and external referencing datastructures,
 * and registers them in the garbage collection framework.
 */
void sylvan_init_tzdd(void);

/**
 * Create a TZDD terminal of type <type> and value <value>.
 * For custom types, the value could be a pointer to some external struct.
 */
TZDD tzdd_makeleaf(uint32_t type, uint64_t value);

/**
 * Create an internal TZDD node.
 * This method does NOT check variable ordering!
 */
TZDD tzdd_makenode(uint32_t var, TZDD pos, TZDD neg, TZDD zero);

/**
 * Returns 1 is the TZDD is a terminal, or 0 otherwise.
 */
int tzdd_isleaf(TZDD tzdd);
#define tzdd_isnode(tzdd) (tzdd_isleaf(tzdd) ? 0 : 1)

/**
 * For TZDD terminals, returns <type> and <value>
 */
uint32_t tzdd_gettype(TZDD terminal);
uint64_t tzdd_getvalue(TZDD terminal);

/**
 * For internal TZDD nodes, returns <var>, <pos>, <neg>, <zero>
 */
uint32_t tzdd_getvar(TZDD node);
TZDD tzdd_getpos(TZDD node);
TZDD tzdd_getneg(TZDD node);
TZDD tzdd_getzero(TZDD node);

/**
 * Convert an MTBDD to a TZDD.
 */
// TASK_DECL_2(TZDD, tzdd_from_mtbdd, MTBDD, MTBDD);
// #define tzdd_from_mtbdd(dd, domain) CALL(tzdd_from_mtbdd, dd, domain)

/**
 * Count the number of TZDD nodes and terminals (excluding tzdd_false and tzdd_true) in the given <count> TZDDs
 */
size_t tzdd_nodecount_more(const TZDD *tzdds, size_t count);

static inline size_t
tzdd_nodecount(const TZDD dd) {
    return tzdd_nodecount_more(&dd, 1);
}

/**
 * Garbage collection
 * Sylvan supplies two default methods to handle references to nodes, but the user
 * is encouraged to implement custom handling. Simply add a handler using sylvan_gc_add_mark
 * and let the handler call tzdd_gc_mark_rec for every TZDD that should be saved
 * during garbage collection.
 */

/**
 * Call tzdd_gc_mark_rec for every tzdd you want to keep in your custom mark functions.
 */
VOID_TASK_DECL_1(tzdd_gc_mark_rec, TZDD);
#define tzdd_gc_mark_rec(tzdd) CALL(tzdd_gc_mark_rec, tzdd)

/**
 * Default external referencing. During garbage collection, TZDDs marked with tzdd_ref will
 * be kept in the forest.
 * It is recommended to prefer tzdd_protect and tzdd_unprotect.
 */
TZDD tzdd_ref(TZDD a);
void tzdd_deref(TZDD a);
size_t tzdd_count_refs(void);

/**
 * Default external pointer referencing. During garbage collection, the pointers are followed and the TZDD
 * that they refer to are kept in the forest.
 */
void tzdd_protect(TZDD* ptr);
void tzdd_unprotect(TZDD* ptr);
size_t tzdd_count_protected(void);

/**
 * Infrastructure for internal references (per-thread, e.g. during TZDD operations)
 * Use tzdd_refs_push and tzdd_refs_pop to put TZDDs on a thread-local reference stack.
 * Use tzdd_refs_spawn and tzdd_refs_sync around SPAWN and SYNC operations when the result
 * of the spawned Task is a TZDD that must be kept during garbage collection.
 */
typedef struct tzdd_refs_internal
{
    size_t r_size, r_count;
    size_t s_size, s_count;
    TZDD *results;
    Task **spawns;
} *tzdd_refs_internal_t;

extern DECLARE_THREAD_LOCAL(tzdd_refs_key, tzdd_refs_internal_t);

static inline TZDD
tzdd_refs_push(TZDD tzdd)
{
    LOCALIZE_THREAD_LOCAL(tzdd_refs_key, tzdd_refs_internal_t);
    if (tzdd_refs_key->r_count >= tzdd_refs_key->r_size) {
        tzdd_refs_key->r_size *= 2;
        tzdd_refs_key->results = (TZDD*)realloc(tzdd_refs_key->results, sizeof(TZDD) * tzdd_refs_key->r_size);
    }
    tzdd_refs_key->results[tzdd_refs_key->r_count++] = tzdd;
    return tzdd;
}

static inline void
tzdd_refs_pop(int amount)
{
    LOCALIZE_THREAD_LOCAL(tzdd_refs_key, tzdd_refs_internal_t);
    tzdd_refs_key->r_count-=amount;
}

static inline void
tzdd_refs_spawn(Task *t)
{
    LOCALIZE_THREAD_LOCAL(tzdd_refs_key, tzdd_refs_internal_t);
    if (tzdd_refs_key->s_count >= tzdd_refs_key->s_size) {
        tzdd_refs_key->s_size *= 2;
        tzdd_refs_key->spawns = (Task**)realloc(tzdd_refs_key->spawns, sizeof(Task*) * tzdd_refs_key->s_size);
    }
    tzdd_refs_key->spawns[tzdd_refs_key->s_count++] = t;
}

static inline TZDD
tzdd_refs_sync(TZDD result)
{
    LOCALIZE_THREAD_LOCAL(tzdd_refs_key, tzdd_refs_internal_t);
    tzdd_refs_key->s_count--;
    return result;
}

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
