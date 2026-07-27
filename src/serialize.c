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

#include <sylvan/internal.h>

#include <stdlib.h>
#include <string.h>

#include "listdd_private.h"
#include "refs.h"

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

#define SYLVAN_SERIALIZATION_HANDLE_BLOCK 256

struct sylvan_serialization_handle_table {
    MTBDD **blocks;
    size_t count;
    size_t block_count;
    size_t block_capacity;
    sylvan_dd_family family;
};

struct sylvan_serialization_dictionary {
    MTBDD *keys;
    uint64_t *ids;
    size_t capacity;
    MTBDD *nodes;
    size_t count;
    size_t node_capacity;
};

struct sylvan_serialization_leaf_codec_entry {
    char *type_name;
    uint32_t type;
    uint32_t format_version;
    uint32_t wire_type;
    int declared;
    void *context;
    sylvan_serialization_leaf_size_cb size;
    sylvan_serialization_leaf_write_cb write;
    sylvan_serialization_leaf_read_cb read;
};

struct sylvan_serialization_listdd_layout_entry {
    listdd_relation_access *positions;
    size_t count;
    int has_action_label;
    listdd_relation_layout *layout;
};

struct sylvan_serialization_writer {
    sylvan_framed_writer *stream;
    struct sylvan_serialization_dictionary dictionary;
    struct sylvan_serialization_dictionary zdd_dictionary;
    struct sylvan_serialization_dictionary listdd_dictionary;
    struct sylvan_serialization_handle_table roots;
    struct sylvan_serialization_handle_table zdd_roots;
    struct sylvan_serialization_handle_table listdd_roots;
    struct sylvan_serialization_leaf_codec_entry *codecs;
    size_t codec_count;
    size_t codec_capacity;
    struct sylvan_serialization_listdd_layout_entry *layouts;
    size_t layout_count;
    size_t layout_capacity;
    int failed;
};

struct sylvan_serialization_reader {
    sylvan_framed_reader *stream;
    sylvan_serialization_frame_cb frame_callback;
    void *context;
    struct sylvan_serialization_handle_table nodes;
    struct sylvan_serialization_handle_table zdd_nodes;
    struct sylvan_serialization_handle_table listdd_nodes;
    struct sylvan_serialization_leaf_codec_entry *codecs;
    size_t codec_count;
    size_t codec_capacity;
    struct sylvan_serialization_listdd_layout_entry *layouts;
    size_t layout_count;
    size_t layout_capacity;
    int failed;
};

static void
sylvan_serialization_codec_clear(
    struct sylvan_serialization_leaf_codec_entry *codecs, size_t count)
{
    for (size_t i = 0; i < count; i++) free(codecs[i].type_name);
    free(codecs);
}

static int
sylvan_serialization_codec_reserve(
    struct sylvan_serialization_leaf_codec_entry **codecs,
    size_t *capacity, size_t count)
{
    if (count < *capacity) return SYLVAN_OK;
    const size_t old_capacity = *capacity;
    const size_t new_capacity =
        old_capacity == 0 ? 4 : old_capacity * 2;
    if (new_capacity < old_capacity ||
        new_capacity > SIZE_MAX / sizeof(**codecs)) {
        return SYLVAN_ERR_OOM;
    }
    struct sylvan_serialization_leaf_codec_entry *grown = realloc(
        *codecs, new_capacity * sizeof(**codecs));
    if (grown == NULL) return SYLVAN_ERR_OOM;
    *codecs = grown;
    *capacity = new_capacity;
    return SYLVAN_OK;
}

static int
sylvan_serialization_layout_reserve(
    struct sylvan_serialization_listdd_layout_entry **layouts,
    size_t *capacity, size_t count)
{
    if (count < *capacity) return SYLVAN_OK;
    const size_t old_capacity = *capacity;
    const size_t new_capacity =
        old_capacity == 0 ? 4 : old_capacity * 2;
    if (new_capacity < old_capacity ||
        new_capacity > SIZE_MAX / sizeof(**layouts)) {
        return SYLVAN_ERR_OOM;
    }
    struct sylvan_serialization_listdd_layout_entry *grown = realloc(
        *layouts, new_capacity * sizeof(**layouts));
    if (grown == NULL) return SYLVAN_ERR_OOM;
    *layouts = grown;
    *capacity = new_capacity;
    return SYLVAN_OK;
}

static void
sylvan_serialization_writer_layouts_clear(
    sylvan_serialization_writer *writer)
{
    for (size_t i = 0; i < writer->layout_count; i++) {
        free(writer->layouts[i].positions);
    }
    free(writer->layouts);
}

static void
sylvan_serialization_reader_layouts_clear(
    sylvan_serialization_reader *reader)
{
    for (size_t i = 0; i < reader->layout_count; i++) {
        listdd_relation_layout_destroy(reader->layouts[i].layout);
    }
    free(reader->layouts);
}

static int
sylvan_serialization_codec_copy(
    struct sylvan_serialization_leaf_codec_entry *destination,
    const sylvan_serialization_leaf_codec *codec, uint32_t type)
{
    const size_t name_size = strlen(codec->type_name) + 1;
    char *name = malloc(name_size);
    if (name == NULL) return SYLVAN_ERR_OOM;
    memcpy(name, codec->type_name, name_size);

    memset(destination, 0, sizeof(*destination));
    destination->type_name = name;
    destination->type = type;
    destination->format_version = codec->format_version;
    destination->context = codec->context;
    destination->size = codec->size;
    destination->write = codec->write;
    destination->read = codec->read;
    return SYLVAN_OK;
}

static struct sylvan_serialization_leaf_codec_entry *
sylvan_serialization_writer_codec(
    sylvan_serialization_writer *writer, uint32_t type)
{
    for (size_t i = 0; i < writer->codec_count; i++) {
        if (writer->codecs[i].type == type) return writer->codecs + i;
    }
    return NULL;
}

static struct sylvan_serialization_leaf_codec_entry *
sylvan_serialization_reader_codec_by_wire(
    sylvan_serialization_reader *reader, uint32_t wire_type)
{
    for (size_t i = 0; i < reader->codec_count; i++) {
        if (reader->codecs[i].declared &&
            reader->codecs[i].wire_type == wire_type) {
            return reader->codecs + i;
        }
    }
    return NULL;
}

