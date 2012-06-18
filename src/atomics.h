#include <stdint.h>

// Some atomics (from Linux Kernel)

#ifndef __ATOMICS_H
#define __ATOMICS_H

#define ACCESS_ONCE(x) (*(volatile typeof(x) *)&(x))

#ifndef LINE_SIZE
  #define LINE_SIZE 64
#endif 

#define ALIGN(x) ((typeof(x))((((size_t)(x))+(LINE_SIZE-1))&(~((size_t)(LINE_SIZE-1)))))

static __always_inline void mfence(void)
{
    asm volatile("mfence" ::: "memory");
}

static __always_inline void barrier(void)
{
    asm volatile("" ::: "memory");
}

static __always_inline void cpu_relax(void) 
{
    asm volatile("rep; nop" ::: "memory");
}

#define __compiletime_error(a) __attribute__((error(a)))

extern void __xchg_wrong_size(void)
    __compiletime_error("Bad argument size for xchg");
extern void __cmpxchg_wrong_size(void)
    __compiletime_error("Bad argument size for cmpxchg");
extern void __xadd_wrong_size(void)
    __compiletime_error("Bad argument size for xadd");
extern void __add_wrong_size(void)
    __compiletime_error("Bad argument size for add");

/*
 * Constants for operation sizes. On 32-bit, the 64-bit size it set to
 * -1 because sizeof will never return -1, thereby making those switch
 * case statements guaranteeed dead code which the compiler will
 * eliminate, and allowing the "missing symbol in the default case" to
 * indicate a usage error.
 */
#define __X86_CASE_B    1
#define __X86_CASE_W    2
#define __X86_CASE_L    4
#ifdef CONFIG_64BIT
#define __X86_CASE_Q    8
#else
#define __X86_CASE_Q    -1      /* sizeof will never return -1 */
#endif

/* 
 * An exchange-type operation, which takes a value and a pointer, and
 * returns a the old value.
 */
#define __xchg_op(ptr, arg, op, lock)                   \
(   {                                                   \
    __typeof__ (*(ptr)) __ret = (arg);                  \
    switch (sizeof(*(ptr))) {                           \
    case __X86_CASE_B:                                  \
        asm volatile (lock #op "b %b0, %1\n"            \
                  : "+r" (__ret), "+m" (*(ptr))         \
                  : : "memory", "cc");                  \
        break;                                          \
    case __X86_CASE_W:                                  \
        asm volatile (lock #op "w %w0, %1\n"            \
                  : "+r" (__ret), "+m" (*(ptr))         \
                  : : "memory", "cc");                  \
        break;                                          \
    case __X86_CASE_L:                                  \
        asm volatile (lock #op "l %0, %1\n"             \
                  : "+r" (__ret), "+m" (*(ptr))         \
                  : : "memory", "cc");                  \
        break;                                          \
    case __X86_CASE_Q:                                  \
        asm volatile (lock #op "q %q0, %1\n"            \
                  : "+r" (__ret), "+m" (*(ptr))         \
                  : : "memory", "cc");                  \
        break;                                          \
    default:                                            \
        __ ## op ## _wrong_size();                      \
    }                                                   \
    __ret;                                              \
})

/*
 * Note: no "lock" prefix even on SMP: xchg always implies lock anyway.
 * Since this is generally used to protect other memory information, we
 * use "asm volatile" and "memory" clobbers to prevent gcc from moving
 * information around.
 */
#define xchg(ptr, v) __xchg_op((ptr), (v), xchg, "")

/*
 * xadd() adds "inc" to "*ptr" and atomically returns the previous
 * value of "*ptr".
 */
#define xadd(ptr, inc) __xchg_op((ptr), (inc), xadd, "lock ")

#define xinc(ptr) xadd(ptr, 1)
#define xdec(ptr) xadd(ptr, -1)

/*
 * Atomic compare and exchange.  Compare OLD with MEM, if identical,
 * store NEW in MEM.  Return the initial value in MEM.  Success is
 * indicated by comparing RETURN with OLD.
 */
#define cmpxchg(ptr, old, new)                                  \
(   {                                                           \
    __typeof__(*(ptr)) __ret;                                   \
    __typeof__(*(ptr)) __old = (old);                           \
    __typeof__(*(ptr)) __new = (new);                           \
    switch (sizeof(*(ptr))) {                                   \
    case __X86_CASE_B:                                          \
    {                                                           \
        volatile uint8_t *__ptr = (volatile uint8_t *)(ptr);    \
        asm volatile("lock cmpxchgb %2,%1"                     \
                 : "=a" (__ret), "+m" (*__ptr)                  \
                 : "q" (__new), "0" (__old)                     \
                 : "memory");                                   \
        break;                                                  \
    }                                                           \
    case __X86_CASE_W:                                          \
    {                                                           \
        volatile uint16_t *__ptr = (volatile uint16_t *)(ptr);  \
        asm volatile("lock cmpxchgw %2,%1"                     \
                 : "=a" (__ret), "+m" (*__ptr)                  \
                 : "r" (__new), "0" (__old)                     \
                 : "memory");                                   \
        break;                                                  \
    }                                                           \
    case __X86_CASE_L:                                          \
    {                                                           \
        volatile uint32_t *__ptr = (volatile uint32_t *)(ptr);  \
        asm volatile("lock cmpxchgl %2,%1"                     \
                 : "=a" (__ret), "+m" (*__ptr)                  \
                 : "r" (__new), "0" (__old)                     \
                 : "memory");                                   \
        break;                                                  \
    }                                                           \
    case __X86_CASE_Q:                                          \
    {                                                           \
        volatile uint64_t *__ptr = (volatile uint64_t *)(ptr);  \
        asm volatile("lock cmpxchgq %2,%1"                     \
                 : "=a" (__ret), "+m" (*__ptr)                  \
                 : "r" (__new), "0" (__old)                     \
                 : "memory");                                   \
        break;                                                  \
    }                                                           \
    default:                                                    \
        __cmpxchg_wrong_size();                                 \
    }                                                           \
    __ret;                                                      \
})

//#define cas(ptr, old, new)                                      
//({                                                              
//    register __typeof__(*(ptr)) __old = (old);                  
//    cmpxchg(ptr,__old,new) == __old ? 1 : 0;                    
//})

#define cas(ptr, old, new) __sync_bool_compare_and_swap((ptr),(old),(new))

#endif
