cmake_minimum_required(VERSION 2.6)
project(sylvan C)

enable_testing()

if (DEFINED SYLVAN_NO_CACHE)
  option(SYLVAN_NO_CACHE "No Operations Cache" ${SYLVAN_NO_CACHE})
else()
  option(SYLVAN_NO_CACHE "No Operations Cache" OFF)
endif()

if(SYLVAN_NO_CACHE)
  add_definitions(-DCACHE=0)
endif()

set(sylvan_hdrs
  src/sylvan.h
  src/runtime.h
)

set(sylvan_sources
  ${sylvan_hdrs}
  src/sylvan.c
  src/fast_hash.c
  src/llgcset.c
  src/llvector.c
  src/runtime.c
)

set(test_sources
  test/main.c
)

add_definitions(--coverage)

add_library(sylvan SHARED ${sylvan_sources})
target_link_libraries(sylvan m)

add_library(sylvan_static ${sylvan_sources})
target_link_libraries(sylvan_static m)

include_directories(src)

add_executable(sylvan_test ${test_sources})
target_link_libraries(sylvan_test sylvan_static rt gcov)
add_test(test ./sylvan_test)
 
