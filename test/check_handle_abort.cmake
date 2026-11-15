execute_process(
    COMMAND "${TEST_PROGRAM}" "${HELPER}"
    RESULT_VARIABLE result
    ERROR_VARIABLE diagnostic
)
# The SIGABRT handler exits with 86. A generic nonzero exit is not enough:
# bounds checking must report the bad index and call abort(), even in Release.
if(NOT "${result}" STREQUAL "86" OR
   NOT diagnostic MATCHES "decision diagram handle index [0-9]+ is outside the nodes data range \\(size [0-9]+\\)")
    message(FATAL_ERROR "${HELPER}: expected a handle diagnostic and SIGABRT, got ${result}: ${diagnostic}")
endif()
