cmake_minimum_required(VERSION 3.14)

project(sylvan
    VERSION 1.10.0
    DESCRIPTION "Sylvan, a parallel decision diagram library"
    HOMEPAGE_URL "https://github.com/trolando/sylvan"
    LANGUAGES C CXX
)

# Check if top level
if(CMAKE_VERSION VERSION_LESS 3.21)
    if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
        set(PROJECT_IS_TOP_LEVEL ON)
    else()
        set(PROJECT_IS_TOP_LEVEL OFF)
    endif()
endif()

# Default build type
if(PROJECT_IS_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Add CMake modules path
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/")

# ── Options ──────────────────────────────────────────────────────────────────

option(BUILD_SHARED_LIBS          "Build Sylvan as a shared library"                OFF)
option(SYLVAN_NATIVE_OPT          "Enable -march=native for compiler optimizations" OFF)
option(SYLVAN_ENABLE_PIC          "Build with -fPIC (for embedding in shared libs)" OFF)
option(SYLVAN_BUILD_CPP           "Build Sylvan C++ bindings"                       ON)
option(SYLVAN_BUILD_TESTS         "Build Sylvan tests"         ${PROJECT_IS_TOP_LEVEL})
option(SYLVAN_BUILD_EXAMPLES      "Build Sylvan examples"      ${PROJECT_IS_TOP_LEVEL})
option(SYLVAN_BUILD_DOCS          "Build Sylvan documentation"                      OFF)

# Enable C++ only when something needs it
if(SYLVAN_BUILD_CPP OR SYLVAN_BUILD_TESTS OR SYLVAN_BUILD_EXAMPLES)
    enable_language(CXX)
endif()

# ── Dependencies ─────────────────────────────────────────────────────────────

# Determine if Lace needs PIC (shared Sylvan or explicit PIC request)
set(_sylvan_needs_pic OFF)
if(BUILD_SHARED_LIBS OR SYLVAN_ENABLE_PIC)
    set(_sylvan_needs_pic ON)
endif()

# First check if lace is already a target (e.g. parent project added it),
# then try find_package, then fall back to FetchContent.
if(NOT TARGET lace)
    find_package(lace 1.6.1 QUIET)
    if(NOT lace_FOUND)
        set(_saved_bsl ${BUILD_SHARED_LIBS})
        set(BUILD_SHARED_LIBS OFF)
        if(_sylvan_needs_pic)
            set(LACE_ENABLE_PIC ON CACHE BOOL "" FORCE)
        endif()

        include(FetchContent)
        FetchContent_Declare(
            lace
            GIT_REPOSITORY https://github.com/trolando/lace.git
            GIT_TAG        v1.6.1
            GIT_SHALLOW    TRUE
        )
        FetchContent_MakeAvailable(lace)

        set(BUILD_SHARED_LIBS ${_saved_bsl})
        unset(_saved_bsl)
    endif()
endif()

# Verify PIC compatibility when needed
if(_sylvan_needs_pic AND TARGET lace)
    get_target_property(_lace_pic lace POSITION_INDEPENDENT_CODE)
    if(NOT _lace_pic)
        message(FATAL_ERROR
            "Sylvan requires PIC (BUILD_SHARED_LIBS=${BUILD_SHARED_LIBS}, "
            "SYLVAN_ENABLE_PIC=${SYLVAN_ENABLE_PIC}) but the lace target "
            "was built without POSITION_INDEPENDENT_CODE. Rebuild Lace with "
            "-DCMAKE_POSITION_INDEPENDENT_CODE=ON or -DLACE_ENABLE_PIC=ON."
        )
    endif()
    unset(_lace_pic)
endif()

unset(_sylvan_needs_pic)

# ── Sanitizer support (top-level builds only) ────────────────────────────────

if(PROJECT_IS_TOP_LEVEL)
    option(SYLVAN_SANITIZE_ADDRESS "Enable AddressSanitizer"           OFF)
    option(SYLVAN_SANITIZE_THREAD  "Enable ThreadSanitizer"            OFF)
    option(SYLVAN_SANITIZE_UB      "Enable UndefinedBehaviorSanitizer" OFF)
    option(SYLVAN_SANITIZE_MEMORY  "Enable MemorySanitizer"            OFF)
 
    if(SYLVAN_SANITIZE_THREAD AND SYLVAN_SANITIZE_ADDRESS)
        message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be combined")
    endif()
    if(SYLVAN_SANITIZE_MEMORY AND SYLVAN_SANITIZE_ADDRESS)
        message(FATAL_ERROR "MemorySanitizer and AddressSanitizer cannot be combined")
    endif()
    if(SYLVAN_SANITIZE_MEMORY AND SYLVAN_SANITIZE_THREAD)
        message(FATAL_ERROR "MemorySanitizer and ThreadSanitizer cannot be combined")
    endif()
 
    set(_san "")
    if(SYLVAN_SANITIZE_ADDRESS)
        list(APPEND _san "address")
    endif()
    if(SYLVAN_SANITIZE_THREAD)
        list(APPEND _san "thread")
    endif()
    if(SYLVAN_SANITIZE_UB)
        list(APPEND _san "undefined")
    endif()
    if(SYLVAN_SANITIZE_MEMORY)
        list(APPEND _san "memory")
    endif()
 
    if(_san)
        if(MSVC)
            message(FATAL_ERROR "Sanitizers are not supported with MSVC in this project")
        endif()
        string(REPLACE ";" "," _san_list "${_san}")
        set(_san_flags "-fsanitize=${_san_list} -fno-omit-frame-pointer")
        string(APPEND CMAKE_C_FLAGS              " ${_san_flags}")
        string(APPEND CMAKE_CXX_FLAGS            " ${_san_flags}")
        string(APPEND CMAKE_EXE_LINKER_FLAGS     " ${_san_flags}")
        string(APPEND CMAKE_SHARED_LINKER_FLAGS  " ${_san_flags}")
        string(APPEND CMAKE_MODULE_LINKER_FLAGS  " ${_san_flags}")
        message(STATUS "Sanitizers enabled: ${_san_list}")
    endif()
    unset(_san)
endif()

# ── Hide options when consumed as a subproject ───────────────────────────────

if(NOT PROJECT_IS_TOP_LEVEL)
    mark_as_advanced(
        BUILD_SHARED_LIBS
        SYLVAN_ENABLE_PIC
        SYLVAN_BUILD_CPP
        SYLVAN_BUILD_TESTS
        SYLVAN_BUILD_EXAMPLES
        SYLVAN_BUILD_DOCS
    )
endif()

# ── Subdirectories ───────────────────────────────────────────────────────────

add_subdirectory(src)

if(SYLVAN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

if(SYLVAN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(SYLVAN_BUILD_DOCS)
    configure_file("docs/conf.py.in" "docs/conf.py" @ONLY)
    find_package(Sphinx REQUIRED)
    Sphinx_add_targets(sylvan
        ${CMAKE_CURRENT_BINARY_DIR}/docs
        ${CMAKE_CURRENT_SOURCE_DIR}/docs
        ${CMAKE_CURRENT_BINARY_DIR}
    )
    add_custom_target(update_gh_pages
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_MODULE_PATH}/UpdateGHPages.cmake"
    )
    add_dependencies(update_gh_pages sylvan_html)
endif()