static uint64_t
sylvan_serialization_hash(MTBDD dd)
{
    uint64_t value = dd;
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

static int
sylvan_serialization_handle_add(
    struct sylvan_serialization_handle_table *table, MTBDD dd,
    sylvan_dd_family family)
{
    if (family == SYLVAN_DD_BDD) family = SYLVAN_DD_MTBDD;
    if (table->count == 0) table->family = family;
    else if (table->family != family) return SYLVAN_ERR_INVALID;

    if (table->count % SYLVAN_SERIALIZATION_HANDLE_BLOCK == 0) {
        if (table->block_count == table->block_capacity) {
            const size_t old_capacity = table->block_capacity;
            const size_t new_capacity =
                old_capacity == 0 ? 8 : old_capacity * 2;
            if (new_capacity < old_capacity ||
                new_capacity > SIZE_MAX / sizeof(*table->blocks)) {
                return SYLVAN_ERR_OOM;
            }
            MTBDD **blocks = realloc(
                table->blocks, new_capacity * sizeof(*blocks));
            if (blocks == NULL) return SYLVAN_ERR_OOM;
            table->blocks = blocks;
            table->block_capacity = new_capacity;
        }
        MTBDD *block = calloc(
            SYLVAN_SERIALIZATION_HANDLE_BLOCK, sizeof(*block));
        if (block == NULL) return SYLVAN_ERR_OOM;
        table->blocks[table->block_count++] = block;
    }

    MTBDD *slot = table->blocks[
        table->count / SYLVAN_SERIALIZATION_HANDLE_BLOCK] +
        table->count % SYLVAN_SERIALIZATION_HANDLE_BLOCK;
    *slot = dd;
    if (family == SYLVAN_DD_ZDD) zdd_protect((ZDD*)slot);
    else if (family == SYLVAN_DD_LISTDD) listdd_protect((LISTDD*)slot);
    else mtbdd_protect(slot);
    table->count++;
    return SYLVAN_OK;
}

static MTBDD
sylvan_serialization_handle_get(
    const struct sylvan_serialization_handle_table *table, uint64_t id)
{
    if (id == 0 || id > table->count) return mtbdd_invalid;
    const size_t index = (size_t)(id - 1);
    return table->blocks[
        index / SYLVAN_SERIALIZATION_HANDLE_BLOCK]
        [index % SYLVAN_SERIALIZATION_HANDLE_BLOCK];
}

static void
sylvan_serialization_handle_clear(
    struct sylvan_serialization_handle_table *table)
{
    for (size_t i = 0; i < table->count; i++) {
        MTBDD *slot = table->blocks[
            i / SYLVAN_SERIALIZATION_HANDLE_BLOCK] +
            i % SYLVAN_SERIALIZATION_HANDLE_BLOCK;
        if (table->family == SYLVAN_DD_ZDD) zdd_unprotect((ZDD*)slot);
        else if (table->family == SYLVAN_DD_LISTDD) {
            listdd_unprotect((LISTDD*)slot);
        } else mtbdd_unprotect(slot);
    }
    for (size_t i = 0; i < table->block_count; i++) {
        free(table->blocks[i]);
    }
    free(table->blocks);
    memset(table, 0, sizeof(*table));
}

static uint64_t
sylvan_serialization_dictionary_find(
    const struct sylvan_serialization_dictionary *dictionary, MTBDD dd)
{
    if (dictionary->capacity == 0) return 0;
    size_t slot = (size_t)sylvan_serialization_hash(dd) &
                  (dictionary->capacity - 1);
    while (dictionary->keys[slot] != 0) {
        if (dictionary->keys[slot] == dd) return dictionary->ids[slot];
        slot = (slot + 1) & (dictionary->capacity - 1);
    }
    return 0;
}

static int
sylvan_serialization_dictionary_grow(
    struct sylvan_serialization_dictionary *dictionary)
{
    const size_t old_capacity = dictionary->capacity;
    const size_t new_capacity =
        old_capacity == 0 ? 64 : old_capacity * 2;
    if (new_capacity < old_capacity ||
        new_capacity > SIZE_MAX / sizeof(*dictionary->keys) ||
        new_capacity > SIZE_MAX / sizeof(*dictionary->ids)) {
        return SYLVAN_ERR_OOM;
    }
    MTBDD *keys = calloc(new_capacity, sizeof(*keys));
    uint64_t *ids = calloc(new_capacity, sizeof(*ids));
    if (keys == NULL || ids == NULL) {
        free(keys);
        free(ids);
        return SYLVAN_ERR_OOM;
    }

    for (size_t i = 0; i < old_capacity; i++) {
        if (dictionary->keys[i] == 0) continue;
        size_t slot = (size_t)sylvan_serialization_hash(
            dictionary->keys[i]) & (new_capacity - 1);
        while (keys[slot] != 0) {
            slot = (slot + 1) & (new_capacity - 1);
        }
        keys[slot] = dictionary->keys[i];
        ids[slot] = dictionary->ids[i];
    }
    free(dictionary->keys);
    free(dictionary->ids);
    dictionary->keys = keys;
    dictionary->ids = ids;
    dictionary->capacity = new_capacity;
    return SYLVAN_OK;
}

static int
sylvan_serialization_dictionary_add(
    struct sylvan_serialization_dictionary *dictionary, MTBDD dd)
{
    if (dictionary->capacity == 0 ||
        dictionary->count + 1 > dictionary->capacity / 2) {
        const int status =
            sylvan_serialization_dictionary_grow(dictionary);
        if (status != SYLVAN_OK) return status;
    }
    if (dictionary->count == dictionary->node_capacity) {
        const size_t old_capacity = dictionary->node_capacity;
        const size_t new_capacity =
            old_capacity == 0 ? 64 : old_capacity * 2;
        if (new_capacity < old_capacity ||
            new_capacity > SIZE_MAX / sizeof(*dictionary->nodes)) {
            return SYLVAN_ERR_OOM;
        }
        MTBDD *nodes = realloc(
            dictionary->nodes, new_capacity * sizeof(*nodes));
        if (nodes == NULL) return SYLVAN_ERR_OOM;
        dictionary->nodes = nodes;
        dictionary->node_capacity = new_capacity;
    }

    const uint64_t id = (uint64_t)dictionary->count + 1;
    dictionary->nodes[dictionary->count++] = dd;
    size_t slot = (size_t)sylvan_serialization_hash(dd) &
                  (dictionary->capacity - 1);
    while (dictionary->keys[slot] != 0) {
        slot = (slot + 1) & (dictionary->capacity - 1);
    }
    dictionary->keys[slot] = dd;
    dictionary->ids[slot] = id;
    return SYLVAN_OK;
}

struct sylvan_serialization_visit {
    MTBDD dd;
    int expanded;
};

static int
sylvan_serialization_collect_bdd(
    struct sylvan_serialization_dictionary *dictionary, BDD root)
{
    size_t count = 0;
    size_t capacity = 64;
    struct sylvan_serialization_visit *stack =
        malloc(capacity * sizeof(*stack));
    if (stack == NULL) return SYLVAN_ERR_OOM;
    stack[count++] = (struct sylvan_serialization_visit){root, 0};

    int status = SYLVAN_OK;
    while (count != 0) {
        const struct sylvan_serialization_visit visit = stack[--count];
        const MTBDD dd = MTBDD_STRIPMARK(visit.dd);
        if (dd == bdd_false ||
            sylvan_serialization_dictionary_find(dictionary, dd) != 0) {
            continue;
        }
        if (mtbdd_is_leaf(dd)) {
            status = SYLVAN_ERR_INVALID;
            break;
        }
        if (visit.expanded) {
            status = sylvan_serialization_dictionary_add(dictionary, dd);
            if (status != SYLVAN_OK) break;
            continue;
        }

        if (count > SIZE_MAX - 3) {
            status = SYLVAN_ERR_OOM;
            break;
        }
        if (count + 3 > capacity) {
            size_t new_capacity = capacity * 2;
            while (new_capacity < count + 3) {
                if (new_capacity > SIZE_MAX / 2) {
                    status = SYLVAN_ERR_OOM;
                    break;
                }
                new_capacity *= 2;
            }
            if (status != SYLVAN_OK ||
                new_capacity > SIZE_MAX / sizeof(*stack)) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            struct sylvan_serialization_visit *grown = realloc(
                stack, new_capacity * sizeof(*stack));
            if (grown == NULL) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            stack = grown;
            capacity = new_capacity;
        }

        stack[count++] = (struct sylvan_serialization_visit){dd, 1};
        stack[count++] = (struct sylvan_serialization_visit){
            mtbdd_node_high(dd), 0
        };
        stack[count++] = (struct sylvan_serialization_visit){
            mtbdd_node_low(dd), 0
        };
    }

    free(stack);
    return status;
}

static int
sylvan_serialization_collect_mtbdd(
    struct sylvan_serialization_dictionary *dictionary, MTBDD root)
{
    size_t count = 0;
    size_t capacity = 64;
    struct sylvan_serialization_visit *stack =
        malloc(capacity * sizeof(*stack));
    if (stack == NULL) return SYLVAN_ERR_OOM;
    stack[count++] = (struct sylvan_serialization_visit){root, 0};

    int status = SYLVAN_OK;
    while (count != 0) {
        const struct sylvan_serialization_visit visit = stack[--count];
        const MTBDD dd = MTBDD_STRIPMARK(visit.dd);
        if (dd == mtbdd_undefined ||
            sylvan_serialization_dictionary_find(dictionary, dd) != 0) {
            continue;
        }
        if (visit.expanded) {
            status = sylvan_serialization_dictionary_add(dictionary, dd);
            if (status != SYLVAN_OK) break;
            continue;
        }
        if (mtbdd_is_leaf(dd)) {
            status = sylvan_serialization_dictionary_add(dictionary, dd);
            if (status != SYLVAN_OK) break;
            continue;
        }

        if (count > SIZE_MAX - 3) {
            status = SYLVAN_ERR_OOM;
            break;
        }
        if (count + 3 > capacity) {
            size_t new_capacity = capacity * 2;
            while (new_capacity < count + 3) {
                if (new_capacity > SIZE_MAX / 2) {
                    status = SYLVAN_ERR_OOM;
                    break;
                }
                new_capacity *= 2;
            }
            if (status != SYLVAN_OK ||
                new_capacity > SIZE_MAX / sizeof(*stack)) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            struct sylvan_serialization_visit *grown = realloc(
                stack, new_capacity * sizeof(*stack));
            if (grown == NULL) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            stack = grown;
            capacity = new_capacity;
        }

        stack[count++] = (struct sylvan_serialization_visit){dd, 1};
        stack[count++] = (struct sylvan_serialization_visit){
            mtbdd_node_high(dd), 0
        };
        stack[count++] = (struct sylvan_serialization_visit){
            mtbdd_node_low(dd), 0
        };
    }

    free(stack);
    return status;
}

static int
sylvan_serialization_is_bdd_set(BDDSET set)
{
    if (set == mtbdd_invalid ||
        (set != bdd_true && MTBDD_HASMARK(set))) {
        return 0;
    }
    uint32_t previous = 0;
    int has_previous = 0;
    while (set != bdd_true) {
        if (set == bdd_false || mtbdd_is_leaf(set) ||
            mtbdd_node_low(set) != bdd_false) {
            return 0;
        }
        const uint32_t level = mtbdd_node_variable(set);
        if (has_previous && level <= previous) return 0;
        previous = level;
        has_previous = 1;
        set = mtbdd_node_high(set);
        if (set != bdd_true && MTBDD_HASMARK(set)) return 0;
    }
    return 1;
}

static int
sylvan_serialization_collect_zdd(
    struct sylvan_serialization_dictionary *dictionary,
    ZDD root, BDDSET domain)
{
    size_t count = 0;
    size_t capacity = 64;
    struct sylvan_serialization_visit *stack =
        malloc(capacity * sizeof(*stack));
    if (stack == NULL) return SYLVAN_ERR_OOM;
    stack[count++] = (struct sylvan_serialization_visit){root, 0};

    int status = SYLVAN_OK;
    while (count != 0) {
        const struct sylvan_serialization_visit visit = stack[--count];
        const ZDD dd = visit.dd;
        if (dd == zdd_false || dd == zdd_base ||
            sylvan_serialization_dictionary_find(dictionary, dd) != 0) {
            continue;
        }
        if (MTBDD_HASMARK(dd) || zdd_is_leaf(dd)) {
            status = SYLVAN_ERR_INVALID;
            break;
        }
        if (visit.expanded) {
            status = sylvan_serialization_dictionary_add(dictionary, dd);
            if (status != SYLVAN_OK) break;
            continue;
        }
        const uint32_t level = zdd_top_var(dd);
        const ZDD low = zdd_node_low(dd);
        const ZDD high = zdd_node_high(dd);
        if (!bdd_set_contains(domain, level) || high == zdd_false ||
            (!zdd_is_leaf(low) && zdd_top_var(low) <= level) ||
            (!zdd_is_leaf(high) && zdd_top_var(high) <= level)) {
            status = SYLVAN_ERR_INVALID;
            break;
        }

        if (count > SIZE_MAX - 3) {
            status = SYLVAN_ERR_OOM;
            break;
        }
        if (count + 3 > capacity) {
            size_t new_capacity = capacity * 2;
            while (new_capacity < count + 3) {
                if (new_capacity > SIZE_MAX / 2) {
                    status = SYLVAN_ERR_OOM;
                    break;
                }
                new_capacity *= 2;
            }
            if (status != SYLVAN_OK ||
                new_capacity > SIZE_MAX / sizeof(*stack)) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            struct sylvan_serialization_visit *grown = realloc(
                stack, new_capacity * sizeof(*stack));
            if (grown == NULL) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            stack = grown;
            capacity = new_capacity;
        }

        stack[count++] = (struct sylvan_serialization_visit){dd, 1};
        stack[count++] = (struct sylvan_serialization_visit){high, 0};
        stack[count++] = (struct sylvan_serialization_visit){low, 0};
    }

    free(stack);
    return status;
}

static int
sylvan_serialization_collect_listdd(
    struct sylvan_serialization_dictionary *dictionary, LISTDD root)
{
    size_t count = 0;
    size_t capacity = 64;
    struct sylvan_serialization_visit *stack =
        malloc(capacity * sizeof(*stack));
    if (stack == NULL) return SYLVAN_ERR_OOM;
    stack[count++] = (struct sylvan_serialization_visit){root, 0};

    int status = SYLVAN_OK;
    while (count != 0) {
        const struct sylvan_serialization_visit visit = stack[--count];
        const LISTDD dd = visit.dd;
        if (dd == listdd_empty || dd == listdd_empty_list ||
            sylvan_serialization_dictionary_find(dictionary, dd) != 0) {
            continue;
        }
        if (dd == listdd_invalid || MTBDD_HASMARK(dd)) {
            status = SYLVAN_ERR_INVALID;
            break;
        }
        if (visit.expanded) {
            status = sylvan_serialization_dictionary_add(dictionary, dd);
            if (status != SYLVAN_OK) break;
            continue;
        }

        if (count > SIZE_MAX - 3) {
            status = SYLVAN_ERR_OOM;
            break;
        }
        if (count + 3 > capacity) {
            size_t new_capacity = capacity * 2;
            while (new_capacity < count + 3) {
                if (new_capacity > SIZE_MAX / 2) {
                    status = SYLVAN_ERR_OOM;
                    break;
                }
                new_capacity *= 2;
            }
            if (status != SYLVAN_OK ||
                new_capacity > SIZE_MAX / sizeof(*stack)) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            struct sylvan_serialization_visit *grown = realloc(
                stack, new_capacity * sizeof(*stack));
            if (grown == NULL) {
                status = SYLVAN_ERR_OOM;
                break;
            }
            stack = grown;
            capacity = new_capacity;
        }

        stack[count++] = (struct sylvan_serialization_visit){dd, 1};
        stack[count++] = (struct sylvan_serialization_visit){
            listdd_node_right(dd), 0
        };
        stack[count++] = (struct sylvan_serialization_visit){
            listdd_node_down(dd), 0
        };
    }

    free(stack);
    return status;
}

static int
sylvan_serialization_encode_reference(
    const struct sylvan_serialization_dictionary *dictionary,
    BDD dd,
    uint64_t *result)
{
    const MTBDD regular = MTBDD_STRIPMARK(dd);
    const uint64_t id = regular == bdd_false
        ? 0 : sylvan_serialization_dictionary_find(dictionary, regular);
    if (regular != bdd_false && id == 0) return SYLVAN_ERR_INVALID;
    if (id > UINT64_MAX / 2) return SYLVAN_ERR_OVERFLOW;
    *result = (id << 1) | (MTBDD_HASMARK(dd) ? 1 : 0);
    return SYLVAN_OK;
}

static int
sylvan_serialization_encode_zdd_reference(
    const struct sylvan_serialization_dictionary *dictionary,
    ZDD dd, uint64_t *result)
{
    if (MTBDD_HASMARK(dd)) return SYLVAN_ERR_INVALID;
    if (dd == zdd_false) {
        *result = 0;
        return SYLVAN_OK;
    }
    if (dd == zdd_base) {
        *result = 1;
        return SYLVAN_OK;
    }

    const uint64_t id =
        sylvan_serialization_dictionary_find(dictionary, dd);
    if (id == 0) return SYLVAN_ERR_INVALID;
    if (id > UINT64_MAX / 2) return SYLVAN_ERR_OVERFLOW;
    *result = id << 1;
    return SYLVAN_OK;
}

static int
sylvan_serialization_encode_listdd_reference(
    const struct sylvan_serialization_dictionary *dictionary,
    LISTDD dd, uint64_t *result)
{
    if (dd == listdd_empty || dd == listdd_empty_list) {
        *result = dd;
        return SYLVAN_OK;
    }
    if (dd == listdd_invalid || MTBDD_HASMARK(dd)) {
        return SYLVAN_ERR_INVALID;
    }

    const uint64_t id =
        sylvan_serialization_dictionary_find(dictionary, dd);
    if (id == 0) return SYLVAN_ERR_INVALID;
    if (id == UINT64_MAX) return SYLVAN_ERR_OVERFLOW;
    *result = id + 1;
    return SYLVAN_OK;
}

int
sylvan_serialization_writer_create(
    sylvan_serialization_writer **destination,
    sylvan_framed_writer *stream)
{
    if (destination == NULL || stream == NULL ||
        sylvan_framed_writer_remaining(stream) != 0) {
        return SYLVAN_ERR_INVALID;
    }
    sylvan_serialization_writer *writer = calloc(1, sizeof(*writer));
    if (writer == NULL) return SYLVAN_ERR_OOM;
    writer->stream = stream;
    *destination = writer;
    return SYLVAN_OK;
}

int
sylvan_serialization_writer_add_leaf_codec(
    sylvan_serialization_writer *writer,
    const sylvan_serialization_leaf_codec *codec)
{
    if (writer == NULL || codec == NULL || codec->type_name == NULL ||
        codec->type_name[0] == '\0' || codec->size == NULL ||
        codec->write == NULL || writer->failed ||
        sylvan_framed_writer_remaining(writer->stream) != 0) {
        return SYLVAN_ERR_INVALID;
    }

    uint32_t type;
    if (sylvan_mt_find_type(codec->type_name, &type) != SYLVAN_OK ||
        type < 3 ||
        sylvan_serialization_writer_codec(writer, type) != NULL ||
        writer->codec_count > UINT32_MAX - 3) {
        return SYLVAN_ERR_INVALID;
    }

    int status = sylvan_serialization_codec_reserve(
        &writer->codecs, &writer->codec_capacity, writer->codec_count);
    if (status != SYLVAN_OK) return status;
    status = sylvan_serialization_codec_copy(
        writer->codecs + writer->codec_count, codec, type);
    if (status != SYLVAN_OK) return status;
    writer->codecs[writer->codec_count].wire_type =
        (uint32_t)writer->codec_count + 3;
    writer->codec_count++;
    return SYLVAN_OK;
}

void
sylvan_serialization_writer_destroy(sylvan_serialization_writer *writer)
{
    if (writer == NULL) return;
    sylvan_serialization_handle_clear(&writer->roots);
    sylvan_serialization_handle_clear(&writer->zdd_roots);
    sylvan_serialization_handle_clear(&writer->listdd_roots);
    free(writer->dictionary.keys);
    free(writer->dictionary.ids);
    free(writer->dictionary.nodes);
    free(writer->zdd_dictionary.keys);
    free(writer->zdd_dictionary.ids);
    free(writer->zdd_dictionary.nodes);
    free(writer->listdd_dictionary.keys);
    free(writer->listdd_dictionary.ids);
    free(writer->listdd_dictionary.nodes);
    sylvan_serialization_codec_clear(writer->codecs, writer->codec_count);
    sylvan_serialization_writer_layouts_clear(writer);
    free(writer);
}

int
sylvan_serialization_write_bdd_CALL(
    lace_worker *lace, sylvan_serialization_writer *writer,
    BDD dd, uint64_t key)
{
    if (writer == NULL || writer->failed || dd == mtbdd_invalid ||
        sylvan_framed_writer_remaining(writer->stream) != 0) {
        return SYLVAN_ERR_INVALID;
    }

    int status = sylvan_serialization_handle_add(
        &writer->roots, dd, SYLVAN_DD_MTBDD);
    if (status != SYLVAN_OK) return status;
    sylvan_gc_test(lace);

    const size_t old_count = writer->dictionary.count;
    status = sylvan_serialization_collect_bdd(&writer->dictionary, dd);
    if (status != SYLVAN_OK) {
        writer->failed = 1;
        return status;
    }
    const size_t new_count = writer->dictionary.count - old_count;

    if (new_count != 0) {
        if (new_count > (UINT64_MAX - 16) / 24) {
            writer->failed = 1;
            return SYLVAN_ERR_OVERFLOW;
        }
        uint8_t batch_header[16];
        sylvan_store_u64(batch_header, (uint64_t)old_count + 1);
        sylvan_store_u64(batch_header + 8, new_count);
        status = sylvan_framed_writer_begin(
            writer->stream, SYLVAN_SERIALIZATION_BDD_NODES,
            16 + (uint64_t)new_count * 24);
        if (status == SYLVAN_OK) {
            status = sylvan_framed_writer_append(
                writer->stream, batch_header, sizeof(batch_header));
        }

        for (size_t i = old_count;
             i < writer->dictionary.count && status == SYLVAN_OK; i++) {
            const BDD node = writer->dictionary.nodes[i];
            uint64_t low;
            uint64_t high;
            status = sylvan_serialization_encode_reference(
                &writer->dictionary, mtbdd_node_low(node), &low);
            if (status == SYLVAN_OK) {
                status = sylvan_serialization_encode_reference(
                    &writer->dictionary, mtbdd_node_high(node), &high);
            }
            if (status != SYLVAN_OK) break;

            uint8_t record[24] = {0};
            sylvan_store_u32(record, mtbdd_node_variable(node));
            sylvan_store_u64(record + 8, low);
            sylvan_store_u64(record + 16, high);
            status = sylvan_framed_writer_append(
                writer->stream, record, sizeof(record));
        }
    }

    uint64_t root_reference;
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_encode_reference(
            &writer->dictionary, dd, &root_reference);
    }
    if (status == SYLVAN_OK) {
        uint8_t root_record[24] = {0};
        sylvan_store_u32(root_record, SYLVAN_DD_BDD);
        sylvan_store_u64(root_record + 8, key);
        sylvan_store_u64(root_record + 16, root_reference);
        status = sylvan_framed_writer_write(
            writer->stream, SYLVAN_SERIALIZATION_ROOT,
            root_record, sizeof(root_record));
    }
    if (status != SYLVAN_OK) writer->failed = 1;
    return status;
}

