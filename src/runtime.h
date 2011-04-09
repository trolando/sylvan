#define _GNU_SOURCE

#include <stdlib.h>

#ifndef RUNTIME
#define RUNTIME

// 6 = 64 bytes
#define CACHE_LINE 6

static const size_t CACHE_LINE_SIZE = 1 << CACHE_LINE;

#define atomic8_read(v)      (*(volatile uint8_t *)v)
#define atomic8_write(v,a)   (*(volatile uint8_t *)v = (a))

#define atomic32_read(v)      (*(volatile uint32_t *)v)
#define atomic32_write(v,a)   (*(volatile uint32_t *)v = (a))

#define atomic64_read(v)    (*(volatile uint64_t *)v)
#define atomic64_write(v,a) (*(volatile uint64_t *)v = (a))

#define cas(a,b,c)        __sync_bool_compare_and_swap(a,b,c)

void rt_report_and_exit(int result, char *format, ...);

void *rt_align(size_t align, size_t size);


#endif
