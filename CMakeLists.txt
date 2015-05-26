# CMake build script for Sylvan
#
# Building:
# > mkdir build && cd build
# > cmake ..
# > make
#
# Install:
# > cd build
# > cmake -DCMAKE_INSTALL_PREFIX=<path> ..
# > make install

cmake_minimum_required(VERSION 2.6)
project(sylvan C CXX)
enable_testing()

option(SYLVAN_OPERATION_STATS "Create operation statistics" OFF)
option(SYLVAN_CACHE_STATS "Create cache statistics" OFF)
option(SYLVAN_BUILD_EXAMPLES "Build example tools" OFF)

include(CheckIncludeFiles)

check_include_files(hwloc.h HAVE_HWLOC)
if(HAVE_HWLOC)
    set(EXTRA_LIBS ${EXTRA_LIBS} hwloc)
    set(HWLOC_DEF "USE_HWLOC=1")
else()
    set(HWLOC_DEF "USE_HWLOC=0")
endif()

set(CMAKE_C_FLAGS "-g -O3 -Wextra -Wall -Werror -fno-strict-aliasing -D${HWLOC_DEF}")

set(sylvan_sources
    src/barrier.h
    src/barrier.c
    src/sha2.h
    src/sha2.c
    src/sylvan.h
    src/sylvan_config.h
    src/sylvan_bdd.h
    src/sylvan_bdd.c
    src/sylvan_ldd.h
    src/sylvan_ldd.c
    src/llmsset.c
    src/llmsset.h
    src/lace.c
    src/lace.h
    src/getrss.h
    src/getrss.c
    src/sylvan_common.h
    src/sylvan_common.c
    src/cache.h
    src/refs.h
    src/refs.c
    ${EXTRA_SRC}
)

include_directories(src)

add_library(sylvan ${sylvan_sources})
target_link_libraries(sylvan m pthread ${EXTRA_LIBS})

install(TARGETS sylvan DESTINATION "lib")
install(FILES src/sylvan.h src/sylvan_config.h src/lace.h src/atomics.h src/llmsset.h src/cache.h src/sylvan_bdd.h src/sylvan_ldd.h DESTINATION "include")

add_executable(sylvan_test test/main.c)
target_link_libraries(sylvan_test sylvan)
add_test(sylvan_test sylvan_test)

add_executable(test_cxx test/test_cxx.cpp)
target_link_libraries(test_cxx sylvan)

if(SYLVAN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
