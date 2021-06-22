target_sources(Sylvan 
  PRIVATE
    lace.c
    sha2.c
    sylvan_bdd.c
    sylvan_cache.c
    sylvan_common.c
    sylvan_gmp.c
    sylvan_hash.c
    sylvan_ldd.c
    sylvan_mt.c
    sylvan_mtbdd.c
    sylvan_obj.cpp
    sylvan_refs.c
    sylvan_sl.c
    sylvan_stats.c
    sylvan_table.c
  PUBLIC
    $<INSTALL_INTERFACE:include/lace.h>
    $<INSTALL_INTERFACE:include/sylvan.h>
    $<INSTALL_INTERFACE:include/sylvan_bdd.h>
    $<INSTALL_INTERFACE:include/sylvan_cache.h>
    $<INSTALL_INTERFACE:include/sylvan_config.h>
    $<INSTALL_INTERFACE:include/sylvan_common.h>
    $<INSTALL_INTERFACE:include/sylvan_gmp.h>
    $<INSTALL_INTERFACE:include/sylvan_hash.h>
    $<INSTALL_INTERFACE:include/sylvan_int.h>
    $<INSTALL_INTERFACE:include/sylvan_ldd.h>
    $<INSTALL_INTERFACE:include/sylvan_ldd_int.h>
    $<INSTALL_INTERFACE:include/sylvan_mt.h>
    $<INSTALL_INTERFACE:include/sylvan_mtbdd.h>
    $<INSTALL_INTERFACE:include/sylvan_mtbdd_int.h>
    $<INSTALL_INTERFACE:include/sylvan_obj.hpp>
    $<INSTALL_INTERFACE:include/sylvan_stats.h>
    $<INSTALL_INTERFACE:include/sylvan_table.h>
    $<INSTALL_INTERFACE:include/sylvan_tls.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/lace.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_bdd.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_cache.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_config.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_common.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_gmp.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_hash.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_int.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_ldd.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_ldd_int.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_mt.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_mtbdd.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_mtbdd_int.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_obj.hpp>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_stats.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_table.h>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_tls.h>
)

target_compile_features(Sylvan PUBLIC c_std_11 cxx_std_11)
target_compile_options(Sylvan PRIVATE -Wall -Wextra -Werror -fno-strict-aliasing -Wno-deprecated)

target_include_directories(Sylvan
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}>
    $<INSTALL_INTERFACE:include>
)

target_link_libraries(Sylvan PUBLIC m pthread)


# We only want to include the custom MTBDD type GMP if we actually have the GMP library
find_package(GMP)
if(GMP_FOUND)
  target_sources(Sylvan 
    PRIVATE
      sylvan_gmp.c
    PUBLIC
      $<INSTALL_INTERFACE:include/sylvan_gmp.h>
      $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/sylvan_gmp.h>
  )
  target_include_directories(Sylvan PRIVATE ${GMP_INCLUDE_DIR})
  target_link_libraries(Sylvan PUBLIC ${GMP_LIBRARIES})
endif()


# Do we want to collect BDD statistics?
option(SYLVAN_STATS "Collect statistics" OFF)
if(SYLVAN_STATS)
  set_target_properties(Sylvan PROPERTIES COMPILE_DEFINITIONS "SYLVAN_STATS")
endif()


get_target_property(Sylvan_PUBLIC_HEADERS Sylvan INTERFACE_SOURCES)
set_target_properties(Sylvan PROPERTIES PUBLIC_HEADER "${Sylvan_PUBLIC_HEADERS}")


if(NOT subproject)
  include(GNUInstallDirs)
  include(CMakePackageConfigHelpers)

  install(
    TARGETS Sylvan
    EXPORT sylvan-export
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  )
  install(
    EXPORT sylvan-export
    FILE SylvanTargets.cmake
    NAMESPACE Sylvan::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sylvan
  )
  configure_file("${CMAKE_CURRENT_SOURCE_DIR}/sylvan.pc.cmake.in" "${CMAKE_CURRENT_BINARY_DIR}/sylvan.pc" @ONLY)
  install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/sylvan.pc"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig"
  )
endif()
