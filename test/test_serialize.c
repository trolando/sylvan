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

struct application_frame {
    uint8_t payload[8];
    size_t size;
    size_t calls;
};

static int
read_application_frame(
    sylvan_framed_reader *stream,
    const sylvan_frame *frame,
    void *context)
{
    struct application_frame *application = context;
    if (frame->type < SYLVAN_SERIALIZATION_APPLICATION ||
        frame->payload_size > sizeof(application->payload)) {
        return SYLVAN_ERR_INVALID;
    }
    application->size = (size_t)frame->payload_size;
    const int status = sylvan_framed_reader_read(
        stream, application->payload, application->size);
    if (status == SYLVAN_OK) application->calls++;
    return status;
}

static int
test_bdd_serialization(void)
{
    struct memory_stream stream = {
        NULL, 0, 0, 0, SIZE_MAX
    };
    sylvan_framed_writer *framed_writer = NULL;
    sylvan_serialization_writer *writer = NULL;
    test_assert(sylvan_framed_writer_create(
        &framed_writer, memory_write, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_writer_create(
        &writer, framed_writer) == SYLVAN_OK);

    BDD x = mtbdd_invalid;
    BDD y = mtbdd_invalid;
    BDD first = mtbdd_invalid;
    BDD second = mtbdd_invalid;
    mtbdd_protect(&x);
    mtbdd_protect(&y);
    mtbdd_protect(&first);
    mtbdd_protect(&second);
    test_assert(bdd_var_at_level(&x, 1) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&y, 4) == SYLVAN_OK);
    test_assert(bdd_and(&first, x, y) == SYLVAN_OK);
    test_assert(bdd_or(&second, x, y) == SYLVAN_OK);

    test_assert(sylvan_serialization_write_bdd(
        writer, first, 11) == SYLVAN_OK);

    /*
     * Start reading before the writer has produced the second root or the end
     * marker. The first root must already be independently consumable.
     */
    sylvan_framed_reader *framed_reader = NULL;
    struct application_frame application = {{0}, 0, 0};
    sylvan_serialization_reader *reader = NULL;
    test_assert(sylvan_framed_reader_create(
        &framed_reader, memory_read, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_reader_create(
        &reader, framed_reader, read_application_frame,
        &application) == SYLVAN_OK);

    sylvan_serialization_root root = {
        SYLVAN_DD_LISTDD, UINT64_MAX, mtbdd_invalid, mtbdd_invalid, NULL
    };
    int has_root = -1;
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_BDD &&
        root.key == 11 && root.dd == first);

    const uint8_t metadata[] = {'a', 'p', 'p'};
    test_assert(sylvan_framed_writer_write(
        framed_writer, SYLVAN_SERIALIZATION_APPLICATION,
        metadata, sizeof(metadata)) == SYLVAN_OK);
    test_assert(sylvan_serialization_write_bdd(
        writer, second, 22) == SYLVAN_OK);
    test_assert(sylvan_serialization_write_bdd(
        writer, bdd_true, 33) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_finish(framed_writer) == SYLVAN_OK);

    sylvan_gc();
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_BDD &&
        root.key == 22 && root.dd == second);
    test_assert(
        application.calls == 1 && application.size == sizeof(metadata) &&
        memcmp(application.payload, metadata, sizeof(metadata)) == 0);
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_BDD &&
        root.key == 33 && root.dd == bdd_true);
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(has_root == 0);

    sylvan_serialization_reader_destroy(reader);
    sylvan_framed_reader_destroy(framed_reader);
    sylvan_serialization_writer_destroy(writer);
    sylvan_framed_writer_destroy(framed_writer);
    mtbdd_unprotect(&second);
    mtbdd_unprotect(&first);
    mtbdd_unprotect(&y);
    mtbdd_unprotect(&x);
    free(stream.data);
    return 0;
}

