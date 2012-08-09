/*
   This file is part of Wool, a library for fine-grained independent 
   task parallelism

   Copyright (C) 2009- Karl-Filip Faxen
      kff@sics.se

   Modified 2012 Tom van Dijk

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the
   License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307, USA.

   The GNU General Public License is contained in the file COPYING.
*/

#include "config.h"
#include "wool.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_NUMA_H
#include <numa.h>
#include "setnuma.h"
#endif

/* Implements 
   leapfrogging, 
   private tasks (fixed depth),
   peek (check stealable before locking),
   trylock (steal from other worker if other thief busy),
   linear stealing (rather than always random)

   Does not implement
   Resizing or overflow checking of task pool
*/

#define SO_STOLE 0
#define SO_BUSY  1
#define SO_NO_WORK 2

#ifndef STEAL_TRYLOCK
  #define STEAL_TRYLOCK 1
#endif

#ifndef STEAL_PEEK
  #define STEAL_PEEK 1
#endif

#ifndef INIT_WORKER_DQ_SIZE
  #define INIT_WORKER_DQ_SIZE 1000
#endif

static Worker **workers;
static int init_worker_dq_size = INIT_WORKER_DQ_SIZE,
           n_workers = 0;
static int backoff_mode = 50; // No of iterations of waiting after a failed leap
static int n_stealable=0;

static int steal( Worker *, Worker *, Task * );

static int more_work = 1;
static pthread_t *ts = NULL;

#if SYNC_MORE
static Lock more_lock = 0;
#endif

static pthread_attr_t worker_attr;
static pthread_key_t worker_key;

static wool_cb_stealing cb_stealing;

int lock_delay = 10;

#define lock(a) ticketlock_lock(a)
#define unlock(a) ticketlock_unlock(a)
#define trylock(a) ticketlock_trylock(a)

balarm_t sync_get_balarm( Task *t )
{
  Worker   *self;
  balarm_t  a;

  self = get_self( t );
  lock( self->dq_lock );
    a = t->balarm;
  unlock( self->dq_lock );
  PR_INC( self, CTR_sync_lock );
  return a;
}

static void spin( Worker *self, int n )
{
  int i,s=0;

  // This code should hopefully confuse every optimizer. 
  for( i=1; i<=n; i++ ) {
    s |= i;
  }
  if( s > 0 ) {
    PR_ADD( self, CTR_spins, n );
  }
}

static int myrand( unsigned int *seedp, int max );

static void steal_random( Worker *_self, Worker *thief, volatile Task *t )
{
  Worker **victim = workers, **self = workers;

  // Find self
  while (*self != _self) self++; // TODO: this is dangerous!

  // Find thief
  while (*victim != thief) victim++; // TODO: this is dangerous!

  unsigned int self_idx = self - workers;
  unsigned int seed = self_idx;
  unsigned int n = n_workers;
  int i=0; 

  while (t->balarm != STOLEN_DONE) { 
    // Computing a random number for every steal is too slow, so we do some amount of
    // sequential scanning of the workers and only randomize once in a while, just 
    // to be sure.

    if( i>0 ) {
      i--;
      victim ++;
      // A couple of if's is faster than a %...
      if( victim == self ) victim++;
      if( victim >= workers + n ) victim = workers;
    } else {
      i = myrand( &seed, 40 );
      victim = workers + ( myrand( &seed, n-1 ) + self_idx + 1 ) % n;
    }

    if (steal(*self, *victim, (Task*)t+1) != SO_STOLE) {
        if (cb_stealing != NULL) cb_stealing();
    }
  }
}