static int
sylvan_serialization_write_mtbdd_leaf(
    sylvan_serialization_writer *writer, uint64_t id, MTBDD leaf)
{
    const uint32_t type = mtbdd_leaf_type(leaf);
    if (type > 2) return SYLVAN_ERR_INVALID;

    uint8_t record[24] = {0};
    sylvan_store_u64(record, id);
    sylvan_store_u32(record + 8, type);
    sylvan_store_u32(record + 12, mtbdd_is_nan(leaf) ? 1 : 0);
    if (!mtbdd_is_nan(leaf)) {
        sylvan_store_u64(record + 16, mtbdd_leaf_value(leaf));
    }
    return sylvan_framed_writer_write(
        writer->stream, SYLVAN_SERIALIZATION_MTBDD_LEAF,
        record, sizeof(record));
}

static int
sylvan_serialization_write_mtbdd_type(
    sylvan_serialization_writer *writer,
    struct sylvan_serialization_leaf_codec_entry *codec)
{
    const size_t name_size = strlen(codec->type_name);
    if ((uint64_t)name_size > UINT64_MAX - 16) {
        return SYLVAN_ERR_OVERFLOW;
    }

    uint8_t header[16];
    sylvan_store_u32(header, codec->wire_type);
    sylvan_store_u32(header + 4, codec->format_version);
    sylvan_store_u64(header + 8, name_size);
    int status = sylvan_framed_writer_begin(
        writer->stream, SYLVAN_SERIALIZATION_MTBDD_TYPE,
        16 + (uint64_t)name_size);
    if (status == SYLVAN_OK) {
        status = sylvan_framed_writer_append(
            writer->stream, header, sizeof(header));
    }
    if (status == SYLVAN_OK) {
        status = sylvan_framed_writer_append(
            writer->stream, codec->type_name, name_size);
    }
    if (status == SYLVAN_OK) codec->declared = 1;
    return status;
}

