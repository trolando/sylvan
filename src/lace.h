/*
 * This file is part of Lace, an implementation of fully-strict fine-grained task parallelism
 *
 * Lace is loosely based on ideas implemented by Karl-Filip Faxen in Wool.
 *
 * Copyright (C) 2012-2013 Tom van Dijk, University of Twente
 *
 * TODO: add BSD license
 */

#include <unistd.h>
#include <stdint.h>
#include <stdio.h>

#ifndef __LACE_H__
#define __LACE_H__

/* Some flags */

#ifndef LACE_PIE_TIMES
#define LACE_PIE_TIMES 0
#endif

#ifndef LACE_COUNT_TASKS
#define LACE_COUNT_TASKS 0
#endif

#ifndef LACE_COUNT_EVENTS
#define LACE_COUNT_EVENTS LACE_PIE_TIMES || LACE_COUNT_TASKS
#endif

/* Common code for atomic operations */

/* Processor cache line size */
#ifndef LINE_SIZE
#define LINE_SIZE 64  /* A common value for current processors */
#endif

/* Ensure a fresh memory read/write */
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

/* The size of a pointer, 8 bytes on a 64-bit architecture */
#define P_SZ (sizeof(void *))

#define PAD(x,b) ( ( (b) - ((x)%(b)) ) & ((b)-1) ) /* b must be power of 2 */
#define ROUND(x,b) ( (x) + PAD( (x), (b) ) )

#ifndef LACE_TASKSIZE
#define LACE_TASKSIZE 4*8
#endif

#if LACE_PIE_TIMES
/* Some code for event counters and timers */
typedef uint64_t hrtime_t;

static inline hrtime_t gethrtime()
{
    uint32_t hi, lo;
    asm volatile ("rdtsc" : "=a"(lo), "=d"(hi) :: "memory");
    return (uint64_t)hi<<32 | lo;
}
#endif

#if LACE_COUNT_EVENTS
void lace_count_reset();
void lace_count_report_file(FILE *file);
#endif

#if LACE_COUNT_TASKS
#define PR_COUNTTASK(s) PR_INC(s,CTR_tasks)
#else
#define PR_COUNTTASK(s) /* Empty */
#endif

#if LACE_COUNT_EVENTS
#define PR_ADD(s,i,k) ( ((s)->ctr[i])+=k )
#else
#define PR_ADD(s,i,k) /* Empty */
#endif
#define PR_INC(s,i) PR_ADD(s,i,1)

typedef enum {
    CTR_tasks=0,     /* Number of tasks spawned */
    CTR_steal_tries, /* Number of steal attempts */
    CTR_leap_tries,  /* Number of leap attempts */
    CTR_steals,      /* Number of succesful steals */
    CTR_leaps,       /* Number of succesful leaps */
    CTR_split,       /* Number of split modifications */
    CTR_splitreq,    /* Number of split requests */
    CTR_fast_sync,   /* Number of fast syncs */
    CTR_slow_sync,   /* Number of slow syncs */
    CTR_init,        /* Timer for initialization */
    CTR_close,       /* Timer for shutdown */
    CTR_wapp,        /* Timer for application code (steal) */
    CTR_lapp,        /* Timer for application code (leap) */
    CTR_wsteal,      /* Timer for steal code (steal) */
    CTR_lsteal,      /* Timer for steal code (leap) */
    CTR_wstealsucc,  /* Timer for succesful steal code (steal) */
    CTR_lstealsucc,  /* Timer for succesful steal code (leap) */
    CTR_wsignal,     /* Timer for signal after work (steal) */
    CTR_lsignal,     /* Timer for signal after work (leap) */
    CTR_MAX
} CTR_index;

struct _Worker;
struct _Task;

#define THIEF_COMPLETED ((struct _Worker*)0x1)

#define TASK_COMMON_FIELDS(type)                               \
    void (*f)(struct _Worker *, struct _Task *, struct type *);  \
struct _Worker *thief;

#define LACE_COMMON_FIELD_SIZE sizeof(struct { TASK_COMMON_FIELDS(_Task) })

