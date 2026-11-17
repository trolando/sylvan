#include <sylvan.h>
#include <string.h>

#include "test_assert.h"

/* Independent byte fixture: type 0x12345678, three bytes, an empty frame
 * with type 0x80000001, and the end marker. Never built by the writer. */
static const uint8_t fixture[] = {
    0x53, 0x59, 0x4c, 0x56, 0x41, 0x4e, 0x32, 0x00,
    1, 0, 0, 0, 0, 0, 0, 0,
    0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0,
    3, 0, 0, 0, 0, 0, 0, 0,
    0x00, 0x80, 0xff,
    1, 0, 0, 0x80, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

struct memory_stream {
    uint8_t *data;
    size_t size, capacity, offset, chunk, fail_after;
    unsigned calls, collections;
    int collect, zero_write, bad_count, status;
};

static struct memory_stream
memory_create(void)
{
    struct memory_stream stream = {0};
    stream.chunk = SIZE_MAX;
    stream.fail_after = SIZE_MAX;
    return stream;
}

static void
memory_call(struct memory_stream *stream)
{
    if (stream->collect && stream->calls % 16 == 0) {
        /* A synchronous collection inside I/O, with the framed context live. */
        sylvan_gc();
        stream->collections++;
    }
    stream->calls++;
}

static int
memory_write(void *context, const void *data, size_t size, size_t *written)
{
    struct memory_stream *stream = context;
    memory_call(stream);
    *written = 0;
    if (stream->bad_count) { *written = size + 1; return SYLVAN_STREAM_OK; }
    if (stream->status != 0) return stream->status;
    if (stream->zero_write) return SYLVAN_STREAM_OK;
    if (stream->size >= stream->fail_after) return SYLVAN_STREAM_ERR_IO;
    if (size > stream->chunk) size = stream->chunk;
    if (size > stream->fail_after - stream->size) size = stream->fail_after - stream->size;
    if (size > SIZE_MAX - stream->size) return SYLVAN_STREAM_ERR_OOM;
    size_t required = stream->size + size;
    if (required > stream->capacity) {
        size_t capacity = stream->capacity ? stream->capacity : 64;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) {
                capacity = required;
                break;
            }
            capacity *= 2;
        }
        uint8_t *grown = realloc(stream->data, capacity);
        if (grown == NULL) return SYLVAN_STREAM_ERR_OOM;
        stream->data = grown;
        stream->capacity = capacity;
    }
    memcpy(stream->data + stream->size, data, size);
    stream->size = required;
    *written = size;
    return SYLVAN_STREAM_OK;
}

static int
memory_read(void *context, void *data, size_t size, size_t *received)
{
    struct memory_stream *stream = context;
    memory_call(stream);
    *received = 0;
    if (stream->bad_count) { *received = size + 1; return SYLVAN_STREAM_OK; }
    if (stream->status != 0) return stream->status;
    if (stream->offset >= stream->fail_after) return SYLVAN_STREAM_ERR_IO;
    if (size > stream->chunk) size = stream->chunk;
    if (size > stream->size - stream->offset) size = stream->size - stream->offset;
    if (size > stream->fail_after - stream->offset) size = stream->fail_after - stream->offset;
    if (size != 0) memcpy(data, stream->data + stream->offset, size);
    stream->offset += size;
    *received = size;
    return SYLVAN_STREAM_OK;
}

static int
write_fixture(sylvan_framed_writer *writer)
{
    test_assert(sylvan_framed_writer_begin(writer, UINT32_C(0x12345678), 3) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_writer_remaining(writer) == 3);
    test_assert(sylvan_framed_writer_append(writer, fixture + 32, 1) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_writer_remaining(writer) == 2);
    test_assert(sylvan_framed_writer_append(writer, fixture + 33, 2) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_writer_remaining(writer) == 0);
    test_assert(sylvan_framed_writer_write(writer, UINT32_C(0x80000001), NULL, 0) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_STREAM_OK);
    return 0;
}

