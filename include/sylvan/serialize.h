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

#include <sylvan/types.h>

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
 * Begin a frame whose payload will be supplied incrementally. No other frame
 * can begin until exactly <payload_size> bytes have been appended.
 */
int sylvan_framed_writer_begin(
    sylvan_framed_writer *writer,
    uint32_t type,
    uint64_t payload_size);

/**
 * Append bytes to the current frame payload. <size> must not exceed the
 * remaining payload size.
 */
int sylvan_framed_writer_append(
    sylvan_framed_writer *writer,
    const void *data,
    size_t size);

uint64_t sylvan_framed_writer_remaining(
    const sylvan_framed_writer *writer);

/**
 * Write one complete frame. This is a convenience composition of begin and
 * append for payloads that already occupy one contiguous buffer.
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

/**
 * Reserved version-1 serialization frame types. Applications may interleave
 * their own frames using types at or above SYLVAN_SERIALIZATION_APPLICATION.
 */
#define SYLVAN_SERIALIZATION_BDD_NODES UINT32_C(0x00001001)
#define SYLVAN_SERIALIZATION_ROOT UINT32_C(0x00001002)
#define SYLVAN_SERIALIZATION_MTBDD_LEAF UINT32_C(0x00001003)
#define SYLVAN_SERIALIZATION_MTBDD_TYPE UINT32_C(0x00001004)
#define SYLVAN_SERIALIZATION_MTBDD_CUSTOM_LEAF UINT32_C(0x00001005)
#define SYLVAN_SERIALIZATION_ZDD_NODES UINT32_C(0x00001006)
#define SYLVAN_SERIALIZATION_LISTDD_NODES UINT32_C(0x00001007)
#define SYLVAN_SERIALIZATION_LISTDD_LAYOUT UINT32_C(0x00001008)
#define SYLVAN_SERIALIZATION_APPLICATION UINT32_C(0x80000000)

typedef enum sylvan_dd_family {
    SYLVAN_DD_BDD = 1,
    SYLVAN_DD_MTBDD = 2,
    SYLVAN_DD_ZDD = 3,
    SYLVAN_DD_LISTDD = 4
} sylvan_dd_family;

typedef struct sylvan_serialization_writer sylvan_serialization_writer;
typedef struct sylvan_serialization_reader sylvan_serialization_reader;

/**
 * Custom MTBDD leaf codec.
 *
 * <type_name> is the stable name registered in sylvan_mt_type_descriptor.
 * <format_version> describes this codec's payload, independently of the
 * enclosing stream version. The context remains caller-owned and must outlive
 * every writer or reader to which the codec is added.
 *
 * For writing, <size> reports the exact payload size and <write> appends
 * exactly that many bytes to <stream>. For reading, <read> consumes exactly
 * <size> bytes and returns a leaf of the registered type in <result>.
 * Callbacks use SYLVAN_OK or a negative status; positive results are treated
 * as callback errors. Typed NaNs carry no payload and bypass the callbacks.
 */
typedef int (*sylvan_serialization_leaf_size_cb)(
    void *context, MTBDD leaf, uint64_t *size);
typedef int (*sylvan_serialization_leaf_write_cb)(
    void *context, MTBDD leaf, sylvan_framed_writer *stream);
typedef int (*sylvan_serialization_leaf_read_cb)(
    void *context, sylvan_framed_reader *stream,
    uint64_t size, MTBDD *result);

typedef struct sylvan_serialization_leaf_codec {
    const char *type_name;
    uint32_t format_version;
    void *context;
    sylvan_serialization_leaf_size_cb size;
    sylvan_serialization_leaf_write_cb write;
    sylvan_serialization_leaf_read_cb read;
} sylvan_serialization_leaf_codec;

/**
 * One committed root returned by the incremental reader.
 *
 * <key> is the caller-supplied stream identifier. <domain> is the decoded
 * BDDSET for a ZDD root and mtbdd_invalid for other families.
 * <listdd_layout> is the decoded relation layout for a ListDD relation root
 * and NULL otherwise. Decoded handles and metadata remain protected and owned
 * by the reader until it is destroyed. Protect handles separately before
 * destroying the reader if they must survive longer.
 */
