/*
 * Copyright 2011-2014 Formal Methods and Tools, University of Twente
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
 * Sylvan: parallel BDD/ListDD package.
 *
 * This is a multi-core implementation of BDDs with complement edges.
 *
 * This package requires parallel the work-stealing framework Lace.
 * Lace must be initialized before initializing Sylvan
 *
 * This package uses explicit referencing.
 * Use sylvan_ref and sylvan_deref to manage external references.
 *
 * Garbage collection requires all workers to cooperate. Garbage collection is either initiated
 * by the user (calling sylvan_gc) or when the nodes table is full. All Sylvan operations
 * check whether they need to cooperate on garbage collection. Garbage collection cannot occur
 * otherwise. This means that it is perfectly fine to do this:
 *              BDD a = sylvan_ref(sylvan_and(b, c));
 * since it is not possible that garbage collection occurs between the two calls.
 *
 * To temporarily disable garbage collection, use sylvan_gc_disable() and sylvan_gc_enable().
 */

#include <stdint.h>
#include <stdio.h> // for FILE
#include <lace.h> // for definitions

#ifndef SYLVAN_H
#define SYLVAN_H

#ifndef SYLVAN_CACHE_STATS
#define SYLVAN_CACHE_STATS 0
#endif

#ifndef SYLVAN_OPERATION_STATS
#define SYLVAN_OPERATION_STATS 0
#endif

// For now, only support 64-bit systems
typedef char __sylvan_check_size_t_is_8_bytes[(sizeof(uint64_t) == sizeof(size_t))?1:-1];

/**
 * Initialize the Sylvan parallel decision diagrams package.
 *
 * After initialization, call sylvan_init_bdd and/or sylvan_init_ldd if you want to use
 * the BDD and/or LDD functionality.
 *
 * BDDs and LDDs share a common node table and operations cache.
 *
 * The node table is resizable.
 * The table is resized automatically when >50% of the table is filled during garbage collection.
 * 
 * Memory usage:
 * Every node requires 32 bytes memory. (16 data + 16 bytes overhead)
 * Every operation cache entry requires 36 bytes memory. (32 bytes data + 4 bytes overhead)
 *
 * Reasonable defaults: datasize of 26 (2048 MB), cachesize of 24 (576 MB)
 */
void sylvan_init_package(size_t initial_tablesize, size_t max_tablesize, size_t cachesize);

/**
 * Frees all Sylvan data.
 */
void sylvan_quit();

/**
 * Return number of occupied buckets in nodes table and total number of buckets.
 */
VOID_TASK_DECL_2(sylvan_table_usage, size_t*, size_t*);
#define sylvan_table_usage(filled, total) (CALL(sylvan_table_usage, filled, total))

/* Perform garbage collection */
VOID_TASK_DECL_0(sylvan_gc);
#define sylvan_gc() (CALL(sylvan_gc))

/* Enable or disable garbage collection. It is enabled by default. */
void sylvan_gc_enable();
void sylvan_gc_disable();

/**
 * Garbage collection "mark" callbacks.
 * These are called during garbage collection to
 * recursively mark references.
 * They receive one parameter (my_id) which is the
 * index of the worker (0..workers-1)
 */
LACE_TYPEDEF_CB(gc_mark_cb, int);
void sylvan_gc_register_mark(gc_mark_cb cb);

#include <sylvan_bdd.h>
#include <sylvan_ldd.h>

#endif