static int
test_mtbdd_serialization(void)
{
    struct memory_stream stream = {
        NULL, 0, 0, 0, SIZE_MAX
    };
    sylvan_framed_writer *framed_writer = NULL;
    sylvan_serialization_writer *writer = NULL;
    test_assert(sylvan_framed_writer_create(
        &framed_writer, memory_write, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_writer_create(
        &writer, framed_writer) == SYLVAN_OK);

    MTBDD shared = mtbdd_invalid;
    MTBDD first = mtbdd_invalid;
    MTBDD second = mtbdd_invalid;
    mtbdd_protect(&shared);
    mtbdd_protect(&first);
    mtbdd_protect(&second);
    shared = mtbdd_make_node(
        4, mtbdd_int64(-17), mtbdd_double(2.5));
    first = mtbdd_make_node(
        1, shared, mtbdd_fraction(5, 7));
    second = mtbdd_make_node(
        2, shared, mtbdd_nan(1));

    test_assert(sylvan_serialization_write_mtbdd(
        writer, first, 101) == SYLVAN_OK);

    sylvan_framed_reader *framed_reader = NULL;
    sylvan_serialization_reader *reader = NULL;
    test_assert(sylvan_framed_reader_create(
        &framed_reader, memory_read, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_reader_create(
        &reader, framed_reader, NULL, NULL) == SYLVAN_OK);

    sylvan_serialization_root root = {
        SYLVAN_DD_BDD, UINT64_MAX, mtbdd_invalid, mtbdd_invalid, NULL
    };
    int has_root = -1;
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_MTBDD &&
        root.key == 101 && root.dd == first);

    test_assert(sylvan_serialization_write_mtbdd(
        writer, second, 102) == SYLVAN_OK);
    test_assert(sylvan_serialization_write_mtbdd(
        writer, mtbdd_undefined, 103) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_finish(framed_writer) == SYLVAN_OK);

    sylvan_gc();
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_MTBDD &&
        root.key == 102 && root.dd == second);
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_MTBDD &&
        root.key == 103 && root.dd == mtbdd_undefined);
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(has_root == 0);

    sylvan_serialization_reader_destroy(reader);
    sylvan_framed_reader_destroy(framed_reader);
    sylvan_serialization_writer_destroy(writer);
    sylvan_framed_writer_destroy(framed_writer);
    mtbdd_unprotect(&second);
    mtbdd_unprotect(&first);
    mtbdd_unprotect(&shared);
    free(stream.data);
    return 0;
}

struct scalar_codec_context {
    uint32_t type;
    unsigned int size_calls;
    unsigned int write_calls;
    unsigned int read_calls;
};

static int
scalar_leaf_size(void *context, MTBDD leaf, uint64_t *size)
{
    struct scalar_codec_context *codec = context;
    if (mtbdd_leaf_type(leaf) != codec->type || mtbdd_is_nan(leaf)) {
        return SYLVAN_ERR_INVALID;
    }
    codec->size_calls++;
    *size = 8;
    return SYLVAN_OK;
}

static int
scalar_leaf_write(
    void *context, MTBDD leaf, sylvan_framed_writer *stream)
{
    struct scalar_codec_context *codec = context;
    uint8_t bytes[8];
    uint64_t value = mtbdd_leaf_value(leaf);
    for (unsigned int i = 0; i < 8; i++) {
        bytes[i] = (uint8_t)(value >> (8 * i));
    }
    codec->write_calls++;
    return sylvan_framed_writer_append(stream, bytes, sizeof(bytes));
}

static int
scalar_leaf_read(
    void *context, sylvan_framed_reader *stream,
    uint64_t size, MTBDD *result)
{
    struct scalar_codec_context *codec = context;
    if (size != 8) return SYLVAN_ERR_INVALID;

    uint8_t bytes[8];
    int status = sylvan_framed_reader_read(
        stream, bytes, sizeof(bytes));
    if (status != SYLVAN_OK) return status;
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; i++) {
        value |= (uint64_t)bytes[i] << (8 * i);
    }
    codec->read_calls++;
    *result = mtbdd_leaf(codec->type, value);
    return SYLVAN_OK;
}