static int
sylvan_serialization_write_mtbdd_custom_leaf(
    sylvan_serialization_writer *writer, uint64_t id, MTBDD leaf)
{
    const uint32_t type = mtbdd_leaf_type(leaf);
    struct sylvan_serialization_leaf_codec_entry *codec =
        sylvan_serialization_writer_codec(writer, type);
    if (codec == NULL) return SYLVAN_ERR_INVALID;

    int status = SYLVAN_OK;
    if (!codec->declared) {
        status = sylvan_serialization_write_mtbdd_type(writer, codec);
    }

    const int is_nan = mtbdd_is_nan(leaf);
    uint64_t size = 0;
    if (status == SYLVAN_OK && !is_nan) {
        status = codec->size(codec->context, leaf, &size);
        if (status > SYLVAN_OK) status = SYLVAN_ERR_CALLBACK;
    }
    if (status == SYLVAN_OK && size > UINT64_MAX - 24) {
        status = SYLVAN_ERR_OVERFLOW;
    }

    uint8_t header[24] = {0};
    if (status == SYLVAN_OK) {
        sylvan_store_u64(header, id);
        sylvan_store_u32(header + 8, codec->wire_type);
        sylvan_store_u32(header + 12, is_nan ? 1 : 0);
        sylvan_store_u64(header + 16, size);
        status = sylvan_framed_writer_begin(
            writer->stream, SYLVAN_SERIALIZATION_MTBDD_CUSTOM_LEAF,
            24 + size);
    }
    if (status == SYLVAN_OK) {
        status = sylvan_framed_writer_append(
            writer->stream, header, sizeof(header));
    }
    if (status == SYLVAN_OK && !is_nan) {
        status = codec->write(codec->context, leaf, writer->stream);
        if (status > SYLVAN_OK) status = SYLVAN_ERR_CALLBACK;
        if (status == SYLVAN_OK &&
            sylvan_framed_writer_remaining(writer->stream) != 0) {
            status = SYLVAN_ERR_CALLBACK;
        }
    }
    return status;
}

static int
sylvan_serialization_write_node_batch(
    sylvan_serialization_writer *writer, size_t first, size_t count)
{
    if (count == 0) return SYLVAN_OK;
    if (count > (UINT64_MAX - 16) / 24) return SYLVAN_ERR_OVERFLOW;

    uint8_t batch_header[16];
    sylvan_store_u64(batch_header, (uint64_t)first + 1);
    sylvan_store_u64(batch_header + 8, count);
    int status = sylvan_framed_writer_begin(
        writer->stream, SYLVAN_SERIALIZATION_BDD_NODES,
        16 + (uint64_t)count * 24);
    if (status == SYLVAN_OK) {
        status = sylvan_framed_writer_append(
            writer->stream, batch_header, sizeof(batch_header));
    }

    const size_t end = first + count;
    for (size_t i = first; i < end && status == SYLVAN_OK; i++) {
        const MTBDD node = writer->dictionary.nodes[i];
        uint64_t low;
        uint64_t high;
        status = sylvan_serialization_encode_reference(
            &writer->dictionary, mtbdd_node_low(node), &low);
        if (status == SYLVAN_OK) {
            status = sylvan_serialization_encode_reference(
                &writer->dictionary, mtbdd_node_high(node), &high);
        }
        if (status != SYLVAN_OK) break;

        uint8_t record[24] = {0};
        sylvan_store_u32(record, mtbdd_node_variable(node));
        sylvan_store_u64(record + 8, low);
        sylvan_store_u64(record + 16, high);
        status = sylvan_framed_writer_append(
            writer->stream, record, sizeof(record));
    }
    return status;
}

int
sylvan_serialization_write_mtbdd_CALL(
    lace_worker *lace, sylvan_serialization_writer *writer,
    MTBDD dd, uint64_t key)
{
    if (writer == NULL || writer->failed || dd == mtbdd_invalid ||
        sylvan_framed_writer_remaining(writer->stream) != 0) {
        return SYLVAN_ERR_INVALID;
    }

    int status = sylvan_serialization_handle_add(
        &writer->roots, dd, SYLVAN_DD_MTBDD);
    if (status != SYLVAN_OK) return status;
    sylvan_gc_test(lace);

    const size_t old_count = writer->dictionary.count;
    status = sylvan_serialization_collect_mtbdd(&writer->dictionary, dd);
    if (status != SYLVAN_OK) {
        writer->failed = 1;
        return status;
    }

    size_t i = old_count;
    while (i < writer->dictionary.count && status == SYLVAN_OK) {
        const MTBDD item = writer->dictionary.nodes[i];
        if (mtbdd_is_leaf(item)) {
            if (mtbdd_leaf_type(item) <= 2) {
                status = sylvan_serialization_write_mtbdd_leaf(
                    writer, (uint64_t)i + 1, item);
            } else {
                status = sylvan_serialization_write_mtbdd_custom_leaf(
                    writer, (uint64_t)i + 1, item);
            }
            i++;
        } else {
            const size_t first = i;
            do {
                i++;
            } while (i < writer->dictionary.count &&
                     !mtbdd_is_leaf(writer->dictionary.nodes[i]));
            status = sylvan_serialization_write_node_batch(
                writer, first, i - first);
        }
    }

    uint64_t root_reference;
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_encode_reference(
            &writer->dictionary, dd, &root_reference);
    }
    if (status == SYLVAN_OK) {
        uint8_t root_record[24] = {0};
        sylvan_store_u32(root_record, SYLVAN_DD_MTBDD);
        sylvan_store_u64(root_record + 8, key);
        sylvan_store_u64(root_record + 16, root_reference);
        status = sylvan_framed_writer_write(
            writer->stream, SYLVAN_SERIALIZATION_ROOT,
            root_record, sizeof(root_record));
    }
    if (status != SYLVAN_OK) writer->failed = 1;
    return status;
}