typedef struct sylvan_serialization_root {
    sylvan_dd_family family;
    uint64_t key;
    MTBDD dd;
    BDDSET domain;
    const listdd_relation_layout *listdd_layout;
} sylvan_serialization_root;

/**
 * Parser hook for application or future frame types.
 *
 * The callback must consume or skip the complete payload through <stream> and
 * return SYLVAN_OK or a negative status. A positive result is a callback error.
 */
typedef int (*sylvan_serialization_frame_cb)(
    sylvan_framed_reader *stream,
    const sylvan_frame *frame,
    void *context);

/**
 * Create an incremental DD writer over a caller-owned framed stream.
 *
 * The caller may write application frames directly to <stream> between
 * complete root writes. The writer retains and protects earlier roots so later
 * roots emit only previously unseen nodes. It is not thread-safe and does not
 * finish or destroy the framed stream.
 */
int sylvan_serialization_writer_create(
    sylvan_serialization_writer **result,
    sylvan_framed_writer *stream);

void sylvan_serialization_writer_destroy(
    sylvan_serialization_writer *writer);

/**
 * Add a custom-leaf codec to a writer.
 *
 * The type must already be registered through sylvan_mt_register_type.
 * Registration copies the codec and its name. <size> and <write> are required.
 */
int sylvan_serialization_writer_add_leaf_codec(
    sylvan_serialization_writer *writer,
    const sylvan_serialization_leaf_codec *codec);

/**
 * Incrementally write a BDD and commit it with <key>.
 *
 * A reader can return this root as soon as the root frame arrives; no later
 * root or stream end marker is required. The caller must protect <dd>.
 */
static inline int sylvan_serialization_write_bdd(
    sylvan_serialization_writer *writer,
    BDD dd,
    uint64_t key);

/**
 * Incrementally write an MTBDD and commit it with <key>.
 *
 * Integer, double, and fraction leaves, including their typed NaN values, are
 * supported. A custom leaf requires a matching writer codec.
 */
static inline int sylvan_serialization_write_mtbdd(
    sylvan_serialization_writer *writer,
    MTBDD dd,
    uint64_t key);

/**
 * Incrementally write a ZDD over <domain> and commit it with <key>.
 *
 * The domain is serialized with the root and must be a BDDSET: a conjunction
 * of positive variables. MTZDD leaves are not supported.
 */
static inline int sylvan_serialization_write_zdd(
    sylvan_serialization_writer *writer,
    ZDD dd,
    BDDSET domain,
    uint64_t key);

/**
 * Incrementally write a ListDD and commit it with <key>.
 *
 * Both ordinary value nodes and relation COPY nodes are preserved.
 */
static inline int sylvan_serialization_write_listdd(
    sylvan_serialization_writer *writer,
    LISTDD dd,
    uint64_t key);

/**
 * Incrementally write a ListDD relation and its typed layout, then commit it
 * with <key>. The layout must have been created by the normal typed builder.
 */
static inline int sylvan_serialization_write_listdd_relation(
    sylvan_serialization_writer *writer,
    LISTDD dd,
    const listdd_relation_layout *layout,
    uint64_t key);

/**
 * Create an incremental DD reader over a caller-owned framed stream.
 *
 * Unknown frames are skipped when <frame_callback> is null, or delegated to
 * the callback otherwise. The reader is not thread-safe and does not destroy
 * the framed stream.
 */
int sylvan_serialization_reader_create(
    sylvan_serialization_reader **result,
    sylvan_framed_reader *stream,
    sylvan_serialization_frame_cb frame_callback,
    void *context);

void sylvan_serialization_reader_destroy(
    sylvan_serialization_reader *reader);

/**
 * Add a custom-leaf codec to a reader.
 *
 * The type must already be registered through sylvan_mt_register_type.
 * Registration copies the codec and its name. <read> is required.
 */
int sylvan_serialization_reader_add_leaf_codec(
    sylvan_serialization_reader *reader,
    const sylvan_serialization_leaf_codec *codec);

/**
 * Consume frames until the next committed DD root or the stream end marker.
 * Sets <has_root> to zero only at the end marker.
 */
static inline int sylvan_serialization_reader_next(
    sylvan_serialization_reader *reader,
    sylvan_serialization_root *root,
    int *has_root);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif
