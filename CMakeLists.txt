cmake_minimum_required(VERSION 2.6)
project(sylvan C)
enable_language(ASM)
enable_testing()

option(SYLVAN_NOCACHE "No Operations Cache" OFF)
option(SYLVAN_STATS "Create statistics" ON)
option(USE_NUMA "Use NUMA" OFF)
option(SET_AFFINITY "Set affinity of workers" OFF)

include(CheckIncludeFiles)
if(USE_NUMA)
  check_include_files(numa.h HAVE_NUMA_H)
  if(HAVE_NUMA_H)
    set (EXTRA_LIBS ${EXTRA_LIBS} numa)
  endif()
else()
  set(HAVE_NUMA_H OFF)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/config.h.cmakein"
  "${CMAKE_CURRENT_BINARY_DIR}/src/config.h"
)

add_definitions(-g)
add_definitions(-O2)

set(sylvan_hdrs
  src/sylvan.h
)

set(sylvan_sources
  ${sylvan_hdrs}
  src/sylvan.c
  src/llgcset.c
  src/hash_mul.s
  src/rehash_mul.s
  src/llcache.c
  src/llsimplecache.c
  src/rng_hash_128.s
)

set(test_sources
  test/main.c
)

add_library(sylvan SHARED ${sylvan_sources})
target_link_libraries(sylvan m ${EXTRA_LIBS})

add_library(sylvan_static ${sylvan_sources})
target_link_libraries(sylvan_static m ${EXTRA_LIBS})

include_directories(src)

add_executable(sylvan_test ${test_sources})
target_link_libraries(sylvan_test sylvan_static rt)
add_test(test ./sylvan_test)
 
