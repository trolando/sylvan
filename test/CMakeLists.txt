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

foreach(test IN ITEMS test_relprod test_markbit test_gc_invalid test_platform test_gc_policy test_gc_retry)
    add_executable(${test} ${test}.c)
    target_link_libraries(${test} PRIVATE sylvan::sylvan)
    target_compile_features(${test} PRIVATE c_std_11)
    sylvan_test_warnings(${test})
    add_test(NAME ${test} COMMAND ${test})
    set_tests_properties(${test} PROPERTIES TIMEOUT 120)
endforeach()

add_test(NAME test_gc_policy_parallel COMMAND test_gc_policy 2)
set_tests_properties(test_gc_policy_parallel PROPERTIES TIMEOUT 120)

add_test(NAME test_gc_retry_max COMMAND test_gc_retry max)
add_test(NAME test_gc_retry_disabled COMMAND test_gc_retry disabled)
set_tests_properties(test_gc_retry_max test_gc_retry_disabled PROPERTIES TIMEOUT 120)

add_executable(test_gc_concurrent test_gc_concurrent.c)
target_link_libraries(test_gc_concurrent PRIVATE sylvan::sylvan)
target_compile_features(test_gc_concurrent PRIVATE c_std_11)
sylvan_test_warnings(test_gc_concurrent)
foreach(mode IN ITEMS recover growth shrink)
    add_test(NAME test_gc_concurrent_${mode} COMMAND test_gc_concurrent ${mode})
    set_tests_properties(test_gc_concurrent_${mode} PROPERTIES TIMEOUT 120)
endforeach()