static int
sylvan_serialization_write_zdd_node_batch(
    sylvan_serialization_writer *writer, size_t first, size_t count)
{
    if (count == 0) return SYLVAN_OK;
    if (count > (UINT64_MAX - 16) / 24) return SYLVAN_ERR_OVERFLOW;

    uint8_t batch_header[16];
    sylvan_store_u64(batch_header, (uint64_t)first + 1);
    sylvan_store_u64(batch_header + 8, count);
    int status = sylvan_framed_writer_begin(
        writer->stream, SYLVAN_SERIALIZATION_ZDD_NODES,
        16 + (uint64_t)count * 24);
    if (status == SYLVAN_OK) {
        status = sylvan_framed_writer_append(
            writer->stream, batch_header, sizeof(batch_header));
    }

    const size_t end = first + count;
    for (size_t i = first; i < end && status == SYLVAN_OK; i++) {
        const ZDD node = writer->zdd_dictionary.nodes[i];
        uint64_t low;
        uint64_t high;
        status = sylvan_serialization_encode_zdd_reference(
            &writer->zdd_dictionary, zdd_node_low(node), &low);
        if (status == SYLVAN_OK) {
            status = sylvan_serialization_encode_zdd_reference(
                &writer->zdd_dictionary, zdd_node_high(node), &high);
        }
        if (status != SYLVAN_OK) break;

        uint8_t record[24] = {0};
        sylvan_store_u32(record, zdd_top_var(node));
        sylvan_store_u64(record + 8, low);
        sylvan_store_u64(record + 16, high);
        status = sylvan_framed_writer_append(
            writer->stream, record, sizeof(record));
    }
    return status;
}

int
sylvan_serialization_write_zdd_CALL(
    lace_worker *lace, sylvan_serialization_writer *writer,
    ZDD dd, BDDSET domain, uint64_t key)
{
    if (writer == NULL || writer->failed || dd == zdd_invalid ||
        !sylvan_serialization_is_bdd_set(domain) ||
        sylvan_framed_writer_remaining(writer->stream) != 0) {
        return SYLVAN_ERR_INVALID;
    }

    int status = sylvan_serialization_handle_add(
        &writer->roots, domain, SYLVAN_DD_MTBDD);
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_handle_add(
            &writer->zdd_roots, dd, SYLVAN_DD_ZDD);
    }
    if (status != SYLVAN_OK) return status;
    sylvan_gc_test(lace);

    const size_t old_domain_count = writer->dictionary.count;
    status = sylvan_serialization_collect_bdd(
        &writer->dictionary, domain);
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_write_node_batch(
            writer, old_domain_count,
            writer->dictionary.count - old_domain_count);
    }

    const size_t old_count = writer->zdd_dictionary.count;
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_collect_zdd(
            &writer->zdd_dictionary, dd, domain);
    }
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_write_zdd_node_batch(
            writer, old_count,
            writer->zdd_dictionary.count - old_count);
    }

    uint64_t root_reference;
    uint64_t domain_reference;
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_encode_zdd_reference(
            &writer->zdd_dictionary, dd, &root_reference);
    }
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_encode_reference(
            &writer->dictionary, domain, &domain_reference);
    }
    if (status == SYLVAN_OK) {
        uint8_t root_record[32] = {0};
        sylvan_store_u32(root_record, SYLVAN_DD_ZDD);
        sylvan_store_u64(root_record + 8, key);
        sylvan_store_u64(root_record + 16, root_reference);
        sylvan_store_u64(root_record + 24, domain_reference);
        status = sylvan_framed_writer_write(
            writer->stream, SYLVAN_SERIALIZATION_ROOT,
            root_record, sizeof(root_record));
    }
    if (status != SYLVAN_OK) writer->failed = 1;
    return status;
}

static int
sylvan_serialization_write_listdd_node_batch(
    sylvan_serialization_writer *writer, size_t first, size_t count)
{
    if (count == 0) return SYLVAN_OK;
    if (count > (UINT64_MAX - 16) / 24) return SYLVAN_ERR_OVERFLOW;

    uint8_t batch_header[16];
    sylvan_store_u64(batch_header, (uint64_t)first + 1);
    sylvan_store_u64(batch_header + 8, count);
    int status = sylvan_framed_writer_begin(
        writer->stream, SYLVAN_SERIALIZATION_LISTDD_NODES,
        16 + (uint64_t)count * 24);
    if (status == SYLVAN_OK) {
        status = sylvan_framed_writer_append(
            writer->stream, batch_header, sizeof(batch_header));
    }

    const size_t end = first + count;
    for (size_t i = first; i < end && status == SYLVAN_OK; i++) {
        const LISTDD node = writer->listdd_dictionary.nodes[i];
        uint64_t down;
        uint64_t right;
        status = sylvan_serialization_encode_listdd_reference(
            &writer->listdd_dictionary,
            listdd_node_down(node), &down);
        if (status == SYLVAN_OK) {
            status = sylvan_serialization_encode_listdd_reference(
                &writer->listdd_dictionary,
                listdd_node_right(node), &right);
        }
        if (status != SYLVAN_OK) break;

        uint8_t record[24] = {0};
        if (listdd_is_copy_node(node)) {
            sylvan_store_u32(record + 4, 1);
        } else {
            sylvan_store_u32(record, listdd_node_value(node));
        }
        sylvan_store_u64(record + 8, down);
        sylvan_store_u64(record + 16, right);
        status = sylvan_framed_writer_append(
            writer->stream, record, sizeof(record));
    }
    return status;
}

static int
sylvan_serialization_is_typed_listdd_layout(
    const listdd_relation_layout *layout)
{
    if (layout == NULL || layout->count == SIZE_MAX ||
        (layout->count != 0 && layout->positions == NULL) ||
        (layout->has_action_label != 0 &&
         layout->has_action_label != 1)) {
        return 0;
    }
    for (size_t i = 0; i < layout->count; i++) {
        if ((unsigned int)layout->positions[i] >
            (unsigned int)LISTDD_RELATION_READ_WRITE) {
            return 0;
        }
    }
    return 1;
}

static int
sylvan_serialization_write_listdd_layout(
    sylvan_serialization_writer *writer,
    const listdd_relation_layout *layout, uint64_t *identifier)
{
    if (!sylvan_serialization_is_typed_listdd_layout(layout)) {
        return SYLVAN_ERR_INVALID;
    }
    for (size_t i = 0; i < writer->layout_count; i++) {
        const struct sylvan_serialization_listdd_layout_entry *entry =
            writer->layouts + i;
        if (entry->count == layout->count &&
            entry->has_action_label == layout->has_action_label &&
            (layout->count == 0 ||
             memcmp(entry->positions, layout->positions,
                    layout->count * sizeof(*layout->positions)) == 0)) {
            *identifier = (uint64_t)i + 1;
            return SYLVAN_OK;
        }
    }

    if (layout->count > UINT64_MAX - 24 ||
        layout->count > SIZE_MAX / sizeof(*layout->positions)) {
        return SYLVAN_ERR_OVERFLOW;
    }
    int status = sylvan_serialization_layout_reserve(
        &writer->layouts, &writer->layout_capacity,
        writer->layout_count);
    if (status != SYLVAN_OK) return status;

    listdd_relation_access *positions = layout->count == 0 ? NULL :
        malloc(layout->count * sizeof(*positions));
    uint8_t *payload = layout->count == 0 ? NULL :
        malloc(layout->count);
    if (layout->count != 0 &&
        (positions == NULL || payload == NULL)) {
        free(payload);
        free(positions);
        return SYLVAN_ERR_OOM;
    }
    if (layout->count != 0) {
        memcpy(positions, layout->positions,
               layout->count * sizeof(*positions));
        for (size_t i = 0; i < layout->count; i++) {
            payload[i] = (uint8_t)layout->positions[i];
        }
    }

    const uint64_t id = (uint64_t)writer->layout_count + 1;
    uint8_t header[24] = {0};
    sylvan_store_u64(header, id);
    sylvan_store_u64(header + 8, layout->count);
    sylvan_store_u32(
        header + 16, (uint32_t)layout->has_action_label);
    status = sylvan_framed_writer_begin(
        writer->stream, SYLVAN_SERIALIZATION_LISTDD_LAYOUT,
        24 + (uint64_t)layout->count);
    if (status == SYLVAN_OK) {
        status = sylvan_framed_writer_append(
            writer->stream, header, sizeof(header));
    }
    if (status == SYLVAN_OK) {
        status = sylvan_framed_writer_append(
            writer->stream, payload, layout->count);
    }
    free(payload);
    if (status != SYLVAN_OK) {
        free(positions);
        return status;
    }

    struct sylvan_serialization_listdd_layout_entry *entry =
        writer->layouts + writer->layout_count;
    entry->positions = positions;
    entry->count = layout->count;
    entry->has_action_label = layout->has_action_label;
    entry->layout = NULL;
    writer->layout_count++;
    *identifier = id;
    return SYLVAN_OK;
}

