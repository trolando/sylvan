execute_process(
    COMMAND "${PROGRAM}" "${NODE}" "${MODE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
    TIMEOUT 20
)

# Builds with SYLVAN_GC_DIAGNOSTICS print a "[sylvan-gc] ..." line per collection
string(REGEX REPLACE "\\[sylvan-gc\\][^\n]*\n" "" error "${error}")

set(expected_result 1)
if(MODE STREQUAL "handler")
    set(expected_result 30)
endif()
if(NOT "${result}" STREQUAL "${expected_result}" OR NOT "${output}" STREQUAL "")
    message(FATAL_ERROR "Expected exit ${expected_result} and empty stdout; got ${result}\n${output}${error}")
endif()

if(NODE MATCHES "^ldd")
    set(kind MDD)
    set(label MDD)
elseif(NODE MATCHES "^zdd")
    set(kind ZDD)
    set(label BDD) # Preserve the historical ZDD diagnostic, byte for byte.
else()
    set(kind BDD)
    set(label BDD)
endif()

if(MODE STREQUAL "handler" OR MODE STREQUAL "return")
    if(NOT error MATCHES "^handler: ${kind}, ([0-9]+) of ([0-9]+) buckets filled\n")
        message(FATAL_ERROR "Missing callback output for ${kind}: ${error}")
    endif()
    set(filled "${CMAKE_MATCH_1}")
    set(total "${CMAKE_MATCH_2}")
    set(expected_error "handler: ${kind}, ${filled} of ${total} buckets filled\n")
    if(MODE STREQUAL "return")
        string(APPEND expected_error "${label} Unique table full, ${filled} of ${total} buckets filled!\n")
    endif()
else()
    if(NOT error MATCHES "^${label} Unique table full, ([0-9]+) of ([0-9]+) buckets filled!\n$")
        message(FATAL_ERROR "Missing original table-full message: ${error}")
    endif()
    set(filled "${CMAKE_MATCH_1}")
    set(total "${CMAKE_MATCH_2}")
    set(expected_error "${label} Unique table full, ${filled} of ${total} buckets filled!\n")
endif()

if(NOT error STREQUAL expected_error)
    message(FATAL_ERROR "Unexpected stderr: ${error}")
endif()
if(NOT total EQUAL 4096 OR filled LESS_EQUAL 2048 OR filled GREATER total)
    message(FATAL_ERROR "Implausible bucket counts: ${filled} of ${total}")
endif()
