# ── Platform configuration ──────────────────────────────────────────────────

# Configure platform features before declaring the generated header file set.
option(SYLVAN_USE_MMAP "Let Sylvan use mmap or VirtualAlloc to allocate (virtual) memory" ON)
if(SYLVAN_USE_MMAP AND NOT WIN32)
    include(CheckSymbolExists)
    check_symbol_exists(mmap "sys/mman.h" HAVE_MMAP)
    if(NOT HAVE_MMAP)
        message(WARNING "mmap not found: disabling mmap support")
        set(SYLVAN_USE_MMAP OFF)
    endif()
endif()

option(SYLVAN_STATS "Let Sylvan collect statistics at runtime" OFF)

# Diagnostic builds only: count variable rank lookups. The counter is a single
# contended atomic, so it distorts both timing and parallel scaling.
option(SYLVAN_RANK_COUNTERS "Let Sylvan count variable rank lookups" OFF)

include(DetectCacheLineSize)
detect_cache_line_size(SYLVAN_CACHE_LINE_SIZE DEFAULT 64)

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/include/sylvan")
configure_file(
    ${PROJECT_SOURCE_DIR}/include/sylvan/config.h.in
    ${PROJECT_BINARY_DIR}/include/sylvan/config.h
    @ONLY
)

# ── Library target ───────────────────────────────────────────────────────────

# Windows builds are static until Sylvan and Lace have an explicit DLL API.
if(WIN32)
    add_library(sylvan STATIC)
else()
    add_library(sylvan)
endif()
add_library(sylvan::sylvan ALIAS sylvan)

# ── Sources ──────────────────────────────────────────────────────────────────

target_sources(sylvan
  PRIVATE
    bdd.c
    cache.c
    common.c
    hash.c
    iterator.c
    listdd.c
    mt.c
    mtbdd.c
    nodes_scratch.c
    platform.c
    ranks.c
    refs.c
    reorder.c
    serialize.c
    sha2.c
    sl.c
    stats.c
    zdd.c
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS
      ${PROJECT_SOURCE_DIR}/include
      ${PROJECT_BINARY_DIR}/include
    FILES
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/bdd.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/bdd_impl.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/cache.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/common.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/hash.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/listdd.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/mt.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/mtbdd.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/mtbdd_impl.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/nodes.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/platform.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/serialize.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/serialize_impl.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/stats.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/types.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/zdd.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal/listdd_internal.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal/mtbdd_internal.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal/ranks_internal.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal/reorder_internal.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal/zdd_internal.h
      ${PROJECT_BINARY_DIR}/include/sylvan/config.h
)

# ── Optional C++ bindings ────────────────────────────────────────────────────

if(SYLVAN_BUILD_CPP)
    target_sources(sylvan
      PRIVATE obj.cpp
      PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${PROJECT_SOURCE_DIR}/include
        FILES ${PROJECT_SOURCE_DIR}/include/sylvan/obj.hpp
    )
endif()

# ── Optional GMP support ─────────────────────────────────────────────────────

option(SYLVAN_GMP "Include custom MTBDD type GMP" OFF)
if(SYLVAN_GMP)
    find_package(GMP REQUIRED)
    target_sources(sylvan
      PRIVATE gmp.c
      PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${PROJECT_SOURCE_DIR}/include
        FILES ${PROJECT_SOURCE_DIR}/include/sylvan/gmp.h
    )
    target_link_libraries(sylvan PUBLIC GMP::GMP)
endif()

# ── Nodes table implementation ───────────────────────────────────────────────

# Choose the nodes table implementation
# Valid values: list, llchst, llcht, llht, trie
set(SYLVAN_NODES_TABLE "trie" CACHE STRING "Nodes table implementation (llht|llcht|llchst|trie|list)")
set_property(CACHE SYLVAN_NODES_TABLE PROPERTY STRINGS llht llcht llchst trie list)

# Normalize and select the source + an identifying macro
string(TOLOWER "${SYLVAN_NODES_TABLE}" _sylvan_nodes_table)
if(_sylvan_nodes_table STREQUAL "llht")
    set(_sylvan_nodes_src nodes_llht.c)
    set(_sylvan_nodes_def SYLVAN_NODES_TABLE_LLHT=1)
