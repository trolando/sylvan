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

#include <sylvan/sylvan.h>

#include <stdlib.h>
#include <string.h>

static const uint8_t sylvan_stream_magic[8] = {
    'S', 'Y', 'L', 'V', 'A', 'N', '2', '\0'
};

enum {
    SYLVAN_STREAM_MAJOR = 1,
    SYLVAN_STREAM_MINOR = 0,
    SYLVAN_STREAM_HEADER_SIZE = 16,
    SYLVAN_FRAME_HEADER_SIZE = 16
};

struct sylvan_framed_writer {
    sylvan_stream_write_cb write;
    void *context;
    uint64_t remaining;
    int finished;
    int failed;
};

struct sylvan_framed_reader {
    sylvan_stream_read_cb read;
    void *context;
    uint64_t remaining;
    int finished;
    int failed;
};

static void
sylvan_store_u16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void
sylvan_store_u32(uint8_t *destination, uint32_t value)
{
    for (unsigned int i = 0; i < 4; i++) {
        destination[i] = (uint8_t)(value >> (8 * i));
    }
}

static void
sylvan_store_u64(uint8_t *destination, uint64_t value)
{
    for (unsigned int i = 0; i < 8; i++) {
        destination[i] = (uint8_t)(value >> (8 * i));
    }
}

static uint16_t
sylvan_load_u16(const uint8_t *source)
{
    return (uint16_t)source[0] | ((uint16_t)source[1] << 8);
}

static uint32_t
sylvan_load_u32(const uint8_t *source)
{
    uint32_t value = 0;
    for (unsigned int i = 0; i < 4; i++) {
        value |= (uint32_t)source[i] << (8 * i);
    }
    return value;
}

static uint64_t
sylvan_load_u64(const uint8_t *source)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; i++) {
        value |= (uint64_t)source[i] << (8 * i);
    }
    return value;
}

static int
sylvan_writer_output(
    sylvan_framed_writer *writer, const void *data, size_t size)
{
    const int status = writer->write(writer->context, data, size);
    if (status == SYLVAN_OK) return SYLVAN_OK;
    writer->failed = 1;
    return status < 0 ? status : SYLVAN_ERR_CALLBACK;
}

static int
sylvan_reader_input(
    sylvan_framed_reader *reader, void *data, size_t size)
{
    const int status = reader->read(reader->context, data, size);
    if (status == SYLVAN_OK) return SYLVAN_OK;
    reader->failed = 1;
    return status < 0 ? status : SYLVAN_ERR_CALLBACK;
}

int
sylvan_framed_writer_create(
    sylvan_framed_writer **destination,
    sylvan_stream_write_cb write,
    void *context)
{
    if (destination == NULL || write == NULL) return SYLVAN_ERR_INVALID;

    sylvan_framed_writer *writer = calloc(1, sizeof(*writer));
    if (writer == NULL) return SYLVAN_ERR_OOM;
    writer->write = write;
    writer->context = context;

    uint8_t header[SYLVAN_STREAM_HEADER_SIZE] = {0};
    memcpy(header, sylvan_stream_magic, sizeof(sylvan_stream_magic));
    sylvan_store_u16(header + 8, SYLVAN_STREAM_MAJOR);
    sylvan_store_u16(header + 10, SYLVAN_STREAM_MINOR);
    sylvan_store_u32(header + 12, 0);
    const int status = sylvan_writer_output(writer, header, sizeof(header));
    if (status != SYLVAN_OK) {
        free(writer);
        return status;
    }

    *destination = writer;
    return SYLVAN_OK;
}

int
sylvan_framed_writer_begin(
    sylvan_framed_writer *writer,
    uint32_t type,
    uint64_t payload_size)
{
    if (writer == NULL || writer->failed || writer->finished ||
        writer->remaining != 0 || type == 0) {
        return SYLVAN_ERR_INVALID;
    }

    uint8_t header[SYLVAN_FRAME_HEADER_SIZE] = {0};
    sylvan_store_u32(header, type);
    sylvan_store_u32(header + 4, 0);
    sylvan_store_u64(header + 8, payload_size);
    const int status = sylvan_writer_output(writer, header, sizeof(header));
    if (status == SYLVAN_OK) writer->remaining = payload_size;
    return status;
}

int
sylvan_framed_writer_append(
    sylvan_framed_writer *writer,
    const void *data,
    size_t size)
{
    if (writer == NULL || writer->failed || writer->finished ||
        (size != 0 && data == NULL) || (uint64_t)size > writer->remaining) {
        return SYLVAN_ERR_INVALID;
    }
    if (size == 0) return SYLVAN_OK;

    const int status = sylvan_writer_output(writer, data, size);
    if (status == SYLVAN_OK) writer->remaining -= size;
    return status;
}

uint64_t
sylvan_framed_writer_remaining(const sylvan_framed_writer *writer)
{
    return writer == NULL ? 0 : writer->remaining;
}

int
sylvan_framed_writer_write(
    sylvan_framed_writer *writer,
    uint32_t type,
    const void *payload,
    size_t payload_size)
{
    if (payload_size != 0 && payload == NULL) return SYLVAN_ERR_INVALID;
    int status = sylvan_framed_writer_begin(writer, type, payload_size);
    if (status == SYLVAN_OK) {
        status = sylvan_framed_writer_append(
            writer, payload, payload_size);
    }
    return status;
}

