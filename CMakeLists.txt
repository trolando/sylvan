cmake_minimum_required(VERSION 2.8)
project(sylvan C CXX)

set(CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake/")

set(CMAKE_C_FLAGS "-O3 -Wextra -Wall -Werror -fno-strict-aliasing -std=gnu11")
set(CMAKE_CXX_FLAGS "-O3 -Wextra -Wall -Werror -fno-strict-aliasing -Wno-deprecated-register -std=gnu++11")

option(WITH_COVERAGE "Add generation of test coverage" OFF)
if(WITH_COVERAGE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g -coverage")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O0 -g -coverage")

    find_program(GCOV_PATH gcov)
    find_program(LCOV_PATH lcov)
    find_program(GENHTML_PATH genhtml)

    add_custom_target(coverage
        # Cleanup lcov
        ${LCOV_PATH} --directory . --zerocounters
        # Run tests
        COMMAND make test
        # Capture counters
        COMMAND ${LCOV_PATH} --gcov-tool ${GCOV_PATH} --directory . --capture --output-file coverage.info
        COMMAND ${LCOV_PATH} --remove coverage.info 'test/*' '/usr/*' 'examples/*' 'src/sylvan_mtbdd*' 'src/lace*' 'src/sylvan_ldd*' 'src/avl.h' 'src/sha2.c' --output-file coverage.info.cleaned
        COMMAND ${GENHTML_PATH} -o coverage coverage.info.cleaned
        COMMAND ${CMAKE_COMMAND} -E remove coverage.info coverage.info.cleaned
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()

include_directories(${CMAKE_SOURCE_DIR}/src)

enable_testing()
add_subdirectory(src)
add_subdirectory(test)

option(SYLVAN_BUILD_EXAMPLES "Build example tools" ON)
if(SYLVAN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Multi-core implementation of decision diagrams")
set(CPACK_PACKAGE_VENDOR "Tom van Dijk")
set(CPACK_PACKAGE_CONTACT "Tom van Dijk <tom@tvandijk.nl>")
set(CPACK_PACKAGE_DESCRIPTION_FILE "${CMAKE_SOURCE_DIR}/README.md")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_VERSION "1.0.0")
set(CPACK_PACKAGE_VERSION_MAJOR "1")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "0")
include(CPack)
