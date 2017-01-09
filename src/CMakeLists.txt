set(SOURCES
    lace.c
    sha2.c
    sylvan_bdd.c
    sylvan_cache.c
    sylvan_common.c
    sylvan_gmp.c
    sylvan_ldd.c
    sylvan_mtbdd.c
    sylvan_obj.cpp
    sylvan_refs.c
    sylvan_sl.c
    sylvan_stats.c
    sylvan_table.c
)

set(HEADERS
    lace.h
    sylvan.h
    sylvan_bdd.h
    sylvan_cache.h
    sylvan_config.h
    sylvan_common.h
    sylvan_gmp.h
    sylvan_int.h
    sylvan_ldd.h
    sylvan_ldd_int.h
    sylvan_mtbdd.h
    sylvan_mtbdd_int.h
    sylvan_obj.hpp
    sylvan_stats.h
    sylvan_table.h
    sylvan_tls.h
)

add_library(sylvan STATIC ${SOURCES})
add_library(sylvan_shared SHARED ${SOURCES})
set_target_properties(sylvan_shared PROPERTIES OUTPUT_NAME sylvan)

find_package(GMP REQUIRED)
find_package(Hwloc REQUIRED)

target_link_libraries(sylvan m pthread ${GMP_LIBRARIES} ${HWLOC_LIBRARIES})
target_link_libraries(sylvan_shared m pthread ${GMP_LIBRARIES} ${HWLOC_LIBRARIES})

if(UNIX AND NOT APPLE)
    target_link_libraries(sylvan rt)
    target_link_libraries(sylvan_shared rt)
endif()

set_target_properties(sylvan PROPERTIES COMPILE_DEFINITIONS "USE_HWLOC=1")
set_target_properties(sylvan_shared PROPERTIES COMPILE_DEFINITIONS "USE_HWLOC=1")

option(SYLVAN_STATS "Collect statistics" OFF)
if(SYLVAN_STATS)
    set_target_properties(sylvan PROPERTIES COMPILE_DEFINITIONS "SYLVAN_STATS")
    set_target_properties(sylvan_shared PROPERTIES COMPILE_DEFINITIONS "SYLVAN_STATS")
endif()

install(TARGETS sylvan DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(TARGETS sylvan_shared DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES ${HEADERS} DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
