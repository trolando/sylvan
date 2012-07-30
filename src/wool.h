#include <pthread.h>

#include "ticketlock.h"

#define SYNC_MORE 0
#ifndef COUNT_EVENTS
  #define COUNT_EVENTS 0
#endif

#ifndef FINEST_GRAIN
  #define FINEST_GRAIN 2000
#endif

#ifndef LINE_SIZE
  #define LINE_SIZE 64  /* A common value for current processors */
#endif

#define SMALL_BODY             2
#define MEDIUM_BODY          100
#define LARGE_BODY  FINEST_GRAIN

#define P_SZ (sizeof(void *))
#define I_SZ (sizeof(int))
#define L_SZ (sizeof(long int))

#define PAD(x,b) ( ( (b) - ((x)%(b)) ) & ((b)-1) ) /* b must be power of 2 */ 
#define ROUND(x,b) ( (x) + PAD( (x), (b) ) )


#ifndef TASK_PAYLOAD
  #define TASK_PAYLOAD 10*8
#endif

#if __sparc__
  #define SFENCE        asm volatile( "membar #StoreStore" )
  #define MFENCE        asm volatile( "membar #StoreLoad|#StoreStore" )
  #define PREFETCH(a)   asm ( "prefetch %0, 2" : : "m"(a) )
#elif __i386__ || __x86_64__
  #define SFENCE        asm volatile( "sfence" )
  #define MFENCE        asm volatile( "mfence" )
  #define PREFETCH(a)   /*  */
#endif

typedef struct _Worker* balarm_t;

#if COUNT_EVENTS
#define PR_ADD(s,i,k) ( ((s)->ctr[i])+= k )
#else
#define PR_ADD(s,i,k) /* Empty */
#endif
#define PR_INC(s,i)  PR_ADD(s,i,1)

#if COUNT_EVENTS_EXP
#define PR_INC_EXP(s,i) (PR_INC(s,i))
#else
#define PR_INC_EXP(s,i) /* Empty */
#endif

typedef enum {
  CTR_spawn=0,
  CTR_inlined,
  CTR_read,
  CTR_waits,
  CTR_sync_lock,
  CTR_steal_tries,
  CTR_steal_locks,
  CTR_steals,
  CTR_leap_tries,
  CTR_leap_locks,
  CTR_leaps,
  CTR_spins,
  CTR_steal_1s,
  CTR_steal_1t,
  CTR_steal_ps,
  CTR_steal_pt,
  CTR_steal_hs,
  CTR_steal_ht,
  CTR_steal_ms,
  CTR_steal_mt,
  CTR_MAX
} CTR_index;

typedef ticketlock_t Lock;

#define TASK_COMMON_FIELDS(ty)    \
  void (*f)(struct _Task *, ty);  \
  balarm_t balarm;                \
  unsigned stealable;             \
  struct _Worker *self;

#define COMMON_FIELD_SIZE sizeof( struct { TASK_COMMON_FIELDS( struct _Task * ) } )

typedef struct _Task {
  TASK_COMMON_FIELDS( struct _Task * );
  char p1[ PAD( COMMON_FIELD_SIZE, P_SZ ) ];
  char d[ TASK_PAYLOAD ];
  char p2[ PAD( ROUND( COMMON_FIELD_SIZE, P_SZ ) + TASK_PAYLOAD, LINE_SIZE ) ];
} Task;

#define WRAPPER_TYPE void (*)( struct _Task *, struct _Task * )

#define T_BUSY ((WRAPPER_TYPE) 0)
#define T_DONE ((WRAPPER_TYPE) 1)
#define T_LAST ((WRAPPER_TYPE) 1)

#define NOT_STOLEN  ( (balarm_t) 0 )
#define STOLEN_BUSY ( (balarm_t) 1 ) // Not used with LF 
#define STOLEN_DONE ( (balarm_t) 2 )
#define B_LAST      STOLEN_DONE

typedef struct _Worker {
  // First cache line, public stuff seldom written by the owner
  Task *dq_base, // Always pointing the base of the dequeue
       *dq_top,  // Not used in this version
       *dq_bot;  // The next task to steal
  Lock *dq_lock; // Mainly used for mutex among thieves, but also as backup for victim
  Lock  the_lock; // dq_lock points here
  char node; // selected NUMA node
  char pad1[ PAD( 4*P_SZ+sizeof(Lock)+1, LINE_SIZE ) ];

  // Second cache line, private stuff often written by the owner
  int  dq_size;
  unsigned int  ctr[CTR_MAX]; 
  char pad2[ PAD( I_SZ+CTR_MAX*I_SZ, LINE_SIZE ) ];
} Worker;

