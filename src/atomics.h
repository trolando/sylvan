#include <stdint.h>

// Some atomics (from Linux Kernel)

#ifndef __ATOMICS_H
#define __ATOMICS_H

#ifndef atomic_read
#define atomic_read(v)      (*(volatile typeof(*v) *)(v))
#define atomic_write(v,a)   (*(volatile typeof(*v) *)(v) = (a))
#endif

#ifndef LINE_SIZE
#define LINE_SIZE 64
#endif 

#ifndef ALIGN
#define ALIGN(x) ((typeof(x))((((size_t)(x))+(LINE_SIZE-1))&(~((size_t)(LINE_SIZE-1)))))
#endif

#ifndef atomic_read
#define atomic_read(v)      (*(volatile typeof(*v) *)(v))
#define atomic_write(v,a)   (*(volatile typeof(*v) *)(v) = (a))
#endif

/* Some fences */
#ifndef compiler_barrier
#define compiler_barrier() { asm volatile("" ::: "memory"); }
#endif

#ifndef mfence
#define mfence() { asm volatile("mfence" ::: "memory"); }
#endif

/* CAS operation */
#ifndef cas
#define cas(ptr, old, new) __sync_bool_compare_and_swap((ptr),(old),(new))
#endif

/* Compilerspecific branch prediction optimization */
#ifndef likely
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#endif

#ifndef xadd
#define xadd(ptr, inc) __sync_fetch_and_add((ptr), (inc))
#endif

#ifndef xinc
#define xinc(ptr) xadd(ptr, 1)
#endif

#endif
