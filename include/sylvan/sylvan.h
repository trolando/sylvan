/*
 * Copyright 2011-2016 Tom van Dijk, University of Twente
 * Copyright 2016-2018 Tom van Dijk, Johannes Kepler University Linz
 * Copyright 2019-2026 Tom van Dijk, University of Twente
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
 * Sylvan: parallel MTBDD/ListDD package.
 */

#pragma once

#include <sylvan/config.h>
#include <sylvan/platform.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <lace.h>

#include <sylvan/common.h>
#include <sylvan/stats.h>
#include <sylvan/mt.h>
#include <sylvan/mtbdd.h>
#include <sylvan/bdd.h>
#include <sylvan/ldd.h>
#include <sylvan/zdd.h>

#include <sylvan/bdd_impl.h>
#include <sylvan/mtbdd_impl.h>


 // TODO: separate headers that declare functions (even static inline) from the implementations
 // Should a set of variables be the same thing whether its BDD, LDD, ZDD or MTBDD?
 // Same for a map?
