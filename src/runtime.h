#define _GNU_SOURCE

#include <stdlib.h>
#include <stdint.h>

#ifndef RUNTIME
#define RUNTIME

// 6 = 64 bytes
#define CACHE_LINE 6
static const size_t CACHE_LINE_SIZE = (1 << CACHE_LINE);

#define LINE_SIZE 64 /* common value for modern processors */
#define SYLVAN_PAD(x,b) ( (b) - ( (x) & ((b)-1) ) ) /* b must be power of 2 */
#define SYLVAN_PAD_CL(x) (SYLVAN_PAD(x, LINE_SIZE))

#define atomic8_read(v)      (*(volatile uint8_t *)v)
#define atomic8_write(v,a)   ((*(volatile uint8_t *)v) = (a))

#define atomic16_read(v)      (*(volatile uint16_t *)v)
#define atomic16_write(v,a)   ((*(volatile uint16_t *)v) = (a))

#define atomic32_read(v)      (*(volatile uint32_t *)v)
#define atomic32_write(v,a)   ((*(volatile uint32_t *)v) = (a))

#define atomic64_read(v)    (*(volatile uint64_t *)v)
#define atomic64_write(v,a) ((*(volatile uint64_t *)v) = (a))

#define cas(a,b,c)        __sync_bool_compare_and_swap((a),(b),(c))

/*#define LFENCE __asm__ __volatile__( "lfence" ::: "memory" )
#define SFENCE __asm__ __volatile__( "sfence" ::: "memory" )
#define MFENCE __asm__ __volatile__( "mfence" ::: "memory" )*/
#define LFENCE asm volatile( "lfence" ::: "memory" )
#define SFENCE asm volatile( "sfence" ::: "memory" )
#define MFENCE asm volatile( "mfence" ::: "memory" )

/* Several primitives from http://locklessinc.com/articles/locks/ */

#define atomic_xadd(P, V) __sync_fetch_and_add((P), (V))
#define cmpxchg(P, O, N) __sync_val_compare_and_swap((P), (O), (N))
#define atomic_inc(P) __sync_add_and_fetch((P), 1)
#define atomic_dec(P) __sync_add_and_fetch((P), -1) 
#define atomic_add(P, V) __sync_add_and_fetch((P), (V))
#define atomic_set_bit(P, V) __sync_or_and_fetch((P), 1<<(V))
#define atomic_clear_bit(P, V) __sync_and_and_fetch((P), ~(1<<(V)))

/* Compile read-write barrier */
//#define barrier() __asm__ __volatile__("": : :"memory")
#define barrier() asm volatile("": : :"memory")

/* Pause instruction to prevent excess processor bus usage */ 
//#define cpu_relax() __asm__ __volatile__("pause\n": : :"memory")
#define cpu_relax() asm volatile("pause\n": : :"memory")

/* Test and set a bit */
static inline char atomic_bitsetandtest(void *ptr, int x)
{
	char out;
	__asm__ __volatile__("lock; bts %2,%1\n"
						"sbb %0,%0\n"
				:"=r" (out), "=m" (*(volatile long long *)ptr)
				:"Ir" (x)
				:"memory");

	return out;
}

void rt_report_and_exit(int result, char *format, ...);

void *rt_align(size_t align, size_t size);
/*
typedef union ticketlock ticketlock;

union ticketlock
{
	unsigned u;
	struct
	{
		unsigned short ticket;
		unsigned short users;
	} s;
};

static void ticket_lock(ticketlock *t)
{
	unsigned short me = atomic_xadd(&t->s.users, 1);
	
	while (t->s.ticket != me) cpu_relax();
}

static void ticket_unlock(ticketlock *t)
{
	barrier();
	t->s.ticket++;
}

static int ticket_trylock(ticketlock *t)
{
	unsigned short me = t->s.users;
	unsigned short menew = me + 1;
	unsigned cmp = ((unsigned) me << 16) + me;
	unsigned cmpnew = ((unsigned) menew << 16) + me;

	if (cmpxchg(&t->u, cmp, cmpnew) == cmp) return 0;
	
	return EBUSY;
}

static int ticket_lockable(ticketlock *t)
{
	ticketlock u = *t;
	barrier();
	return (u.s.ticket == u.s.users);
}
*/

static __attribute__((noinline)) unsigned next_pow2(unsigned x)
{
	x -= 1;
	x |= (x >> 1);
	x |= (x >> 2);
	x |= (x >> 4);
	x |= (x >> 8);
	x |= (x >> 16);
	
	return x + 1;
}






#endif
