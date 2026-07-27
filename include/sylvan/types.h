/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
 * Copyright 2019-2026 Tom van Dijk, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#ifndef SYLVAN_TYPES_H
#define SYLVAN_TYPES_H

#include <stdint.h>

/** Opaque handles used by the decision-diagram family APIs. */
typedef uint64_t MTBDD;
typedef MTBDD BDD;
typedef BDD BDDSET;
typedef MTBDD MTBDDMAP;
typedef uint64_t ZDD;
typedef uint64_t LISTDD;

typedef struct listdd_projection listdd_projection;
typedef struct listdd_relation_layout listdd_relation_layout;

#endif
