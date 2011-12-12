#include "sylvan_runtime.h"

#include <pthread.h>
#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

void rt_report_and_exit(int result, char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    exit(result);
}

void *rt_align(size_t align, size_t size)
{
    void *ret;
    int result = posix_memalign(&ret, align, size);
    if (result != 0) {
        switch (result) {
        case ENOMEM:
            rt_report_and_exit(1, "out of memory on allocating %d bytes aligned at %d", size, align);
        case EINVAL:
            rt_report_and_exit(1, "invalid alignment %d", align);
        default:
            rt_report_and_exit(1, "unknown error allocating %d bytes aligned at %d", size, align);
        }
    }
    assert(NULL != ret);
    return ret;
}