static int
test_custom_leaf_serialization(void)
{
    uint32_t type = UINT32_MAX;
    const sylvan_mt_type_descriptor descriptor = {
        "sylvan.test.scalar",
        UINT64_C(0xd479c2f5a161054d),
        NULL, NULL, NULL, NULL, NULL, NULL, NULL
    };
    test_assert(sylvan_mt_register_type(
        &type, &descriptor) == SYLVAN_OK);
    uint32_t found = UINT32_MAX;
    test_assert(sylvan_mt_find_type(
        descriptor.name, &found) == SYLVAN_OK && found == type);

    struct scalar_codec_context codec_context = {
        type, 0, 0, 0
    };
    const sylvan_serialization_leaf_codec codec = {
        descriptor.name, 7, &codec_context,
        scalar_leaf_size, scalar_leaf_write, scalar_leaf_read
    };

    struct memory_stream stream = {
        NULL, 0, 0, 0, SIZE_MAX
    };
    sylvan_framed_writer *framed_writer = NULL;
    sylvan_serialization_writer *writer = NULL;
    test_assert(sylvan_framed_writer_create(
        &framed_writer, memory_write, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_writer_create(
        &writer, framed_writer) == SYLVAN_OK);
    test_assert(sylvan_serialization_writer_add_leaf_codec(
        writer, &codec) == SYLVAN_OK);

    MTBDD root = mtbdd_make_node(
        6, mtbdd_leaf(type, UINT64_C(0x123456789abcdef0)),
        mtbdd_nan(type));
    mtbdd_protect(&root);
    test_assert(sylvan_serialization_write_mtbdd(
        writer, root, 201) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_finish(framed_writer) == SYLVAN_OK);
    test_assert(
        codec_context.size_calls == 1 &&
        codec_context.write_calls == 1);

    sylvan_framed_reader *framed_reader = NULL;
    sylvan_serialization_reader *reader = NULL;
    test_assert(sylvan_framed_reader_create(
        &framed_reader, memory_read, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_reader_create(
        &reader, framed_reader, NULL, NULL) == SYLVAN_OK);
    test_assert(sylvan_serialization_reader_add_leaf_codec(
        reader, &codec) == SYLVAN_OK);

    sylvan_serialization_root decoded = {
        SYLVAN_DD_BDD, UINT64_MAX, mtbdd_invalid, mtbdd_invalid, NULL
    };
    int has_root = -1;
    test_assert(sylvan_serialization_reader_next(
        reader, &decoded, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && decoded.family == SYLVAN_DD_MTBDD &&
        decoded.key == 201 && decoded.dd == root &&
        codec_context.read_calls == 1);
    test_assert(sylvan_serialization_reader_next(
        reader, &decoded, &has_root) == SYLVAN_OK);
    test_assert(has_root == 0);

    sylvan_serialization_reader_destroy(reader);
    sylvan_framed_reader_destroy(framed_reader);
    sylvan_serialization_writer_destroy(writer);
    sylvan_framed_writer_destroy(framed_writer);
    mtbdd_unprotect(&root);
    free(stream.data);
    return 0;
}

static int
test_zdd_serialization(void)
{
    BDDSET domain = mtbdd_invalid;
    BDD x = mtbdd_invalid;
    BDD y = mtbdd_invalid;
    BDD function = mtbdd_invalid;
    BDD invalid_domain = mtbdd_invalid;
    ZDD first = zdd_invalid;
    mtbdd_protect(&domain);
    mtbdd_protect(&x);
    mtbdd_protect(&y);
    mtbdd_protect(&function);
    mtbdd_protect(&invalid_domain);
    zdd_protect(&first);

    const uint32_t levels[] = {2, 5, 9};
    test_assert(bdd_set_from_array(
        &domain, levels, 3) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&x, 2) == SYLVAN_OK);
    test_assert(bdd_var_at_level(&y, 9) == SYLVAN_OK);
    test_assert(bdd_xor(&function, x, y) == SYLVAN_OK);
    test_assert(bdd_or(&invalid_domain, x, y) == SYLVAN_OK);
    test_assert(zdd_from_bdd(
        &first, function, domain) == SYLVAN_OK);

    struct memory_stream stream = {
        NULL, 0, 0, 0, SIZE_MAX
    };
    sylvan_framed_writer *framed_writer = NULL;
    sylvan_serialization_writer *writer = NULL;
    test_assert(sylvan_framed_writer_create(
        &framed_writer, memory_write, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_writer_create(
        &writer, framed_writer) == SYLVAN_OK);
    test_assert(sylvan_serialization_write_zdd(
        writer, first, invalid_domain, 300) == SYLVAN_ERR_INVALID);
    test_assert(sylvan_serialization_write_zdd(
        writer, first, domain, 301) == SYLVAN_OK);

    sylvan_framed_reader *framed_reader = NULL;
    sylvan_serialization_reader *reader = NULL;
    test_assert(sylvan_framed_reader_create(
        &framed_reader, memory_read, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_reader_create(
        &reader, framed_reader, NULL, NULL) == SYLVAN_OK);

    sylvan_serialization_root root = {
        SYLVAN_DD_BDD, UINT64_MAX, mtbdd_invalid, mtbdd_invalid, NULL
    };
    int has_root = -1;
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_ZDD &&
        root.key == 301 && root.dd == first && root.domain == domain);

    test_assert(sylvan_serialization_write_zdd(
        writer, zdd_false, domain, 302) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_finish(framed_writer) == SYLVAN_OK);
    sylvan_gc();

    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_ZDD &&
        root.key == 302 && root.dd == zdd_false &&
        root.domain == domain);
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(has_root == 0);

    sylvan_serialization_reader_destroy(reader);
    sylvan_framed_reader_destroy(framed_reader);
    sylvan_serialization_writer_destroy(writer);
    sylvan_framed_writer_destroy(framed_writer);
    zdd_unprotect(&first);
    mtbdd_unprotect(&invalid_domain);
    mtbdd_unprotect(&function);
    mtbdd_unprotect(&y);
    mtbdd_unprotect(&x);
    mtbdd_unprotect(&domain);
    free(stream.data);
    return 0;
}

static int
test_listdd_serialization(void)
{
    LISTDD first = listdd_empty;
    LISTDD second = listdd_empty;
    LISTDD copy = listdd_invalid;
    LISTDD relation = listdd_invalid;
    listdd_relation_layout *layout = NULL;
    listdd_protect(&first);
    listdd_protect(&second);
    listdd_protect(&copy);
    listdd_protect(&relation);

    const uint32_t one[] = {1, 2, 3};
    const uint32_t two[] = {1, 4, 3};
    const uint32_t three[] = {5, 6, 7};
    test_assert(listdd_add(
        &first, first, one, 3) == SYLVAN_OK);
    test_assert(listdd_add(
        &first, first, two, 3) == SYLVAN_OK);
    test_assert(listdd_add(
        &second, first, three, 3) == SYLVAN_OK);
    copy = listdd_make_copy_node(
        listdd_empty_list, listdd_empty);
    const listdd_relation_access accesses[] = {
        LISTDD_RELATION_READ_WRITE,
        LISTDD_RELATION_UNUSED,
        LISTDD_RELATION_WRITE
    };
    const uint32_t read_values[] = {10, 0, 0};
    const uint32_t write_values[] = {10, 0, 20};
    const uint8_t retain_source[] = {1, 0, 0};
    const uint32_t action_label = 7;
    test_assert(listdd_relation_layout_create(
        &layout, accesses, 3, 1) == SYLVAN_OK);
    test_assert(listdd_relation_singleton(
        &relation, layout, read_values, write_values,
        retain_source, 3, &action_label) == SYLVAN_OK);

    struct memory_stream stream = {
        NULL, 0, 0, 0, SIZE_MAX
    };
    sylvan_framed_writer *framed_writer = NULL;
    sylvan_serialization_writer *writer = NULL;
    test_assert(sylvan_framed_writer_create(
        &framed_writer, memory_write, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_writer_create(
        &writer, framed_writer) == SYLVAN_OK);
    test_assert(sylvan_serialization_write_listdd(
        writer, first, 401) == SYLVAN_OK);

    sylvan_framed_reader *framed_reader = NULL;
    sylvan_serialization_reader *reader = NULL;
    test_assert(sylvan_framed_reader_create(
        &framed_reader, memory_read, &stream) == SYLVAN_OK);
    test_assert(sylvan_serialization_reader_create(
        &reader, framed_reader, NULL, NULL) == SYLVAN_OK);

    sylvan_serialization_root root = {
        SYLVAN_DD_BDD, UINT64_MAX, mtbdd_invalid, mtbdd_invalid, NULL
    };
    int has_root = -1;
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_LISTDD &&
        root.key == 401 && root.dd == first &&
        root.domain == mtbdd_invalid &&
        root.listdd_layout == NULL);

    test_assert(sylvan_serialization_write_listdd(
        writer, second, 402) == SYLVAN_OK);
    test_assert(sylvan_serialization_write_listdd(
        writer, copy, 403) == SYLVAN_OK);
    test_assert(sylvan_serialization_write_listdd(
        writer, listdd_empty_list, 404) == SYLVAN_OK);
    test_assert(sylvan_serialization_write_listdd_relation(
        writer, relation, layout, 405) == SYLVAN_OK);
    test_assert(sylvan_framed_writer_finish(framed_writer) == SYLVAN_OK);
    sylvan_gc();

    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_LISTDD &&
        root.key == 402 && root.dd == second);
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_LISTDD &&
        root.key == 403 && root.dd == copy &&
        listdd_is_copy_node((LISTDD)root.dd));
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_LISTDD &&
        root.key == 404 && root.dd == listdd_empty_list);
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    int contains = 0;
    test_assert(
        has_root == 1 && root.family == SYLVAN_DD_LISTDD &&
        root.key == 405 && root.dd == relation &&
        root.listdd_layout != NULL);
    test_assert(listdd_relation_contains(
        &contains, (LISTDD)root.dd, root.listdd_layout,
        read_values, write_values, retain_source, 3,
        &action_label) == SYLVAN_OK);
    test_assert(contains);
    test_assert(sylvan_serialization_reader_next(
        reader, &root, &has_root) == SYLVAN_OK);
    test_assert(has_root == 0);

    sylvan_serialization_reader_destroy(reader);
    sylvan_framed_reader_destroy(framed_reader);
    sylvan_serialization_writer_destroy(writer);
    sylvan_framed_writer_destroy(framed_writer);
    listdd_relation_layout_destroy(layout);
    listdd_unprotect(&relation);
    listdd_unprotect(&copy);
    listdd_unprotect(&second);
    listdd_unprotect(&first);
    free(stream.data);
    return 0;
}

TASK(int, test_dd_serialization)

int
test_dd_serialization_CALL(lace_worker *lace)
{
    (void)lace;
    int result = test_bdd_serialization();
    if (result == 0) result = test_mtbdd_serialization();
    if (result == 0) result = test_custom_leaf_serialization();
    if (result == 0) result = test_zdd_serialization();
    if (result == 0) result = test_listdd_serialization();
    return result;
}

int
main(void)
{
    if (test_roundtrip()) return 1;
    if (test_failures()) return 1;

    lace_start(2, 0, 0);
    sylvan_set_sizes(
        (size_t)1 << 20, (size_t)1 << 20,
        (size_t)1 << 16, (size_t)1 << 16);
    sylvan_init_package();
    mtbdd_init();
    zdd_init();
    listdd_init();
    int result = test_dd_serialization();
    sylvan_quit();
    lace_stop();
    return result;
}
