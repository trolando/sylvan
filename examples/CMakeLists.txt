macro(add_c_example NAME SOURCE)
    add_executable(${NAME} ${SOURCE})
    target_include_directories(${NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR})
    target_link_libraries(${NAME} PRIVATE sylvan::sylvan)
    target_compile_features(${NAME} PRIVATE c_std_11)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${NAME} PRIVATE -Wall -Wextra -Wno-unused)
    endif()
endmacro()

add_c_example(bddmc bddmc.c)
target_sources(bddmc PRIVATE getrss.c getrss.h)

add_c_example(lddmc lddmc.c)
target_sources(lddmc PRIVATE getrss.c getrss.h)

add_c_example(ldd2bdd ldd2bdd.c)
target_sources(ldd2bdd PRIVATE getrss.c getrss.h)

add_c_example(nqueens nqueens.c)

if(SYLVAN_BUILD_CPP)
    add_executable(simple simple.cpp)
    target_link_libraries(simple PRIVATE sylvan::sylvan)
    target_compile_features(simple PRIVATE cxx_std_11)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(simple PRIVATE -Wall -Wextra -Wno-unused -Wno-deprecated)
    endif()
endif()