void wool_sync( volatile Task *t, balarm_t a )
{
  Worker *self = get_self( t );

  lock( self->dq_lock );
    if( a == STOLEN_DONE || t->balarm == STOLEN_DONE ) {

      /* Stolen and completed */
      PR_INC( self, CTR_read );
      /* Do nothing */

    } else if( a > B_LAST ) {

      /* Stolen and in progress; let's leapfrog! */
      int done = 0;
      Worker *thief = a;

      unlock( self->dq_lock ); 

      PR_INC( self, CTR_waits ); // It isn't waiting any more, though ...

      /* Now leapfrog */   

      do {
        int steal_outcome;

        PR_INC( self, CTR_leap_tries );
        steal_outcome = steal( self, thief, (Task *) t + 1 );
        if( steal_outcome != SO_BUSY ) {
           PR_INC( self, CTR_leap_locks );
        }
        if( steal_outcome == SO_STOLE ) {
          PR_INC( self, CTR_leaps );
        } else {
          spin( self, backoff_mode );
          // Oh , just go steal somewhere else then
          steal_random(self, thief, t);
        }
        if( t->balarm == STOLEN_DONE ) { // Leapfrogging is over!
          done = 1;
        }
      } while( !done );

      lock( self->dq_lock );

    } else {
      fprintf( stderr, "Unknown task state %lu in sync\n", (unsigned long) a );
      exit( 1 );
    }
    self->dq_bot --;
    t->balarm = NOT_STOLEN;

  unlock( self->dq_lock );
  
}

static void init_worker( int worker, char node )
{
  int i;

  register int dq_size = init_worker_dq_size;
  Worker *w;

#ifdef HAVE_NUMA_H
  // Use NUMA allocator...
  if (node>=0) {
    workers[worker] = w = (Worker*)numa_alloc_onnode(sizeof(Worker), node);
    w->dq_base = (Task*)numa_alloc_onnode(dq_size * sizeof(Task), node);
  } else {
#endif
    posix_memalign((void**)&w, LINE_SIZE, sizeof(Worker));
    workers[worker] = w;
    posix_memalign((void**)&w->dq_base, LINE_SIZE, dq_size * sizeof(Task));
#ifdef HAVE_NUMA_H
  }
#endif

  w->dq_size = dq_size;
  w->node = node;

  for( i=0; i < w->dq_size; i++ ) {
    w->dq_base[i].f = T_BUSY;
    w->dq_base[i].balarm = NOT_STOLEN;
    w->dq_base[i].stealable = i < n_stealable ? 1 : 0;
    w->dq_base[i].self = w;
  }
  w->dq_bot = w->dq_base;
  w->dq_top = w->dq_base; // Not used, really
  w->dq_lock = &( w->the_lock );
  w->dq_lock->u = 0;

  for( i=0; i < CTR_MAX; i++ ) {
    w->ctr[i] = 0;
  }
}


static int steal( Worker *self, Worker *victim, Task *dq_top )
{
  volatile Task *tp;
  void (*f) (Task *, Task *) = NULL; // To suppress a warning!

#if STEAL_PEEK
  tp = victim->dq_bot;
  if( tp->balarm != NOT_STOLEN || tp->f <= T_LAST || !(tp->stealable) ) {
    return SO_NO_WORK;
  }
#else
  tp = victim->dq_bot;
  if( ! (tp->stealable) ) {
    return SO_NO_WORK;
  }
#endif

  PREFETCH( tp->balarm ); // Start getting exclusive access

#if STEAL_TRYLOCK
  if( trylock( victim->dq_lock ) != 0 ) {
    return SO_BUSY;
  }
#else
  lock( victim->dq_lock );
#endif
  // now locked!
    tp = victim->dq_bot;  // Yes, we need to reread after aquiring lock!

    // The victim might have sync'ed or somebody else might have stolen
    // while we were obtaining the lock;
    // no point in getting exclusive access in that case.
    if( tp->stealable && tp->balarm == NOT_STOLEN && tp->f > T_LAST  ) { 
      tp->balarm = self;
      MFENCE;
      f = tp->f;
      if( f > T_LAST ) {  // Check again after the fence!
        victim->dq_bot ++;
      } else {
        tp->balarm = NOT_STOLEN;
        tp = NULL;
      }
    } else {
      tp = NULL; // Already stolen by someone else
    }
  unlock( victim->dq_lock );
   
  if( tp != NULL ) {

    f( dq_top, (Task *) tp ); 

      // instead of locking, so that the return value is really updated
      // the return value is already written by the wrapper (f)
      SFENCE;
      tp->balarm = STOLEN_DONE;

    return SO_STOLE;
  }
  return SO_NO_WORK;
}

