cmake_minimum_required(VERSION 2.8)
project(sylvan C CXX)

add_library(sylvan
    avl.h
    lace.h
    lace.c
    llmsset.c
    llmsset.h
    sha2.h
    sha2.c
    sylvan.h
    sylvan_bdd.h
    sylvan_bdd.c
    sylvan_cache.h
    sylvan_cache.c
    sylvan_config.h
    sylvan_common.h
    sylvan_common.c
    sylvan_gmp.h
    sylvan_gmp.c
    sylvan_int.h
    sylvan_ldd.h
    sylvan_ldd.c
    sylvan_mtbdd.h
    sylvan_mtbdd.c
    sylvan_mtbdd_int.h
    sylvan_obj.hpp
    sylvan_obj.cpp
    sylvan_refs.h
    sylvan_refs.c
    sylvan_sl.h
    sylvan_sl.c
    sylvan_stats.h
    sylvan_stats.c
    tls.h
)

target_link_libraries(sylvan m pthread gmp)

if(UNIX AND NOT APPLE)
    target_link_libraries(sylvan rt)
endif()

find_package(GMP REQUIRED)

option(USE_HWLOC "Use HWLOC library if available" ON)

if(USE_HWLOC)
    include(CheckIncludeFiles)
    check_include_files(hwloc.h HAVE_HWLOC_H)
    if(HAVE_HWLOC_H)
        set_target_properties(sylvan PROPERTIES COMPILE_DEFINITIONS "USE_HWLOC=1")
        target_link_libraries(sylvan hwloc)
    endif()
endif()

option(SYLVAN_STATS "Collect statistics" OFF)
if(SYLVAN_STATS)
    set_target_properties(sylvan PROPERTIES COMPILE_DEFINITIONS "SYLVAN_STATS")
endif()

install(TARGETS
    sylvan
    DESTINATION "lib")

install(FILES 
    lace.h
    llmsset.h
    sylvan.h
    sylvan_bdd.h
    sylvan_cache.h
    sylvan_common.h
    sylvan_config.h
    sylvan_gmp.h
    sylvan_int.h
    sylvan_ldd.h
    sylvan_mtbdd.h
    sylvan_mtbdd_int.h
    sylvan_obj.hpp
    sylvan_stats.h
    tls.h
    DESTINATION "include")