static int
sylvan_serialization_write_listdd_root_CALL(
    lace_worker *lace, sylvan_serialization_writer *writer,
    LISTDD dd, const listdd_relation_layout *layout, uint64_t key)
{
    if (writer == NULL || writer->failed || dd == listdd_invalid ||
        (layout != NULL &&
         !sylvan_serialization_is_typed_listdd_layout(layout)) ||
        sylvan_framed_writer_remaining(writer->stream) != 0) {
        return SYLVAN_ERR_INVALID;
    }

    int status = sylvan_serialization_handle_add(
        &writer->listdd_roots, dd, SYLVAN_DD_LISTDD);
    if (status != SYLVAN_OK) return status;
    sylvan_gc_test(lace);

    const size_t old_count = writer->listdd_dictionary.count;
    status = sylvan_serialization_collect_listdd(
        &writer->listdd_dictionary, dd);
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_write_listdd_node_batch(
            writer, old_count,
            writer->listdd_dictionary.count - old_count);
    }

    uint64_t root_reference;
    uint64_t layout_identifier = 0;
    if (status == SYLVAN_OK) {
        status = sylvan_serialization_encode_listdd_reference(
            &writer->listdd_dictionary, dd, &root_reference);
    }
    if (status == SYLVAN_OK && layout != NULL) {
        status = sylvan_serialization_write_listdd_layout(
            writer, layout, &layout_identifier);
    }
    if (status == SYLVAN_OK) {
        uint8_t root_record[32] = {0};
        sylvan_store_u32(root_record, SYLVAN_DD_LISTDD);
        sylvan_store_u64(root_record + 8, key);
        sylvan_store_u64(root_record + 16, root_reference);
        sylvan_store_u64(root_record + 24, layout_identifier);
        status = sylvan_framed_writer_write(
            writer->stream, SYLVAN_SERIALIZATION_ROOT,
            root_record, layout == NULL ? 24 : 32);
    }
    if (status != SYLVAN_OK) writer->failed = 1;
    return status;
}

int
sylvan_serialization_write_listdd_CALL(
    lace_worker *lace, sylvan_serialization_writer *writer,
    LISTDD dd, uint64_t key)
{
    return sylvan_serialization_write_listdd_root_CALL(
        lace, writer, dd, NULL, key);
}

int
sylvan_serialization_write_listdd_relation_CALL(
    lace_worker *lace, sylvan_serialization_writer *writer,
    LISTDD dd, const listdd_relation_layout *layout, uint64_t key)
{
    if (layout == NULL) return SYLVAN_ERR_INVALID;
    return sylvan_serialization_write_listdd_root_CALL(
        lace, writer, dd, layout, key);
}

static int
sylvan_serialization_decode_reference(
    const struct sylvan_serialization_handle_table *nodes,
    uint64_t reference,
    MTBDD *result)
{
    const uint64_t id = reference >> 1;
    MTBDD dd;
    if (id == 0) dd = bdd_false;
    else {
        dd = sylvan_serialization_handle_get(nodes, id);
        if (dd == mtbdd_invalid) return SYLVAN_ERR_INVALID;
    }
    if ((reference & 1) != 0) dd = bdd_not(dd);
    *result = dd;
    return SYLVAN_OK;
}

static int
sylvan_serialization_decode_zdd_reference(
    const struct sylvan_serialization_handle_table *nodes,
    uint64_t reference, ZDD *result)
{
    const uint64_t id = reference >> 1;
    if (id == 0) {
        *result = (reference & 1) != 0 ? zdd_base : zdd_false;
        return SYLVAN_OK;
    }
    if ((reference & 1) != 0) return SYLVAN_ERR_INVALID;

    const ZDD dd = sylvan_serialization_handle_get(nodes, id);
    if (dd == zdd_invalid) return SYLVAN_ERR_INVALID;
    *result = dd;
    return SYLVAN_OK;
}

static int
sylvan_serialization_decode_listdd_reference(
    const struct sylvan_serialization_handle_table *nodes,
    uint64_t reference, LISTDD *result)
{
    if (reference <= 1) {
        *result = reference;
        return SYLVAN_OK;
    }

    const LISTDD dd =
        sylvan_serialization_handle_get(nodes, reference - 1);
    if (dd == listdd_invalid) return SYLVAN_ERR_INVALID;
    *result = dd;
    return SYLVAN_OK;
}

static int
sylvan_serialization_read_bdd_nodes_CALL(
    lace_worker *lace, sylvan_serialization_reader *reader,
    const sylvan_frame *frame)
{
    uint8_t batch_header[16];
    int status = sylvan_framed_reader_read(
        reader->stream, batch_header, sizeof(batch_header));
    if (status != SYLVAN_OK) return status;

    const uint64_t first = sylvan_load_u64(batch_header);
    const uint64_t count = sylvan_load_u64(batch_header + 8);
    if (first != (uint64_t)reader->nodes.count + 1 ||
        count > (UINT64_MAX - 16) / 24 ||
        frame->payload_size != 16 + count * 24 ||
        count > SIZE_MAX - reader->nodes.count) {
        return SYLVAN_ERR_INVALID;
    }

    for (uint64_t i = 0; i < count; i++) {
        uint8_t record[24];
        status = sylvan_framed_reader_read(
            reader->stream, record, sizeof(record));
        if (status != SYLVAN_OK) return status;
        const uint32_t level = sylvan_load_u32(record);
        if (sylvan_load_u32(record + 4) != 0) return SYLVAN_ERR_INVALID;

        MTBDD low;
        MTBDD high;
        status = sylvan_serialization_decode_reference(
            &reader->nodes, sylvan_load_u64(record + 8), &low);
        if (status == SYLVAN_OK) {
            status = sylvan_serialization_decode_reference(
                &reader->nodes, sylvan_load_u64(record + 16), &high);
        }
        if (status != SYLVAN_OK || MTBDD_HASMARK(low) || low == high) {
            return SYLVAN_ERR_INVALID;
        }
        if ((!mtbdd_is_leaf(low) && mtbdd_node_variable(low) <= level) ||
            (!mtbdd_is_leaf(MTBDD_STRIPMARK(high)) &&
             mtbdd_node_variable(MTBDD_STRIPMARK(high)) <= level)) {
            return SYLVAN_ERR_INVALID;
        }

        MTBDD node = mtbdd_invalid;
        mtbdd_refs_pushptr(&node);
        status = _mtbdd_try_make_node(&node, level, low, high);
        if (status == SYLVAN_OK && MTBDD_HASMARK(node)) {
            status = SYLVAN_ERR_INVALID;
        }
        if (status == SYLVAN_OK) {
            status = sylvan_serialization_handle_add(
                &reader->nodes, node, SYLVAN_DD_MTBDD);
        }
        mtbdd_refs_popptr(1);
        if (status != SYLVAN_OK) return status;
        sylvan_gc_test(lace);
    }

    return sylvan_framed_reader_remaining(reader->stream) == 0
        ? SYLVAN_OK : SYLVAN_ERR_INVALID;
}

static int
sylvan_serialization_read_mtbdd_leaf_CALL(
    lace_worker *lace, sylvan_serialization_reader *reader,
    const sylvan_frame *frame)
{
    if (frame->payload_size != 24) return SYLVAN_ERR_INVALID;

    uint8_t record[24];
    int status = sylvan_framed_reader_read(
        reader->stream, record, sizeof(record));
    if (status != SYLVAN_OK) return status;

    const uint64_t id = sylvan_load_u64(record);
    const uint32_t type = sylvan_load_u32(record + 8);
    const uint32_t flags = sylvan_load_u32(record + 12);
    const uint64_t value = sylvan_load_u64(record + 16);
    if (id != (uint64_t)reader->nodes.count + 1 ||
        type > 2 || flags > 1 || (flags != 0 && value != 0)) {
        return SYLVAN_ERR_INVALID;
    }

    MTBDD leaf = flags == 0 ? mtbdd_leaf(type, value) : mtbdd_nan(type);
    status = sylvan_serialization_handle_add(
        &reader->nodes, leaf, SYLVAN_DD_MTBDD);
    if (status != SYLVAN_OK) return status;
    sylvan_gc_test(lace);
    return SYLVAN_OK;
}

static int
sylvan_serialization_read_zdd_nodes_CALL(
    lace_worker *lace, sylvan_serialization_reader *reader,
    const sylvan_frame *frame)
{
    uint8_t batch_header[16];
    int status = sylvan_framed_reader_read(
        reader->stream, batch_header, sizeof(batch_header));
    if (status != SYLVAN_OK) return status;

    const uint64_t first = sylvan_load_u64(batch_header);
    const uint64_t count = sylvan_load_u64(batch_header + 8);
    if (first != (uint64_t)reader->zdd_nodes.count + 1 ||
        count > (UINT64_MAX - 16) / 24 ||
        frame->payload_size != 16 + count * 24 ||
        count > SIZE_MAX - reader->zdd_nodes.count) {
        return SYLVAN_ERR_INVALID;
    }

    for (uint64_t i = 0; i < count; i++) {
        uint8_t record[24];
        status = sylvan_framed_reader_read(
            reader->stream, record, sizeof(record));
        if (status != SYLVAN_OK) return status;
        const uint32_t level = sylvan_load_u32(record);
        if (sylvan_load_u32(record + 4) != 0) {
            return SYLVAN_ERR_INVALID;
        }

        ZDD low;
        ZDD high;
        status = sylvan_serialization_decode_zdd_reference(
            &reader->zdd_nodes, sylvan_load_u64(record + 8), &low);
        if (status == SYLVAN_OK) {
            status = sylvan_serialization_decode_zdd_reference(
                &reader->zdd_nodes,
                sylvan_load_u64(record + 16), &high);
        }
        if (status != SYLVAN_OK || high == zdd_false ||
            (!zdd_is_leaf(low) && zdd_top_var(low) <= level) ||
            (!zdd_is_leaf(high) && zdd_top_var(high) <= level)) {
            return SYLVAN_ERR_INVALID;
        }

        ZDD node = zdd_invalid;
        zdd_refs_pushptr(&node);
        status = _zdd_try_make_node(&node, level, low, high);
        if (status == SYLVAN_OK &&
            (node == zdd_false || node == zdd_base)) {
            status = SYLVAN_ERR_INVALID;
        }
        if (status == SYLVAN_OK) {
            status = sylvan_serialization_handle_add(
                &reader->zdd_nodes, node, SYLVAN_DD_ZDD);
        }
        zdd_refs_popptr(1);
        if (status != SYLVAN_OK) return status;
        sylvan_gc_test(lace);
    }

    return sylvan_framed_reader_remaining(reader->stream) == 0
        ? SYLVAN_OK : SYLVAN_ERR_INVALID;
}

