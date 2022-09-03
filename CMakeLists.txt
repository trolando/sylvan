cmake_minimum_required(VERSION 3.14)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_LIST_DIR}/cmake/")


# Check if we are solo or as part of a larger project
# If we're a subproject, disable testing/benchmark tools
if(DEFINED PROJECT_NAME)
  set(subproject ON)
else()
  set(subproject OFF)
endif()


project(sylvan
    VERSION 1.7.0
    DESCRIPTION "Sylvan, a parallel decision diagram library"
    HOMEPAGE_URL "https://github.com/trolando/sylvan"
    LANGUAGES C CXX
)


# Print some info
message(STATUS "CMake build configuration for Sylvan ${PROJECT_VERSION}")
message(STATUS "OS: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")


# Include a few useful CMake functionality
include(CheckSymbolExists)


# By default, build as RelWithDebInfo
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel." FORCE)
endif(NOT CMAKE_BUILD_TYPE)


# Setup dependencies
include(FetchContent)
FetchContent_Declare(
    lace
    GIT_REPOSITORY https://github.com/trolando/lace.git
    GIT_TAG        fa3ed8a1a1f117c389f1c62ad1e40aadb1ca6f43
)
FetchContent_MakeAvailable(lace)


# Set up the target library
add_library(sylvan STATIC)
add_library(sylvan::sylvan ALIAS sylvan)
set_target_properties(sylvan PROPERTIES VERSION ${sylvan_VERSION} SOVERSION ${sylvan_VERSION_MAJOR})
target_link_libraries(sylvan PUBLIC lace)
add_subdirectory(src)


# Maybe make the examples?
if(NOT subproject)
    option(SYLVAN_BUILD_EXAMPLES "Build example tools" ON)
    option(SYLVAN_BUILD_DOCS "Build documentation" OFF)
    option(SYLVAN_BUILD_TESTS "Build documentation" ON)

    # Build examples
    if(SYLVAN_BUILD_EXAMPLES)
        add_subdirectory(examples)
    endif()

    # Make documentation
    if(SYLVAN_BUILD_DOCS)
        configure_file("docs/conf.py.in" "docs/conf.py" @ONLY)
        find_package(Sphinx REQUIRED)
    Sphinx_add_targets(sylvan ${CMAKE_CURRENT_BINARY_DIR}/docs ${CMAKE_CURRENT_SOURCE_DIR}/docs ${CMAKE_CURRENT_BINARY_DIR})
        add_custom_target(update_gh_pages
                COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_MODULE_PATH}/UpdateGHPages.cmake")
        add_dependencies(update_gh_pages sylvan_html)
    endif()

    # Add tests
    if(SYLVAN_BUILD_TESTS)
        enable_testing()
        add_subdirectory(test)
    endif()

    # Setup basic CPack support
    set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${PROJECT_DESCRIPTION})
    set(CMAKE_PROJECT_HOMEPAGE_URL ${PROJECT_URL})
    set(CPACK_PACKAGE_VENDOR "Tom van Dijk")
    set(CPACK_PACKAGE_CONTACT "Tom van Dijk <tom@tvandijk.nl>")
    set(CPACK_PACKAGE_DESCRIPTION_FILE "${PROJECT_SOURCE_DIR}/README.md")
    set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")
    set(CPACK_RESOURCE_FILE_README "${PROJECT_SOURCE_DIR}/README.md")
    set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
    set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_MAJOR_VERSION})
    set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_MINOR_VERSION})
    set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_PATCH_VERSION})
    include(CPack)
endif()