elseif(_sylvan_nodes_table STREQUAL "llcht")
    set(_sylvan_nodes_src nodes_llcht.c)
    set(_sylvan_nodes_def SYLVAN_NODES_TABLE_LLCHT=1)
elseif(_sylvan_nodes_table STREQUAL "llchst")
    set(_sylvan_nodes_src nodes_llchst.c)
    set(_sylvan_nodes_def SYLVAN_NODES_TABLE_LLCHST=1)
    set(SYLVAN_LLCHST_LOAD_FACTOR "2" CACHE STRING
        "Target live nodes per LLCHST head after garbage collection")
    set(SYLVAN_LLCHST_INITIAL_HEAD_LOG2 "8" CACHE STRING
        "Initial log2 head count for a newly used LLCHST subtable")
    set(SYLVAN_LLCHST_DENSE_LEVELS "65536" CACHE STRING
        "Number of directly indexed LLCHST decision descriptors")
    set(SYLVAN_LLCHST_HEAD_SLACK_LOG2 "0" CACHE STRING
        "Extra power-of-two head capacity after LLCHST garbage collection")
    if(NOT SYLVAN_LLCHST_LOAD_FACTOR MATCHES "^[1-9][0-9]*$" OR
       SYLVAN_LLCHST_LOAD_FACTOR GREATER 1048576)
        message(FATAL_ERROR "SYLVAN_LLCHST_LOAD_FACTOR must be in [1, 1048576]")
    endif()
    if(NOT SYLVAN_LLCHST_INITIAL_HEAD_LOG2 MATCHES "^[0-9]+$" OR
       SYLVAN_LLCHST_INITIAL_HEAD_LOG2 LESS 3 OR
       SYLVAN_LLCHST_INITIAL_HEAD_LOG2 GREATER 39)
        message(FATAL_ERROR "SYLVAN_LLCHST_INITIAL_HEAD_LOG2 must be in [3, 39]")
    endif()
    if(NOT SYLVAN_LLCHST_DENSE_LEVELS MATCHES "^[0-9]+$" OR
       SYLVAN_LLCHST_DENSE_LEVELS LESS 1 OR
       SYLVAN_LLCHST_DENSE_LEVELS GREATER 16777216)
        message(FATAL_ERROR "SYLVAN_LLCHST_DENSE_LEVELS must be in [1, 16777216]")
    endif()
    if(NOT SYLVAN_LLCHST_HEAD_SLACK_LOG2 MATCHES "^[0-9]+$" OR
       SYLVAN_LLCHST_HEAD_SLACK_LOG2 GREATER 15)
        message(FATAL_ERROR "SYLVAN_LLCHST_HEAD_SLACK_LOG2 must be in [0, 15]")
    endif()
    list(APPEND _sylvan_nodes_def
        SYLVAN_LLCHST_LOAD_FACTOR=${SYLVAN_LLCHST_LOAD_FACTOR}
        SYLVAN_LLCHST_INITIAL_HEAD_LOG2=${SYLVAN_LLCHST_INITIAL_HEAD_LOG2}
        SYLVAN_LLCHST_DENSE_LEVELS=${SYLVAN_LLCHST_DENSE_LEVELS}
        SYLVAN_LLCHST_HEAD_SLACK_LOG2=${SYLVAN_LLCHST_HEAD_SLACK_LOG2})
elseif(_sylvan_nodes_table STREQUAL "trie")
    set(_sylvan_nodes_src nodes_trie.c)
    set(_sylvan_nodes_def SYLVAN_NODES_TABLE_TRIE=1)
elseif(_sylvan_nodes_table STREQUAL "list")
    set(_sylvan_nodes_src nodes_list.c)
    set(_sylvan_nodes_def SYLVAN_NODES_TABLE_LIST=1)
else()
    message(FATAL_ERROR "Invalid SYLVAN_NODES_TABLE='${SYLVAN_NODES_TABLE}'. Valid: list, llchst, llcht, llht, trie")
