/*
 * Copyright 2026 Tom van Dijk, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

/* Do not include this file directly. Instead, include sylvan.h. */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

TASK(int, sylvan_serialization_write_bdd,
     sylvan_serialization_writer*, writer, BDD, dd, uint64_t, key)

TASK(int, sylvan_serialization_write_mtbdd,
     sylvan_serialization_writer*, writer, MTBDD, dd, uint64_t, key)

TASK(int, sylvan_serialization_write_zdd,
     sylvan_serialization_writer*, writer,
     ZDD, dd, BDDSET, domain, uint64_t, key)

TASK(int, sylvan_serialization_reader_next,
     sylvan_serialization_reader*, reader,
     sylvan_serialization_root*, root, int*, has_root)

#ifdef __cplusplus
}
#endif /* __cplusplus */
