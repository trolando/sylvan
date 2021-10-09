macro(set_compilation_settings NAME)
    set_target_properties(${NAME} PROPERTIES COMPILE_OPTIONS "-Wno-unused")
    target_compile_features(${NAME} PRIVATE c_std_11 cxx_std_11)
    target_compile_options(${NAME} PRIVATE -Wall -Wextra -Werror -Wno-deprecated)  
endmacro(set_compilation_settings)

macro(add_example NAME SOURCE)
    add_executable(${NAME} ${SOURCE})
    target_include_directories(${NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${NAME} PRIVATE Sylvan::Sylvan)
    set_compilation_settings(${NAME})
endmacro(add_example)

add_example(bddmc bddmc.c)
target_sources(bddmc PRIVATE getrss.c getrss.h)

add_example(lddmc lddmc.c)
target_sources(lddmc PRIVATE getrss.c getrss.h)

add_example(ldd2bdd ldd2bdd.c)
target_sources(ldd2bdd PRIVATE getrss.c getrss.h)

add_example(nqueens nqueens.c)

add_example(simple simple.cpp)

# Check if we have Meddly
find_library(MEDDLY_FOUND meddly)
if(MEDDLY_FOUND)
    add_executable(ldd2meddly ldd2meddly.cpp)
    target_link_libraries(ldd2meddly PRIVATE Sylvan::Sylvan stdc++ meddly)
    set_compilation_settings(ldd2meddly)

    add_executable(medmc medmc.cpp)
    target_link_libraries(medmc PRIVATE stdc++ meddly)
    set_compilation_settings(medmc)
endif()

# Add profiler to some of the examples, if available
# In the future, maybe not use gperftools anymore, just Linux perf...
include(CheckIncludeFiles)
check_include_files("gperftools/profiler.h" HAVE_PROFILER)

if(HAVE_PROFILER)
    macro(set_profiler_flag NAME)
        set_target_properties(${NAME} PROPERTIES COMPILE_DEFINITIONS "HAVE_PROFILER")
        target_link_libraries(${NAME} PRIVATE profiler)
    endmacro(set_profiler_flag)

    set_profiler_flag(bddmc)
    set_profiler_flag(lddmc)
    set_profiler_flag(nqueens)
endif()

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    # add argp library for OSX
    find_package(Argp REQUIRED)
    include_directories(${ARGP_INCLUDE_PATH})
    target_link_libraries(bddmc PRIVATE ${ARGP_LIBRARIES})
    target_link_libraries(lddmc PRIVATE ${ARGP_LIBRARIES})
    if(MEDDLY_FOUND)
        target_link_libraries(medmc PRIVATE ${ARGP_LIBRARIES})
        target_link_libraries(ldd2meddly PRIVATE ${ARGP_LIBRARIES})
    endif()
    target_link_libraries(ldd2bdd PRIVATE ${ARGP_LIBRARIES})
    target_link_libraries(nqueens PRIVATE ${ARGP_LIBRARIES})
endif()