#define get_self( t ) ( t->self )

void  wool_sync( volatile Task *, balarm_t );
balarm_t sync_get_balarm( Task * );

void wool_set_workers(int workers);
void wool_set_stealable(int stealable);
void wool_start();

void wool_init2(int, int, int);
void wool_init( int*, char*** );
void wool_fini( void );
Task *wool_get_top( void );

#define SYNC( f )          (__dq_top--, SYNC_##f( __dq_top ) )
#define SPAWN( f, ... )    ( SPAWN_##f( __dq_top ,##__VA_ARGS__ ), __dq_top++ )
#define CALL( f, ... )     ( CALL_##f( __dq_top , ##__VA_ARGS__ ) )
#define FOR( f, ... )      ( CALL( TREE_##f , ##__VA_ARGS__ ) )
#define ROOT_CALL( f, ...) ( CALL_##f( wool_get_top() , ##__VA_ARGS__ ) )
#define ROOT_FOR( f, ... ) ( ROOT_CALL( TREE_##f , ##__VA_ARGS__ ) )



// Task definition for arity 0

#define TASK_0(RTYPE, NAME )                                          \
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    RTYPE res;                                                        \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top);                                    \
                                                                      \
void SPAWN_##NAME(Task *__dq_top)                                     \
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top );                                 \
}                                                                     \
                                                                      \
RTYPE SYNC_##NAME(Task *__dq_top)                                     \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top );                                   \
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top );                                   \
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top)                                     \
 
 
#define VOID_TASK_0(NAME)                                             \
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top);                                     \
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top)                              \
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top );                                           \
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top );                                   \
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top );                                   \
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top)                                      \

#define LOOP_BODY_0(NAME, COST, IXTY, IXNAME)                         \
                                                                      \
static unsigned long const __min_iters__##NAME                        \
   = COST > FINEST_GRAIN ? 1 : FINEST_GRAIN / ( COST ? COST : 20 );   \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME);                 \
                                                                      \
VOID_TASK_2(TREE_##NAME, IXTY, __from, IXTY, __to)                    \
{                                                                     \
  if( __to - __from <= __min_iters__##NAME ) {                        \
    IXTY __i;                                                         \
    for( __i = __from; __i < __to; __i++ ) {                          \
      LOOP_##NAME( __dq_top, __i );                                   \
    }                                                                 \
  } else {                                                            \
    IXTY __mid = (__from + __to) / 2;                                 \
    SPAWN( TREE_##NAME, __mid, __to );                                \
    CALL( TREE_##NAME, __from, __mid );                               \
    SYNC( TREE_##NAME );                                              \
  }                                                                   \
}                                                                     \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME)                  \

// Task definition for arity 1

#define TASK_1(RTYPE, NAME, ATYPE_1, ARG_1 )                          \
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1);                     \
                                                                      \
void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1)               \
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1 );                   \
}                                                                     \
                                                                      \
RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1 );                     \
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1 );                     \
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1)                      \
 
 
#define VOID_TASK_1(NAME, ATYPE_1, ARG_1 )                            \
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1);                      \
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1)               \
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1 );                             \
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1 );                     \
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1 );                     \
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1)                       \
 
 
#define LOOP_BODY_1(NAME, COST, IXTY, IXNAME, ATYPE_1, ARG_1)         \
                                                                      \
static unsigned long const __min_iters__##NAME                        \
   = COST > FINEST_GRAIN ? 1 : FINEST_GRAIN / ( COST ? COST : 20 );   \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1);  \
                                                                      \