static int
read_fixture(sylvan_framed_reader *reader)
{
    sylvan_frame frame;
    int has_frame;
    uint8_t payload[3];
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK);
    test_assert(has_frame == 1 && frame.type == UINT32_C(0x12345678));
    test_assert(frame.flags == 0 && frame.payload_size == 3);
    test_assert(sylvan_framed_reader_read(reader, payload, 1) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_reader_remaining(reader) == 2);
    test_assert(sylvan_framed_reader_read(reader, payload + 1, 2) == SYLVAN_STREAM_OK);
    test_assert(memcmp(payload, fixture + 32, 3) == 0);
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK);
    test_assert(has_frame == 1 && frame.type == UINT32_C(0x80000001));
    test_assert(frame.flags == 0 && frame.payload_size == 0);
    test_assert(sylvan_framed_reader_read(reader, NULL, 0) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_reader_skip(reader) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK);
    test_assert(has_frame == 0 && frame.type == UINT32_C(0x80000001));
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK);
    test_assert(has_frame == 0);
    return 0;
}

static int
check_fixture(int collect)
{
    /* Exhaust small transfer sizes, including one byte and exact headers. */
    for (size_t chunk = 1; chunk <= 20; chunk++) {
        struct memory_stream stream = memory_create();
        stream.chunk = chunk;
        stream.collect = collect;
        sylvan_framed_writer *writer = NULL;
        test_assert(sylvan_framed_writer_create(&writer, memory_write, &stream) == SYLVAN_STREAM_OK);
        test_assert(write_fixture(writer) == 0);
        sylvan_framed_writer_destroy(writer);
        test_assert(stream.size == sizeof(fixture));
        test_assert(memcmp(stream.data, fixture, sizeof(fixture)) == 0);
        sylvan_framed_reader *reader = NULL;
        test_assert(sylvan_framed_reader_create(&reader, memory_read, &stream) == SYLVAN_STREAM_OK);
        test_assert(read_fixture(reader) == 0);
        sylvan_framed_reader_destroy(reader);
        test_assert(stream.offset == sizeof(fixture));
        if (collect) test_assert(stream.collections > 0);
        free(stream.data);
    }
    return 0;
}

/* Consume a borrowed byte array, preserving the first error and verifying
 * unchanged outputs and the poisoned state after a failed next/skip. */
static int
parse_bytes(uint8_t *data, size_t size, size_t chunk, int expected)
{
    struct memory_stream stream = memory_create();
    stream.data = data;
    stream.size = size;
    stream.chunk = chunk;
    sylvan_framed_reader *reader = NULL;
    int status = sylvan_framed_reader_create(&reader, memory_read, &stream);
    if (status != SYLVAN_STREAM_OK) {
        test_assert(reader == NULL && status == expected);
        return 0;
    }
    for (;;) {
        sylvan_frame frame = {7, 8, 9};
        int has_frame = -1;
        status = sylvan_framed_reader_next(reader, &frame, &has_frame);
        if (status != SYLVAN_STREAM_OK) {
            test_assert(frame.type == 7 && frame.flags == 8 && frame.payload_size == 9);
            test_assert(has_frame == -1);
            break;
        }
        if (!has_frame) break;
        status = sylvan_framed_reader_skip(reader);
        if (status != SYLVAN_STREAM_OK) break;
    }
    test_assert(status == expected);
    if (status != SYLVAN_STREAM_OK) {
        unsigned calls = stream.calls;
        sylvan_frame frame;
        int has_frame;
        test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_ERR_INVALID);
        test_assert(sylvan_framed_reader_read(reader, NULL, 0) == SYLVAN_STREAM_ERR_INVALID);
        test_assert(sylvan_framed_reader_skip(reader) == SYLVAN_STREAM_ERR_INVALID);
        test_assert(calls == stream.calls);
    }
    sylvan_framed_reader_destroy(reader);
    return 0;
}