typedef struct _Task {
    TASK_COMMON_FIELDS(_Task);
    char p1[PAD(LACE_COMMON_FIELD_SIZE, P_SZ)];
    char d[LACE_TASKSIZE];
    char p2[PAD(ROUND(LACE_COMMON_FIELD_SIZE, P_SZ) + LACE_TASKSIZE, LINE_SIZE)];
} Task;

typedef union __attribute__((packed)) {
    struct {
        uint32_t tail;
        uint32_t split;
    } ts;
    uint64_t v;
} TailSplit;

typedef union __attribute__((packed)) {
    struct {
        uint8_t allstolen;   // same as allstolen on thief cache line
        uint8_t movesplit;   // split required? (written by thieves)
    } o;
    uint16_t all;
} LaceFlags;

typedef struct _Worker {
    // Thief cache line
    Task *dq;
    TailSplit ts;
    uint8_t allstolen;

    char pad1[PAD(P_SZ+sizeof(TailSplit)+1, LINE_SIZE)];

    // Owner cache line
    Task *o_dq;        // same as dq
    Task *o_split;     // same as dq+ts.ts.split
    Task *o_end;       // dq+dq_size

    int16_t worker;     // what is my worker id?

    char pad2[PAD(3*P_SZ+2, LINE_SIZE)];

    LaceFlags flags;

    char pad3[PAD(sizeof(LaceFlags), LINE_SIZE)];

#if LACE_COUNT_EVENTS
    uint64_t ctr[CTR_MAX]; // counters
    volatile hrtime_t time;
    volatile int level;
#endif
} Worker;

int lace_steal(Worker *self, Task *head, Worker *victim);

/**
 * Either use lace_init and lace_exit, or use lace_boot with a callback function.
 * lace_init will start w-1 workers, lace_boot will start w workers and run the callback function in a worker.
 * Use lace_boot is recommended because there is more control over the program stack allocation then.
 */
void lace_boot(int workers, size_t dq_size, size_t stack_size, void (*function)(void));
void lace_init(int workers, size_t dq_size, size_t stack_size);
void lace_exit();
int lace_inited();
size_t lace_workers();

extern void (*lace_cb_stealing)(void);
void lace_set_callback(void (*cb)(void));

Task *lace_get_head();
Worker *lace_get_worker();

#define LACE_STOLEN   0
#define LACE_BUSY     1
#define LACE_NOWORK   2

/*
 * The DISPATCH functions are a trick to allow using
 * the macros SPAWN, SYNC, CALL in code outside Lace code.
 * Note that using SPAWN and SYNC outside Lace code is probably
 * not something you really want.
 *
 * The __lace_worker, __lace_dq_head and __lace_in_task variables
 * are usually set to appropriate values in Lace functions.
 * If using SYNC, SPAWN and CALL outside Lace functions, the default
 * values below are used and the value of __lace_in_task triggers the
 * special behavior from outside Lace functions.
 *
 * The DISPATCH functions are always inlined and due to compiler
 * optimization they do not generate any overhead.
 */

__attribute__((unused))
static const Worker *__lace_worker = NULL;
__attribute__((unused))
static const Task *__lace_dq_head = NULL;
__attribute__((unused))
static const int __lace_in_task = 0;

#define SYNC(f)           ( __lace_dq_head--, SYNC_DISPATCH_##f((Worker *)__lace_worker, (Task *)__lace_dq_head, __lace_in_task))
#define SPAWN(f, ...)     ( SPAWN_DISPATCH_##f((Worker *)__lace_worker, (Task *)__lace_dq_head, __lace_in_task, ##__VA_ARGS__), __lace_dq_head++ )
#define CALL(f, ...)      ( CALL_DISPATCH_##f((Worker *)__lace_worker, (Task *)__lace_dq_head, __lace_in_task, ##__VA_ARGS__) )
#define LACE_WORKER_ID    ( (int16_t) (__lace_worker == NULL ? lace_get_worker()->worker : __lace_worker->worker) )

