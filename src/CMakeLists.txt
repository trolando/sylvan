add_library(sylvan STATIC)
add_library(sylvan::sylvan ALIAS sylvan)

set(SYLVAN_HDRS
    sylvan.h
    sylvan_bdd.h
    sylvan_cache.h
    sylvan_config.h
    sylvan_common.h
    sylvan_hash.h
    sylvan_int.h
    sylvan_ldd.h
    sylvan_ldd_int.h
    sylvan_mt.h
    sylvan_mtbdd.h
    sylvan_mtbdd_int.h
    sylvan_obj.hpp
    sylvan_stats.h
    sylvan_table.h
    sylvan_tls.h
    sylvan_zdd.h
    sylvan_zdd_int.h
)

target_sources(sylvan
  PRIVATE
    sha2.c
    sylvan_bdd.c
    sylvan_cache.c
    sylvan_common.c
    sylvan_hash.c
    sylvan_ldd.c
    sylvan_mt.c
    sylvan_mtbdd.c
    sylvan_obj.cpp
    sylvan_refs.c
    sylvan_sl.c
    sylvan_stats.c
    sylvan_table.c
    sylvan_zdd.c
    ${SYLVAN_HDRS}
)

option(SYLVAN_GMP "Include custom MTBDD type GMP")
if(SYLVAN_GMP)
    # We only want to include the custom MTBDD type GMP if we actually have the GMP library
    find_package(GMP REQUIRED)
    set(SYLVAN_HDRS ${SYLVAN_HDRS} sylvan_gmp.h)
    target_sources(sylvan PRIVATE sylvan_gmp.c sylvan_gmp.h)
    target_include_directories(sylvan PRIVATE ${GMP_INCLUDE_DIR})
    target_link_libraries(sylvan PUBLIC ${GMP_LIBRARIES})
    set(PKGC_LINK_GMP -lgmp)
else()
    set(PKGC_LINK_GMP "")
endif()

set_target_properties(sylvan PROPERTIES VERSION ${sylvan_VERSION} SOVERSION ${sylvan_VERSION_MAJOR})
set_target_properties(sylvan PROPERTIES PUBLIC_HEADER "${SYLVAN_HDRS}")

target_compile_features(sylvan PUBLIC c_std_11 cxx_std_11)

target_compile_options(sylvan PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:GNU>>:
        -pipe -march=native
        $<$<CONFIG:Debug>:-O0 -Wall -Wextra>>
    $<$<CXX_COMPILER_ID:MSVC>:
        $<$<CONFIG:Debug>:/Od /Wall /Zi>>
)

target_include_directories(sylvan PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(sylvan PUBLIC m pthread lace::lace)

option(SYLVAN_USE_MMAP "Let Sylvan use mmap to allocate (virtual) memory" ON)
if(SYLVAN_USE_MMAP)
    include(CheckSymbolExists)
    check_symbol_exists(mmap "sys/mman.h" HAVE_MMAP)
    if(NOT HAVE_MMAP)
        message(WARNING " mmap not found: disabling mmap support")
        set(SYLVAN_USE_MMAP OFF)
    else()
        set_target_properties(sylvan PROPERTIES COMPILE_DEFINITIONS "SYLVAN_USE_MMAP")
    endif()
endif()

# Do we want to collect BDD statistics?
option(SYLVAN_STATS "Let Sylvan collect statistics at runtime" OFF)
if(SYLVAN_STATS)
    set_target_properties(sylvan PROPERTIES COMPILE_DEFINITIONS "SYLVAN_STATS")
endif()

set_target_properties(sylvan PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin
)

include(GNUInstallDirs)

install(TARGETS sylvan
    EXPORT sylvan-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT sylvan-targets
    FILE sylvan-targets.cmake
    NAMESPACE sylvan::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/sylvan-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config-version.cmake
    VERSION ${SYLVAN_VERSION}
    COMPATIBILITY SameMinorVersion
)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/sylvan.pc.in" "${CMAKE_CURRENT_BINARY_DIR}/sylvan.pc" @ONLY)
    
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/sylvan.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

export(EXPORT sylvan-targets
    FILE ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-targets.cmake
    NAMESPACE sylvan::
)

