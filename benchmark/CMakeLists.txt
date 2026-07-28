add_executable(benchmark_apply_abstract)
target_sources(benchmark_apply_abstract PRIVATE apply_abstract.c)
target_link_libraries(benchmark_apply_abstract PRIVATE sylvan::sylvan)
target_compile_features(benchmark_apply_abstract PRIVATE c_std_11)
target_compile_options(benchmark_apply_abstract PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall -Wextra -Werror>
    $<$<COMPILE_LANG_AND_ID:C,MSVC>:/W4>
)

add_executable(benchmark_make_node)
target_sources(benchmark_make_node PRIVATE make_node.c)
target_link_libraries(benchmark_make_node PRIVATE sylvan::sylvan)
target_compile_features(benchmark_make_node PRIVATE c_std_11)
target_compile_options(benchmark_make_node PRIVATE
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall -Wextra -Werror>
    $<$<COMPILE_LANG_AND_ID:C,MSVC>:/W4>
)