static int
check_truncation_and_headers(void)
{
    uint8_t bytes[sizeof(fixture)];
    memcpy(bytes, fixture, sizeof(bytes));
    for (size_t chunk = 1; chunk <= sizeof(bytes); chunk += 7) {
        for (size_t end = 0; end < sizeof(bytes); end++) {
            test_assert(parse_bytes(bytes, end, chunk, SYLVAN_STREAM_ERR_TRUNCATED) == 0);
        }
        test_assert(parse_bytes(bytes, sizeof(bytes), chunk, SYLVAN_STREAM_OK) == 0);
    }
    /* Every bit of the magic, major version, and stream flags is checked. */
    for (size_t offset = 0; offset < 16; offset++) {
        for (unsigned bit = 0; bit < 8; bit++) {
            bytes[offset] ^= (uint8_t)(1u << bit);
            int expected = offset == 10 || offset == 11 ? SYLVAN_STREAM_OK : SYLVAN_STREAM_ERR_FORMAT;
            test_assert(parse_bytes(bytes, sizeof(bytes), 3, expected) == 0);
            bytes[offset] ^= (uint8_t)(1u << bit);
        }
    }
    const size_t headers[] = {16, 35, 51};
    for (size_t h = 0; h < 3; h++) {
        for (size_t offset = 4; offset < 8; offset++) {
            bytes[headers[h] + offset] = 1;
            test_assert(parse_bytes(bytes, sizeof(bytes), 5, SYLVAN_STREAM_ERR_FORMAT) == 0);
            bytes[headers[h] + offset] = 0;
        }
    }
    /* A zero type with nonzero length is malformed, including the end marker. */
    memset(bytes + 16, 0, 4);
    test_assert(parse_bytes(bytes, sizeof(bytes), 5, SYLVAN_STREAM_ERR_FORMAT) == 0);
    memcpy(bytes, fixture, sizeof(bytes));
    for (size_t offset = 59; offset < 67; offset++) {
        bytes[offset] = 1;
        test_assert(parse_bytes(bytes, sizeof(bytes), 7, SYLVAN_STREAM_ERR_FORMAT) == 0);
        bytes[offset] = 0;
    }
    return 0;
}

