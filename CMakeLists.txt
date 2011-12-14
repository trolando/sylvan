cmake_minimum_required(VERSION 2.6)
project(SYLVAN)

ENABLE_TESTING()

add_subdirectory(src) 
add_subdirectory(test)

ADD_TEST(SimpleTest ${CMAKE_CURRENT_BINARY_DIR}/test/test)
