/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
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

#include <sylvan_config.h>

#include <stdint.h>
#include <stdio.h> // for FILE
#include <stdlib.h>
#include <lace.h> // for definitions
#include <tls.h>

#include <sylvan_cache.h>
#include <llmsset.h>
#include <sylvan_stats.h>

#include <sylvan_common.h>
#include <sylvan_mtbdd.h>
#include <sylvan_bdd.h>
#include <sylvan_ldd.h>
