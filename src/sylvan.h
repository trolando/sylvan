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

/**
 * Sylvan: parallel MTBDD/ListDD package.
 * Include this file.
 */

#include <sylvan_config.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h> // for FILE
#include <stdlib.h> // for realloc

#include <sylvan_platform.h>

/**
 * Sylvan header files outside the namespace
 */

#include <lace.h>
#include <sylvan_serialize.h>

#ifdef __cplusplus
namespace sylvan {

/* The standalone stream API is declared globally; keep the usual Sylvan
 * namespace aliases available regardless of header inclusion order. */
using ::sylvan_stream_status;
using ::SYLVAN_STREAM_OK;
using ::SYLVAN_STREAM_ERR_INVALID;
using ::SYLVAN_STREAM_ERR_OOM;
using ::SYLVAN_STREAM_ERR_IO;
using ::SYLVAN_STREAM_ERR_TRUNCATED;
using ::SYLVAN_STREAM_ERR_FORMAT;
using ::SYLVAN_STREAM_ERR_CALLBACK;
using ::sylvan_framed_writer;
using ::sylvan_framed_reader;
using ::sylvan_stream_write_cb;
using ::sylvan_stream_read_cb;
using ::sylvan_frame;
using ::sylvan_framed_writer_create;
using ::sylvan_framed_writer_begin;
using ::sylvan_framed_writer_append;
using ::sylvan_framed_writer_remaining;
using ::sylvan_framed_writer_write;
using ::sylvan_framed_writer_finish;
using ::sylvan_framed_writer_destroy;
using ::sylvan_framed_reader_create;
using ::sylvan_framed_reader_next;
using ::sylvan_framed_reader_read;
using ::sylvan_framed_reader_skip;
using ::sylvan_framed_reader_remaining;
using ::sylvan_framed_reader_destroy;
using ::sylvan_stream_write_file;
using ::sylvan_stream_read_file;
#endif

/**
 * Sylvan header files inside the namespace
 */

#include <sylvan_common.h>
#include <sylvan_stats.h>
#include <sylvan_mt.h>
#include <sylvan_mtbdd.h>
#include <sylvan_bdd.h>
#include <sylvan_ldd.h>
#include <sylvan_zdd.h>

#ifdef __cplusplus
}
#endif