#if LACE_PIE_TIMES
static void lace_time_event( Worker *w, int event )
{
    hrtime_t now = gethrtime(),
             prev = w->time;

    switch( event ) {

        // Enter application code
        case 1 :
            if(  w->level /* level */ == 0 ) {
                PR_ADD( w, CTR_init, now - prev );
                w->level = 1;
            } else if( w->level /* level */ == 1 ) {
                PR_ADD( w, CTR_wsteal, now - prev );
                PR_ADD( w, CTR_wstealsucc, now - prev );
            } else {
                PR_ADD( w, CTR_lsteal, now - prev );
                PR_ADD( w, CTR_lstealsucc, now - prev );
            }
            break;

            // Exit application code
        case 2 :
            if( w->level /* level */ == 1 ) {
                PR_ADD( w, CTR_wapp, now - prev );
            } else {
                PR_ADD( w, CTR_lapp, now - prev );
            }
            break;

            // Enter sync on stolen
        case 3 :
            if( w->level /* level */ == 1 ) {
                PR_ADD( w, CTR_wapp, now - prev );
            } else {
                PR_ADD( w, CTR_lapp, now - prev );
            }
            w->level++;
            break;

            // Exit sync on stolen
        case 4 :
            if( w->level /* level */ == 1 ) {
                fprintf( stderr, "This should not happen, level = %d\n", w->level );
            } else {
                PR_ADD( w, CTR_lsteal, now - prev );
            }
            w->level--;
            break;

            // Return from failed steal
        case 7 :
            if( w->level /* level */ == 0 ) {
                PR_ADD( w, CTR_init, now - prev );
            } else if( w->level /* level */ == 1 ) {
                PR_ADD( w, CTR_wsteal, now - prev );
            } else {
                PR_ADD( w, CTR_lsteal, now - prev );
            }
            break;

            // Signalling time
        case 8 :
            if( w->level /* level */ == 1 ) {
                PR_ADD( w, CTR_wsignal, now - prev );
                PR_ADD( w, CTR_wsteal, now - prev );
            } else {
                PR_ADD( w, CTR_lsignal, now - prev );
                PR_ADD( w, CTR_lsteal, now - prev );
            }
            break;

            // Done
        case 9 :
            if( w->level /* level */ == 0 ) {
                PR_ADD( w, CTR_init, now - prev );
            } else {
                PR_ADD( w, CTR_close, now - prev );
            }
            break;

        default: return;
    }

    w->time = now;
}
#else
#define lace_time_event( w, e ) /* Empty */
#endif



// Task macros for tasks of arity 0

#define TASK_DECL_0(RTYPE, NAME)                                                      \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  } args;                                                                 \
    RTYPE res;                                                                        \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
RTYPE NAME##_CALL(Worker *, Task * );                                                 \
static inline RTYPE NAME##_SYNC_FAST(Worker *, Task *);                               \
static RTYPE NAME##_SYNC_SLOW(Worker *, Task *);                                      \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head )                                        \
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
                                                                                      \
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
RTYPE NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head );                                            \
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head );                                            \
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ((TD_##NAME *)t)->d.res;                                                   \
}                                                                                     \
                                                                                      \
static inline                                                                         \
RTYPE NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head );                                            \
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask )                 \
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head );                                 \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() );                    \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                  \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask )                 \
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head );                                  \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() );                     \
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define TASK_IMPL_0(RTYPE, NAME)                                                      \
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
    t->d.res = NAME##_CALL(w, __dq_head );                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task );  \
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
RTYPE NAME##_CALL(Worker *w, Task *__dq_head )                                        \
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 );                                             \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task )   \
 
#define TASK_0(RTYPE, NAME) TASK_DECL_0(RTYPE, NAME) TASK_IMPL_0(RTYPE, NAME)

#define VOID_TASK_DECL_0(NAME)                                                        \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  } args;                                                                 \
                                                                                      \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
void NAME##_CALL(Worker *, Task * );                                                  \
static inline void NAME##_SYNC_FAST(Worker *, Task *);                                \
static void NAME##_SYNC_SLOW(Worker *, Task *);                                       \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head )                                        \
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
                                                                                      \
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
void NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head );                                            \
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head );                                            \
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ;                                                                          \
}                                                                                     \
                                                                                      \
