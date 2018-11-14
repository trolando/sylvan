include_directories(.)

add_executable(bddmc bddmc.c getrss.h getrss.c)
target_link_libraries(bddmc sylvan)

add_executable(lddmc lddmc.c getrss.h getrss.c)
target_link_libraries(lddmc sylvan)

add_executable(ldd2bdd ldd2bdd.c getrss.c)
target_link_libraries(ldd2bdd sylvan)

add_executable(nqueens nqueens.c)
target_link_libraries(nqueens sylvan)

add_executable(simple simple.cpp)
target_link_libraries(simple sylvan stdc++)

# Check if we have Meddly
find_library(MEDDLY_FOUND meddly)
if(MEDDLY_FOUND)
    add_executable(ldd2meddly ldd2meddly.cpp)
    target_link_libraries(ldd2meddly sylvan stdc++ meddly)
    set_target_properties(ldd2meddly PROPERTIES COMPILE_OPTIONS "-Wno-unused")

    add_executable(medmc medmc.cpp)
    target_link_libraries(medmc stdc++ meddly)
    set_target_properties(medmc PROPERTIES COMPILE_OPTIONS "-Wno-unused")
endif()

include(CheckIncludeFiles)
check_include_files("gperftools/profiler.h" HAVE_PROFILER)

if(HAVE_PROFILER)
    set_target_properties(bddmc PROPERTIES COMPILE_DEFINITIONS "HAVE_PROFILER")
    target_link_libraries(bddmc profiler)

    set_target_properties(lddmc PROPERTIES COMPILE_DEFINITIONS "HAVE_PROFILER")
    target_link_libraries(lddmc profiler)

    set_target_properties(nqueens PROPERTIES COMPILE_DEFINITIONS "HAVE_PROFILER")
    target_link_libraries(nqueens profiler)
endif()

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    # add argp library for OSX
    target_link_libraries(bddmc argp)
    target_link_libraries(lddmc argp)
    if(MEDDLY_FOUND)
        target_link_libraries(medmc argp)
        target_link_libraries(ldd2meddly argp)
    endif()
    target_link_libraries(ldd2bdd argp)
    target_link_libraries(nqueens argp)
endif()