VOID_TASK_3(TREE_##NAME, IXTY, __from, IXTY, __to, ATYPE_1, ARG_1)    \
{                                                                     \
  if( __to - __from <= __min_iters__##NAME ) {                        \
    IXTY __i;                                                         \
    for( __i = __from; __i < __to; __i++ ) {                          \
      LOOP_##NAME( __dq_top, __i, ARG_1 );                            \
    }                                                                 \
  } else {                                                            \
    IXTY __mid = (__from + __to) / 2;                                 \
    SPAWN( TREE_##NAME, __mid, __to, ARG_1 );                         \
    CALL( TREE_##NAME, __from, __mid, ARG_1 );                        \
    SYNC( TREE_##NAME );                                              \
  }                                                                   \
}                                                                     \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1)   \

// Task definition for arity 2

#define TASK_2(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2 )          \
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2);      \
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2 );     \
}                                                                     \
                                                                      \
inline RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2 );       \
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2 );       \
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2)       \
 
 
#define VOID_TASK_2(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2 )            \
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2);       \
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2 );               \
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2 );       \
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2 );       \
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2)        \
 
 
#define LOOP_BODY_2(NAME, COST, IXTY, IXNAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2)\
                                                                      \
static unsigned long const __min_iters__##NAME                        \
   = COST > FINEST_GRAIN ? 1 : FINEST_GRAIN / ( COST ? COST : 20 );   \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2);\
                                                                      \
VOID_TASK_4(TREE_##NAME, IXTY, __from, IXTY, __to, ATYPE_1, ARG_1, ATYPE_2, ARG_2)\
{                                                                     \
  if( __to - __from <= __min_iters__##NAME ) {                        \
    IXTY __i;                                                         \
    for( __i = __from; __i < __to; __i++ ) {                          \
      LOOP_##NAME( __dq_top, __i, ARG_1, ARG_2 );                     \
    }                                                                 \
  } else {                                                            \
    IXTY __mid = (__from + __to) / 2;                                 \
    SPAWN( TREE_##NAME, __mid, __to, ARG_1, ARG_2 );                  \
    CALL( TREE_##NAME, __from, __mid, ARG_1, ARG_2 );                 \
    SYNC( TREE_##NAME );                                              \
  }                                                                   \
}                                                                     \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2)\

// Task definition for arity 3

#define TASK_3(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3 );\
}                                                                     \
                                                                      \
inline RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3)\
 
 
#define VOID_TASK_3(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3 ); \
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3)\
 
 
#define LOOP_BODY_3(NAME, COST, IXTY, IXNAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3)\
                                                                      \
static unsigned long const __min_iters__##NAME                        \
   = COST > FINEST_GRAIN ? 1 : FINEST_GRAIN / ( COST ? COST : 20 );   \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3);\
                                                                      \
VOID_TASK_5(TREE_##NAME, IXTY, __from, IXTY, __to, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3)\
{                                                                     \
  if( __to - __from <= __min_iters__##NAME ) {                        \
    IXTY __i;                                                         \
    for( __i = __from; __i < __to; __i++ ) {                          \
      LOOP_##NAME( __dq_top, __i, ARG_1, ARG_2, ARG_3 );              \
    }                                                                 \
  } else {                                                            \
    IXTY __mid = (__from + __to) / 2;                                 \
    SPAWN( TREE_##NAME, __mid, __to, ARG_1, ARG_2, ARG_3 );           \
    CALL( TREE_##NAME, __from, __mid, ARG_1, ARG_2, ARG_3 );          \
    SYNC( TREE_##NAME );                                              \
  }                                                                   \
}                                                                     \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3)\

// Task definition for arity 4

#define TASK_4(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4 );\
}                                                                     \
                                                                      \
inline RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4)\
 
 
#define VOID_TASK_4(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4 );\
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4)\
 
 
#define LOOP_BODY_4(NAME, COST, IXTY, IXNAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4)\
                                                                      \
static unsigned long const __min_iters__##NAME                        \
   = COST > FINEST_GRAIN ? 1 : FINEST_GRAIN / ( COST ? COST : 20 );   \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4);\
                                                                      \
VOID_TASK_6(TREE_##NAME, IXTY, __from, IXTY, __to, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4)\
{                                                                     \
  if( __to - __from <= __min_iters__##NAME ) {                        \
    IXTY __i;                                                         \
    for( __i = __from; __i < __to; __i++ ) {                          \
      LOOP_##NAME( __dq_top, __i, ARG_1, ARG_2, ARG_3, ARG_4 );       \
    }                                                                 \
  } else {                                                            \
    IXTY __mid = (__from + __to) / 2;                                 \
    SPAWN( TREE_##NAME, __mid, __to, ARG_1, ARG_2, ARG_3, ARG_4 );    \
    CALL( TREE_##NAME, __from, __mid, ARG_1, ARG_2, ARG_3, ARG_4 );   \
    SYNC( TREE_##NAME );                                              \
  }                                                                   \
}                                                                     \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4)\

// Task definition for arity 5

#define TASK_5(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5 );\
}                                                                     \
                                                                      \
inline RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5)\
 
 
#define VOID_TASK_5(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5 );\
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5)\
 
 
#define LOOP_BODY_5(NAME, COST, IXTY, IXNAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5)\
                                                                      \
