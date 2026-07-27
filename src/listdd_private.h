/*
 * Copyright 2026 Tom van Dijk, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/* Build-private ListDD metadata representation. */

#ifndef SYLVAN_LISTDD_PRIVATE_H
#define SYLVAN_LISTDD_PRIVATE_H

#include <sylvan/listdd.h>

struct listdd_relation_layout {
    LISTDD root;
    listdd_relation_access *positions;
    size_t count;
    size_t field_count;
    int has_action_label;
};

#endif