endif()

message(STATUS "Sylvan: using '${_sylvan_nodes_table}' nodes table implementation (${_sylvan_nodes_src})")

# Add the chosen implementation and a compile definition
target_sources(sylvan PRIVATE ${_sylvan_nodes_src})
target_compile_definitions(sylvan PRIVATE ${_sylvan_nodes_def})

# ── Include directories ──────────────────────────────────────────────────────

target_include_directories(sylvan
  PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
)

# ── Compile features ────────────────────────────────────────────────────────

target_compile_features(sylvan PUBLIC c_std_11)
if(SYLVAN_BUILD_CPP)
    target_compile_features(sylvan PUBLIC cxx_std_11)
endif()

# ── Target properties ───────────────────────────────────────────────────────

# PIC: required for shared libraries, optional for static (e.g. embedding
# Sylvan in a downstream shared library).
if((BUILD_SHARED_LIBS AND NOT WIN32) OR SYLVAN_ENABLE_PIC)
    set_target_properties(sylvan PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()

# Shared library versioning (ignored for static builds)
if(BUILD_SHARED_LIBS AND NOT WIN32)
    set_target_properties(sylvan PROPERTIES
        VERSION   ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
    )
endif()

# ── Compiler flags ──────────────────────────────────────────────────────────

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sylvan PRIVATE
        -pipe
        $<$<CONFIG:Debug>:-O0 -Wall -Wextra -Wpedantic -Wvla -Werror=vla>
        $<$<BOOL:${SYLVAN_NATIVE_OPT}>:-march=native>
    )
elseif(MSVC)
    target_compile_options(sylvan
      PUBLIC
        /Zc:__STDC__
        /experimental:c11atomics
      PRIVATE
        $<$<CONFIG:Debug>:/Od /W4 /Zi>
    )
endif()

# ── Link dependencies ───────────────────────────────────────────────────────

find_package(Threads REQUIRED)
target_link_libraries(sylvan PUBLIC Threads::Threads)
target_link_libraries(sylvan PUBLIC lace::lace)
if(UNIX AND NOT APPLE)
    target_link_libraries(sylvan PUBLIC m)
endif()

# ── Hide tunables when consumed as a subproject ──────────────────────────────

if(NOT sylvan_IS_TOP_LEVEL)
    mark_as_advanced(
        SYLVAN_GMP
        SYLVAN_ENABLE_PIC
        SYLVAN_NATIVE_OPT
        SYLVAN_USE_MMAP
        SYLVAN_STATS
    )
endif()

# ── Installation ─────────────────────────────────────────────────────────────

if(NOT SYLVAN_INSTALL)
    return()
endif()

include(GNUInstallDirs)

install(TARGETS sylvan
    EXPORT sylvan-targets
    ARCHIVE          DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY          DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME          DESTINATION ${CMAKE_INSTALL_BINDIR}
    FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT sylvan-targets
    FILE        sylvan-targets.cmake
    NAMESPACE   sylvan::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
)

# ── CMake package config ─────────────────────────────────────────────────────

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
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion
)

install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/cmake/sylvan-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
)

if(SYLVAN_GMP)
    install(FILES ${PROJECT_SOURCE_DIR}/tools/FindGMP.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
    )
endif()

# ── pkg-config ───────────────────────────────────────────────────────────────

if(SYLVAN_GMP)
    set(PKGC_REQUIRES "lace >= 2.4.0 gmp")
else()
    set(PKGC_REQUIRES "lace >= 2.4.0")
endif()

# -lm is needed by consumers for static linking; Threads are already pulled
# in transitively via the lace.pc Requires line.
set(PKGC_LIBS_PRIVATE "")
if(UNIX AND NOT APPLE)
    set(PKGC_LIBS_PRIVATE "-lm")
endif()

if(NOT MSVC)
    configure_file(
        "${PROJECT_SOURCE_DIR}/tools/sylvan.pc.in"
        "${CMAKE_CURRENT_BINARY_DIR}/sylvan.pc"
        @ONLY
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/sylvan.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )
endif()
