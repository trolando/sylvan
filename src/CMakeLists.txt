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

include(DetectCacheLineSize)
detect_cache_line_size(SYLVAN_CACHE_LINE_SIZE DEFAULT 64)

file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/include/sylvan")
configure_file(
    ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_config.h.in
    ${PROJECT_BINARY_DIR}/include/sylvan/sylvan_config.h
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
    sha2.c
    sylvan_bdd.c
    sylvan_cache.c
    sylvan_common.c
    sylvan_hash.c
    sylvan_ldd.c
    sylvan_mt.c
    sylvan_mtbdd.c
    sylvan_platform.c
    sylvan_refs.c
    sylvan_sl.c
    sylvan_stats.c
    sylvan_table.c
    sylvan_zdd.c
  PUBLIC
    FILE_SET HEADERS
    BASE_DIRS
      ${PROJECT_SOURCE_DIR}/include
      ${PROJECT_BINARY_DIR}/include
    FILES
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_bdd.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_cache.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_common.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_hash.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal/sylvan_int.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_ldd.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal/sylvan_ldd_int.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_mt.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_mtbdd.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal/sylvan_mtbdd_int.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_platform.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_stats.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_table.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_zdd.h
      ${PROJECT_SOURCE_DIR}/include/sylvan/internal/sylvan_zdd_int.h
      ${PROJECT_BINARY_DIR}/include/sylvan/sylvan_config.h
)

# ── Optional C++ bindings ────────────────────────────────────────────────────

if(SYLVAN_BUILD_CPP)
    target_sources(sylvan
      PRIVATE sylvan_obj.cpp
      PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${PROJECT_SOURCE_DIR}/include
        FILES ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_obj.hpp
    )
endif()

# ── Optional GMP support ─────────────────────────────────────────────────────

option(SYLVAN_GMP "Include custom MTBDD type GMP" OFF)
if(SYLVAN_GMP)
    find_package(GMP REQUIRED)
    target_sources(sylvan
      PRIVATE sylvan_gmp.c
      PUBLIC
        FILE_SET HEADERS
        BASE_DIRS ${PROJECT_SOURCE_DIR}/include
        FILES ${PROJECT_SOURCE_DIR}/include/sylvan/sylvan_gmp.h
    )
    target_link_libraries(sylvan PUBLIC GMP::GMP)
endif()

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