static inline                                                                         \
void NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head );                                            \
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask )                 \
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head );                                 \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() );                    \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                   \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask )                  \
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head );                                  \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() );                     \
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define VOID_TASK_IMPL_0(NAME)                                                        \
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
     NAME##_CALL(w, __dq_head );                                                      \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task );   \
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
void NAME##_CALL(Worker *w, Task *__dq_head )                                         \
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 );                                             \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task )    \
 
#define VOID_TASK_0(NAME) VOID_TASK_DECL_0(NAME) VOID_TASK_IMPL_0(NAME)


// Task macros for tasks of arity 1

#define TASK_DECL_1(RTYPE, NAME, ATYPE_1)                                             \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  ATYPE_1 arg_1; } args;                                                  \
    RTYPE res;                                                                        \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
RTYPE NAME##_CALL(Worker *, Task * , ATYPE_1 arg_1);                                  \
static inline RTYPE NAME##_SYNC_FAST(Worker *, Task *);                               \
static RTYPE NAME##_SYNC_SLOW(Worker *, Task *);                                      \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head , ATYPE_1 arg_1)                         \
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
     t->d.args.arg_1 = arg_1;                                                         \
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
RTYPE NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1);                           \
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1);                           \
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ((TD_##NAME *)t)->d.res;                                                   \
}                                                                                     \
                                                                                      \
static inline                                                                         \
RTYPE NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1);                           \
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1)  \
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head , arg_1);                          \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() , arg_1);             \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                  \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1)  \
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head , arg_1);                           \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() , arg_1);              \
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define TASK_IMPL_1(RTYPE, NAME, ATYPE_1, ARG_1)                                      \
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
    t->d.res = NAME##_CALL(w, __dq_head , t->d.args.arg_1);                           \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1);\
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
RTYPE NAME##_CALL(Worker *w, Task *__dq_head , ATYPE_1 arg_1)                         \
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 , arg_1);                                      \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1 ARG_1)\
 
#define TASK_1(RTYPE, NAME, ATYPE_1, ARG_1) TASK_DECL_1(RTYPE, NAME, ATYPE_1) TASK_IMPL_1(RTYPE, NAME, ATYPE_1, ARG_1)

#define VOID_TASK_DECL_1(NAME, ATYPE_1)                                               \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  ATYPE_1 arg_1; } args;                                                  \
                                                                                      \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
void NAME##_CALL(Worker *, Task * , ATYPE_1 arg_1);                                   \
static inline void NAME##_SYNC_FAST(Worker *, Task *);                                \
static void NAME##_SYNC_SLOW(Worker *, Task *);                                       \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head , ATYPE_1 arg_1)                         \
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
     t->d.args.arg_1 = arg_1;                                                         \
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
void NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1);                           \
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1);                           \
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ;                                                                          \
}                                                                                     \
                                                                                      \
static inline                                                                         \
void NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1);                           \
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1)  \
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head , arg_1);                          \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() , arg_1);             \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                   \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1)   \
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head , arg_1);                           \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() , arg_1);              \
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define VOID_TASK_IMPL_1(NAME, ATYPE_1, ARG_1)                                        \
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
     NAME##_CALL(w, __dq_head , t->d.args.arg_1);                                     \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1);\
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
void NAME##_CALL(Worker *w, Task *__dq_head , ATYPE_1 arg_1)                          \
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 , arg_1);                                      \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1 ARG_1)\
 
#define VOID_TASK_1(NAME, ATYPE_1, ARG_1) VOID_TASK_DECL_1(NAME, ATYPE_1) VOID_TASK_IMPL_1(NAME, ATYPE_1, ARG_1)


// Task macros for tasks of arity 2

#define TASK_DECL_2(RTYPE, NAME, ATYPE_1, ATYPE_2)                                    \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  ATYPE_1 arg_1; ATYPE_2 arg_2; } args;                                   \
    RTYPE res;                                                                        \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