if(SYLVAN_USE_MMAP AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # Compile a private allocator copy so failure injection adds no code or
    # public API to the library, even when building the tests.
    add_executable(test_platform_fallback test_platform.c ../src/sylvan_platform.c)
    target_include_directories(test_platform_fallback PRIVATE
        ${PROJECT_SOURCE_DIR}/src ${PROJECT_BINARY_DIR}/src)
    target_compile_definitions(test_platform_fallback PRIVATE
        SYLVAN_TEST_MADV_DONTNEED_FAIL=1
        SYLVAN_HUGEPAGES=$<BOOL:${SYLVAN_HUGEPAGES}>)
    target_compile_features(test_platform_fallback PRIVATE c_std_11)
    sylvan_test_warnings(test_platform_fallback)
    add_test(NAME test_platform_fallback COMMAND test_platform_fallback)
    set_tests_properties(test_platform_fallback PROPERTIES TIMEOUT 120)
endif()

add_test(NAME test_gc_invalid_zdd COMMAND test_gc_invalid zdd)
set_tests_properties(test_gc_invalid_zdd PROPERTIES TIMEOUT 120)

foreach(test IN ITEMS test_probability test_probability_gc)
    add_executable(${test} test_probability.c)
    if(test STREQUAL "test_probability_gc")
        target_sources(${test} PRIVATE test_probability_gc.c)
        target_compile_definitions(${test} PRIVATE SYLVAN_TEST_PROBABILITY_GC=1)
    endif()
    target_link_libraries(${test} PRIVATE sylvan::sylvan)
    target_compile_features(${test} PRIVATE c_std_11)
    sylvan_test_warnings(${test})
    foreach(workers IN ITEMS 1 4)
        add_test(NAME ${test}_${workers} COMMAND ${test} ${workers})
        set_tests_properties(${test}_${workers} PROPERTIES TIMEOUT 120)
    endforeach()
endforeach()

add_executable(test_mtbdd_compare test_mtbdd_compare.c)
target_link_libraries(test_mtbdd_compare PRIVATE sylvan::sylvan)
target_compile_features(test_mtbdd_compare PRIVATE c_std_11)
sylvan_test_warnings(test_mtbdd_compare)
foreach(workers IN ITEMS 1 4)
    add_test(NAME test_mtbdd_compare_${workers} COMMAND test_mtbdd_compare ${workers})
    set_tests_properties(test_mtbdd_compare_${workers} PROPERTIES TIMEOUT 120)
endforeach()

add_executable(test_reach)
target_sources(test_reach PRIVATE test_reach.c)
target_link_libraries(test_reach PRIVATE sylvan::sylvan)
target_compile_features(test_reach PRIVATE c_std_11)
sylvan_test_warnings(test_reach)

add_test(NAME test_basic COMMAND test_basic)
add_test(NAME test_gc COMMAND test_gc)
set_tests_properties(test_gc PROPERTIES TIMEOUT 120)
add_test(NAME test_refs COMMAND test_refs)
set_tests_properties(test_refs PROPERTIES TIMEOUT 120)
add_test(NAME test_gc_roots COMMAND test_gc_roots)
set_tests_properties(test_gc_roots PROPERTIES TIMEOUT 120)
add_test(NAME test_reach COMMAND test_reach)
add_test(NAME test_reach_single COMMAND test_reach 1)
add_test(NAME test_reach_parallel COMMAND test_reach 4)
set_tests_properties(test_reach test_reach_single test_reach_parallel PROPERTIES TIMEOUT 120)

add_executable(test_reach_wide test_reach_wide.c)
target_link_libraries(test_reach_wide PRIVATE sylvan::sylvan)
target_compile_features(test_reach_wide PRIVATE c_std_11)
sylvan_test_warnings(test_reach_wide)
add_test(NAME test_reach_wide COMMAND test_reach_wide 1)
add_test(NAME test_reach_wide_parallel COMMAND test_reach_wide 4)
set_tests_properties(test_reach_wide test_reach_wide_parallel PROPERTIES TIMEOUT 120)

if(SYLVAN_BUILD_EXAMPLES)
    add_executable(test_empty_models test_empty_models.c)
    target_link_libraries(test_empty_models PRIVATE sylvan::sylvan)
    target_compile_features(test_empty_models PRIVATE c_std_11)
    sylvan_test_warnings(test_empty_models)
    add_test(NAME test_empty_models COMMAND test_empty_models)
    set_tests_properties(test_empty_models PROPERTIES FIXTURES_SETUP empty_models TIMEOUT 30)
    foreach(strategy IN ITEMS bfs par sat chaining reach)
        foreach(kind IN ITEMS bdd ldd)
            add_test(NAME test_${kind}_empty_${strategy}
                COMMAND ${CMAKE_COMMAND}
                    -DPROGRAM=$<TARGET_FILE:${kind}mc> -DSTRATEGY=${strategy}
                    -DMODEL=${CMAKE_CURRENT_BINARY_DIR}/empty-groups.${kind}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/check_empty_model.cmake)
            set_tests_properties(test_${kind}_empty_${strategy} PROPERTIES
                FIXTURES_REQUIRED empty_models TIMEOUT 30)
        endforeach()
        add_test(NAME test_bdd_empty_merge_${strategy}
            COMMAND ${CMAKE_COMMAND}
                -DPROGRAM=$<TARGET_FILE:bddmc> -DSTRATEGY=${strategy} -DMERGE=ON
                -DMODEL=${CMAKE_CURRENT_BINARY_DIR}/empty-groups.bdd
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_empty_model.cmake)
        set_tests_properties(test_bdd_empty_merge_${strategy} PROPERTIES
            FIXTURES_REQUIRED empty_models TIMEOUT 30)
    endforeach()
endif()
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

add_executable(test_count_eval test_count_eval.c)
target_link_libraries(test_count_eval PRIVATE sylvan::sylvan)
target_compile_features(test_count_eval PRIVATE c_std_11)
sylvan_test_warnings(test_count_eval)
if(SYLVAN_GMP)
    target_compile_definitions(test_count_eval PRIVATE SYLVAN_TEST_GMP=1)
endif()
add_test(NAME test_count_eval COMMAND test_count_eval)
add_test(NAME test_count_eval_parallel COMMAND test_count_eval 4)
set_tests_properties(test_count_eval test_count_eval_parallel PROPERTIES TIMEOUT 120)

add_executable(test_bdd_ops test_bdd_ops.c)
target_link_libraries(test_bdd_ops PRIVATE sylvan::sylvan)
target_compile_features(test_bdd_ops PRIVATE c_std_11)
sylvan_test_warnings(test_bdd_ops)
add_test(NAME test_bdd_ops COMMAND test_bdd_ops)
add_test(NAME test_bdd_ops_parallel COMMAND test_bdd_ops 4)
set_tests_properties(test_bdd_ops test_bdd_ops_parallel PROPERTIES TIMEOUT 120)

add_executable(test_zdd_boolean test_zdd_boolean.c)
target_link_libraries(test_zdd_boolean PRIVATE sylvan::sylvan)
target_compile_features(test_zdd_boolean PRIVATE c_std_11)
sylvan_test_warnings(test_zdd_boolean)
add_test(NAME test_zdd_boolean COMMAND test_zdd_boolean)
add_test(NAME test_zdd_boolean_parallel COMMAND test_zdd_boolean 4)
set_tests_properties(test_zdd_boolean test_zdd_boolean_parallel PROPERTIES TIMEOUT 120)

add_executable(test_mtbdd_ops test_mtbdd_ops.c)
target_link_libraries(test_mtbdd_ops PRIVATE sylvan::sylvan)
target_compile_features(test_mtbdd_ops PRIVATE c_std_11)
sylvan_test_warnings(test_mtbdd_ops)
if(SYLVAN_GMP)
    target_compile_definitions(test_mtbdd_ops PRIVATE SYLVAN_TEST_GMP=1)
endif()
add_test(NAME test_mtbdd_ops COMMAND test_mtbdd_ops)
add_test(NAME test_mtbdd_ops_parallel COMMAND test_mtbdd_ops 4)
set_tests_properties(test_mtbdd_ops test_mtbdd_ops_parallel PROPERTIES TIMEOUT 120)

if(SYLVAN_BUILD_CPP)
    add_executable(test_mtbdd_ops_cxx test_mtbdd_ops_cxx.cpp)
    target_link_libraries(test_mtbdd_ops_cxx PRIVATE sylvan::sylvan)
    target_compile_features(test_mtbdd_ops_cxx PRIVATE cxx_std_11)
    sylvan_test_warnings(test_mtbdd_ops_cxx)
    add_test(NAME test_mtbdd_ops_cxx COMMAND test_mtbdd_ops_cxx)
    set_tests_properties(test_mtbdd_ops_cxx PROPERTIES TIMEOUT 120)
endif()
