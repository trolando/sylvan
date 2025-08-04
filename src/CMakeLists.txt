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
)

target_include_directories(sylvan
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)

set_target_properties(sylvan PROPERTIES VERSION ${sylvan_VERSION} SOVERSION ${sylvan_VERSION_MAJOR})

option(SYLVAN_ENABLE_PIC "Build Sylvan with -fPIC (e.g. for linking into shared libs)" OFF)
set_target_properties(sylvan PROPERTIES POSITION_INDEPENDENT_CODE ${SYLVAN_ENABLE_PIC})

target_compile_features(sylvan PUBLIC c_std_11 cxx_std_11)

option(SYLVAN_NATIVE_OPT "Enable -march=native for compiler optimizations" OFF)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sylvan PRIVATE
        $<$<CONFIG:Debug>:-O0 -Wall -Wextra -pipe>
        $<$<CONFIG:Release>:-pipe>
        $<$<BOOL:${SYLVAN_NATIVE_OPT}>:-march=native>
    )
elseif(MSVC)
    target_compile_options(sylvan PRIVATE
        $<$<CONFIG:Debug>:/Od /Wall /Zi>
    )
endif()

find_package(Threads REQUIRED)
target_link_libraries(sylvan PUBLIC Threads::Threads lace::lace)
if(NOT WIN32)
    target_link_libraries(sylvan PUBLIC m)
endif()

option(SYLVAN_USE_MMAP "Let Sylvan use mmap to allocate (virtual) memory" ON)
if(SYLVAN_USE_MMAP)
    include(CheckSymbolExists)
    check_symbol_exists(mmap "sys/mman.h" HAVE_MMAP)
    if(NOT HAVE_MMAP)
        message(WARNING " mmap not found: disabling mmap support")
        set(SYLVAN_USE_MMAP OFF)
    else()
        target_compile_definitions(sylvan PUBLIC SYLVAN_USE_MMAP)
    endif()
endif()

# Do we want to collect BDD statistics?
option(SYLVAN_STATS "Let Sylvan collect statistics at runtime" OFF)
if(SYLVAN_STATS)
    target_compile_definitions(sylvan PUBLIC SYLVAN_STATS)
endif()

option(SYLVAN_GMP "Include custom MTBDD type GMP")
if(SYLVAN_GMP)
    # We only want to include the custom MTBDD type GMP if we actually have the GMP library
    find_package(GMP REQUIRED)
    target_sources(sylvan PRIVATE sylvan_gmp.c)
    target_include_directories(sylvan PRIVATE ${GMP_INCLUDE_DIR})
    target_link_libraries(sylvan PUBLIC ${GMP_LIBRARIES})
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
)

set(SYLVAN_PUBLIC_HDRS
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_bdd.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_cache.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_config.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_common.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_hash.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/internal/sylvan_int.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_ldd.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/internal/sylvan_ldd_int.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_mt.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_mtbdd.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/internal/sylvan_mtbdd_int.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_obj.hpp
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_stats.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_table.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_tls.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_zdd.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/internal/sylvan_zdd_int.h
)

if(SYLVAN_GMP)
    set(SYLVAN_PUBLIC_HDRS ${SYLVAN_PUBLIC_HDRS} ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_gmp.h)
endif()

foreach(hdr IN LISTS SYLVAN_PUBLIC_HDRS)
    file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}/include/sylvan" "${hdr}")
    get_filename_component(dir "${rel}" DIRECTORY)
    install(FILES "${hdr}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sylvan/${dir}")
endforeach()

install(EXPORT sylvan-targets
    FILE sylvan-targets.cmake
    NAMESPACE sylvan::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
)

include(CMakePackageConfigHelpers)

if(SYLVAN_GMP)
    set(SYLVAN_CONFIG_FIND_GMP "find_dependency(GMP REQUIRED)")
else()
    set(SYLVAN_CONFIG_FIND_GMP "")
endif()

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/tools/sylvan-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config-version.cmake
    VERSION ${SYLVAN_VERSION}
    COMPATIBILITY SameMinorVersion
)

# Compute PKG_CONFIG requires line
if(SYLVAN_GMP)
    set(PKGC_REQUIRES "lace >= 1.4.2 gmp")
else()
    set(PKGC_REQUIRES "lace >= 1.4.2")
endif()

set(PKGC_LIBS_PRIVATE "")
list(APPEND PKGC_LIBS_PRIVATE -lpthread)
if(NOT APPLE)
    list(APPEND PKGC_LIBS_PRIVATE -lm)
endif()

# Join into space-separated string
string(REPLACE ";" " " PKGC_LIBS_PRIVATE "${PKGC_LIBS_PRIVATE}")

install(CODE "
  file(MAKE_DIRECTORY \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig\")
  file(WRITE \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/pkgconfig/sylvan.pc\" \"
prefix=\${CMAKE_INSTALL_PREFIX}
exec_prefix=\\\${prefix}
libdir=\\\${prefix}/${CMAKE_INSTALL_LIBDIR}
includedir=\\\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}

Name: ${PROJECT_NAME}
Description: ${PROJECT_DESCRIPTION}
URL: ${PROJECT_HOMEPAGE_URL}
Version: ${PROJECT_VERSION}
Cflags: -I\\\${includedir}
Libs: -L\\\${libdir} -lsylvan
Libs.private: ${PKGC_LIBS_PRIVATE}
Requires: ${PKGC_REQUIRES}
\")
")
    
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
)

if(SYLVAN_GMP)
    install(FILES ${PROJECT_SOURCE_DIR}/tools/FindGMP.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan)
endif()

export(EXPORT sylvan-targets
    FILE ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-targets.cmake
    NAMESPACE sylvan::
)