static void *do_work( void * );

static int myrand( unsigned int *seedp, int max )
{
  return rand_r( seedp ) % max;
}

int rand_interval = 40; // By default, scan sequentially for 0..39 attempts

static void *do_work( void *arg )
{
  Worker **self = (Worker **) arg, **victim = NULL;
  unsigned int self_idx = self - workers;
  unsigned int seed = self_idx;
  unsigned int n = n_workers;
  int more, i=0;
  int attempts = 0;

  // Register as a worker
  
#ifdef HAVE_NUMA_H
  if ((*self)->node>=0) {
    numa_run_on_node((*self)->node);
  }
#endif

  pthread_setspecific( worker_key, *self );

  victim = self;

  do {
    int steal_outcome;

    // Computing a random number for every steal is too slow, so we do some amount of
    // sequential scanning of the workers and only randomize once in a while, just 
    // to be sure.

    if( i>0 ) {
      i--;
      victim ++;
      // A couple of if's is faster than a %...
      if( victim == self ) victim++;
      if( victim >= workers + n ) victim = workers;
    } else {
      i = myrand( &seed, rand_interval );
      victim = workers + ( myrand( &seed, n-1 ) + self_idx + 1 ) % n;
    }

    PR_INC( *self, CTR_steal_tries );

    steal_outcome = steal( *self, *victim, (*self)->dq_base );
    attempts++;
    if( steal_outcome != SO_BUSY ) {
       PR_INC( *self, CTR_steal_locks );
    }
    if( steal_outcome == SO_STOLE ) {
      PR_INC( *self, CTR_steals );

      // Ok, this is clunky, but the idea is to record if we had to try many times
      // before we managed to steal.

      if( attempts == 1 ) {
        PR_INC( *self, CTR_steal_1s );
        PR_ADD( *self, CTR_steal_1t, attempts );
      } else if( attempts < n_workers ) {
        PR_INC( *self, CTR_steal_ps );
        PR_ADD( *self, CTR_steal_pt, attempts );
      } else if( attempts < 3*n_workers ) {
        PR_INC( *self, CTR_steal_hs );
        PR_ADD( *self, CTR_steal_ht, attempts );
      } else {
        PR_INC( *self, CTR_steal_ms );
        PR_ADD( *self, CTR_steal_mt, attempts );
      }
      attempts = 0;
    }
    else { if (cb_stealing != NULL) cb_stealing(); }

#if SYNC_MORE
    lock( &more_lock );
      more = more_work;
    unlock( &more_lock );
#else
    more = more_work;
#endif
  } while( more );

  return NULL;
  }

static void start_workers( void )
{
  int i, n = n_workers;
  more_work =1;

  if( n_stealable == 0 ) {
    n_stealable = 3;
    for( i=n_workers; i>0; i >>= 1 ) {
      n_stealable++;
    }
    if( n_workers == 1 ) {
      n_stealable = 0;
    }
  }

  posix_memalign((void**)&workers, LINE_SIZE, n*sizeof(Worker*));
  ts      = (pthread_t *) malloc( (n-1) * sizeof(pthread_t) );

  pthread_attr_init( &worker_attr );
  pthread_attr_setscope( &worker_attr, PTHREAD_SCOPE_SYSTEM );

  pthread_key_create( &worker_key, NULL );

#ifdef HAVE_NUMA_H
  if (numa_available()!=-1) {
    int8_t *best = setnuma_calculate_best(n);
    if (best == 0) { printf("Error: no suitable NUMA config found!\n"); exit(-1); }
    int nodes;
    for (nodes=0;best[nodes]!=-1;nodes++) {}
    for( i=0; i<n; i++ ) {
      init_worker(i, best[i%nodes]); 
      //printf("Assigned node %d to worker %d\n", best[i%nodes], i);
    }
    free(best);
  } else {
    printf("Warning: NUMA not available!\n");
#endif
    for( i=0; i<n; i++ ) {
      init_worker(i, -1);
    }
#ifdef HAVE_NUMA_H
  }
#endif

  for( i=0; i < n-1; i++ ) {
    pthread_create( ts+i, &worker_attr, &do_work, workers+i+1 );
  }
 
#ifdef HAVE_NUMA_H
  // Pin ourself
  if ((*workers)->node>=0) {
    numa_run_on_node((*workers)->node);
  }
#endif

  pthread_setspecific( worker_key, *workers );
}

