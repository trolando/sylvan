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
 * Framed byte streams, independent of decision diagrams and Lace. No package
 * initialization is needed. This header declares the API in the global scope
 * for standalone C/C++ byte-stream use. Including sylvan.h also exposes aliases
 * in the usual C++ sylvan namespace, regardless of header inclusion order.
 *
 * Wire format (no padding, alignment, compression or checksum): all integers
 * are unsigned and little-endian. The 16-byte stream header contains eight
 * magic bytes 53 59 4c 56 41 4e 32 00 ("SYLVAN2\0"), a uint16 major version at
 * offset 8, a uint16 minor version at offset 10, and uint32 flags at offset 12.
 * Writers emit major 1, minor 0, flags 0. Readers require this magic, major 1
 * and flags 0, and ignore the minor version.
 *
 * Each frame has a 16-byte header: uint32 type at offset 0, uint32 flags at
 * offset 4, uint64 payload length in bytes at offset 8, then exactly that many
 * uninterpreted payload bytes. Flags must be zero. Any nonzero type is allowed,
 * including for an empty payload. A header of sixteen zero bytes (type 0,
 * flags 0, length 0) ends the stream and has no payload. Physical EOF before
 * this end marker is truncation, even at a frame boundary. Bytes after the end
 * marker are left unread, allowing concatenated streams. Payload corruption
 * and changes to otherwise valid types/lengths cannot be detected without an
 * application-level integrity check.
 *
 * A context borrows its callback and user context until destroyed. Callbacks
 * must not reenter that same framed context. One context must not be used by
 * multiple threads at once; separate contexts are independent if their I/O is.
 * No function closes or flushes an underlying stream. Destruction performs no
 * I/O and accepts NULL. All int-returning functions return SYLVAN_STREAM_OK on
 * success or a negative error code. Invalid arguments/state cause no I/O and
 * leave the context usable. I/O, format and callback errors make a context
 * unusable: subsequent I/O operations return SYLVAN_STREAM_ERR_INVALID. After
 * such an error the I/O position and partially transferred buffer contents are
 * unspecified; destroy the context. Remaining-byte counts are then diagnostic
 * only, not a way to resume the stream. Buffer capacities are the caller's
 * responsibility; NULL buffers are allowed only for zero bytes.
 */

#ifndef SYLVAN_SERIALIZE_H
#define SYLVAN_SERIALIZE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** Return codes for framed streams, without a process-wide error state. */
typedef enum sylvan_stream_status {
    SYLVAN_STREAM_OK = 0,              /**< Operation completed successfully. */
    SYLVAN_STREAM_ERR_INVALID = -1,   /**< Invalid argument or context state. */
    SYLVAN_STREAM_ERR_OOM = -2,       /**< Context allocation failed. */
    SYLVAN_STREAM_ERR_IO = -3,        /**< I/O failed, or a write made no progress. */
    SYLVAN_STREAM_ERR_TRUNCATED = -4, /**< Physical EOF before the end marker. */
    SYLVAN_STREAM_ERR_FORMAT = -5,    /**< Invalid magic, major version or header. */
    SYLVAN_STREAM_ERR_CALLBACK = -6   /**< Positive callback status or excess count. */
} sylvan_stream_status;

/** Opaque writer; payload storage is never allocated or retained by Sylvan. */
typedef struct sylvan_framed_writer sylvan_framed_writer;
/** Opaque reader; input lengths never cause allocations. */
typedef struct sylvan_framed_reader sylvan_framed_reader;

/**
 * Transfer up to <size> bytes, set <transferred> to the actual byte count, and
 * return SYLVAN_STREAM_OK or a negative error code (propagated unchanged).
 * The framed layer supplies a nonzero <size> and a count initialized to zero.
 * It retries successful short transfers until the requested bytes are complete.
 * A successful zero-byte read means physical EOF; a successful zero-byte write
 * is an I/O error. Positive return codes or counts larger than <size> are
 * callback errors. Callbacks must never access beyond the supplied buffer and
 * must not retain it. These are blocking callbacks: there is no would-block
 * or resumable-error protocol.
 */
typedef int (*sylvan_stream_write_cb)(
    void *context, const void *data, size_t size, size_t *transferred);
/** Read callback with the same transfer and error contract as the writer. */
typedef int (*sylvan_stream_read_cb)(
    void *context, void *data, size_t size, size_t *transferred);

/** Decoded frame header; this struct is not the on-disk representation. */
typedef struct sylvan_frame {
    uint32_t type;         /**< Nonzero application-defined frame type. */
    uint32_t flags;        /**< Always zero in accepted frames. */
    uint64_t payload_size; /**< Payload byte count, possibly larger than SIZE_MAX. */
} sylvan_frame;

/**
 * Allocate a writer and immediately emit a version-1.0 stream header. <result>
 * and <write> must be non-NULL; <context> is passed unchanged to the callback.
 * On failure, *result is unchanged, but output may have been partially written.
 */
