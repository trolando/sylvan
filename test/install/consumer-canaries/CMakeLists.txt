cmake_minimum_required(VERSION 3.17)
project(sylvan_consumer_canaries LANGUAGES C CXX)

include(CTest)
find_package(sylvan REQUIRED)

add_executable(storm-rational storm-rational.cpp)
target_link_libraries(storm-rational PRIVATE sylvan::sylvan)
target_compile_features(storm-rational PRIVATE cxx_std_17)

add_test(NAME storm-rational-one-worker COMMAND storm-rational 1)
add_test(NAME storm-rational-two-workers COMMAND storm-rational 2)