RTYPE NAME##_CALL(Worker *, Task * , ATYPE_1 arg_1, ATYPE_2 arg_2);                   \
static inline RTYPE NAME##_SYNC_FAST(Worker *, Task *);                               \
static RTYPE NAME##_SYNC_SLOW(Worker *, Task *);                                      \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2)          \
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
     t->d.args.arg_1 = arg_1; t->d.args.arg_2 = arg_2;                                \
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
RTYPE NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2);          \
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2);          \
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ((TD_##NAME *)t)->d.res;                                                   \
}                                                                                     \
                                                                                      \
static inline                                                                         \
RTYPE NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2);          \
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2)\
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head , arg_1, arg_2);                   \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() , arg_1, arg_2);      \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                  \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2)\
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head , arg_1, arg_2);                    \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() , arg_1, arg_2);       \
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define TASK_IMPL_2(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2)                      \
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
    t->d.res = NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2);          \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1, ATYPE_2);\
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
RTYPE NAME##_CALL(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2)          \
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 , arg_1, arg_2);                               \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1 ARG_1, ATYPE_2 ARG_2)\
 
#define TASK_2(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2) TASK_DECL_2(RTYPE, NAME, ATYPE_1, ATYPE_2) TASK_IMPL_2(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2)

#define VOID_TASK_DECL_2(NAME, ATYPE_1, ATYPE_2)                                      \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  ATYPE_1 arg_1; ATYPE_2 arg_2; } args;                                   \
                                                                                      \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
void NAME##_CALL(Worker *, Task * , ATYPE_1 arg_1, ATYPE_2 arg_2);                    \
static inline void NAME##_SYNC_FAST(Worker *, Task *);                                \
static void NAME##_SYNC_SLOW(Worker *, Task *);                                       \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2)          \
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
     t->d.args.arg_1 = arg_1; t->d.args.arg_2 = arg_2;                                \
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
void NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2);          \
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2);          \
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ;                                                                          \
}                                                                                     \
                                                                                      \
static inline                                                                         \
void NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2);          \
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2)\
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head , arg_1, arg_2);                   \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() , arg_1, arg_2);      \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                   \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2)\
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head , arg_1, arg_2);                    \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() , arg_1, arg_2);       \
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define VOID_TASK_IMPL_2(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2)                        \
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
     NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2);                    \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1, ATYPE_2);\
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
void NAME##_CALL(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2)           \
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 , arg_1, arg_2);                               \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1 ARG_1, ATYPE_2 ARG_2)\
 
#define VOID_TASK_2(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2) VOID_TASK_DECL_2(NAME, ATYPE_1, ATYPE_2) VOID_TASK_IMPL_2(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2)


// Task macros for tasks of arity 3

#define TASK_DECL_3(RTYPE, NAME, ATYPE_1, ATYPE_2, ATYPE_3)                           \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  ATYPE_1 arg_1; ATYPE_2 arg_2; ATYPE_3 arg_3; } args;                    \
    RTYPE res;                                                                        \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
RTYPE NAME##_CALL(Worker *, Task * , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3);    \
static inline RTYPE NAME##_SYNC_FAST(Worker *, Task *);                               \
static RTYPE NAME##_SYNC_SLOW(Worker *, Task *);                                      \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3)\
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
     t->d.args.arg_1 = arg_1; t->d.args.arg_2 = arg_2; t->d.args.arg_3 = arg_3;       \
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
RTYPE NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3);\
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3);\
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ((TD_##NAME *)t)->d.res;                                                   \
}                                                                                     \
                                                                                      \
static inline                                                                         \
RTYPE NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3);\
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3)\
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head , arg_1, arg_2, arg_3);            \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() , arg_1, arg_2, arg_3);\
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                  \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3)\
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head , arg_1, arg_2, arg_3);             \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() , arg_1, arg_2, arg_3);\
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define TASK_IMPL_3(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3)      \
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
    t->d.res = NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3);\
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1, ATYPE_2, ATYPE_3);\
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
RTYPE NAME##_CALL(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3)\
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 , arg_1, arg_2, arg_3);                        \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3)\
 
#define TASK_3(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3) TASK_DECL_3(RTYPE, NAME, ATYPE_1, ATYPE_2, ATYPE_3) TASK_IMPL_3(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3)

