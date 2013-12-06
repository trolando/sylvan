#ifndef __ATOMICS_H
#define __ATOMICS_H

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

/* Compilerspecific branch prediction optimization */
#ifndef likely
#define likely(x)       __builtin_expect((x),1)
#define unlikely(x)     __builtin_expect((x),0)
#endif

#ifndef xinc
#define xinc(ptr) (__sync_fetch_and_add((ptr), 1))
#endif

#endif
