cmake_minimum_required(VERSION 2.6)
project(sylvan C)
enable_language(ASM)
enable_testing()

option(SYLVAN_STATS "Create statistics" ON)
option(USE_NUMA "Use NUMA" OFF)

include(CheckIncludeFiles)
if(USE_NUMA)
  check_include_files(numa.h HAVE_NUMA_H)
  if(HAVE_NUMA_H)
    set (EXTRA_LIBS ${EXTRA_LIBS} numa)
    set (EXTRA_SRC ${EXTRA_SRC} src/numa_tools.c)
  else()
    message(send_error "ERROR: Using NUMA but no numa.h found!")
  endif()
else()
  set(HAVE_NUMA_H OFF)
endif()

configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/config.h.cmakein"
  "${CMAKE_CURRENT_BINARY_DIR}/src/config.h"
)

set (CMAKE_ASM_FLAGS "-g")
set (CMAKE_C_FLAGS "-g -O3 -Wall")

set(sylvan_sources
  src/sylvan.h
  src/sylvan.c
  src/llmsset.c
  src/llmsset.h
  src/hash_mul.S
  src/rehash_mul.S
  src/lace.c
  src/lace.h
  ${EXTRA_SRC}
)

set(test_sources
  src/llmsset.c
  src/llmsset.h
  test/main.c
)

add_library(sylvan ${sylvan_sources})
target_link_libraries(sylvan m pthread ${EXTRA_LIBS})

include_directories(src)

add_executable(sylvan_test ${test_sources})
target_link_libraries(sylvan_test sylvan)
add_test(test ./sylvan_test)
 