#define VOID_TASK_DECL_3(NAME, ATYPE_1, ATYPE_2, ATYPE_3)                             \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  ATYPE_1 arg_1; ATYPE_2 arg_2; ATYPE_3 arg_3; } args;                    \
                                                                                      \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
void NAME##_CALL(Worker *, Task * , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3);     \
static inline void NAME##_SYNC_FAST(Worker *, Task *);                                \
static void NAME##_SYNC_SLOW(Worker *, Task *);                                       \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3)\
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
     t->d.args.arg_1 = arg_1; t->d.args.arg_2 = arg_2; t->d.args.arg_3 = arg_3;       \
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
void NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3);\
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3);\
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ;                                                                          \
}                                                                                     \
                                                                                      \
static inline                                                                         \
void NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3);\
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3)\
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head , arg_1, arg_2, arg_3);            \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() , arg_1, arg_2, arg_3);\
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                   \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3)\
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head , arg_1, arg_2, arg_3);             \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() , arg_1, arg_2, arg_3);\
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define VOID_TASK_IMPL_3(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3)        \
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
     NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3);   \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1, ATYPE_2, ATYPE_3);\
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
void NAME##_CALL(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3)\
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 , arg_1, arg_2, arg_3);                        \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3)\
 
#define VOID_TASK_3(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3) VOID_TASK_DECL_3(NAME, ATYPE_1, ATYPE_2, ATYPE_3) VOID_TASK_IMPL_3(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3)


// Task macros for tasks of arity 4

#define TASK_DECL_4(RTYPE, NAME, ATYPE_1, ATYPE_2, ATYPE_3, ATYPE_4)                  \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  ATYPE_1 arg_1; ATYPE_2 arg_2; ATYPE_3 arg_3; ATYPE_4 arg_4; } args;     \
    RTYPE res;                                                                        \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
RTYPE NAME##_CALL(Worker *, Task * , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4);\
static inline RTYPE NAME##_SYNC_FAST(Worker *, Task *);                               \
static RTYPE NAME##_SYNC_SLOW(Worker *, Task *);                                      \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4)\
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
     t->d.args.arg_1 = arg_1; t->d.args.arg_2 = arg_2; t->d.args.arg_3 = arg_3; t->d.args.arg_4 = arg_4;\
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
RTYPE NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3, t->d.args.arg_4);\
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3, t->d.args.arg_4);\
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ((TD_##NAME *)t)->d.res;                                                   \
}                                                                                     \
                                                                                      \
static inline                                                                         \
RTYPE NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                    \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3, t->d.args.arg_4);\
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4)\
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head , arg_1, arg_2, arg_3, arg_4);     \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() , arg_1, arg_2, arg_3, arg_4);\
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                  \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4)\
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head , arg_1, arg_2, arg_3, arg_4);      \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() , arg_1, arg_2, arg_3, arg_4);\
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define TASK_IMPL_4(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4)\
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
    t->d.res = NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3, t->d.args.arg_4);\
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1, ATYPE_2, ATYPE_3, ATYPE_4);\
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
RTYPE NAME##_CALL(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4)\
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 , arg_1, arg_2, arg_3, arg_4);                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
RTYPE NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4)\
 
#define TASK_4(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4) TASK_DECL_4(RTYPE, NAME, ATYPE_1, ATYPE_2, ATYPE_3, ATYPE_4) TASK_IMPL_4(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4)

#define VOID_TASK_DECL_4(NAME, ATYPE_1, ATYPE_2, ATYPE_3, ATYPE_4)                    \
                                                                                      \
typedef struct _TD_##NAME {                                                           \
  TASK_COMMON_FIELDS(_TD_##NAME)                                                      \
  union {                                                                             \
    struct {  ATYPE_1 arg_1; ATYPE_2 arg_2; ATYPE_3 arg_3; ATYPE_4 arg_4; } args;     \
                                                                                      \
  } d;                                                                                \
} TD_##NAME;                                                                          \
                                                                                      \