static unsigned long const __min_iters__##NAME                        \
   = COST > FINEST_GRAIN ? 1 : FINEST_GRAIN / ( COST ? COST : 20 );   \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5);\
                                                                      \
VOID_TASK_7(TREE_##NAME, IXTY, __from, IXTY, __to, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5)\
{                                                                     \
  if( __to - __from <= __min_iters__##NAME ) {                        \
    IXTY __i;                                                         \
    for( __i = __from; __i < __to; __i++ ) {                          \
      LOOP_##NAME( __dq_top, __i, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5 );\
    }                                                                 \
  } else {                                                            \
    IXTY __mid = (__from + __to) / 2;                                 \
    SPAWN( TREE_##NAME, __mid, __to, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5 );\
    CALL( TREE_##NAME, __from, __mid, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5 );\
    SYNC( TREE_##NAME );                                              \
  }                                                                   \
}                                                                     \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5)\

// Task definition for arity 6

#define TASK_6(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6 );\
}                                                                     \
                                                                      \
inline RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6)\
 
 
#define VOID_TASK_6(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6 );\
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6)\
 
 
#define LOOP_BODY_6(NAME, COST, IXTY, IXNAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6)\
                                                                      \
static unsigned long const __min_iters__##NAME                        \
   = COST > FINEST_GRAIN ? 1 : FINEST_GRAIN / ( COST ? COST : 20 );   \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6);\
                                                                      \
VOID_TASK_8(TREE_##NAME, IXTY, __from, IXTY, __to, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6)\
{                                                                     \
  if( __to - __from <= __min_iters__##NAME ) {                        \
    IXTY __i;                                                         \
    for( __i = __from; __i < __to; __i++ ) {                          \
      LOOP_##NAME( __dq_top, __i, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5, ARG_6 );\
    }                                                                 \
  } else {                                                            \
    IXTY __mid = (__from + __to) / 2;                                 \
    SPAWN( TREE_##NAME, __mid, __to, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5, ARG_6 );\
    CALL( TREE_##NAME, __from, __mid, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5, ARG_6 );\
    SYNC( TREE_##NAME );                                              \
  }                                                                   \
}                                                                     \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6)\

// Task definition for arity 7

#define TASK_7(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
      ATYPE_7 ARG_7;                                                  \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  p->d.a.ARG_7 = ARG_7;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7 );\
}                                                                     \
                                                                      \
inline RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7)\
 
 
#define VOID_TASK_7(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
      ATYPE_7 ARG_7;                                                  \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  p->d.a.ARG_7 = ARG_7;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7 );\
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7)\
 
 
#define LOOP_BODY_7(NAME, COST, IXTY, IXNAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7)\
                                                                      \
static unsigned long const __min_iters__##NAME                        \
   = COST > FINEST_GRAIN ? 1 : FINEST_GRAIN / ( COST ? COST : 20 );   \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7);\
                                                                      \
VOID_TASK_9(TREE_##NAME, IXTY, __from, IXTY, __to, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7)\
{                                                                     \
  if( __to - __from <= __min_iters__##NAME ) {                        \
    IXTY __i;                                                         \
    for( __i = __from; __i < __to; __i++ ) {                          \
      LOOP_##NAME( __dq_top, __i, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5, ARG_6, ARG_7 );\
    }                                                                 \
  } else {                                                            \
    IXTY __mid = (__from + __to) / 2;                                 \
    SPAWN( TREE_##NAME, __mid, __to, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5, ARG_6, ARG_7 );\
    CALL( TREE_##NAME, __from, __mid, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5, ARG_6, ARG_7 );\
    SYNC( TREE_##NAME );                                              \
  }                                                                   \
}                                                                     \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7)\

// Task definition for arity 8

#define TASK_8(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7, ATYPE_8, ARG_8 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
      ATYPE_7 ARG_7;                                                  \
      ATYPE_8 ARG_8;                                                  \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  p->d.a.ARG_7 = ARG_7;                                               \
  p->d.a.ARG_8 = ARG_8;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8 );\
}                                                                     \
                                                                      \
inline RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8)\
 
 
#define VOID_TASK_8(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7, ATYPE_8, ARG_8 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
      ATYPE_7 ARG_7;                                                  \
      ATYPE_8 ARG_8;                                                  \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  p->d.a.ARG_7 = ARG_7;                                               \
  p->d.a.ARG_8 = ARG_8;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8 );\
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8)\
 
 
#define LOOP_BODY_8(NAME, COST, IXTY, IXNAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7, ATYPE_8, ARG_8)\
                                                                      \
