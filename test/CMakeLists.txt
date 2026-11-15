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

add_executable(test_gc)
target_sources(test_gc PRIVATE test_gc.c)
target_link_libraries(test_gc PRIVATE sylvan::sylvan)
target_compile_features(test_gc PRIVATE c_std_11)
sylvan_test_warnings(test_gc)

add_executable(test_refs)
target_sources(test_refs PRIVATE test_refs.c)
target_link_libraries(test_refs PRIVATE sylvan::sylvan)
target_compile_features(test_refs PRIVATE c_std_11)
sylvan_test_warnings(test_refs)

add_executable(test_gc_roots)
target_sources(test_gc_roots PRIVATE test_gc_roots.c)
target_link_libraries(test_gc_roots PRIVATE sylvan::sylvan)
target_compile_features(test_gc_roots PRIVATE c_std_11)
sylvan_test_warnings(test_gc_roots)

foreach(test IN ITEMS test_relprod test_markbit test_gc_invalid)
    add_executable(${test} ${test}.c)
    target_link_libraries(${test} PRIVATE sylvan::sylvan)
    target_compile_features(${test} PRIVATE c_std_11)
    sylvan_test_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
    set_tests_properties(${test} PROPERTIES TIMEOUT 120)
endforeach()
add_test(NAME test_gc_invalid_zdd COMMAND test_gc_invalid zdd)
set_tests_properties(test_gc_invalid_zdd PROPERTIES TIMEOUT 120)

add_test(NAME test_basic COMMAND test_basic)
add_test(NAME test_gc COMMAND test_gc)
set_tests_properties(test_gc PROPERTIES TIMEOUT 120)
add_test(NAME test_refs COMMAND test_refs)
set_tests_properties(test_refs PROPERTIES TIMEOUT 120)
add_test(NAME test_gc_roots COMMAND test_gc_roots)
set_tests_properties(test_gc_roots PROPERTIES TIMEOUT 120)
add_test(NAME test_zdd COMMAND test_zdd)
add_test(NAME test_table COMMAND test_table)
add_test(NAME test_table_ownership COMMAND test_table ownership)
set_tests_properties(test_table_ownership PROPERTIES TIMEOUT 120)
add_test(NAME test_table_cache COMMAND test_table cache)
set_tests_properties(test_table_cache PROPERTIES TIMEOUT 120)
add_test(NAME test_table_maxsize COMMAND test_table maxsize)
set_tests_properties(test_table_maxsize PROPERTIES TIMEOUT 120)
add_test(NAME test_table_limit COMMAND test_table oversized)
set_tests_properties(test_table_limit PROPERTIES WILL_FAIL TRUE TIMEOUT 120)

if(SYLVAN_CHECK_HANDLES)
    add_executable(test_check_handles test_check_handles.c)
    target_link_libraries(test_check_handles PRIVATE sylvan::sylvan)
    target_compile_features(test_check_handles PRIVATE c_std_11)
    sylvan_test_warnings(test_check_handles)

    add_test(NAME test_check_handles COMMAND test_check_handles)
    foreach(helper IN ITEMS mtbdd zdd ldd table)
        add_test(NAME test_check_handles_${helper}
            COMMAND ${CMAKE_COMMAND}
                -DTEST_PROGRAM=$<TARGET_FILE:test_check_handles>
                -DHELPER=${helper}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_handle_abort.cmake
        )
        set_tests_properties(test_check_handles_${helper} PROPERTIES TIMEOUT 120)
    endforeach()
    set_tests_properties(test_check_handles PROPERTIES TIMEOUT 120)
endif()

add_executable(test_table_full test_table_full.c)
target_link_libraries(test_table_full PRIVATE sylvan::sylvan)
target_compile_features(test_table_full PRIVATE c_std_11)
sylvan_test_warnings(test_table_full)

foreach(node IN ITEMS bdd bdd_leaf bdd_map ldd ldd_copy zdd zdd_leaf zdd_map)
    foreach(mode IN ITEMS handler default reset return)
        set(test_name test_table_full_${node}_${mode})
        add_test(NAME ${test_name} COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:test_table_full>"
            "-DNODE=${node}" "-DMODE=${mode}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/check_table_full.cmake")
        set_tests_properties(${test_name} PROPERTIES TIMEOUT 30)
    endforeach()
endforeach()

if(SYLVAN_BUILD_CPP)
    add_executable(test_cxx)
    target_sources(test_cxx PRIVATE test_cxx.cpp)
    target_link_libraries(test_cxx PRIVATE sylvan::sylvan)
    target_compile_features(test_cxx PRIVATE cxx_std_11)
    sylvan_test_warnings(test_cxx)

    add_test(NAME test_cxx COMMAND test_cxx)
endif()
