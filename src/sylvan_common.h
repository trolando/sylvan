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

#include <assert.h>
#include <sylvan.h>
#include <tls.h>

#ifndef SYLVAN_COMMON_H
#define SYLVAN_COMMON_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/* Garbage collection test task - t */
#define sylvan_gc_test() YIELD_NEWFRAME()

// BDD operations
#define CACHE_ITE             0
#define CACHE_COUNT           1
#define CACHE_EXISTS          2
#define CACHE_AND_EXISTS      3
#define CACHE_RELNEXT         4
#define CACHE_RELPREV         5
#define CACHE_SATCOUNT        6
#define CACHE_COMPOSE         7
#define CACHE_RESTRICT        8
#define CACHE_CONSTRAIN       9
#define CACHE_CLOSURE         10
#define CACHE_ISBDD           11

// MDD operations
#define CACHE_MDD_RELPROD     20
#define CACHE_MDD_MINUS       21
#define CACHE_MDD_UNION       22
#define CACHE_MDD_INTERSECT   23
#define CACHE_MDD_PROJECT     24
#define CACHE_MDD_JOIN        25
#define CACHE_MDD_MATCH       26
#define CACHE_MDD_RELPREV     27
#define CACHE_MDD_SATCOUNT    28
#define CACHE_MDD_SATCOUNTL1  29
#define CACHE_MDD_SATCOUNTL2  30

/**
 * Registration of quit functions
 */
typedef void (*quit_cb)();
void sylvan_register_quit(quit_cb cb);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
