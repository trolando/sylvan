# Runs an example model checker on a model without transition groups and
# requires both a successful exit and the two initial states as the result.
set(options -w 1 -s "${STRATEGY}")
if(MERGE)
    list(APPEND options --merge-relations)
endif()
execute_process(
    COMMAND "${PROGRAM}" ${options} "${MODEL}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT "${result}" STREQUAL "0" OR NOT "${output}${error}" MATCHES "Final states: 2 states")
    message(FATAL_ERROR "${PROGRAM} ${options} ${MODEL}: exit ${result}\n${output}\n${error}")
endif()