int
sylvan_framed_writer_finish(sylvan_framed_writer *writer)
{
    if (writer == NULL || writer->failed || writer->finished ||
        writer->remaining != 0) {
        return SYLVAN_ERR_INVALID;
    }
    uint8_t header[SYLVAN_FRAME_HEADER_SIZE] = {0};
    const int status = sylvan_writer_output(writer, header, sizeof(header));
    if (status == SYLVAN_OK) writer->finished = 1;
    return status;
}

void
sylvan_framed_writer_destroy(sylvan_framed_writer *writer)
{
    free(writer);
}

int
sylvan_framed_reader_create(
    sylvan_framed_reader **destination,
    sylvan_stream_read_cb read,
    void *context)
{
    if (destination == NULL || read == NULL) return SYLVAN_ERR_INVALID;

    sylvan_framed_reader *reader = calloc(1, sizeof(*reader));
    if (reader == NULL) return SYLVAN_ERR_OOM;
    reader->read = read;
    reader->context = context;

    uint8_t header[SYLVAN_STREAM_HEADER_SIZE];
    int status = sylvan_reader_input(reader, header, sizeof(header));
    if (status == SYLVAN_OK &&
        (memcmp(header, sylvan_stream_magic, sizeof(sylvan_stream_magic)) != 0 ||
         sylvan_load_u16(header + 8) != SYLVAN_STREAM_MAJOR ||
         sylvan_load_u32(header + 12) != 0)) {
        status = SYLVAN_ERR_INVALID;
    }
    if (status != SYLVAN_OK) {
        free(reader);
        return status;
    }

    *destination = reader;
    return SYLVAN_OK;
}

int
sylvan_framed_reader_next(
    sylvan_framed_reader *reader,
    sylvan_frame *frame,
    int *has_frame)
{
    if (reader == NULL || frame == NULL || has_frame == NULL ||
        reader->failed || reader->remaining != 0) {
        return SYLVAN_ERR_INVALID;
    }
    if (reader->finished) {
        *has_frame = 0;
        return SYLVAN_OK;
    }

    uint8_t header[SYLVAN_FRAME_HEADER_SIZE];
    const int status = sylvan_reader_input(reader, header, sizeof(header));
    if (status != SYLVAN_OK) return status;

    const uint32_t type = sylvan_load_u32(header);
    const uint32_t flags = sylvan_load_u32(header + 4);
    const uint64_t payload_size = sylvan_load_u64(header + 8);
    if (flags != 0 || (type == 0 && payload_size != 0)) {
        reader->failed = 1;
        return SYLVAN_ERR_INVALID;
    }
    if (type == 0) {
        reader->finished = 1;
        *has_frame = 0;
        return SYLVAN_OK;
    }

    frame->type = type;
    frame->flags = flags;
    frame->payload_size = payload_size;
    reader->remaining = payload_size;
    *has_frame = 1;
    return SYLVAN_OK;
}

int
sylvan_framed_reader_read(
    sylvan_framed_reader *reader,
    void *data,
    size_t size)
{
    if (reader == NULL || reader->failed || reader->finished ||
        (size != 0 && data == NULL) || (uint64_t)size > reader->remaining) {
        return SYLVAN_ERR_INVALID;
    }
    if (size == 0) return SYLVAN_OK;

    const int status = sylvan_reader_input(reader, data, size);
    if (status == SYLVAN_OK) reader->remaining -= size;
    return status;
}

int
sylvan_framed_reader_skip(sylvan_framed_reader *reader)
{
    if (reader == NULL || reader->failed || reader->finished) {
        return SYLVAN_ERR_INVALID;
    }

    uint8_t buffer[4096];
    while (reader->remaining != 0) {
        size_t size = sizeof(buffer);
        if (reader->remaining < size) size = (size_t)reader->remaining;
        const int status = sylvan_framed_reader_read(reader, buffer, size);
        if (status != SYLVAN_OK) return status;
    }
    return SYLVAN_OK;
}

uint64_t
sylvan_framed_reader_remaining(const sylvan_framed_reader *reader)
{
    return reader == NULL ? 0 : reader->remaining;
}

void
sylvan_framed_reader_destroy(sylvan_framed_reader *reader)
{
    free(reader);
}

int
sylvan_stream_write_file(void *context, const void *data, size_t size)
{
    if (context == NULL || (size != 0 && data == NULL)) {
        return SYLVAN_ERR_INVALID;
    }
    if (size == 0) return SYLVAN_OK;
    return fwrite(data, 1, size, (FILE*)context) == size
        ? SYLVAN_OK : SYLVAN_ERR_IO;
}

int
sylvan_stream_read_file(void *context, void *data, size_t size)
{
    if (context == NULL || (size != 0 && data == NULL)) {
        return SYLVAN_ERR_INVALID;
    }
    if (size == 0) return SYLVAN_OK;
    return fread(data, 1, size, (FILE*)context) == size
        ? SYLVAN_OK : SYLVAN_ERR_IO;
}
