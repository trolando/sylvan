cmake_minimum_required(VERSION 2.6)

add_executable (test main.c)

add_definitions(-std=gnu99)
include_directories( ../src)
target_link_libraries (test sylvan pthread rt)
