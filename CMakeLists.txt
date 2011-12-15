cmake_minimum_required(VERSION 2.6)
project(sylvan C)

enable_testing()

option(SYLVAN_SHARED "Build shared lib" ON)
option(SYLVAN_STATIC "Build static lib" ON)

set(sylvan_hdrs
  src/sylvan.h
  src/runtime.h
)

set(sylvan_sources
  ${sylvan_hdrs}
  src/sylvan.c
  src/fast_hash.c
  src/llgcset.c
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

add_executable(test ${test_sources})
target_link_libraries(test sylvan)
#add_test(test ./test)
 
