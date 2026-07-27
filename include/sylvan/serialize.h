/*
 * Copyright 2026 Tom van Dijk, University of Twente
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
 * Versioned framed byte streams used by Sylvan serialization.
 */

#ifndef SYLVAN_SERIALIZE_H
#define SYLVAN_SERIALIZE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct sylvan_framed_writer sylvan_framed_writer;
typedef struct sylvan_framed_reader sylvan_framed_reader;

/**
 * Exact byte-I/O callbacks.
 *
 * A successful callback consumes or produces exactly <size> bytes. On failure
 * it returns a negative Sylvan status; the stream position is then unspecified
 * and the framed context rejects further operations. The callback and
 * <context> are borrowed until the framed context is destroyed.
 */
typedef int (*sylvan_stream_write_cb)(
    void *context, const void *data, size_t size);
typedef int (*sylvan_stream_read_cb)(
    void *context, void *data, size_t size);

/**
 * Header of the current frame.
 *
 * Frame type zero is reserved for the end marker and is never returned by
 * sylvan_framed_reader_next(). Flags are zero in format version 1. Unknown
 * nonzero frame types can be consumed or skipped by an application parser.
 */
typedef struct sylvan_frame {
    uint32_t type;
    uint32_t flags;
    uint64_t payload_size;
} sylvan_frame;

/**
 * Create a version-1 writer and immediately write the stream header.
 * On failure, <result> is unchanged.
 */
int sylvan_framed_writer_create(
    sylvan_framed_writer **result,
    sylvan_stream_write_cb write,
    void *context);

/**
 * Write one complete frame. <type> must be nonzero.
 */
int sylvan_framed_writer_write(
    sylvan_framed_writer *writer,
    uint32_t type,
    const void *payload,
    size_t payload_size);

/**
 * Write the end marker. No further frames can be written.
 */
int sylvan_framed_writer_finish(sylvan_framed_writer *writer);

/**
 * Destroy a writer without performing I/O. A missing finish call therefore
 * remains observable as a truncated stream.
 */
void sylvan_framed_writer_destroy(sylvan_framed_writer *writer);

/**
 * Create a reader and validate the stream magic and major format version.
 * On failure, <result> is unchanged.
 */
int sylvan_framed_reader_create(
    sylvan_framed_reader **result,
    sylvan_stream_read_cb read,
    void *context);

/**
 * Read the next frame header. The previous payload must have been consumed or
 * skipped. Sets <has_frame> to zero after the end marker.
 */
int sylvan_framed_reader_next(
    sylvan_framed_reader *reader,
    sylvan_frame *frame,
    int *has_frame);

/**
 * Consume part or all of the current payload. <size> must not exceed the
 * remaining payload size. This permits application parsers to process large
 * frames incrementally without an intermediate allocation.
 */
int sylvan_framed_reader_read(
    sylvan_framed_reader *reader,
    void *data,
    size_t size);

/**
 * Skip the remainder of the current payload.
 */
int sylvan_framed_reader_skip(sylvan_framed_reader *reader);

/**
 * Return the unread byte count in the current payload.
 */
uint64_t sylvan_framed_reader_remaining(
    const sylvan_framed_reader *reader);

void sylvan_framed_reader_destroy(sylvan_framed_reader *reader);

/**
 * Exact FILE adapters for the byte-I/O callbacks.
 */
int sylvan_stream_write_file(
    void *file, const void *data, size_t size);
int sylvan_stream_read_file(
    void *file, void *data, size_t size);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
