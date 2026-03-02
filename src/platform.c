/*
 * Copyright 2026 Tom van Dijk, Formal Methods and Tools, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the License);
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
 
#if defined(__GLIBC__) && !defined(_GNU_SOURCE)
    #define _GNU_SOURCE
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sylvan/platform.h>

#if SYLVAN_USE_MMAP && !defined(_WIN32)
    #include <sys/mman.h> // for mmap
#elif defined(_WIN32)
    #include <malloc.h>
#endif

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <windows.h>
    #include <intrin.h>
#endif

#if defined(__APPLE__)
    #include <sys/sysctl.h>
#endif

#if !SYLVAN_MSVC
    #include <pthread.h>
    #include <unistd.h>
#endif

// Default cache line size
// If the actual cache line size is lower, this default value overrides it

#ifndef SYLVAN_CACHE_LINE_SIZE
    #define SYLVAN_CACHE_LINE_SIZE 64
#endif

static_assert((SYLVAN_CACHE_LINE_SIZE& (SYLVAN_CACHE_LINE_SIZE - 1)) == 0,
    "SYLVAN_CACHE_LINE_SIZE must be power of two");


static inline size_t
sylvan_detect_cache_line_size(void)
{
#if defined(_WIN32)
    DWORD bytes = 0;
    GetLogicalProcessorInformation(NULL, &bytes);
    if (bytes == 0) return SYLVAN_CACHE_LINE_SIZE;

    SYSTEM_LOGICAL_PROCESSOR_INFORMATION* buf =
        (SYSTEM_LOGICAL_PROCESSOR_INFORMATION*)malloc(bytes);
    if (!buf) return SYLVAN_CACHE_LINE_SIZE;

    if (!GetLogicalProcessorInformation(buf, &bytes)) {
        free(buf);
        return SYLVAN_CACHE_LINE_SIZE;
    }

    size_t line = 0;
    DWORD count = bytes / (DWORD)sizeof(*buf);
    for (DWORD i = 0; i < count; i++) {
        if (buf[i].Relationship == RelationCache &&
            buf[i].Cache.Level == 1 &&
            buf[i].Cache.LineSize != 0) {
            line = (size_t)buf[i].Cache.LineSize;
            break;
        }
    }

    free(buf);
    return line ? line : SYLVAN_CACHE_LINE_SIZE;

#elif defined(__APPLE__)
    size_t line = 0;
    size_t sz = sizeof(line);
    if (sysctlbyname("hw.cachelinesize", &line, &sz, NULL, 0) == 0 && line)
        return line;
    return SYLVAN_CACHE_LINE_SIZE;

#else
#if defined(_SC_LEVEL1_DCACHE_LINESIZE)
    long line = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
    return (line > 0) ? (size_t)line : SYLVAN_CACHE_LINE_SIZE;
#else
    return SYLVAN_CACHE_LINE_SIZE;
#endif
#endif
}


static size_t cache_line_size = 0;

size_t sylvan_get_cache_line_size(void)
{
    if (cache_line_size == 0) {
        size_t v = sylvan_detect_cache_line_size();
        if (v < SYLVAN_CACHE_LINE_SIZE) v = SYLVAN_CACHE_LINE_SIZE;
        cache_line_size = v;
    }
    return cache_line_size;
}


void*
sylvan_alloc_aligned(size_t size)
{
    if (size == 0) return NULL;

#if SYLVAN_USE_MMAP
    // Use virtual memory, either using mmap or with VirtualAlloc
#if defined(_WIN32)
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else 
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
#endif

#elif defined(__MINGW32__)
    void* res = __mingw_aligned_malloc(size, sylvan_get_cache_line_size());
    if (res != NULL) memset(res, 0, size);
    return res;

#elif defined(_MSC_VER) || defined(__MINGW64_VERSION_MAJOR)
    void* res = _aligned_malloc(size, sylvan_get_cache_line_size());
    if (res != NULL) memset(res, 0, size);
    return res;

#else
    void* res = NULL;
    /* posix_memalign is widely available on Linux/macOS/MSYS2 */
    if (posix_memalign(&res, sylvan_get_cache_line_size(), size) != 0) return NULL;
    memset(res, 0, size);
    return res;
#endif
}


void
sylvan_free_aligned(void* ptr, size_t size)
{
    if (!ptr) return;

#if SYLVAN_USE_MMAP
#if defined(_WIN32)
    (void)size;
    (void)VirtualFree(ptr, 0, MEM_RELEASE);
#else 
    (void)munmap(ptr, size);
#endif

#elif defined(__MINGW32__)
    (void)size;
    __mingw_aligned_free(ptr);

#elif defined(_MSC_VER) || defined(__MINGW64_VERSION_MAJOR)
    (void)size;
    _aligned_free(ptr);

#else
    (void)size;
    free(ptr);
#endif
}


void
sylvan_clear_aligned(void* ptr, size_t size)
{
    if (!ptr || size == 0) return;

#if SYLVAN_USE_MMAP&& !defined(_WIN32)
    void* res = mmap(ptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (res == MAP_FAILED) memset(ptr, 0, size);
#else
    memset(ptr, 0, size);
#endif
}