int sylvan_framed_writer_create(
    sylvan_framed_writer **result,
    sylvan_stream_write_cb write,
    void *context);

/**
 * Emit a frame header with nonzero <type> and the declared payload byte count.
 * The writer must be usable, unfinished, with no outstanding payload. Exactly
 * <payload_size> bytes must be appended before another begin/write/finish call.
 * Zero-length frames complete immediately. Lengths up to UINT64_MAX are valid;
 * this function does not allocate payload storage or add lengths together.
 */
int sylvan_framed_writer_begin(
    sylvan_framed_writer *writer,
    uint32_t type,
    uint64_t payload_size);

/**
 * Write <size> bytes of the outstanding payload. The writer must be usable and
 * unfinished; <size> must not exceed remaining(). Zero bytes is a no-op even
 * before the first frame or between frames. On success remaining() decreases
 * by <size>; on error it is unchanged, though I/O may have partially completed.
 */
int sylvan_framed_writer_append(
    sylvan_framed_writer *writer,
    const void *data,
    size_t size);

/** Return the outstanding payload byte count, or zero for NULL. No I/O. */
uint64_t sylvan_framed_writer_remaining(const sylvan_framed_writer *writer);

/**
 * Write a complete frame by composing begin and append. The same state/type
 * requirements apply. <payload> must contain <payload_size> readable bytes;
 * the size must fit uint64_t. Argument errors emit no frame header.
 */
int sylvan_framed_writer_write(
    sylvan_framed_writer *writer,
    uint32_t type,
    const void *payload,
    size_t payload_size);

/**
 * Emit the end marker. The writer must be usable and unfinished, with no
 * outstanding payload. Further I/O, including another finish, is invalid.
 * This does not flush buffered I/O; check fflush/fclose errors separately.
 */
int sylvan_framed_writer_finish(sylvan_framed_writer *writer);

/** Free the writer without I/O; omitting finish leaves a truncated stream. */
void sylvan_framed_writer_destroy(sylvan_framed_writer *writer);

/**
 * Allocate a reader and consume/validate the 16-byte stream header. <result>
 * and <read> must be non-NULL; <context> is passed unchanged to the callback.
 * On failure, *result is unchanged, but input may have been consumed.
 */
int sylvan_framed_reader_create(
    sylvan_framed_reader **result,
    sylvan_stream_read_cb read,
    void *context);

/**
 * Read the next header, after consuming/skipping the previous payload. <frame>
 * and <has_frame> must be non-NULL. On a frame, set *has_frame to 1 and fill
 * *frame. At the end marker, set *has_frame to 0 and leave *frame unchanged;
 * repeated calls then succeed without I/O. On error both outputs are unchanged.
 * Accept any nonzero type; callers can skip unknown types. A declared length
 * does not guarantee that the payload exists: read/skip can report truncation.
 */
int sylvan_framed_reader_next(
    sylvan_framed_reader *reader,
    sylvan_frame *frame,
    int *has_frame);

/**
 * Read <size> payload bytes into <data>. The reader must be usable and not at
 * the end marker; <size> must not exceed remaining(). Zero bytes is a no-op
 * even before the first frame or between frames. On success remaining()
 * decreases by <size>; on error it is unchanged, but <data> may be partially
 * filled. Large frames can be read in bounded chunks without an allocation.
 */
int sylvan_framed_reader_read(
    sylvan_framed_reader *reader,
    void *data,
    size_t size);

/**
 * Consume the entire unread payload using at most 4096 bytes of scratch space.
 * The reader must be usable and not at the end marker. With no unread payload
 * this is a no-op, including before the first frame. Time is linear in bytes
 * skipped; an application may instead reject a length and destroy the reader.
 */
int sylvan_framed_reader_skip(sylvan_framed_reader *reader);

/** Return the unread payload byte count, or zero for NULL. No I/O. */
uint64_t sylvan_framed_reader_remaining(const sylvan_framed_reader *reader);

/** Free the reader without consuming input or closing the underlying stream. */
void sylvan_framed_reader_destroy(sylvan_framed_reader *reader);

/**
 * Write callback adapter: <file> is a borrowed, writable FILE* opened in binary
 * mode. Performs fwrite, reports its byte count and returns ERR_IO on ferror.
 * Short transfers are retried by the framed writer. With non-NULL <transferred>,
 * invalid arguments set its count to zero and return ERR_INVALID. Zero bytes
 * with a valid file is a no-op. Does not flush, close or clear file errors.
 */
int sylvan_stream_write_file(
    void *file, const void *data, size_t size, size_t *transferred);

/**
 * Read callback adapter: <file> is a borrowed, readable FILE* opened in binary
 * mode. Performs fread, reports its byte count and returns ERR_IO on ferror;
 * EOF is a successful short/zero read, mapped to truncation by a framed reader.
 * Argument and zero-size behavior matches sylvan_stream_write_file. Does not
 * seek, close or clear file errors. Use these adapters with tmpfile() as well.
 */
int sylvan_stream_read_file(
    void *file, void *data, size_t size, size_t *transferred);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