static int
check_states(void)
{
    struct memory_stream stream = memory_create();
    sylvan_framed_writer *writer = NULL;
    sylvan_framed_reader *reader = NULL;
    sylvan_frame frame;
    int has_frame;
    uint8_t byte = 0;
    test_assert(sylvan_framed_writer_create(NULL, memory_write, &stream) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_create(&writer, NULL, &stream) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(writer == NULL && stream.calls == 0);
    test_assert(sylvan_framed_reader_create(NULL, memory_read, &stream) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_create(&reader, NULL, &stream) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(reader == NULL && stream.calls == 0);
    test_assert(sylvan_framed_writer_begin(NULL, 1, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_append(NULL, NULL, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_write(NULL, 1, NULL, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_finish(NULL) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_remaining(NULL) == 0);
    test_assert(sylvan_framed_reader_next(NULL, &frame, &has_frame) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_read(NULL, NULL, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_skip(NULL) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_remaining(NULL) == 0);
    sylvan_framed_writer_destroy(NULL);
    sylvan_framed_reader_destroy(NULL);

    test_assert(sylvan_framed_writer_create(&writer, memory_write, &stream) == SYLVAN_STREAM_OK);
    unsigned calls = stream.calls;
    test_assert(sylvan_framed_writer_begin(writer, 0, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_write(writer, 1, NULL, 1) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_append(writer, &byte, 1) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_append(writer, NULL, 0) == SYLVAN_STREAM_OK);
    test_assert(calls == stream.calls);
    test_assert(sylvan_framed_writer_begin(writer, 2, 1) == SYLVAN_STREAM_OK);
    calls = stream.calls;
    test_assert(sylvan_framed_writer_begin(writer, 3, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_write(writer, 3, NULL, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_append(writer, NULL, 1) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_append(writer, &byte, SIZE_MAX) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_remaining(writer) == 1 && calls == stream.calls);
    test_assert(sylvan_framed_writer_append(writer, &byte, 1) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_STREAM_OK);
    calls = stream.calls;
    test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_begin(writer, 2, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_write(writer, 2, NULL, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_writer_append(writer, NULL, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(calls == stream.calls);
    sylvan_framed_writer_destroy(writer);

    test_assert(sylvan_framed_reader_create(&reader, memory_read, &stream) == SYLVAN_STREAM_OK);
    calls = stream.calls;
    test_assert(sylvan_framed_reader_next(reader, NULL, &has_frame) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_next(reader, &frame, NULL) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_read(reader, &byte, 1) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_read(reader, NULL, 0) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_reader_skip(reader) == SYLVAN_STREAM_OK);
    test_assert(calls == stream.calls);
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK);
    calls = stream.calls;
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_read(reader, &byte, SIZE_MAX) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_read(reader, NULL, 1) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_remaining(reader) == 1 && calls == stream.calls);
    test_assert(sylvan_framed_reader_skip(reader) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK && !has_frame);
    calls = stream.calls;
    test_assert(sylvan_framed_reader_read(reader, NULL, 0) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_skip(reader) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK && !has_frame);
    test_assert(calls == stream.calls);
    sylvan_framed_reader_destroy(reader);
    free(stream.data);
    return 0;
}

static int
check_callback_errors(void)
{
    /* Fail at every output byte: stream/frame headers, incremental payloads,
     * empty frames and the end marker, including after a short transfer. */
    for (size_t fail = 0; fail < sizeof(fixture); fail++) {
        struct memory_stream stream = memory_create();
        stream.chunk = 2;
        stream.fail_after = fail;
        sylvan_framed_writer *writer = NULL;
        int status = sylvan_framed_writer_create(&writer, memory_write, &stream);
        if (fail < 16) test_assert(writer == NULL);
        if (status == SYLVAN_STREAM_OK) status = sylvan_framed_writer_begin(writer, UINT32_C(0x12345678), 3);
        if (status == SYLVAN_STREAM_OK) status = sylvan_framed_writer_append(writer, fixture + 32, 1);
        if (status == SYLVAN_STREAM_OK) status = sylvan_framed_writer_append(writer, fixture + 33, 2);
        if (status == SYLVAN_STREAM_OK) status = sylvan_framed_writer_write(writer, UINT32_C(0x80000001), NULL, 0);
        if (status == SYLVAN_STREAM_OK) status = sylvan_framed_writer_finish(writer);
        test_assert(status == SYLVAN_STREAM_ERR_IO && stream.size == fail);
        if (writer != NULL) {
            unsigned calls = stream.calls;
            test_assert(sylvan_framed_writer_begin(writer, 1, 0) == SYLVAN_STREAM_ERR_INVALID);
            test_assert(sylvan_framed_writer_append(writer, NULL, 0) == SYLVAN_STREAM_ERR_INVALID);
            test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_STREAM_ERR_INVALID);
            test_assert(calls == stream.calls);
        }
        sylvan_framed_writer_destroy(writer);
        free(stream.data);
    }
    for (int mode = 0; mode < 4; mode++) {
        struct memory_stream stream = memory_create();
        if (mode == 0) stream.zero_write = 1;
        if (mode == 1) stream.bad_count = 1;
        if (mode == 2) stream.status = 1;
        if (mode == 3) stream.status = SYLVAN_STREAM_ERR_IO;
        int expected = mode == 0 || mode == 3 ? SYLVAN_STREAM_ERR_IO : SYLVAN_STREAM_ERR_CALLBACK;
        sylvan_framed_writer *writer = NULL;
        test_assert(sylvan_framed_writer_create(&writer, memory_write, &stream) == expected);
        test_assert(writer == NULL && stream.calls == 1);
        stream.calls = 0;
        if (mode == 0) expected = SYLVAN_STREAM_ERR_TRUNCATED;
        sylvan_framed_reader *reader = NULL;
        test_assert(sylvan_framed_reader_create(&reader, memory_read, &stream) == expected);
        test_assert(reader == NULL && stream.calls == 1);
    }
    /* Read errors are distinct from EOF, including partial payload failure. */
    uint8_t bytes[sizeof(fixture)];
    memcpy(bytes, fixture, sizeof(bytes));
    for (size_t fail = 16; fail < sizeof(bytes); fail++) {
        struct memory_stream stream = memory_create();
        stream.data = bytes;
        stream.size = sizeof(bytes);
        stream.chunk = 2;
        stream.fail_after = fail;
        sylvan_framed_reader *reader;
        test_assert(sylvan_framed_reader_create(&reader, memory_read, &stream) == SYLVAN_STREAM_OK);
        int status;
        for (;;) {
            sylvan_frame frame;
            int has_frame;
            status = sylvan_framed_reader_next(reader, &frame, &has_frame);
            if (status != SYLVAN_STREAM_OK) break;
            test_assert(has_frame);
            status = sylvan_framed_reader_skip(reader);
            if (status != SYLVAN_STREAM_OK) break;
        }
        test_assert(status == SYLVAN_STREAM_ERR_IO);
        unsigned calls = stream.calls;
        test_assert(sylvan_framed_reader_skip(reader) == SYLVAN_STREAM_ERR_INVALID);
        test_assert(calls == stream.calls);
        sylvan_framed_reader_destroy(reader);
    }
    return 0;
}

static int
check_large_lengths(void)
{
    const uint64_t sizes[] = {UINT64_C(0x100000001), UINT64_C(0x8000000000000000), UINT64_MAX};
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        struct memory_stream stream = memory_create();
        sylvan_framed_writer *writer;
        test_assert(sylvan_framed_writer_create(&writer, memory_write, &stream) == SYLVAN_STREAM_OK);
        test_assert(sylvan_framed_writer_begin(writer, 1, sizes[i]) == SYLVAN_STREAM_OK);
        test_assert(sylvan_framed_writer_remaining(writer) == sizes[i]);
        /* Independently check the length bytes, including high-bit values. */
        for (unsigned b = 0; b < 8; b++) test_assert(stream.data[24 + b] == (uint8_t)(sizes[i] >> (8 * b)));
        test_assert(sylvan_framed_writer_append(writer, fixture, 5) == SYLVAN_STREAM_OK);
        test_assert(sylvan_framed_writer_remaining(writer) == sizes[i] - 5);
        test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_STREAM_ERR_INVALID);
        sylvan_framed_writer_destroy(writer);
        sylvan_framed_reader *reader;
        test_assert(sylvan_framed_reader_create(&reader, memory_read, &stream) == SYLVAN_STREAM_OK);
        sylvan_frame frame;
        int has_frame;
        test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK);
        test_assert(has_frame && frame.payload_size == sizes[i]);
        uint8_t data[5];
        test_assert(sylvan_framed_reader_read(reader, data, sizeof(data)) == SYLVAN_STREAM_OK);
        test_assert(memcmp(data, fixture, sizeof(data)) == 0);
        test_assert(sylvan_framed_reader_remaining(reader) == sizes[i] - 5);
        test_assert(sylvan_framed_reader_skip(reader) == SYLVAN_STREAM_ERR_TRUNCATED);
        sylvan_framed_reader_destroy(reader);
        free(stream.data);
    }
    return 0;
}

static int
check_many_and_large(void)
{
    struct memory_stream stream = memory_create();
    stream.chunk = 97;
    sylvan_framed_writer *writer;
    test_assert(sylvan_framed_writer_create(&writer, memory_write, &stream) == SYLVAN_STREAM_OK);
    for (unsigned i = 0; i < 1000; i++) {
        uint8_t data[7];
        for (unsigned j = 0; j < sizeof(data); j++) data[j] = (uint8_t)(i + j);
        test_assert(sylvan_framed_writer_write(writer, i + 1, data, i % 8) == SYLVAN_STREAM_OK);
    }
    const size_t large_size = 1024 * 1024 + 17;
    uint8_t *large = malloc(large_size);
    test_assert(large != NULL);
    for (size_t i = 0; i < large_size; i++) large[i] = (uint8_t)(i * 13 + i / 251);
    test_assert(sylvan_framed_writer_begin(writer, UINT32_MAX, large_size) == SYLVAN_STREAM_OK);
    for (size_t i = 0; i < large_size;) {
        size_t n = large_size - i > 1009 ? 1009 : large_size - i;
        test_assert(sylvan_framed_writer_append(writer, large + i, n) == SYLVAN_STREAM_OK);
        i += n;
    }
    /* A second large frame exercises skip's bounded scratch buffer. */
    test_assert(sylvan_framed_writer_write(writer, 99, large, large_size) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_STREAM_OK);
    sylvan_framed_writer_destroy(writer);
    sylvan_framed_reader *reader;
    test_assert(sylvan_framed_reader_create(&reader, memory_read, &stream) == SYLVAN_STREAM_OK);
    sylvan_frame frame;
    int has_frame;
    for (unsigned i = 0; i < 1000; i++) {
        test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK);
        test_assert(has_frame && frame.type == i + 1 && frame.flags == 0 && frame.payload_size == i % 8);
        uint8_t data[7];
        test_assert(sylvan_framed_reader_read(reader, data, i % 8) == SYLVAN_STREAM_OK);
        for (unsigned j = 0; j < i % 8; j++) test_assert(data[j] == (uint8_t)(i + j));
    }
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK);
    test_assert(has_frame && frame.type == UINT32_MAX && frame.payload_size == large_size);
    for (size_t i = 0; i < large_size;) {
        uint8_t data[307];
        size_t n = large_size - i > sizeof(data) ? sizeof(data) : large_size - i;
        test_assert(sylvan_framed_reader_read(reader, data, n) == SYLVAN_STREAM_OK);
        test_assert(memcmp(data, large + i, n) == 0);
        i += n;
    }
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK);
    test_assert(has_frame && frame.type == 99 && frame.payload_size == large_size);
    test_assert(sylvan_framed_reader_skip(reader) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK && !has_frame);
    sylvan_framed_reader_destroy(reader);
    free(large);
    free(stream.data);
    return 0;
}

static int
check_file(void)
{
    FILE *file = tmpfile();
    test_assert(file != NULL);
    size_t transferred = 99;
    test_assert(sylvan_stream_write_file(NULL, NULL, 0, &transferred) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(transferred == 0);
    test_assert(sylvan_stream_read_file(NULL, NULL, 0, &transferred) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_stream_write_file(file, NULL, 1, &transferred) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_stream_read_file(file, NULL, 1, &transferred) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_stream_write_file(file, NULL, 0, NULL) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_stream_read_file(file, NULL, 0, NULL) == SYLVAN_STREAM_ERR_INVALID);
    test_assert(sylvan_stream_write_file(file, NULL, 0, &transferred) == SYLVAN_STREAM_OK && transferred == 0);
    test_assert(sylvan_stream_read_file(file, NULL, 0, &transferred) == SYLVAN_STREAM_OK && transferred == 0);
    sylvan_framed_writer *writer;
    test_assert(sylvan_framed_writer_create(&writer, sylvan_stream_write_file, file) == SYLVAN_STREAM_OK);
    test_assert(write_fixture(writer) == 0);
    sylvan_framed_writer_destroy(writer);
    /* Concatenation: no frames, just stream header and end marker. */
    test_assert(sylvan_framed_writer_create(&writer, sylvan_stream_write_file, file) == SYLVAN_STREAM_OK);
    test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_STREAM_OK);
    sylvan_framed_writer_destroy(writer);
    test_assert(fflush(file) == 0);
    test_assert(fseek(file, 0, SEEK_SET) == 0);
    uint8_t bytes[sizeof(fixture)];
    test_assert(fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes));
    test_assert(memcmp(bytes, fixture, sizeof(bytes)) == 0);
    test_assert(fseek(file, 0, SEEK_SET) == 0);
    sylvan_framed_reader *reader;
    test_assert(sylvan_framed_reader_create(&reader, sylvan_stream_read_file, file) == SYLVAN_STREAM_OK);
    test_assert(read_fixture(reader) == 0);
    sylvan_framed_reader_destroy(reader);
    test_assert(ftell(file) == (long)sizeof(fixture));
    test_assert(sylvan_framed_reader_create(&reader, sylvan_stream_read_file, file) == SYLVAN_STREAM_OK);
    sylvan_frame frame;
    int has_frame;
    test_assert(sylvan_framed_reader_next(reader, &frame, &has_frame) == SYLVAN_STREAM_OK && !has_frame);
    sylvan_framed_reader_destroy(reader);
    reader = NULL;
    test_assert(sylvan_framed_reader_create(&reader, sylvan_stream_read_file, file) == SYLVAN_STREAM_ERR_TRUNCATED);
    test_assert(reader == NULL);
    test_assert(fclose(file) == 0);
    /* A short fread in the middle of a stream header. */
    file = tmpfile();
    test_assert(file != NULL);
    test_assert(fwrite(fixture, 1, 7, file) == 7);
    test_assert(fseek(file, 0, SEEK_SET) == 0);
    test_assert(sylvan_framed_reader_create(&reader, sylvan_stream_read_file, file) == SYLVAN_STREAM_ERR_TRUNCATED);
    test_assert(fclose(file) == 0);
    return 0;
}

TASK_0(int, check_gc)
{
    BDD root = sylvan_ithvar(5);
    sylvan_protect(&root);
    test_assert(check_fixture(1) == 0);
    test_assert(root == sylvan_ithvar(5));
    sylvan_unprotect(&root);
    return 0;
}

int
main(int argc, char **argv)
{
    /* All core behavior works before either Lace or Sylvan is initialized. */
    test_assert(check_fixture(0) == 0);
    test_assert(check_truncation_and_headers() == 0);
    test_assert(check_states() == 0);
    test_assert(check_callback_errors() == 0);
    test_assert(check_large_lengths() == 0);
    test_assert(check_many_and_large() == 0);
    test_assert(check_file() == 0);

    lace_start(argc > 1 ? (unsigned)atoi(argv[1]) : 1, 0);
    sylvan_set_sizes(1<<10, 1<<12, 1<<10, 1<<12);
    sylvan_init_package();
    sylvan_init_mtbdd();
    test_assert(RUN(check_gc) == 0);
    sylvan_quit();
    lace_stop();
    test_assert(check_fixture(0) == 0);
    return 0;
}