static unsigned long const __min_iters__##NAME                        \
   = COST > FINEST_GRAIN ? 1 : FINEST_GRAIN / ( COST ? COST : 20 );   \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8);\
                                                                      \
VOID_TASK_10(TREE_##NAME, IXTY, __from, IXTY, __to, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7, ATYPE_8, ARG_8)\
{                                                                     \
  if( __to - __from <= __min_iters__##NAME ) {                        \
    IXTY __i;                                                         \
    for( __i = __from; __i < __to; __i++ ) {                          \
      LOOP_##NAME( __dq_top, __i, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5, ARG_6, ARG_7, ARG_8 );\
    }                                                                 \
  } else {                                                            \
    IXTY __mid = (__from + __to) / 2;                                 \
    SPAWN( TREE_##NAME, __mid, __to, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5, ARG_6, ARG_7, ARG_8 );\
    CALL( TREE_##NAME, __from, __mid, ARG_1, ARG_2, ARG_3, ARG_4, ARG_5, ARG_6, ARG_7, ARG_8 );\
    SYNC( TREE_##NAME );                                              \
  }                                                                   \
}                                                                     \
                                                                      \
inline void LOOP_##NAME(Task *__dq_top, IXTY IXNAME, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8)\

// Task definition for arity 9

#define TASK_9(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7, ATYPE_8, ARG_8, ATYPE_9, ARG_9 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
      ATYPE_7 ARG_7;                                                  \
      ATYPE_8 ARG_8;                                                  \
      ATYPE_9 ARG_9;                                                  \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  p->d.a.ARG_7 = ARG_7;                                               \
  p->d.a.ARG_8 = ARG_8;                                               \
  p->d.a.ARG_9 = ARG_9;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9 );\
}                                                                     \
                                                                      \
inline RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9)\
 
 
#define VOID_TASK_9(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7, ATYPE_8, ARG_8, ATYPE_9, ARG_9 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
      ATYPE_7 ARG_7;                                                  \
      ATYPE_8 ARG_8;                                                  \
      ATYPE_9 ARG_9;                                                  \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  p->d.a.ARG_7 = ARG_7;                                               \
  p->d.a.ARG_8 = ARG_8;                                               \
  p->d.a.ARG_9 = ARG_9;                                               \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9 );\
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9)\
 
 

// Task definition for arity 10

#define TASK_10(RTYPE, NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7, ATYPE_8, ARG_8, ATYPE_9, ARG_9, ATYPE_10, ARG_10 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
      ATYPE_7 ARG_7;                                                  \
      ATYPE_8 ARG_8;                                                  \
      ATYPE_9 ARG_9;                                                  \
      ATYPE_10 ARG_10;                                                \
    } a;                                                              \
    RTYPE res;                                                        \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9, ATYPE_10 ARG_10);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9, ATYPE_10 ARG_10)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  p->d.a.ARG_7 = ARG_7;                                               \
  p->d.a.ARG_8 = ARG_8;                                               \
  p->d.a.ARG_9 = ARG_9;                                               \
  p->d.a.ARG_10 = ARG_10;                                             \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
  t->d.res = CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9, t->d.a.ARG_10 );\
}                                                                     \
                                                                      \
inline RTYPE SYNC_##NAME(Task *__dq_top)                              \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9, t->d.a.ARG_10 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9, t->d.a.ARG_10 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ( (TD_##NAME *) q )->d.res;                                \
  }                                                                   \
}                                                                     \
                                                                      \
