# The sifting evaluation harness: sylvan_bench, and cudd_bench when a built
# CUDD tree is available. See README.md in this directory.

if(MSVC)
    message(WARNING "The sifting harness needs POSIX (clock_gettime, strtok_r); skipping it under MSVC")
    return()
endif()

set(_sift_bench_warnings
    $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-Wall -Wextra -Werror>
)

# The BLIF reader both drivers share, so that both see the same circuit, the
# same starting order and the same gate construction sequence.
add_library(sift_bench_blif OBJECT blif.c)
target_compile_features(sift_bench_blif PRIVATE c_std_11)
target_compile_options(sift_bench_blif PRIVATE ${_sift_bench_warnings})

# ── sylvan_bench ─────────────────────────────────────────────────────────────

string(TOLOWER "${SYLVAN_NODES_TABLE}" _sift_bench_backend)

add_executable(sylvan_bench)
target_sources(sylvan_bench PRIVATE sylvan_bench.c $<TARGET_OBJECTS:sift_bench_blif>)
target_link_libraries(sylvan_bench PRIVATE sylvan::sylvan)
target_compile_features(sylvan_bench PRIVATE c_std_11)
target_compile_options(sylvan_bench PRIVATE ${_sift_bench_warnings})
# Every RESULT line names the nodes table it was measured on.
target_compile_definitions(sylvan_bench PRIVATE
    SIFT_BENCH_BACKEND="${_sift_bench_backend}"
)

# ── cudd_bench ───────────────────────────────────────────────────────────────

# cudd_bench reads dd->ddTotalNumberSwapping, so it needs cuddInt.h and the
# config.h that CUDD's configure generates, not just an installed cudd.h. Point
# CUDD_DIR at a CUDD source tree that has been configured and built in place.
set(CUDD_DIR "" CACHE PATH
    "A CUDD source tree configured and built in place (enables cudd_bench)")

set(_sift_bench_cudd_missing "")
if(NOT CUDD_DIR)
    set(_sift_bench_cudd_missing "CUDD_DIR is not set")
else()
    foreach(_f cudd/cudd.h cudd/cuddInt.h config.h util/util.h st/st.h
               mtr/mtr.h epd/epd.h cudd/.libs/libcudd.a)
        if(NOT EXISTS "${CUDD_DIR}/${_f}")
            list(APPEND _sift_bench_cudd_missing "${CUDD_DIR}/${_f}")
        endif()
    endforeach()
endif()

if(_sift_bench_cudd_missing)
    string(REPLACE ";" ", " _sift_bench_cudd_missing "${_sift_bench_cudd_missing}")
    message(STATUS "Sift harness: not building cudd_bench (${_sift_bench_cudd_missing})")
else()
    message(STATUS "Sift harness: building cudd_bench against ${CUDD_DIR}")
    add_executable(cudd_bench)
    target_sources(cudd_bench PRIVATE cudd_bench.c $<TARGET_OBJECTS:sift_bench_blif>)
    target_include_directories(cudd_bench SYSTEM PRIVATE
        "${CUDD_DIR}/cudd" "${CUDD_DIR}/util" "${CUDD_DIR}/st"
        "${CUDD_DIR}/mtr" "${CUDD_DIR}/epd" "${CUDD_DIR}"
    )
    target_link_libraries(cudd_bench PRIVATE "${CUDD_DIR}/cudd/.libs/libcudd.a" m)
    target_compile_features(cudd_bench PRIVATE c_std_11)
    target_compile_options(cudd_bench PRIVATE ${_sift_bench_warnings})
endif()
