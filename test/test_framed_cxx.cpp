#ifdef SYLVAN_TEST_SERIALIZE_FIRST
#include <sylvan_serialize.h>
#include <sylvan.h>
#else
#include <sylvan.h>
#include <sylvan_serialize.h>
#endif
#include <type_traits>
#include "test_assert.h"

static_assert(std::is_same<sylvan_frame, sylvan::sylvan_frame>::value,
    "Both headers must expose the same frame type");
static_assert(std::is_same<sylvan_framed_writer, sylvan::sylvan_framed_writer>::value,
    "Both headers must expose the same writer type");
static_assert(std::is_same<sylvan_framed_reader, sylvan::sylvan_framed_reader>::value,
    "Both headers must expose the same reader type");
static_assert(std::is_same<sylvan_stream_status, sylvan::sylvan_stream_status>::value,
    "Both headers must expose the same status type");
static_assert(std::is_same<sylvan_stream_write_cb, sylvan::sylvan_stream_write_cb>::value,
    "Both headers must expose the same write callback type");
static_assert(std::is_same<sylvan_stream_read_cb, sylvan::sylvan_stream_read_cb>::value,
    "Both headers must expose the same read callback type");

int
main()
{
    /* Mix the global API and namespace aliases in both include orders,
     * without Lace or Sylvan initialization. */
    FILE *file = tmpfile();
    test_assert(file != nullptr);
    sylvan::sylvan_framed_writer *writer = nullptr;
    test_assert(sylvan_framed_writer_create(
        &writer, sylvan_stream_write_file, file) == SYLVAN_STREAM_OK);
    test_assert(sylvan::sylvan_framed_writer_write(writer, 17, "abc", 3) == sylvan::SYLVAN_STREAM_OK);
    test_assert(sylvan::sylvan_framed_writer_finish(writer) == sylvan::SYLVAN_STREAM_OK);
    sylvan::sylvan_framed_writer_destroy(writer);
    test_assert(fflush(file) == 0);
    test_assert(fseek(file, 0, SEEK_SET) == 0);
    sylvan_framed_reader *reader = nullptr;
    test_assert(sylvan::sylvan_framed_reader_create(
        &reader, sylvan::sylvan_stream_read_file, file) == sylvan::SYLVAN_STREAM_OK);
    sylvan_frame frame;
    int has_frame;
    test_assert(sylvan::sylvan_framed_reader_next(reader, &frame, &has_frame) == sylvan::SYLVAN_STREAM_OK);
    test_assert(has_frame && frame.type == 17 && frame.payload_size == 3);
    char bytes[3];
    test_assert(sylvan::sylvan_framed_reader_read(reader, bytes, 3) == sylvan::SYLVAN_STREAM_OK);
    test_assert(bytes[0] == 'a' && bytes[1] == 'b' && bytes[2] == 'c');
    test_assert(sylvan::sylvan_framed_reader_next(reader, &frame, &has_frame) == sylvan::SYLVAN_STREAM_OK);
    test_assert(!has_frame);
    sylvan::sylvan_framed_reader_destroy(reader);
    test_assert(fclose(file) == 0);
    return 0;
}
