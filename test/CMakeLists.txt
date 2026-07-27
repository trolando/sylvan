function(sylvan_test_warnings target)
    target_compile_options(${target} PRIVATE
        $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall -Wextra -Werror -Wno-deprecated>
        $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall -Wextra -Werror -Wno-deprecated>
        $<$<COMPILE_LANG_AND_ID:C,MSVC>:/W4>
        $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4>
    )
endfunction()

add_executable(test_basic)
target_sources(test_basic PRIVATE test_basic.c)
target_link_libraries(test_basic PRIVATE sylvan::sylvan)
target_compile_features(test_basic PRIVATE c_std_11)
sylvan_test_warnings(test_basic)

add_executable(test_zdd)
target_sources(test_zdd PRIVATE test_zdd.c)
target_link_libraries(test_zdd PRIVATE sylvan::sylvan)
target_compile_features(test_zdd PRIVATE c_std_11)
sylvan_test_warnings(test_zdd)

add_executable(test_listdd)
target_sources(test_listdd PRIVATE test_listdd.c)
target_link_libraries(test_listdd PRIVATE sylvan::sylvan)
target_compile_features(test_listdd PRIVATE c_std_11)
sylvan_test_warnings(test_listdd)

add_executable(test_table)
target_sources(test_table PRIVATE test_table.c)
target_link_libraries(test_table PRIVATE sylvan::sylvan)
target_compile_features(test_table PRIVATE c_std_11)
sylvan_test_warnings(test_table)

add_executable(test_serialize)
target_sources(test_serialize PRIVATE test_serialize.c)
target_link_libraries(test_serialize PRIVATE sylvan::sylvan)
target_compile_features(test_serialize PRIVATE c_std_11)
sylvan_test_warnings(test_serialize)

add_test(NAME test_basic COMMAND test_basic)
add_test(NAME test_zdd COMMAND test_zdd)
add_test(NAME test_listdd COMMAND test_listdd)
add_test(NAME test_table COMMAND test_table)
add_test(NAME test_serialize COMMAND test_serialize)

if(SYLVAN_GMP)
    add_executable(test_gmp)
    target_sources(test_gmp PRIVATE test_gmp.c)
    target_link_libraries(test_gmp PRIVATE sylvan::sylvan)
    target_compile_features(test_gmp PRIVATE c_std_11)
    sylvan_test_warnings(test_gmp)
    target_compile_options(test_gmp PRIVATE
        $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wno-unused-function>
    )

    add_test(NAME test_gmp COMMAND test_gmp)
endif()

if(SYLVAN_BUILD_CPP)
    add_executable(test_cxx)
    target_sources(test_cxx PRIVATE test_cxx.cpp)
    target_link_libraries(test_cxx PRIVATE sylvan::sylvan)
    target_compile_features(test_cxx PRIVATE cxx_std_11)
    sylvan_test_warnings(test_cxx)

    add_test(NAME test_cxx COMMAND test_cxx)
endif()

# MinGW-built executables depend on runtime DLLs beside the compiler (for
# example libgcc_s_seh-1.dll and libwinpthread-1.dll in MSYS2). Make CTest use
# that directory without requiring the user's shell PATH to be configured.
if(WIN32 AND CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND
   NOT CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
    get_filename_component(_sylvan_compiler_runtime_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    set(_sylvan_tests test_basic test_zdd test_listdd test_table test_serialize)
    if(SYLVAN_GMP)
        list(APPEND _sylvan_tests test_gmp)
    endif()
    if(SYLVAN_BUILD_CPP)
        list(APPEND _sylvan_tests test_cxx)
    endif()
    set_tests_properties(${_sylvan_tests} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_sylvan_compiler_runtime_dir}"
    )
    unset(_sylvan_tests)
    unset(_sylvan_compiler_runtime_dir)
endif()
