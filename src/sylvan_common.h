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
#include <llmsset.h>
#include <sylvan.h>
#include <tls.h>

#ifndef SYLVAN_COMMON_H
#define SYLVAN_COMMON_H

/**
 * Thread-local insert index for LLMSset
 */
extern DECLARE_THREAD_LOCAL(insert_index, uint64_t*);
uint64_t* initialize_insert_index();

/**
 * Global variables (number of workers, nodes table)
 */

extern int workers;
extern llmsset_t nodes;
#endif