void NAME##_WRAP(Worker *, Task *, TD_##NAME *);                                      \
void NAME##_CALL(Worker *, Task * , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4);\
static inline void NAME##_SYNC_FAST(Worker *, Task *);                                \
static void NAME##_SYNC_SLOW(Worker *, Task *);                                       \
                                                                                      \
static inline                                                                         \
void NAME##_SPAWN(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4)\
{                                                                                     \
    PR_COUNTTASK(w);                                                                  \
                                                                                      \
    TD_##NAME *t;                                                                     \
    TailSplit ts;                                                                     \
    uint32_t head, tail, newsplit;                                                    \
                                                                                      \
    /* assert(__dq_head < w->o_end); */ /* Assuming to be true */                     \
                                                                                      \
    t = (TD_##NAME *)__dq_head;                                                       \
    t->f = &NAME##_WRAP;                                                              \
    t->thief = 0;                                                                     \
     t->d.args.arg_1 = arg_1; t->d.args.arg_2 = arg_2; t->d.args.arg_3 = arg_3; t->d.args.arg_4 = arg_4;\
    compiler_barrier();                                                               \
                                                                                      \
    /* Using w->flags.all to check for both flags at once                             \
       will actually worsen performance! */                                           \
                                                                                      \
    if (unlikely(w->flags.o.allstolen)) {                                             \
        head = __dq_head - w->o_dq;                                                   \
        ts.ts.tail = head;                                                            \
        ts.ts.split = head+1;                                                         \
        w->ts.v = ts.v;                                                               \
        compiler_barrier();                                                           \
        w->allstolen = 0;                                                             \
        w->o_split = __dq_head+1;                                                     \
        w->flags.all = 0;                                                             \
    } else if (unlikely(w->flags.o.movesplit)) {                                      \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head + tail + 2)/2;                                               \
        w->ts.ts.split = newsplit;                                                    \
        w->o_split = w->o_dq + newsplit;                                              \
        w->flags.all = 0;                                                             \
    }                                                                                 \
}                                                                                     \
                                                                                      \
static __attribute__((noinline))                                                      \
void NAME##_SYNC_SLOW(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
    uint32_t head, tail, newsplit, oldsplit;                                          \
                                                                                      \
    if (w->flags.o.allstolen) goto lace_allstolen_##NAME;                             \
                                                                                      \
    if ((w->flags.o.movesplit)) {                                                     \
        tail = w->ts.ts.tail;                                                         \
        head = __dq_head - w->o_dq;                                                   \
        newsplit = (head+tail+1)/2;                                                   \
        oldsplit = w->ts.ts.split;                                                    \
        if (newsplit != oldsplit) {                                                   \
            w->ts.ts.split = newsplit;                                                \
            if (newsplit < oldsplit) {                                                \
                mfence();                                                             \
                tail = atomic_read(&(w->ts.ts.tail));                                 \
                if (tail > newsplit) {                                                \
                    newsplit = (head+tail+1)/2;                                       \
                    w->ts.ts.split = newsplit;                                        \
                }                                                                     \
            }                                                                         \
            w->o_split = w->o_dq+newsplit;                                            \
        }                                                                             \
        w->flags.o.movesplit=0;                                                       \
    }                                                                                 \
                                                                                      \
    if (likely(w->o_split <= __dq_head)) {                                            \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3, t->d.args.arg_4);\
    }                                                                                 \
                                                                                      \
    tail = w->ts.ts.tail;                                                             \
    head = __dq_head - w->o_dq;                                                       \
    newsplit = (head+tail+1)/2;                                                       \
    oldsplit = w->ts.ts.split;                                                        \
    if (newsplit != oldsplit) {                                                       \
        w->ts.ts.split = newsplit;                                                    \
        mfence();                                                                     \
        tail = atomic_read(&(w->ts.ts.tail));                                         \
        if (tail > newsplit) {                                                        \
            newsplit = (head+tail+1)/2;                                               \
            w->ts.ts.split = newsplit;                                                \
        }                                                                             \
        w->o_split = w->o_dq+newsplit;                                                \
    }                                                                                 \
                                                                                      \
    if (likely(newsplit <= head)) {                                                   \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3, t->d.args.arg_4);\
    }                                                                                 \
                                                                                      \
    w->allstolen = 1;                                                                 \
    w->flags.o.allstolen = 1;                                                         \
                                                                                      \
