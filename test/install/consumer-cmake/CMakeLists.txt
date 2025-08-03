cmake_minimum_required(VERSION 3.17)
project(test_sylvan_cmake C)

find_package(sylvan REQUIRED)

add_executable(test main.c)
target_link_libraries(test PRIVATE sylvan::sylvan)