#if COUNT_EVENTS

char *ctr_h[] = {
  "    Spawns",
  "   Inlined",
  "   Read",
  "   Wait",
  NULL, // "Sync lck",
  "St tries",
  NULL, // "St locks",
  " Steals",
  " L tries",
  " L locks",
  "  Leaps",
  "     Spins",
  NULL, // "1 steal",
  NULL, // " 1 tries",
  NULL, // "p steal",
  NULL, // " p tries",
  NULL, // "h steal",
  NULL, // " h tries",
  NULL, // "m steal",
  NULL, // " m tries"
};

unsigned long ctr_all[ CTR_MAX ];

#endif

static void stop_workers( void )
{
  int i;
#if COUNT_EVENTS
  int j;
#endif

#if SYNC_MORE
  lock( &more_lock );
    more_work = 0;
  unlock( &more_lock );
#else
  more_work = 0;
#endif
  for( i = 0; i < n_workers-1; i++ ) {
    pthread_join( ts[i], NULL );
  }

#if COUNT_EVENTS

  fprintf( stderr, " Worker" );
  for( j = 0; j < CTR_MAX; j++ ) {
    if( ctr_h[j] != NULL ) {
      fprintf( stderr, "%s ", ctr_h[j] );
      ctr_all[j] = 0;
    }
  }
  for( i = 0; i < n_workers; i++ ) {
    unsigned *lctr = workers[i].ctr;
    lctr[ CTR_spawn ] = lctr[ CTR_inlined ] + lctr[ CTR_read ] + lctr[ CTR_waits ];
    fprintf( stderr, "\nSTAT %2d", i );
    for( j = 0; j < CTR_MAX; j++ ) {
      if( ctr_h[j] != NULL ) {
        ctr_all[j] += lctr[j];
        fprintf( stderr, "%*u ", strlen( ctr_h[j] ), lctr[j] );
      }
    }
  }
  fprintf( stderr, "\n    ALL" );
  for( j = 0; j < CTR_MAX; j++ ) {
    if( ctr_h[j] != NULL ) {
      fprintf( stderr, "%*lu ", strlen( ctr_h[j] ), ctr_all[j] );
    }
  }
  fprintf( stderr, "\n" );

#endif

}

Task *wool_get_top( )
{
  Worker *self = pthread_getspecific( worker_key );
  Task *low = self->dq_base, *high = low + self->dq_size;

  lock( self->dq_lock );
    while( low < high-1 ) {
      Task *mid = low + (high-low)/2;

      if( mid->f == T_BUSY && mid->balarm == NOT_STOLEN ) {
        high = mid;
      } else {
        low = mid;
      }
    }
  unlock( self->dq_lock );
  
  return low;
}

void wool_start()
{
  start_workers();
}

void wool_init2( int workers, int dq_size, int stealable, wool_cb_stealing cb )
{
  n_workers = workers;
  init_worker_dq_size = dq_size;
  n_stealable = stealable;
  cb_stealing = cb;
  start_workers();
}

void wool_fini( )
{
  stop_workers( );
  // TODO: free memories
}


