cmake_minimum_required(VERSION 2.6)
project(sylvan C CXX)

include_directories(src)

add_subdirectory(src)

option(SYLVAN_BUILD_TEST "Build test programs" ON)

if(SYLVAN_BUILD_TEST)
    add_subdirectory(test)
endif()

option(SYLVAN_BUILD_EXAMPLES "Build example tools" ON)

if(SYLVAN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
