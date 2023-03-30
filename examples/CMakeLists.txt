macro(set_compilation_settings NAME)
    set_target_properties(${NAME} PROPERTIES COMPILE_OPTIONS "-Wno-unused")
    target_include_directories(${NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(${NAME} PRIVATE sylvan::sylvan)
    target_compile_features(${NAME} PRIVATE c_std_11 cxx_std_11)
    target_compile_options(${NAME} PRIVATE -Wall -Wextra -Werror -Wno-deprecated)
endmacro(set_compilation_settings)

macro(add_example NAME SOURCE)
    add_executable(${NAME} ${SOURCE})
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
    find_path(MEDDLY_INCLUDE_DIR "meddly.h")

    add_example(ldd2meddly ldd2meddly.cpp)
    target_link_libraries(ldd2meddly PRIVATE stdc++ ${MEDDLY_FOUND})
    target_include_directories(ldd2meddly PRIVATE ${MEDDLY_INCLUDE_DIR})

    add_example(medmc medmc.cpp)
    target_link_libraries(medmc PRIVATE stdc++ ${MEDDLY_FOUND})
    target_include_directories(medmc PRIVATE ${MEDDLY_INCLUDE_DIR})
endif()