static int
sylvan_serialization_read_listdd_nodes_CALL(
    lace_worker *lace, sylvan_serialization_reader *reader,
    const sylvan_frame *frame)
{
    uint8_t batch_header[16];
    int status = sylvan_framed_reader_read(
        reader->stream, batch_header, sizeof(batch_header));
    if (status != SYLVAN_OK) return status;

    const uint64_t first = sylvan_load_u64(batch_header);
    const uint64_t count = sylvan_load_u64(batch_header + 8);
    if (first != (uint64_t)reader->listdd_nodes.count + 1 ||
        count > (UINT64_MAX - 16) / 24 ||
        frame->payload_size != 16 + count * 24 ||
        count > SIZE_MAX - reader->listdd_nodes.count) {
        return SYLVAN_ERR_INVALID;
    }

    for (uint64_t i = 0; i < count; i++) {
        uint8_t record[24];
        status = sylvan_framed_reader_read(
            reader->stream, record, sizeof(record));
        if (status != SYLVAN_OK) return status;
        const uint32_t value = sylvan_load_u32(record);
        const uint32_t flags = sylvan_load_u32(record + 4);
        if (flags > 1 || (flags != 0 && value != 0)) {
            return SYLVAN_ERR_INVALID;
        }

        LISTDD down;
        LISTDD right;
        status = sylvan_serialization_decode_listdd_reference(
            &reader->listdd_nodes,
            sylvan_load_u64(record + 8), &down);
        if (status == SYLVAN_OK) {
            status = sylvan_serialization_decode_listdd_reference(
                &reader->listdd_nodes,
                sylvan_load_u64(record + 16), &right);
        }
        if (status != SYLVAN_OK) return status;

        LISTDD node = listdd_invalid;
        listdd_refs_pushptr(&node);
        if (flags != 0) {
            status = _listdd_try_make_copy_node(&node, down, right);
        } else {
            status = _listdd_try_make_node(
                &node, value, down, right);
        }
        if (status == SYLVAN_OK &&
            (node == listdd_empty || node == listdd_empty_list)) {
            status = SYLVAN_ERR_INVALID;
        }
        if (status == SYLVAN_OK) {
            status = sylvan_serialization_handle_add(
                &reader->listdd_nodes, node, SYLVAN_DD_LISTDD);
        }
        listdd_refs_popptr(1);
        if (status != SYLVAN_OK) return status;
        sylvan_gc_test(lace);
    }

    return sylvan_framed_reader_remaining(reader->stream) == 0
        ? SYLVAN_OK : SYLVAN_ERR_INVALID;
}

static int
sylvan_serialization_read_listdd_layout(
    sylvan_serialization_reader *reader, const sylvan_frame *frame)
{
    if (frame->payload_size < 24) return SYLVAN_ERR_INVALID;

    uint8_t header[24];
    int status = sylvan_framed_reader_read(
        reader->stream, header, sizeof(header));
    if (status != SYLVAN_OK) return status;

    const uint64_t identifier = sylvan_load_u64(header);
    const uint64_t count = sylvan_load_u64(header + 8);
    const uint32_t has_action_label = sylvan_load_u32(header + 16);
    if (identifier != (uint64_t)reader->layout_count + 1 ||
        count != frame->payload_size - 24 ||
        count > SIZE_MAX ||
        count > SIZE_MAX / sizeof(listdd_relation_access) ||
        has_action_label > 1 ||
        sylvan_load_u32(header + 20) != 0) {
        return SYLVAN_ERR_INVALID;
    }

    uint8_t *payload = count == 0 ? NULL : malloc((size_t)count);
    listdd_relation_access *positions = count == 0 ? NULL :
        malloc((size_t)count * sizeof(*positions));
    if (count != 0 && (payload == NULL || positions == NULL)) {
        free(positions);
        free(payload);
        return SYLVAN_ERR_OOM;
    }
    status = sylvan_framed_reader_read(
        reader->stream, payload, (size_t)count);
    for (size_t i = 0; i < (size_t)count && status == SYLVAN_OK; i++) {
        if (payload[i] > LISTDD_RELATION_READ_WRITE) {
            status = SYLVAN_ERR_INVALID;
        } else {
            positions[i] = (listdd_relation_access)payload[i];
        }
    }
    free(payload);
    if (status != SYLVAN_OK) {
        free(positions);
        return status;
    }

    listdd_relation_layout *layout = NULL;
    status = listdd_relation_layout_create(
        &layout, positions, (size_t)count, (int)has_action_label);
    free(positions);
    if (status != SYLVAN_OK) return status;

    status = sylvan_serialization_layout_reserve(
        &reader->layouts, &reader->layout_capacity,
        reader->layout_count);
    if (status != SYLVAN_OK) {
        listdd_relation_layout_destroy(layout);
        return status;
    }
    struct sylvan_serialization_listdd_layout_entry *entry =
        reader->layouts + reader->layout_count;
    memset(entry, 0, sizeof(*entry));
    entry->layout = layout;
    reader->layout_count++;
    return SYLVAN_OK;
}

static int
sylvan_serialization_read_mtbdd_type(
    sylvan_serialization_reader *reader, const sylvan_frame *frame)
{
    if (frame->payload_size < 16) return SYLVAN_ERR_INVALID;

    uint8_t header[16];
    int status = sylvan_framed_reader_read(
        reader->stream, header, sizeof(header));
    if (status != SYLVAN_OK) return status;

    const uint32_t wire_type = sylvan_load_u32(header);
    const uint32_t format_version = sylvan_load_u32(header + 4);
    const uint64_t name_size = sylvan_load_u64(header + 8);
    if (wire_type < 3 || name_size == 0 ||
        name_size != frame->payload_size - 16 ||
        name_size > SIZE_MAX - 1 ||
        sylvan_serialization_reader_codec_by_wire(
            reader, wire_type) != NULL) {
        return SYLVAN_ERR_INVALID;
    }

    char *name = malloc((size_t)name_size + 1);
    if (name == NULL) return SYLVAN_ERR_OOM;
    status = sylvan_framed_reader_read(
        reader->stream, name, (size_t)name_size);
    if (status == SYLVAN_OK &&
        memchr(name, '\0', (size_t)name_size) != NULL) {
        status = SYLVAN_ERR_INVALID;
    }
    name[(size_t)name_size] = '\0';

    struct sylvan_serialization_leaf_codec_entry *match = NULL;
    if (status == SYLVAN_OK) {
        for (size_t i = 0; i < reader->codec_count; i++) {
            struct sylvan_serialization_leaf_codec_entry *codec =
                reader->codecs + i;
            if (!codec->declared &&
                codec->format_version == format_version &&
                strcmp(codec->type_name, name) == 0) {
                match = codec;
                break;
            }
        }
        if (match == NULL) status = SYLVAN_ERR_INVALID;
    }
    free(name);

    if (status == SYLVAN_OK) {
        match->wire_type = wire_type;
        match->declared = 1;
    }
    return status;
}

static int
sylvan_serialization_read_mtbdd_custom_leaf_CALL(
    lace_worker *lace, sylvan_serialization_reader *reader,
    const sylvan_frame *frame)
{
    if (frame->payload_size < 24) return SYLVAN_ERR_INVALID;

    uint8_t header[24];
    int status = sylvan_framed_reader_read(
        reader->stream, header, sizeof(header));
    if (status != SYLVAN_OK) return status;

    const uint64_t id = sylvan_load_u64(header);
    const uint32_t wire_type = sylvan_load_u32(header + 8);
    const uint32_t flags = sylvan_load_u32(header + 12);
    const uint64_t size = sylvan_load_u64(header + 16);
    struct sylvan_serialization_leaf_codec_entry *codec =
        sylvan_serialization_reader_codec_by_wire(reader, wire_type);
    if (id != (uint64_t)reader->nodes.count + 1 ||
        codec == NULL || flags > 1 ||
        size != frame->payload_size - 24 ||
        (flags != 0 && size != 0)) {
        return SYLVAN_ERR_INVALID;
    }

    MTBDD leaf = mtbdd_invalid;
    if (flags != 0) {
        leaf = mtbdd_nan(codec->type);
    } else {
        status = codec->read(
            codec->context, reader->stream, size, &leaf);
        if (status > SYLVAN_OK) status = SYLVAN_ERR_CALLBACK;
        if (status == SYLVAN_OK &&
            sylvan_framed_reader_remaining(reader->stream) != 0) {
            status = SYLVAN_ERR_CALLBACK;
        }
        if (status == SYLVAN_OK &&
            (leaf == mtbdd_invalid || leaf == mtbdd_undefined ||
             leaf == bdd_true || MTBDD_HASMARK(leaf) ||
             !mtbdd_is_leaf(leaf) || mtbdd_is_nan(leaf) ||
             mtbdd_leaf_type(leaf) != codec->type)) {
            status = SYLVAN_ERR_CALLBACK;
        }
    }
    if (status != SYLVAN_OK) return status;

    status = sylvan_serialization_handle_add(
        &reader->nodes, leaf, SYLVAN_DD_MTBDD);
    if (status != SYLVAN_OK) return status;
    sylvan_gc_test(lace);
    return SYLVAN_OK;
}