lace_allstolen_##NAME:                                                                \
                                                                                      \
    lace_time_event(w, 3);                                                            \
    t = (TD_##NAME *)__dq_head;                                                       \
    Worker *thief = t->thief;                                                         \
    if (thief != THIEF_COMPLETED) {                                                   \
        while (thief == 0) thief = atomic_read(&(t->thief));                          \
                                                                                      \
        /* Now leapfrog */                                                            \
        while (thief != THIEF_COMPLETED) {                                            \
            if (lace_steal(w, __dq_head+1, thief) == LACE_NOWORK) lace_cb_stealing(); \
            thief = atomic_read(&(t->thief));                                         \
        }                                                                             \
        w->allstolen = 1;                                                             \
        w->flags.o.allstolen = 1;                                                     \
    }                                                                                 \
                                                                                      \
    t->f = 0;                                                                         \
    lace_time_event(w, 4);                                                            \
    return ;                                                                          \
}                                                                                     \
                                                                                      \
static inline                                                                         \
void NAME##_SYNC_FAST(Worker *w, Task *__dq_head)                                     \
{                                                                                     \
    TD_##NAME *t;                                                                     \
                                                                                      \
    /* assert (head > 0); */  /* Commented out because we assume contract */          \
    if (likely(0 == w->flags.o.movesplit && w->o_split <= __dq_head)) {               \
        t = (TD_##NAME *)__dq_head;                                                   \
        t->f = 0;                                                                     \
        return NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3, t->d.args.arg_4);\
    }                                                                                 \
                                                                                      \
    return NAME##_SYNC_SLOW(w, __dq_head);                                            \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SPAWN_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4)\
{                                                                                     \
    if (__intask) return NAME##_SPAWN(w, __dq_head , arg_1, arg_2, arg_3, arg_4);     \
    else return NAME##_SPAWN(lace_get_worker(), lace_get_head() , arg_1, arg_2, arg_3, arg_4);\
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void SYNC_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask)                   \
{                                                                                     \
    if (__intask) return NAME##_SYNC_FAST(w, __dq_head);                              \
    else return NAME##_SYNC_FAST(lace_get_worker(), lace_get_head());                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void CALL_DISPATCH_##NAME(Worker *w, Task *__dq_head, int __intask , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4)\
{                                                                                     \
    if (__intask) return NAME##_CALL(w, __dq_head , arg_1, arg_2, arg_3, arg_4);      \
    else return NAME##_CALL(lace_get_worker(), lace_get_head() , arg_1, arg_2, arg_3, arg_4);\
}                                                                                     \
                                                                                      \
                                                                                      \
                                                                                      \
 
#define VOID_TASK_IMPL_4(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4)\
void NAME##_WRAP(Worker *w, Task *__dq_head, TD_##NAME *t)                            \
{                                                                                     \
     NAME##_CALL(w, __dq_head , t->d.args.arg_1, t->d.args.arg_2, t->d.args.arg_3, t->d.args.arg_4);\
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1, ATYPE_2, ATYPE_3, ATYPE_4);\
                                                                                      \
/* NAME##_WORK is inlined in NAME##_CALL and the parameter __lace_in_task will disappear */\
void NAME##_CALL(Worker *w, Task *__dq_head , ATYPE_1 arg_1, ATYPE_2 arg_2, ATYPE_3 arg_3, ATYPE_4 arg_4)\
{                                                                                     \
    return NAME##_WORK(w, __dq_head, 1 , arg_1, arg_2, arg_3, arg_4);                 \
}                                                                                     \
                                                                                      \
static inline __attribute__((always_inline))                                          \
void NAME##_WORK(Worker *__lace_worker, Task *__lace_dq_head, int __lace_in_task , ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4)\
 
#define VOID_TASK_4(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4) VOID_TASK_DECL_4(NAME, ATYPE_1, ATYPE_2, ATYPE_3, ATYPE_4) VOID_TASK_IMPL_4(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4)

#endif
