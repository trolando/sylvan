# FindMEDDLY.cmake - Locate the MEDDLY decision diagram library
#
# Defines:
#   MEDDLY_FOUND - system has MEDDLY
#   MEDDLY_INCLUDE_DIRS - include directories
#   MEDDLY_LIBRARIES - libraries to link
#   MEDDLY::MEDDLY - imported target

find_path(MEDDLY_INCLUDE_DIR
    meddly.h
    HINTS
        ENV CMAKE_INCLUDE_PATH
        ${CMAKE_INCLUDE_PATH}
        /usr/local/include/meddly
        /usr/include/meddly
)

# Search for libmeddly.so / libmeddly.a
find_library(MEDDLY_LIBRARY
    NAMES meddly
    HINTS
        ENV CMAKE_LIBRARY_PATH
        ${CMAKE_LIBRARY_PATH}
        /usr/local/lib
        /usr/lib
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MEDDLY
    REQUIRED_VARS MEDDLY_LIBRARY MEDDLY_INCLUDE_DIR
    VERSION_VAR MEDDLY_VERSION
)

if(MEDDLY_FOUND AND NOT TARGET MEDDLY::MEDDLY)
    add_library(MEDDLY::MEDDLY UNKNOWN IMPORTED)
    set_target_properties(MEDDLY::MEDDLY PROPERTIES
        IMPORTED_LOCATION "${MEDDLY_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MEDDLY_INCLUDE_DIR}"
    )
endif()

# Backward-compatible variables
set(MEDDLY_LIBRARIES ${MEDDLY_LIBRARY})
set(MEDDLY_INCLUDE_DIRS ${MEDDLY_INCLUDE_DIR})

mark_as_advanced(MEDDLY_LIBRARY MEDDLY_INCLUDE_DIR)

