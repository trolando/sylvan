/*
 * Copyright 2011-2016 Formal Methods and Tools, University of Twente
 * Copyright 2016-2017 Tom van Dijk, Johannes Kepler University Linz
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

#ifndef SYLVAN_MT_PRIVATE_H
#define SYLVAN_MT_PRIVATE_H

#include <sylvan/mt.h>

/*
 * Transitional support for the legacy FILE-based MTBDD serializer. New custom
 * leaf codecs belong to the incremental serializer rather than to the public
 * immutable type descriptor.
 */
int sylvan_mt_bind_legacy_binary(
    uint32_t type, sylvan_mt_write_binary_cb write_binary,
    sylvan_mt_read_binary_cb read_binary);
void sylvan_mt_release_legacy_binary_value(uint32_t type, uint64_t value);

#endif
