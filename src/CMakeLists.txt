target_sources(sylvan
  PRIVATE
    sha2.c
    bdd.c
    cache.c
    common.c
    hash.c
    ldd.c
    mt.c
    mtbdd.c
    obj.cpp
    refs.c
    sl.c
    stats.c
    zdd.c
)

# Choose the nodes table implementation
# Valid values: list, llht, trie
set(SYLVAN_NODES_TABLE "trie" CACHE STRING "Nodes table implementation (llht|trie|list)")
set_property(CACHE SYLVAN_NODES_TABLE PROPERTY STRINGS llht trie list)

# Normalize and select the source + an identifying macro
string(TOLOWER "${SYLVAN_NODES_TABLE}" _sylvan_nodes_table)
if(_sylvan_nodes_table STREQUAL "llht")
  set(_sylvan_nodes_src nodes_llht.c)
  set(_sylvan_nodes_def SYLVAN_NODES_TABLE_LLHT=1)
elseif(_sylvan_nodes_table STREQUAL "trie")
  set(_sylvan_nodes_src nodes_trie.c)
  set(_sylvan_nodes_def SYLVAN_NODES_TABLE_TRIE=1)
elseif(_sylvan_nodes_table STREQUAL "list")
  set(_sylvan_nodes_src nodes_list.c)
  set(_sylvan_nodes_def SYLVAN_NODES_TABLE_LIST=1)
else()
  message(FATAL_ERROR "Invalid SYLVAN_NODES_TABLE='${SYLVAN_NODES_TABLE}'. Valid: list, llht, trie")
endif()

message(STATUS "Sylvan: using '${_sylvan_nodes_table}' nodes table implementation (${_sylvan_nodes_src})")

# Add the chosen implementation and a compile definition
target_sources(sylvan PRIVATE ${_sylvan_nodes_src})
target_compile_definitions(sylvan PRIVATE ${_sylvan_nodes_def})

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

include(DetectCacheLineSize)
detect_cache_line_size(SYLVAN_CACHE_LINE_SIZE DEFAULT 64)

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
    target_sources(sylvan PRIVATE gmp.c)
    target_include_directories(sylvan PRIVATE ${GMP_INCLUDE_DIR})
    target_link_libraries(sylvan PUBLIC ${GMP_LIBRARIES})
endif()

set_target_properties(sylvan PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin
)

configure_file(${PROJECT_SOURCE_DIR}/include/sylvan/config.h.in ${PROJECT_BINARY_DIR}/include/sylvan/config.h @ONLY)
target_include_directories(sylvan
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
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
    ${PROJECT_SOURCE_DIR}/include/sylvan/bdd.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/cache.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/common.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/hash.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/ldd.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/mt.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/mtbdd.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/obj.hpp
    ${PROJECT_SOURCE_DIR}/include/sylvan/stats.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/nodes.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/tls.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/zdd.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/internal/internal.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/internal/ldd_internal.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/internal/mtbdd_internal.h
    ${PROJECT_SOURCE_DIR}/include/sylvan/internal/zdd_internal.h
)

if(SYLVAN_GMP)
    set(SYLVAN_PUBLIC_HDRS ${SYLVAN_PUBLIC_HDRS} ${PROJECT_SOURCE_DIR}/include/sylvan/gmp.h)
endif()

foreach(hdr IN LISTS SYLVAN_PUBLIC_HDRS)
    file(RELATIVE_PATH rel "${PROJECT_SOURCE_DIR}/include/sylvan" "${hdr}")
    get_filename_component(dir "${rel}" DIRECTORY)
    install(FILES "${hdr}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sylvan/${dir}")
endforeach()

# special case for config.h
install(FILES "${PROJECT_BINARY_DIR}/include/sylvan/config.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/sylvan")

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

