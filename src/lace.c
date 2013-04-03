#include "lace.h"
#include <stdio.h>  // for fprintf
#include <stdlib.h> // for memalign, malloc
#include <sys/mman.h> // for mprotect
#include <pthread.h>

#ifndef USE_NUMA
#define USE_NUMA 0 // by default, don't use special numa handling code
#endif

#if USE_NUMA
#include <numa.h>
#include "numa_tools.h"
#endif

static Worker **workers;

static int n_workers = 0;

static int more_work = 1;
static pthread_t *ts = NULL;

static pthread_attr_t worker_attr;
static pthread_key_t worker_key;

static void init_worker(int worker, size_t dq_size)
{
    Worker *w;

#if USE_NUMA
    int node;
    numa_worker_info(worker, &node, 0, 0, 0);
    w = (Worker *)numa_alloc_onnode(sizeof(Worker), node);
    w->dq = (Task*)numa_alloc_onnode(dq_size * sizeof(Task), node);
#else
    posix_memalign((void**)&w, LINE_SIZE, sizeof(Worker));
    posix_memalign((void**)&w->dq, LINE_SIZE, dq_size * sizeof(Task));
#endif

    w->ts.v = 0;
    w->allstolen = 0;
    w->o_dq = w->dq;
    w->o_end = w->dq + dq_size;
    w->o_split = w->o_dq;
    w->flags.all = 0;
    w->worker = worker;


    workers[worker] = w;
}

int __attribute__((noinline)) lace_steal(Worker *self, Task *__dq_head, Worker *victim)
{
    if (victim->allstolen) {
        return LACE_NOWORK;
    }

    register TailSplit ts = victim->ts;
    if (ts.ts.tail >= ts.ts.split) {
        if (victim->flags.o.movesplit == 0) victim->flags.o.movesplit = 1;
        return LACE_NOWORK;
    }

    register TailSplit ts_new = ts;
    ts_new.ts.tail++;
    if (!cas(&victim->ts.v, ts.v, ts_new.v)) {
        return LACE_BUSY; 
    }

    // Stolen
    Task *t = &victim->dq[ts.ts.tail];
    t->thief = self;
    t->f(self, __dq_head, t);
    t->thief = THIEF_COMPLETED;
    return LACE_STOLEN;
}

static inline uint32_t rng(uint32_t *seed, int max) 
{
    uint32_t next = *seed;

    next *= 1103515245;
    next += 12345;

    *seed = next;

    return next % max;
}

// By default, scan sequentially for 0..39 attempts
#define rand_interval 40

static void *worker_thread( void *arg )
{
    Worker **self = (Worker **) arg, **victim = NULL;
    int worker_id = (*self)->worker;
    uint32_t seed = worker_id;
    unsigned int n = n_workers;
    int i=0;

#if USE_NUMA
    numa_bind_me((*self)->worker);
#endif

    pthread_setspecific( worker_key, *self );


    victim = self;

    do {
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
            i = rng(&seed, rand_interval);
            victim = workers + (rng(&seed, n-1) + worker_id + 1) % n;
        }

        if (lace_steal(*self, (*self)->o_dq, *victim) == LACE_NOWORK) lace_cb_stealing();

        if (!more_work) break;

    } while(1);

    return NULL;
}

Task *lace_get_head()
{
    Worker *self = pthread_getspecific(worker_key);

    Task *low = self->o_dq;
    Task *high = self->o_end;

    if (low->f == 0) return low;

    while (low < high) {
        Task *mid = low + (high-low)/2;
        if (mid->f == 0) high = mid;
        else low = mid + 1;
    }

    return low;
}

Worker *lace_get_worker()
{
    return pthread_getspecific(worker_key);
}

static void lace_default_cb()
{
}

void lace_init(int n, size_t dq_size, size_t stacksize)
{

    n_workers = n;
    int i;

    more_work = 1;
    lace_cb_stealing = &lace_default_cb;

    posix_memalign((void**)&workers, LINE_SIZE, n*sizeof(Worker*));
    ts      = (pthread_t *)malloc((n-1) * sizeof(pthread_t));
    pthread_attr_init(&worker_attr);
    pthread_attr_setscope(&worker_attr, PTHREAD_SCOPE_SYSTEM);

    if (stacksize == 0) {
        pthread_attr_getstacksize(&worker_attr, &stacksize);
    } else {
        if (0 != pthread_attr_setstacksize(&worker_attr, stacksize)) {
            fprintf(stderr, "Error: Cannot set stacksize for new pthreads in Lace!\n");
            exit(1);
            pthread_attr_getstacksize(&worker_attr, &stacksize);
        }
    }

    if (stacksize == 0) {
        fprintf(stderr, "Error: Unable to get stacksize for new pthreads in Lace!\n");
        exit(1);
    }

    pthread_key_create(&worker_key, NULL);

#if USE_NUMA
    if (numa_available() != 0) {
        fprintf(stderr, "Error: NUMA not available!\n");
        exit(1); 
    }
    else fprintf(stderr, "Initializing Lace with NUMA support.\n");

    if (numa_distribute(n) != 0) { 
        fprintf(stderr, "Error: no suitable NUMA configuration found!\n"); 
        exit(1); 
    }
#endif

    // Initialize data structures for work stealing
    for (i=0; i<n; i++) init_worker(i, dq_size);

#if USE_NUMA
    // Create threads
    size_t pagesize = numa_pagesize();
    stacksize = (stacksize + pagesize - 1) & ~(pagesize - 1); // ceil(stacksize, pagesize)
    for (i=1; i<n; i++) {
        if (stacksize != 0) {
            // Allocate memory for the program stack on the NUMA nodes
            int node;
            numa_worker_info(i, &node, 0, 0, 0);
            void *stack_location = numa_alloc_onnode(stacksize + pagesize, node);
            if (stack_location == 0) {
                fprintf(stderr, "Error: Unable to allocate memory for the pthread stack!\n");
                exit(1);
            }
            if (0 != mprotect(stack_location, pagesize, PROT_NONE)) {
                fprintf(stderr, "Error: Unable to protect the allocated stack memory with a guard page!\n");
                exit(1);
            }
            stack_location = stack_location + pagesize; // skip protected page.
            if (0 != pthread_attr_setstack(&worker_attr, stack_location, stacksize)) {
                fprintf(stderr, "Error: Unable to set the pthread stack in Lace!\n");
                exit(1);
            }
        }
        pthread_create(ts+i-1, &worker_attr, &worker_thread, workers+i);
    }
#else
    for (i=0; i<n-1; i++) pthread_create(ts+i, &worker_attr, &worker_thread, workers+i+1);
#endif

#if USE_NUMA
    // Pin ourself
    numa_bind_me(0);
#endif

    pthread_setspecific(worker_key, *workers);
}

void lace_exit()
{

    more_work = 0;

    int i;
    for(i=0; i<n_workers-1; i++) pthread_join(ts[i], NULL);

}

void (*lace_cb_stealing)(void);

void lace_set_callback(void (*cb)(void))
{
    lace_cb_stealing = cb;
}