int
sylvan_serialization_reader_create(
    sylvan_serialization_reader **destination,
    sylvan_framed_reader *stream,
    sylvan_serialization_frame_cb frame_callback,
    void *context)
{
    if (destination == NULL || stream == NULL ||
        sylvan_framed_reader_remaining(stream) != 0) {
        return SYLVAN_ERR_INVALID;
    }
    sylvan_serialization_reader *reader = calloc(1, sizeof(*reader));
    if (reader == NULL) return SYLVAN_ERR_OOM;
    reader->stream = stream;
    reader->frame_callback = frame_callback;
    reader->context = context;
    *destination = reader;
    return SYLVAN_OK;
}

int
sylvan_serialization_reader_add_leaf_codec(
    sylvan_serialization_reader *reader,
    const sylvan_serialization_leaf_codec *codec)
{
    if (reader == NULL || codec == NULL || codec->type_name == NULL ||
        codec->type_name[0] == '\0' || codec->read == NULL ||
        reader->failed ||
        sylvan_framed_reader_remaining(reader->stream) != 0) {
        return SYLVAN_ERR_INVALID;
    }

    uint32_t type;
    if (sylvan_mt_find_type(codec->type_name, &type) != SYLVAN_OK ||
        type < 3) {
        return SYLVAN_ERR_INVALID;
    }
    for (size_t i = 0; i < reader->codec_count; i++) {
        if (reader->codecs[i].type == type &&
            reader->codecs[i].format_version == codec->format_version) {
            return SYLVAN_ERR_INVALID;
        }
    }

    int status = sylvan_serialization_codec_reserve(
        &reader->codecs, &reader->codec_capacity, reader->codec_count);
    if (status != SYLVAN_OK) return status;
    status = sylvan_serialization_codec_copy(
        reader->codecs + reader->codec_count, codec, type);
    if (status != SYLVAN_OK) return status;
    reader->codec_count++;
    return SYLVAN_OK;
}

void
sylvan_serialization_reader_destroy(sylvan_serialization_reader *reader)
{
    if (reader == NULL) return;
    sylvan_serialization_handle_clear(&reader->nodes);
    sylvan_serialization_handle_clear(&reader->zdd_nodes);
    sylvan_serialization_handle_clear(&reader->listdd_nodes);
    sylvan_serialization_codec_clear(reader->codecs, reader->codec_count);
    sylvan_serialization_reader_layouts_clear(reader);
    free(reader);
}

int
sylvan_serialization_reader_next_CALL(
    lace_worker *lace, sylvan_serialization_reader *reader,
    sylvan_serialization_root *root, int *has_root)
{
    if (reader == NULL || root == NULL || has_root == NULL ||
        reader->failed) {
        return SYLVAN_ERR_INVALID;
    }

    for (;;) {
        sylvan_frame frame;
        int has_frame;
        int status = sylvan_framed_reader_next(
            reader->stream, &frame, &has_frame);
        if (status != SYLVAN_OK) {
            reader->failed = 1;
            return status;
        }
        if (!has_frame) {
            *has_root = 0;
            return SYLVAN_OK;
        }

        if (frame.type == SYLVAN_SERIALIZATION_BDD_NODES) {
            status = sylvan_serialization_read_bdd_nodes_CALL(
                lace, reader, &frame);
        } else if (frame.type == SYLVAN_SERIALIZATION_MTBDD_LEAF) {
            status = sylvan_serialization_read_mtbdd_leaf_CALL(
                lace, reader, &frame);
        } else if (frame.type == SYLVAN_SERIALIZATION_MTBDD_TYPE) {
            status = sylvan_serialization_read_mtbdd_type(
                reader, &frame);
        } else if (frame.type ==
                   SYLVAN_SERIALIZATION_MTBDD_CUSTOM_LEAF) {
            status = sylvan_serialization_read_mtbdd_custom_leaf_CALL(
                lace, reader, &frame);
        } else if (frame.type == SYLVAN_SERIALIZATION_ZDD_NODES) {
            status = sylvan_serialization_read_zdd_nodes_CALL(
                lace, reader, &frame);
        } else if (frame.type == SYLVAN_SERIALIZATION_LISTDD_NODES) {
            status = sylvan_serialization_read_listdd_nodes_CALL(
                lace, reader, &frame);
        } else if (frame.type == SYLVAN_SERIALIZATION_LISTDD_LAYOUT) {
            status = sylvan_serialization_read_listdd_layout(
                reader, &frame);
        } else if (frame.type == SYLVAN_SERIALIZATION_ROOT) {
            if (frame.payload_size != 24 &&
                frame.payload_size != 32) {
                status = SYLVAN_ERR_INVALID;
            } else {
                uint8_t record[32] = {0};
                status = sylvan_framed_reader_read(
                    reader->stream, record, (size_t)frame.payload_size);
                uint32_t family = 0;
                if (status == SYLVAN_OK) {
                    family = sylvan_load_u32(record);
                }
                if (status == SYLVAN_OK &&
                    (((family == SYLVAN_DD_BDD ||
                       family == SYLVAN_DD_MTBDD) &&
                      frame.payload_size != 24) ||
                     (family == SYLVAN_DD_ZDD &&
                      frame.payload_size != 32) ||
                     (family == SYLVAN_DD_LISTDD &&
                      frame.payload_size != 24 &&
                      frame.payload_size != 32) ||
                     (family != SYLVAN_DD_BDD &&
                      family != SYLVAN_DD_MTBDD &&
                      family != SYLVAN_DD_ZDD &&
                      family != SYLVAN_DD_LISTDD) ||
                     sylvan_load_u32(record + 4) != 0)) {
                    status = SYLVAN_ERR_INVALID;
                }
                MTBDD dd = mtbdd_invalid;
                BDDSET domain = mtbdd_invalid;
                const listdd_relation_layout *listdd_layout = NULL;
                if (status == SYLVAN_OK) {
                    if (family == SYLVAN_DD_ZDD) {
                        status = sylvan_serialization_decode_zdd_reference(
                            &reader->zdd_nodes,
                            sylvan_load_u64(record + 16), (ZDD*)&dd);
                        if (status == SYLVAN_OK) {
                            status = sylvan_serialization_decode_reference(
                                &reader->nodes,
                                sylvan_load_u64(record + 24), &domain);
                        }
                        if (status == SYLVAN_OK &&
                            !sylvan_serialization_is_bdd_set(domain)) {
                            status = SYLVAN_ERR_INVALID;
                        }
                        if (status == SYLVAN_OK) {
                            struct sylvan_serialization_dictionary check = {0};
                            status = sylvan_serialization_collect_zdd(
                                &check, (ZDD)dd, domain);
                            free(check.keys);
                            free(check.ids);
                            free(check.nodes);
                        }
                    } else if (family == SYLVAN_DD_LISTDD) {
                        status =
                            sylvan_serialization_decode_listdd_reference(
                                &reader->listdd_nodes,
                                sylvan_load_u64(record + 16),
                                (LISTDD*)&dd);
                        if (status == SYLVAN_OK &&
                            frame.payload_size == 32) {
                            const uint64_t layout_identifier =
                                sylvan_load_u64(record + 24);
                            if (layout_identifier == 0 ||
                                layout_identifier >
                                    reader->layout_count) {
                                status = SYLVAN_ERR_INVALID;
                            } else {
                                listdd_layout = reader->layouts[
                                    (size_t)layout_identifier - 1].layout;
                            }
                        }
                    } else {
                        status = sylvan_serialization_decode_reference(
                            &reader->nodes,
                            sylvan_load_u64(record + 16), &dd);
                    }
                }
                if (status == SYLVAN_OK) {
                    root->family = (sylvan_dd_family)family;
                    root->key = sylvan_load_u64(record + 8);
                    root->dd = dd;
                    root->domain = domain;
                    root->listdd_layout = listdd_layout;
                    *has_root = 1;
                    return SYLVAN_OK;
                }
            }
        } else if (reader->frame_callback == NULL) {
            status = sylvan_framed_reader_skip(reader->stream);
        } else {
            status = reader->frame_callback(
                reader->stream, &frame, reader->context);
            if (status > SYLVAN_OK) status = SYLVAN_ERR_CALLBACK;
            if (status == SYLVAN_OK &&
                sylvan_framed_reader_remaining(reader->stream) != 0) {
                status = SYLVAN_ERR_CALLBACK;
            }
        }

        if (status != SYLVAN_OK) {
            reader->failed = 1;
            return status;
        }
    }
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
