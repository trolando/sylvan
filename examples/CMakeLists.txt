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
    target_link_libraries(ldd2bdd argp)
    target_link_libraries(nqueens argp)
endif()


