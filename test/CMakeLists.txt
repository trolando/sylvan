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

add_executable(test_table)
target_sources(test_table PRIVATE test_table.c)
target_link_libraries(test_table PRIVATE sylvan::sylvan)
target_compile_features(test_table PRIVATE c_std_11)
sylvan_test_warnings(test_table)

add_test(NAME test_basic COMMAND test_basic)
add_test(NAME test_zdd COMMAND test_zdd)
add_test(NAME test_table COMMAND test_table)

if(SYLVAN_BUILD_CPP)
    add_executable(test_cxx)
    target_sources(test_cxx PRIVATE test_cxx.cpp)
    target_link_libraries(test_cxx PRIVATE sylvan::sylvan)
    target_compile_features(test_cxx PRIVATE cxx_std_11)
    sylvan_test_warnings(test_cxx)

    add_test(NAME test_cxx COMMAND test_cxx)
endif()
