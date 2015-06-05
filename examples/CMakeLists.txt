cmake_minimum_required(VERSION 2.6)
project(sylvan C)

include(CheckIncludeFiles)
check_include_files("gperftools/profiler.h" HAVE_PROFILER)

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    # add argp library for OSX
    set(ARGP_LIBS "argp")
endif()

set(CMAKE_C_FLAGS "-g -O3 -Wextra -Wall -Werror -fno-strict-aliasing -D${HWLOC_DEF} -std=gnu11")

add_executable(mc mc.c)
target_link_libraries(mc sylvan ${ARGP_LIBS})
if(HAVE_PROFILER)
    set_target_properties(mc PROPERTIES COMPILE_DEFINITIONS "HAVE_PROFILER")
    target_link_libraries(mc profiler)
endif()

add_executable(lddmc lddmc.c)
target_link_libraries(lddmc sylvan ${ARGP_LIBS})
if(HAVE_PROFILER)
    set_target_properties(lddmc PROPERTIES COMPILE_DEFINITIONS "HAVE_PROFILER")
    target_link_libraries(lddmc profiler)
endif()
