#include <sylvan/internal.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <new>
#include <string>
#include <vector>

using namespace sylvan;

namespace {

constexpr uint32_t layout_frame =
    SYLVAN_SERIALIZATION_APPLICATION + UINT32_C(0x10);
constexpr uint32_t tuples_frame =
    SYLVAN_SERIALIZATION_APPLICATION + UINT32_C(0x11);

struct MemoryStream {
    std::vector<uint8_t> data;
    size_t read_offset = 0;
};

int
memory_write(void *context, const void *data, size_t size)
{
    try {
        auto& stream = *static_cast<MemoryStream*>(context);
        const auto *bytes = static_cast<const uint8_t*>(data);
        stream.data.insert(stream.data.end(), bytes, bytes + size);
        return SYLVAN_OK;
    } catch (const std::bad_alloc&) {
        return SYLVAN_ERR_OOM;
    } catch (...) {
        return SYLVAN_ERR_IO;
    }
}

int
memory_read(void *context, void *data, size_t size)
{
    auto& stream = *static_cast<MemoryStream*>(context);
    if (size > stream.data.size() - stream.read_offset) {
        return SYLVAN_ERR_IO;
    }
    std::memcpy(data, stream.data.data() + stream.read_offset, size);
    stream.read_offset += size;
    return SYLVAN_OK;
}

void
store_u32(uint8_t *destination, uint32_t value)
{
    for (unsigned int i = 0; i < 4; i++) {
        destination[i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

uint32_t
load_u32(const uint8_t *source)
{
    uint32_t result = 0;
    for (unsigned int i = 0; i < 4; i++) {
        result |= static_cast<uint32_t>(source[i]) << (8 * i);
    }
    return result;
}

#define REQUIRE(condition)                                                   \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "requirement failed at line " << __LINE__ << "\n"; \
            return SYLVAN_ERR_INVALID;                                       \
        }                                                                    \
    } while (0)

TASK(int, run_listdd_consumer_canary)

int
run_listdd_consumer_canary_CALL(lace_worker *lace)
{
    const listdd_projection_kind projection_positions[] = {
        LISTDD_KEEP_POSITION,
        LISTDD_PROJECT_POSITION
    };
    const listdd_relation_access relation_positions[] = {
        LISTDD_RELATION_READ_WRITE,
        LISTDD_RELATION_READ_WRITE
    };

    listdd_projection *projection = nullptr;
    listdd_projection *raw_projection = nullptr;
    listdd_relation_layout *layout = nullptr;
    listdd_relation_layout *raw_layout = nullptr;
    REQUIRE(listdd_projection_create(
        &projection, projection_positions, 2) == SYLVAN_OK);
    REQUIRE(listdd_projection_create_raw(
        &raw_projection, listdd_projection_raw(projection)) == SYLVAN_OK);
    REQUIRE(listdd_relation_layout_create(
        &layout, relation_positions, 2, 0) == SYLVAN_OK);
    REQUIRE(listdd_relation_layout_create_raw(
        &raw_layout, listdd_relation_layout_raw(layout)) == SYLVAN_OK);

    LISTDD states = listdd_invalid;
    LISTDD relation = listdd_invalid;
    LISTDD successors = listdd_invalid;
    LISTDD raw_successors = listdd_invalid;
    LISTDD predecessors = listdd_invalid;
    LISTDD projected = listdd_invalid;
    LISTDD raw_projected = listdd_invalid;
    LISTDD roundtrip = listdd_invalid;
    LISTDD *protected_values[] = {
        &states, &relation, &successors, &raw_successors,
        &predecessors, &projected, &raw_projected, &roundtrip
    };
    for (LISTDD *value : protected_values) listdd_refs_pushptr(value);

    const uint32_t first_state[] = {1, 0};
    const uint32_t second_state[] = {2, 0};
    REQUIRE(listdd_singleton(
        &states, first_state, 2) == SYLVAN_OK);
    REQUIRE(listdd_add(
        &states, states, second_state, 2) == SYLVAN_OK);

    const uint32_t read_values[] = {0, 0};
    const uint32_t write_values[] = {0, 1};
    const uint8_t retain_source[] = {1, 0};
    REQUIRE(listdd_relation_singleton(
        &relation, layout, read_values, write_values,
        retain_source, 2, nullptr) == SYLVAN_OK);
    REQUIRE(listdd_is_copy_node(relation));
    REQUIRE(listdd_is_copy_node(listdd_follow_copy(relation)));

    int contains = 0;
    REQUIRE(listdd_relation_contains(
        &contains, relation, layout, read_values, write_values,
        retain_source, 2, nullptr) == SYLVAN_OK);
    REQUIRE(contains == 1);

    sylvan_gc_CALL(lace);

    REQUIRE(listdd_rel_next_CALL(
        lace, &successors, states, relation, layout) == SYLVAN_OK);
    REQUIRE(listdd_rel_next_raw_CALL(
        lace, &raw_successors, states, relation,
        listdd_relation_layout_raw(raw_layout)) == SYLVAN_OK);
    REQUIRE(raw_successors == successors);
    const uint32_t first_successor[] = {1, 1};
    const uint32_t second_successor[] = {2, 1};
    REQUIRE(listdd_contains(successors, first_successor, 2));
    REQUIRE(listdd_contains(successors, second_successor, 2));

    REQUIRE(listdd_rel_prev_CALL(
        lace, &predecessors, successors, relation, layout, states) ==
        SYLVAN_OK);
    REQUIRE(predecessors == states);

    REQUIRE(listdd_project_CALL(
        lace, &projected, successors, projection) == SYLVAN_OK);
    REQUIRE(listdd_project_raw_CALL(
        lace, &raw_projected, successors,
        listdd_projection_raw(raw_projection)) == SYLVAN_OK);
    REQUIRE(raw_projected == projected);
    const uint32_t first_projected[] = {1};
    const uint32_t second_projected[] = {2};
    REQUIRE(listdd_contains(projected, first_projected, 1));
    REQUIRE(listdd_contains(projected, second_projected, 1));

    MemoryStream stream;
    sylvan_framed_writer *writer = nullptr;
    REQUIRE(sylvan_framed_writer_create(
        &writer, memory_write, &stream) == SYLVAN_OK);

    const uint8_t layout_payload[] = {
        2,
        static_cast<uint8_t>(LISTDD_RELATION_READ_WRITE),
        static_cast<uint8_t>(LISTDD_RELATION_READ_WRITE),
        0
    };
    REQUIRE(sylvan_framed_writer_begin(
        writer, layout_frame, sizeof(layout_payload)) == SYLVAN_OK);
    REQUIRE(sylvan_framed_writer_append(
        writer, layout_payload, 2) == SYLVAN_OK);
    REQUIRE(sylvan_framed_writer_append(
        writer, layout_payload + 2, 2) == SYLVAN_OK);

    /*
     * Consume the first complete application frame before the writer has
     * produced the tuple frame or end marker.
     */
    sylvan_framed_reader *reader = nullptr;
    REQUIRE(sylvan_framed_reader_create(
        &reader, memory_read, &stream) == SYLVAN_OK);
    sylvan_frame frame{};
    int has_frame = 0;
    REQUIRE(sylvan_framed_reader_next(
        reader, &frame, &has_frame) == SYLVAN_OK);
    REQUIRE(has_frame == 1 && frame.type == layout_frame &&
            frame.payload_size == sizeof(layout_payload));
    uint8_t decoded_layout[sizeof(layout_payload)]{};
    REQUIRE(sylvan_framed_reader_read(
        reader, decoded_layout, sizeof(decoded_layout)) == SYLVAN_OK);
    REQUIRE(std::memcmp(
        decoded_layout, layout_payload, sizeof(layout_payload)) == 0);

    uint32_t tuples[4]{};
    sylvan_iterator *iterator = nullptr;
    REQUIRE(listdd_iterator_create(
        &iterator, successors) == SYLVAN_OK);
    for (size_t i = 0; i < 2; i++) {
        int has_item = 0;
        REQUIRE(listdd_iterator_next(
            iterator, tuples + 2 * i, 2, &has_item) == SYLVAN_OK);
        REQUIRE(has_item == 1);
    }
    int has_item = 1;
    uint32_t unused[2]{};
    REQUIRE(listdd_iterator_next(
        iterator, unused, 2, &has_item) == SYLVAN_OK);
    REQUIRE(has_item == 0);
    sylvan_iterator_destroy(iterator);

    uint8_t tuple_payload[24]{};
    store_u32(tuple_payload, 2);
    store_u32(tuple_payload + 4, 2);
    for (size_t i = 0; i < 4; i++) {
        store_u32(tuple_payload + 8 + 4 * i, tuples[i]);
    }
    REQUIRE(sylvan_framed_writer_begin(
        writer, tuples_frame, sizeof(tuple_payload)) == SYLVAN_OK);
    REQUIRE(sylvan_framed_writer_append(
        writer, tuple_payload, 8) == SYLVAN_OK);
    REQUIRE(sylvan_framed_writer_append(
        writer, tuple_payload + 8, 16) == SYLVAN_OK);
    REQUIRE(sylvan_framed_writer_finish(writer) == SYLVAN_OK);

    REQUIRE(sylvan_framed_reader_next(
        reader, &frame, &has_frame) == SYLVAN_OK);
    REQUIRE(has_frame == 1 && frame.type == tuples_frame &&
            frame.payload_size == sizeof(tuple_payload));
    uint8_t decoded_tuples[sizeof(tuple_payload)]{};
    REQUIRE(sylvan_framed_reader_read(
        reader, decoded_tuples, sizeof(decoded_tuples)) == SYLVAN_OK);
    REQUIRE(load_u32(decoded_tuples) == 2);
    REQUIRE(load_u32(decoded_tuples + 4) == 2);
    roundtrip = listdd_empty;
    for (size_t row = 0; row < 2; row++) {
        uint32_t tuple[2] = {
            load_u32(decoded_tuples + 8 + 8 * row),
            load_u32(decoded_tuples + 12 + 8 * row)
        };
        REQUIRE(listdd_add(
            &roundtrip, roundtrip, tuple, 2) == SYLVAN_OK);
    }
    REQUIRE(roundtrip == successors);
    REQUIRE(sylvan_framed_reader_next(
        reader, &frame, &has_frame) == SYLVAN_OK);
    REQUIRE(has_frame == 0);

    sylvan_framed_reader_destroy(reader);
    sylvan_framed_writer_destroy(writer);
    listdd_projection_destroy(raw_projection);
    listdd_projection_destroy(projection);
    listdd_relation_layout_destroy(raw_layout);
    listdd_relation_layout_destroy(layout);

    sylvan_gc_CALL(lace);
    REQUIRE(roundtrip == successors);

    listdd_refs_popptr(
        sizeof(protected_values) / sizeof(protected_values[0]));
    return SYLVAN_OK;
}

} // namespace

int
main(int argc, char **argv)
{
    const int workers = argc == 2 ? std::stoi(argv[1]) : 1;
    lace_start(static_cast<unsigned int>(workers), 0, 0);
    sylvan_set_sizes(
        static_cast<size_t>(1) << 14,
        static_cast<size_t>(1) << 14,
        static_cast<size_t>(1) << 12,
        static_cast<size_t>(1) << 12);
    sylvan_init_package();
    listdd_init();

    const int status = run_listdd_consumer_canary();

    sylvan_quit();
    lace_stop();
    return status == SYLVAN_OK ? 0 : 1;
}
