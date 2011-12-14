cmake_minimum_required(VERSION 2.6)

add_library (sylvan sylvan.c fast_hash.c llsched.c llvector.c llset.c llgcset.c runtime.c)

#add_definitions(-std=gnu99)
target_link_libraries (sylvan m)
