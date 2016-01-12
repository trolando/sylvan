cmake_minimum_required(VERSION 2.6)
project(sylvan C CXX)
enable_testing()

set(CMAKE_C_FLAGS "-g -O3 -Wextra -Wall -Werror -fno-strict-aliasing -std=gnu11")

add_executable(sylvan_test main.c)
target_link_libraries(sylvan_test sylvan)

add_executable(test_basic test_basic.c)
target_link_libraries(test_basic sylvan)

add_executable(test_cxx test_cxx.cpp)
target_link_libraries(test_cxx sylvan stdc++)

add_test(test_cxx test_cxx)
add_test(test_basic test_basic)
