#ifndef __ATOMICS_H
#define __ATOMICS_H

#if defined(__cplusplus) && __cplusplus >= 201103L
#define ATOMIC_READ(x) (*(volatile decltype(x) *)&(x))
#define ATOMIC_WRITE(v,a) (*(volatile decltype(v) *)(&(v)) = (a))
#else
#define ATOMIC_READ(x)  (*(volatile typeof(x) *)&(x))
#define ATOMIC_WRITE(v,a) (*(volatile  typeof(v) *)(&(v)) = (a))
#endif

/* Size of processor cache line */
#ifndef LINE_SIZE
#define LINE_SIZE 64
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
#define cas(ptr, old, new) (__sync_bool_compare_and_swap((ptr),(old),(new)))
#endif

/* Atomic add and fetch operation */
#ifndef add_fetch
#define add_fetch(a, b) __sync_add_and_fetch(&a,b)
#endif

/* Compiler specific branch prediction optimization */
#ifndef likely
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#endif

#ifndef xinc
#define xinc(ptr) (__sync_fetch_and_add((ptr), 1))
#endif

#endif
