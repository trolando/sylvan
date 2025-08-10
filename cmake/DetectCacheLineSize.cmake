# DetectCacheLineSize.cmake
#
# Usage:
#   include(DetectCacheLineSize)
#   detect_cache_line_size(SYLVAN_CACHE_LINE_SIZE)                  # detect, cache var
#   detect_cache_line_size(MY_LINE DEFAULT 64 DEFINE)               # with default + add -DMY_LINE=<val>
#
# Notes:
# - If the cache variable is already set (e.g., -DSYLVAN_CACHE_LINE_SIZE=128),
#   it is respected and no probe is run.
# - On cross-compiling, uses DEFAULT (or 64) unless manually overridden.

function(detect_cache_line_size OUT_VAR)
  if(NOT OUT_VAR)
    message(FATAL_ERROR "detect_cache_line_size requires an output variable name")
  endif()

  set(options DEFINE)
  set(oneValueArgs DEFAULT)
  cmake_parse_arguments(DCLS "${options}" "${oneValueArgs}" "" ${ARGN})

  set(_default "64")
  if(DCLS_DEFAULT)
    set(_default "${DCLS_DEFAULT}")
  endif()

  # Respect manual override from command line cache: -D<OUT_VAR>=...
  if(DEFINED CACHE{${OUT_VAR}})
    # Optionally add compile definition
    if(DCLS_DEFINE)
      add_compile_definitions(${OUT_VAR}=$CACHE{${OUT_VAR}})
    endif()
    return()
  endif()

  if(CMAKE_CROSSCOMPILING)
    message(WARNING "Cross-compiling: defaulting ${OUT_VAR} to ${_default}. Override with -D${OUT_VAR}=…")
    set(${OUT_VAR} "${_default}" CACHE STRING "Hardware L1 data cache line size in bytes" FORCE)
    if(DCLS_DEFINE)
      add_compile_definitions(${OUT_VAR}=${_default})
    endif()
    return()
  endif()

  set(_probe "${CMAKE_BINARY_DIR}/_detect_cacheline.c")
  file(WRITE "${_probe}" [=[
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <intrin.h>
static unsigned detect_win(void){
    DWORD len = 0;
    if(!GetLogicalProcessorInformation(NULL, &len) && GetLastError()==ERROR_INSUFFICIENT_BUFFER){
        SYSTEM_LOGICAL_PROCESSOR_INFORMATION *buf = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)malloc(len);
        if(buf && GetLogicalProcessorInformation(buf, &len)){
            for (BYTE *p=(BYTE*)buf; p < (BYTE*)buf + len; ){
                SYSTEM_LOGICAL_PROCESSOR_INFORMATION *it = (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)p;
                if (it->Relationship == RelationCache) {
                    CACHE_DESCRIPTOR c = it->Cache;
                    if (c.Level == 1 && c.Type == CacheData && c.LineSize) {
                        unsigned v = (unsigned)c.LineSize;
                        free(buf);
                        return v;
                    }
                }
                p += sizeof(*it);
            }
        }
        if (buf) free(buf);
    }
    int regs[4] = {0};
    __cpuid(regs, 1);
    unsigned clflush8 = (unsigned)((regs[1] >> 8) & 0xFF);
    if (clflush8) return clflush8 * 8;
    return 0;
}
#endif

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#  include <sys/types.h>
#  include <sys/sysctl.h>
static unsigned detect_sysctl(void){
    size_t line = 0; size_t sz = sizeof(line);
    if (sysctlbyname("hw.cachelinesize", &line, &sz, NULL, 0) == 0 && line) return (unsigned)line;
#  ifdef HW_CACHELINE
    int mib[2] = { CTL_HW, HW_CACHELINE };
    sz = sizeof(line);
    if (sysctl(mib, 2, &line, &sz, NULL, 0) == 0 && line) return (unsigned)line;
#  endif
    return 0;
}
#endif

#if defined(__linux__)
#  include <unistd.h>
static unsigned detect_linux(void){
#  ifdef _SC_LEVEL1_DCACHE_LINESIZE
    long v = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    if (v > 0) return (unsigned)v;
#  endif
    const char *paths[] = {
        "/sys/devices/system/cpu/cpu0/cache/index0/coherency_line_size",
        "/sys/devices/system/cpu/cpu0/cache/index1/coherency_line_size",
        NULL
    };
    for (int i=0; paths[i]; ++i){
        FILE *f = fopen(paths[i], "r");
        if (f){
            unsigned v=0;
            if (fscanf(f, "%u", &v)==1 && v){ fclose(f); return v; }
            fclose(f);
        }
    }
    return 0;
}
#endif

int main(void){
    unsigned line = 0;
    #if defined(_WIN32)
        line = detect_win();
    #elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        line = detect_sysctl();
    #elif defined(__linux__)
        line = detect_linux();
    #endif
    if (!line) line = 64;
    printf("%u\n", line);
    return 0;
}
]=])

  try_run(_run_res _compile_res
          "${CMAKE_BINARY_DIR}" "${_probe}"
          RUN_OUTPUT_VARIABLE _cacheline_out)

  if(NOT _compile_res)
    message(WARNING "Cache line probe failed to compile; defaulting ${OUT_VAR} to ${_default}.")
    set(_cacheline_out "${_default}")
  elseif(NOT _run_res EQUAL 0)
    message(WARNING "Cache line probe failed to run; defaulting ${OUT_VAR} to ${_default}.")
    set(_cacheline_out "${_default}")
  endif()

  string(STRIP "${_cacheline_out}" _cacheline_out)
  set(${OUT_VAR} "${_cacheline_out}" CACHE STRING "Hardware L1 data cache line size in bytes" FORCE)
  message(STATUS "Detected ${OUT_VAR} = ${_cacheline_out}")

  if(DCLS_DEFINE)
    add_compile_definitions(${OUT_VAR}=${_cacheline_out})
  endif()
endfunction()

