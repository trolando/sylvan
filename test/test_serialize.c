#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sylvan/sylvan.h>

#include "test_assert.h"

struct memory_stream {
    uint8_t *data;
    size_t size;
    size_t capacity;
    size_t offset;
    size_t fail_after;
};

static int
memory_write(void *context, const void *data, size_t size)
{
    struct memory_stream *stream = context;
    if (stream->size > stream->fail_after ||
        size > stream->fail_after - stream->size) {
        return SYLVAN_ERR_IO;
    }
    if (size > SIZE_MAX - stream->size) return SYLVAN_ERR_OOM;
    const size_t required = stream->size + size;
    if (required > stream->capacity) {
        size_t capacity = stream->capacity == 0 ? 64 : stream->capacity;
        while (capacity < required) {
            if (capacity > SIZE_MAX / 2) return SYLVAN_ERR_OOM;
            capacity *= 2;
        }
        uint8_t *grown = realloc(stream->data, capacity);
        if (grown == NULL) return SYLVAN_ERR_OOM;
        stream->data = grown;
        stream->capacity = capacity;
    }
    memcpy(stream->data + stream->size, data, size);
    stream->size = required;
    return SYLVAN_OK;
}

static int
memory_read(void *context, void *data, size_t size)
{
    struct memory_stream *stream = context;
    if (stream->offset > stream->size ||
        size > stream->size - stream->offset) {
        return SYLVAN_ERR_IO;
    }
    memcpy(data, stream->data + stream->offset, size);
    stream->offset += size;
    return SYLVAN_OK;
}

static int
test_roundtrip(void)
{
    struct memory_stream stream = {
        NULL, 0, 0, 0, SIZE_MAX
    };
    sylvan_framed_writer *writer = NULL;
    test_assert(sylvan_framed_writer_create(
        &writer, memory_write, &stream) == SYLVAN_OK);
    const uint8_t first[] = {1, 2, 3, 4, 5};
    test_assert(sylvan_framed_writer_begin(
        writer, 17, sizeof(first)) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_remaining(writer) == sizeof(first));
    test_assert(sylvan_framed_writer_append(
        writer, first, 2) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_begin(
        writer, 18, 0) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_framed_writer_append(
        writer, first + 2, 3) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_remaining(writer) == 0);
    test_assert(sylvan_framed_writer_write(
        writer, UINT32_C(0x80000001), NULL, 0) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_finish(writer) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_framed_writer_write(
        writer, 18, first, sizeof(first)) == SYLVAN_ERR_INVALID);
    sylvan_framed_writer_destroy(writer);

    sylvan_framed_reader *reader = NULL;
    test_assert(sylvan_framed_reader_create(
        &reader, memory_read, &stream) == SYLVAN_OK);
    sylvan_frame frame = {0, 0, 0};
    int has_frame = -1;
    test_assert(sylvan_framed_reader_next(
        reader, &frame, &has_frame) == SYLVAN_OK);
    test_assert(
        has_frame == 1 && frame.type == 17 &&
        frame.flags == 0 && frame.payload_size == sizeof(first));
    uint8_t output[sizeof(first)] = {0};
    test_assert(sylvan_framed_reader_read(
        reader, output, 2) == SYLVAN_OK);
    test_assert(sylvan_framed_reader_remaining(reader) == 3);
    test_assert(sylvan_framed_reader_next(
        reader, &frame, &has_frame) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_framed_reader_read(
        reader, output + 2, 3) == SYLVAN_OK);
    test_assert(memcmp(output, first, sizeof(first)) == 0);
    test_assert(sylvan_framed_reader_next(
        reader, &frame, &has_frame) == SYLVAN_OK);
    test_assert(
        has_frame == 1 && frame.type == UINT32_C(0x80000001) &&
        frame.payload_size == 0);
    test_assert(sylvan_framed_reader_skip(reader) == SYLVAN_OK);
    test_assert(sylvan_framed_reader_next(
        reader, &frame, &has_frame) == SYLVAN_OK);
    test_assert(has_frame == 0);
    test_assert(sylvan_framed_reader_next(
        reader, &frame, &has_frame) == SYLVAN_OK);
    test_assert(has_frame == 0);
    sylvan_framed_reader_destroy(reader);

    free(stream.data);
    return 0;
}

static int
test_failures(void)
{
    struct memory_stream stream = {
        NULL, 0, 0, 0, 8
    };
    sylvan_framed_writer *writer =
        (sylvan_framed_writer*)(uintptr_t)1;
    test_assert(sylvan_framed_writer_create(
        &writer, memory_write, &stream) == SYLVAN_ERR_IO);
    test_assert(writer == (sylvan_framed_writer*)(uintptr_t)1);
    test_assert(sylvan_framed_writer_create(
        &writer, NULL, &stream) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_framed_writer_create(
        NULL, memory_write, &stream) == SYLVAN_ERR_INVALID);
    free(stream.data);

    stream = (struct memory_stream){
        NULL, 0, 0, 0, SIZE_MAX
    };
    test_assert(sylvan_framed_writer_create(
        &writer, memory_write, &stream) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_write(
        writer, 0, NULL, 0) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_framed_writer_write(
        writer, 1, NULL, 1) == SYLVAN_ERR_INVALID);
    sylvan_framed_writer_destroy(writer);

    stream.offset = 0;
    sylvan_framed_reader *reader =
        (sylvan_framed_reader*)(uintptr_t)1;
    stream.data[0] ^= 1;
    test_assert(sylvan_framed_reader_create(
        &reader, memory_read, &stream) == SYLVAN_ERR_INVALID);
    test_assert(reader == (sylvan_framed_reader*)(uintptr_t)1);
    stream.data[0] ^= 1;
    stream.data[8] = 2;
    stream.offset = 0;
    test_assert(sylvan_framed_reader_create(
        &reader, memory_read, &stream) == SYLVAN_ERR_INVALID);
    stream.data[8] = 1;
    stream.offset = 0;
    stream.size = 8;
    test_assert(sylvan_framed_reader_create(
        &reader, memory_read, &stream) == SYLVAN_ERR_IO);
    test_assert(sylvan_framed_reader_create(
        NULL, memory_read, &stream) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_framed_reader_create(
        &reader, NULL, &stream) == SYLVAN_ERR_INVALID);

    free(stream.data);
    return 0;
}

int
main(void)
{
    if (test_roundtrip()) return 1;
    if (test_failures()) return 1;
    return 0;
}