RTYPE CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9, ATYPE_10 ARG_10)\
 
 
#define VOID_TASK_10(NAME, ATYPE_1, ARG_1, ATYPE_2, ARG_2, ATYPE_3, ARG_3, ATYPE_4, ARG_4, ATYPE_5, ARG_5, ATYPE_6, ARG_6, ATYPE_7, ARG_7, ATYPE_8, ARG_8, ATYPE_9, ARG_9, ATYPE_10, ARG_10 )\
                                                                      \
typedef struct _TD_##NAME {                                           \
  TASK_COMMON_FIELDS( struct _TD_##NAME * )                           \
  union {                                                             \
    struct {                                                          \
      ATYPE_1 ARG_1;                                                  \
      ATYPE_2 ARG_2;                                                  \
      ATYPE_3 ARG_3;                                                  \
      ATYPE_4 ARG_4;                                                  \
      ATYPE_5 ARG_5;                                                  \
      ATYPE_6 ARG_6;                                                  \
      ATYPE_7 ARG_7;                                                  \
      ATYPE_8 ARG_8;                                                  \
      ATYPE_9 ARG_9;                                                  \
      ATYPE_10 ARG_10;                                                \
    } a;                                                              \
                                                                      \
  } d;                                                                \
} TD_##NAME;                                                          \
                                                                      \
static void WRAP_##NAME(Task *, TD_##NAME *);                         \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9, ATYPE_10 ARG_10);\
                                                                      \
inline void SPAWN_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9, ATYPE_10 ARG_10)\
{                                                                     \
  TD_##NAME *p = (TD_##NAME *) __dq_top;                              \
  p->d.a.ARG_1 = ARG_1;                                               \
  p->d.a.ARG_2 = ARG_2;                                               \
  p->d.a.ARG_3 = ARG_3;                                               \
  p->d.a.ARG_4 = ARG_4;                                               \
  p->d.a.ARG_5 = ARG_5;                                               \
  p->d.a.ARG_6 = ARG_6;                                               \
  p->d.a.ARG_7 = ARG_7;                                               \
  p->d.a.ARG_8 = ARG_8;                                               \
  p->d.a.ARG_9 = ARG_9;                                               \
  p->d.a.ARG_10 = ARG_10;                                             \
  if( p->stealable ) {                                                \
    SFENCE;                                                           \
  }                                                                   \
  p->f     = &WRAP_##NAME;                                            \
}                                                                     \
                                                                      \
static void WRAP_##NAME(Task *__dq_top, TD_##NAME *t)                 \
{                                                                     \
   CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9, t->d.a.ARG_10 );\
}                                                                     \
                                                                      \
inline void SYNC_##NAME(Task *__dq_top)                               \
{                                                                     \
  Task *q = __dq_top;                                                 \
  balarm_t a;                                                         \
                                                                      \
  if( ! q->stealable ) {                                              \
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9, t->d.a.ARG_10 );\
  }                                                                   \
  q->f = T_BUSY;                                                      \
  MFENCE;                                                             \
  a = q->balarm;                                                      \
                                                                      \
  if( a == NOT_STOLEN || ( a = sync_get_balarm( q ) ) == NOT_STOLEN ) {\
    TD_##NAME *t = (TD_##NAME *) q; /* Used in TASK_GET_FROM_t */     \
    /* Not stolen, nobody else might be using it */                   \
    PR_INC( get_self( q ), CTR_inlined );                             \
    return CALL_##NAME( __dq_top, t->d.a.ARG_1, t->d.a.ARG_2, t->d.a.ARG_3, t->d.a.ARG_4, t->d.a.ARG_5, t->d.a.ARG_6, t->d.a.ARG_7, t->d.a.ARG_8, t->d.a.ARG_9, t->d.a.ARG_10 );\
  } else {                                                            \
    wool_sync( __dq_top, a );                                         \
    return ;                                                          \
  }                                                                   \
}                                                                     \
                                                                      \
void CALL_##NAME(Task *__dq_top, ATYPE_1 ARG_1, ATYPE_2 ARG_2, ATYPE_3 ARG_3, ATYPE_4 ARG_4, ATYPE_5 ARG_5, ATYPE_6 ARG_6, ATYPE_7 ARG_7, ATYPE_8 ARG_8, ATYPE_9 ARG_9, ATYPE_10 ARG_10)\
 
 
