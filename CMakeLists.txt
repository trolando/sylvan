cmake_minimum_required(VERSION 2.6)
project(sylvan C)
enable_testing()

option(SYLVAN_OPERATION_STATS "Create operation statistics" OFF)
option(SYLVAN_CACHE_STATS "Create cache statistics" OFF)
option(USE_NUMA "Use NUMA" OFF)

include(CheckIncludeFiles)

if(USE_NUMA)
  check_include_files(numa.h HAVE_NUMA)
  if(HAVE_NUMA)
    set (EXTRA_LIBS ${EXTRA_LIBS} numa)
    set (EXTRA_SRC ${EXTRA_SRC} src/numa_tools.c)
    set (NUMA_DEF "USE_NUMA=1")
  else()
    message(send_error "ERROR: Using NUMA but no numa.h found!")
    set (NUMA_DEF "USE_NUMA=0")
  endif()
else()
  set (NUMA_DEF "USE_NUMA=0")
endif()

set (CMAKE_ASM_FLAGS "-g")
set (CMAKE_C_FLAGS "-g -O3 -Wextra -Wall -Werror -D${NUMA_DEF}")

set(sylvan_sources
  src/barrier.h
  src/barrier.c
  src/sha2.h
  src/sha2.c
  src/sylvan.h
  src/sylvan.c
  src/llmsset.c
  src/llmsset.h
  src/lace.c
  src/lace.h
  src/hash24.h
  src/hash16.h
  src/cache.h
  src/refs.h
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

add_executable(mc test/mc.c)
target_link_libraries(mc sylvan)
