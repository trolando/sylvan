/*
 * Copyright 2013-2014 Formal Methods and Tools, University of Twente
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sched.h> // for sched_getaffinity
#include <stdio.h>  // for fprintf
#include <stdlib.h> // for memalign, malloc
#include <string.h> // for memset
#include <sys/mman.h> // for mprotect
#include <sys/time.h> // for gettimeofday
#include <pthread.h>
#include <unistd.h>
#include <assert.h>

#include <lace.h>

#ifndef USE_NUMA
#define USE_NUMA 0 // by default, don't use special numa handling code
#endif

#if USE_NUMA
#include <numa.h>
#include <numa_tools.h>
#endif

// public Worker data
static Worker **workers;
static size_t default_stacksize = 4*1024*1024; // 4 megabytes
static size_t default_dqsize = 100000;

static int n_workers = 0;

// set to 0 when quitting
static volatile int more_work = 1;

// for storing private Worker data
static pthread_attr_t worker_attr;
static pthread_key_t worker_key;

static pthread_cond_t wait_until_done = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t wait_until_done_mutex = PTHREAD_MUTEX_INITIALIZER;

WorkerP*
lace_get_worker()
{
    return (WorkerP*)pthread_getspecific(worker_key);
}

Task*
lace_get_head(WorkerP *self)
{
    Task *low = self->dq;
    Task *high = self->end;

    if (low->f == 0) return low;

    while (low < high) {
        Task *mid = low + (high-low)/2;
        if (mid->f == 0) high = mid;
        else low = mid + 1;
    }

    return low;
}

size_t
lace_workers()
{
    return n_workers;
}

#if LACE_PIE_TIMES
static hrtime_t count_at_start, count_at_end;
static long long unsigned us_elapsed_timer;

static void
us_elapsed_start(void)
{
    struct timeval now;
    gettimeofday(&now, NULL);
    us_elapsed_timer = now.tv_sec * 1000000LL + now.tv_usec;
}

static long long unsigned
us_elapsed(void)
{
    struct timeval now;
    long long unsigned t;

    gettimeofday( &now, NULL );

    t = now.tv_sec * 1000000LL + now.tv_usec;

    return t - us_elapsed_timer;
}
#endif

#if USE_NUMA
// Lock used only during parallel lace_init_worker...
static volatile int __attribute__((aligned(64))) lock = 0;
static inline void
lock_acquire()
{
    while (1) {
        while (lock) {}
        if (cas(&lock, 0, 1)) return;
    }
}
static inline void
lock_release()
{
    lock=0;
}
#endif

/* Barrier */
#define BARRIER_MAX_THREADS 128

typedef union __attribute__((__packed__)) asize_u
{
    volatile size_t val;
    char            pad[LINE_SIZE - sizeof(size_t)];
} asize_t;

typedef struct barrier_s {
    size_t __attribute__((aligned(LINE_SIZE))) ids;
    size_t __attribute__((aligned(LINE_SIZE))) threads;
    size_t __attribute__((aligned(LINE_SIZE))) count;
    size_t __attribute__((aligned(LINE_SIZE))) wait;
    /* the following is needed only for destroy: */
    asize_t             entered[BARRIER_MAX_THREADS];
    pthread_key_t       tls_key;
} barrier_t;

static inline size_t
barrier_get_next_id(barrier_t *b)
{
    size_t val, new_val;
    do { // cas is faster than __sync_fetch_and_inc / __sync_inc_and_fetch
        val = ATOMIC_READ (b->ids);
        new_val = val + 1;
    } while (!cas(&b->ids, val, new_val));
    return val;
}

static inline int
barrier_get_id(barrier_t *b)
{
    int *id = (int*)pthread_getspecific(b->tls_key);
    if (id == NULL) {
        id = (int*)malloc(sizeof(int));
        *id = barrier_get_next_id(b);
        pthread_setspecific(b->tls_key, id);
    }
    return *id;
}

static int
barrier_wait(barrier_t *b)
{
    // get id ( only needed for destroy :( )
    int id = barrier_get_id(b);

    // signal entry
    ATOMIC_WRITE (b->entered[id].val, 1);

    size_t wait = ATOMIC_READ(b->wait);
    if (b->threads == add_fetch(b->count, 1)) {
        ATOMIC_WRITE(b->count, 0); // reset counter
        ATOMIC_WRITE(b->wait, 1 - wait); // flip wait
        ATOMIC_WRITE(b->entered[id].val, 0); // signal exit
        return -1; // master return value
    } else {
        while (wait == ATOMIC_READ(b->wait)) {} // wait
        ATOMIC_WRITE(b->entered[id].val, 0); // signal exit
        return 0; // slave return value
    }
}

static void
barrier_init(barrier_t *b, unsigned count)
{
    assert(count <= BARRIER_MAX_THREADS);
    memset(b, 0, sizeof(barrier_t));
    pthread_key_create(&b->tls_key, NULL);
    b->threads = count;
}

static void
barrier_destroy(barrier_t *b)
{
    // wait for all to exit
    size_t i;
    for (i=0; i<b->threads; i++)
        while (1 == b->entered[i].val) {}
    pthread_key_delete(b->tls_key);
}

static size_t default_stacksize;
static barrier_t bar;

void
lace_init_worker(int worker, size_t dq_size)
{
    Worker *wt;
    WorkerP *w;

    if (dq_size == 0) dq_size = default_dqsize;

#if USE_NUMA
    // Retrieve our NUMA node...
    size_t node;
    numa_worker_info(worker, &node, 0, 0, 0);

    // Pin our thread...
    numa_run_on_node(node);

    // Allocate memory on our NUMA node...
    lock_acquire();
    wt = (Worker *)numa_alloc_onnode(sizeof(Worker), node);
    w = (WorkerP *)numa_alloc_onnode(sizeof(WorkerP), node);
    if (wt == NULL || w == NULL || (w->dq = (Task*)numa_alloc_onnode(dq_size * sizeof(Task), node)) == NULL) {
        fprintf(stderr, "Lace error: Unable to allocate memory for the Lace worker!\n");
        exit(1);
    }
    lock_release();
#else
    // Allocate memory...
    if (posix_memalign((void**)&wt, LINE_SIZE, sizeof(Worker)) ||
        posix_memalign((void**)&w, LINE_SIZE, sizeof(WorkerP)) || 
        posix_memalign((void**)&w->dq, LINE_SIZE, dq_size * sizeof(Task))) {
            fprintf(stderr, "Lace error: Unable to allocate memory for the Lace worker!\n");
            exit(1);
    }
#endif

    // Initialize public worker data
    wt->dq = w->dq;
    wt->ts.v = 0;
    wt->allstolen = 0;
    wt->movesplit = 0;

    /// Initialize private worker data
    w->public = wt;
    w->end = w->dq + dq_size;
    w->split = w->dq;
    w->allstolen = 0;
    w->worker = worker;

#if LACE_COUNT_EVENTS
    // Reset counters
    { int k; for (k=0; k<CTR_MAX; k++) w->ctr[k] = 0; }
#endif

    // Set pointers
    pthread_setspecific(worker_key, w);
    workers[worker] = wt;

    // Synchronize with others
    barrier_wait(&bar);

#if LACE_PIE_TIMES
    w->time = gethrtime();
    w->level = 0;
#endif
}

static inline uint32_t
rng(uint32_t *seed, int max)
{
    uint32_t next = *seed;

    next *= 1103515245;
    next += 12345;

    *seed = next;

    return next % max;
}

void
lace_steal_random(WorkerP *self, Task *head)
{
    Worker *victim = workers[(self->worker + 1 + rng(&self->seed, n_workers-1)) % n_workers];

    PR_COUNTSTEALS(self, CTR_steal_tries);
    Worker *res = lace_steal(self, head, victim);
    if (res == LACE_NOWORK) {
        lace_cb_stealing();
    } else if (res == LACE_STOLEN) {
        PR_COUNTSTEALS(self, CTR_steals);
    } else if (res == LACE_BUSY) {
        PR_COUNTSTEALS(self, CTR_steal_busy);
    }
}

void
lace_steal_random_loop()
{
    // Determine who I am
    WorkerP * const me = lace_get_worker();
    Task * const head = me->dq;
    while (more_work) lace_steal_random(me, head);
}

static lace_callback_f main_cb;

static void*
lace_main_wrapper(void *arg)
{
    lace_init_worker(0, 0);
    WorkerP *self = lace_get_worker();

#if LACE_PIE_TIMES
    self->time = gethrtime();
#endif

    lace_time_event(self, 1);
    main_cb(self, self->dq, 1, arg);
    lace_exit();
    pthread_cond_broadcast(&wait_until_done);

    return NULL;
}

void
lace_steal_loop()
{
    // Determine who I am
    WorkerP * const me = lace_get_worker();
    const int worker_id = me->worker;

    // Prepare self, victim
    Worker ** const self = &workers[worker_id];
    Worker **victim = self;

#if LACE_PIE_TIMES
    (*self)->time = gethrtime();
#endif

    uint32_t seed = worker_id;
    unsigned int n = n_workers;
    int i=0;

    while(more_work) {
        // Select victim
        if( i>0 ) {
            i--;
            victim++;
            if (victim == self) victim++;
            if (victim >= workers + n) victim = workers;
            if (victim == self) victim++;
        } else {
            i = rng(&seed, 40); // compute random i 0..40
            victim = workers + (rng(&seed, n-1) + worker_id + 1) % n;
        }

        PR_COUNTSTEALS(me, CTR_steal_tries);
        Worker *res = lace_steal(me, me->dq, *victim);
        if (res == LACE_NOWORK) {
            lace_cb_stealing();
        } else if (res == LACE_STOLEN) {
            PR_COUNTSTEALS(me, CTR_steals);
        } else if (res == LACE_BUSY) {
            PR_COUNTSTEALS(me, CTR_steal_busy);
        }
    }
}

static void*
lace_default_worker(void* arg)
{
    lace_init_worker((size_t)arg, 0);
    lace_steal_loop();
    lace_time_event(lace_get_worker(), 9);
    barrier_wait(&bar);
    return NULL;
}

static void
lace_default_cb()
{
}

pthread_t
lace_spawn_worker(int worker, size_t stacksize, void* (*fun)(void*), void* arg)
{
    // Determine stack size
    if (stacksize == 0) stacksize = default_stacksize;

#if USE_NUMA
    if (stacksize != 0) {
        size_t pagesize = numa_pagesize();
        stacksize = (stacksize + pagesize - 1) & ~(pagesize - 1); // ceil(stacksize, pagesize)

        // Allocate memory for the program stack on the NUMA nodes
        size_t node;
        numa_worker_info(worker, &node, 0, 0, 0);
        lock_acquire();
        void *stack_location = numa_alloc_onnode(stacksize + pagesize, node);
        if (stack_location == 0) {
            fprintf(stderr, "Lace error: Unable to allocate memory for the pthread stack!\n");
            exit(1);
        }
        if (0 != mprotect(stack_location, pagesize, PROT_NONE)) {
            fprintf(stderr, "Lace error: Unable to protect the allocated stack memory with a guard page!\n");
            exit(1);
        }
        stack_location = (uint8_t *)stack_location + pagesize; // skip protected page.
        if (0 != pthread_attr_setstack(&worker_attr, stack_location, stacksize)) {
            fprintf(stderr, "Lace error: Unable to set the pthread stack in Lace!\n");
            exit(1);
        }
        lock_release();
    }
#else
    if (pthread_attr_setstacksize(&worker_attr, stacksize) != 0) {
        fprintf(stderr, "Lace warning: Cannot set stacksize for new pthreads!\n");
    }
    (void)worker;
#endif

    if (fun == 0) {
        fun = lace_default_worker;
        arg = (void*)(size_t)worker;
    }

    pthread_t res;
    pthread_create(&res, &worker_attr, fun, arg);
    return res;
}

static int
get_cpu_count()
{
#ifdef sched_getaffinity
    /* Best solution: find actual available cpus */
    cpu_set_t cs;
    CPU_ZERO(&cs);
    sched_getaffinity(0, sizeof(cs), &cs);
    int count = CPU_COUNT(&cs);
#elif defined(_SC_NPROCESSORS_ONLN)
    /* Fallback */
    int count = sysconf(_SC_NPROCESSORS_ONLN);
#else
    /* Okay... */
    int count = 1;
#endif
    return count < 1 ? 1 : count;
}

void
lace_init(int n, size_t dqsize)
{
    // Initialize globals
    n_workers = n;
    if (n_workers == 0) n_workers = get_cpu_count();
    if (dqsize != 0) default_dqsize = dqsize;
    more_work = 1;
    lace_cb_stealing = &lace_default_cb;

    // Create barrier for all workers
    barrier_init(&bar, n_workers);

    // Allocate array with all workers
    if (posix_memalign((void**)&workers, LINE_SIZE, n_workers*sizeof(Worker*)) != 0) {
        fprintf(stderr, "Lace error: unable to allocate memory!\n");
        exit(1);
    }

    // Create pthread key
    pthread_key_create(&worker_key, NULL);

    // Prepare structures for thread creation
    pthread_attr_init(&worker_attr);

    // Set contention scope to system (instead of process)
    pthread_attr_setscope(&worker_attr, PTHREAD_SCOPE_SYSTEM);

    // Get default stack size
    if (pthread_attr_getstacksize(&worker_attr, &default_stacksize) != 0) {
        fprintf(stderr, "Lace warning: pthread_attr_getstacksize returned error!\n");
        default_stacksize = 0;
    }

#if USE_NUMA
    // If we have NUMA, initialize it
    if (numa_available() != 0) {
        fprintf(stderr, "Lace error: NUMA not available!\n");
        exit(1);
    } else {
        fprintf(stderr, "Initializing Lace with NUMA support.\n");
        if (numa_distribute(n_workers) != 0) {
            fprintf(stderr, "Lace error: no suitable NUMA configuration found!\n");
            exit(1);
        }
    }
#endif

#if LACE_PIE_TIMES
    // Initialize counters for pie times
    us_elapsed_start();
    count_at_start = gethrtime();
#endif
}

void
lace_startup(size_t stacksize, lace_callback_f cb, void *arg)
{
    /* Spawn workers */
    int i;
    for (i=1; i<n_workers; i++) lace_spawn_worker(i, stacksize, 0, 0);

    if (cb != 0) {
        main_cb = cb;
        lace_spawn_worker(0, stacksize, lace_main_wrapper, arg);

        // Suspend this thread until cb returns
        pthread_mutex_lock(&wait_until_done_mutex);
        pthread_cond_wait(&wait_until_done, &wait_until_done_mutex);
        pthread_mutex_unlock(&wait_until_done_mutex);
    } else {
        // use this thread as worker and return control
        lace_init_worker(0, 0);
        lace_time_event(lace_get_worker(), 1);
    }
}

#if LACE_COUNT_EVENTS
static uint64_t ctr_all[CTR_MAX];
#endif

void
lace_count_reset()
{
#if LACE_COUNT_EVENTS
    size_t i, j;

    for (i=0;i<n_workers;i++) {
        for (j=0;j<CTR_MAX;j++) {
            workers[i]->ctr[j] = 0;
        }
    }

#if LACE_PIE_TIMES
    for (i=0;i<n_workers;i++) {
        workers[i]->time = gethrtime();
        if (i != 0) workers[i]->level = 0;
    }

    us_elapsed_start();
    count_at_start = gethrtime();
#endif
#endif
}

void
lace_count_report_file(FILE *file)
{
#if LACE_COUNT_EVENTS
    size_t i, j;

    for (j=0;j<CTR_MAX;j++) ctr_all[j] = 0;
    for (i=0;i<n_workers;i++) {
        uint64_t *wctr = workers[i]->ctr;
        for (j=0;j<CTR_MAX;j++) {
            ctr_all[j] += wctr[j];
        }
    }

#if LACE_COUNT_TASKS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Tasks (%zu): %zu\n", i, workers[i]->ctr[CTR_tasks]);
    }
    fprintf(file, "Tasks (sum): %zu\n", ctr_all[CTR_tasks]);
    fprintf(file, "\n");
#endif

#if LACE_COUNT_STEALS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Steals (%zu): %zu good/%zu busy of %zu tries; leaps: %zu good/%zu busy of %zu tries\n", i,
            workers[i]->ctr[CTR_steals], workers[i]->ctr[CTR_steal_busy],
            workers[i]->ctr[CTR_steal_tries], workers[i]->ctr[CTR_leaps], 
            workers[i]->ctr[CTR_leap_busy], workers[i]->ctr[CTR_leap_tries]);
    }
    fprintf(file, "Steals (sum): %zu good/%zu busy of %zu tries; leaps: %zu good/%zu busy of %zu tries\n", 
        ctr_all[CTR_steals], ctr_all[CTR_steal_busy],
        ctr_all[CTR_steal_tries], ctr_all[CTR_leaps], 
        ctr_all[CTR_leap_busy], ctr_all[CTR_leap_tries]);
    fprintf(file, "\n");
#endif

#if LACE_COUNT_STEALS && LACE_COUNT_TASKS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Tasks per steal (%zu): %zu\n", i, 
            workers[i]->ctr[CTR_tasks]/(workers[i]->ctr[CTR_steals]+workers[i]->ctr[CTR_leaps]));
    }
    fprintf(file, "Tasks per steal (sum): %zu\n", ctr_all[CTR_tasks]/(ctr_all[CTR_steals]+ctr_all[CTR_leaps]));
    fprintf(file, "\n");
#endif

#if LACE_COUNT_SPLITS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Splits (%zu): %zu shrinks, %zu grows, %zu outgoing requests\n", i,
            workers[i]->ctr[CTR_split_shrink], workers[i]->ctr[CTR_split_grow], workers[i]->ctr[CTR_split_req]);
    }
    fprintf(file, "Splits (sum): %zu shrinks, %zu grows, %zu outgoing requests\n",
        ctr_all[CTR_split_shrink], ctr_all[CTR_split_grow], ctr_all[CTR_split_req]);
    fprintf(file, "\n");
#endif

#if LACE_PIE_TIMES
    count_at_end = gethrtime();

    uint64_t count_per_ms = (count_at_end - count_at_start) / (us_elapsed() / 1000);
    double dcpm = (double)count_per_ms;

    uint64_t sum_count;
    sum_count = ctr_all[CTR_init] + ctr_all[CTR_wapp] + ctr_all[CTR_lapp] + ctr_all[CTR_wsteal] + ctr_all[CTR_lsteal]
              + ctr_all[CTR_close] + ctr_all[CTR_wstealsucc] + ctr_all[CTR_lstealsucc] + ctr_all[CTR_wsignal]
              + ctr_all[CTR_lsignal];

    fprintf(file, "Measured clock (tick) frequency: %.2f GHz\n", count_per_ms / 1000000.0);
    fprintf(file, "Aggregated time per pie slice, total time: %.2f CPU seconds\n\n", sum_count / (1000*dcpm));

    for (i=0;i<n_workers;i++) {
        fprintf(file, "Startup time (%zu):    %10.2f ms\n", i, workers[i]->ctr[CTR_init] / dcpm);
        fprintf(file, "Steal work (%zu):      %10.2f ms\n", i, workers[i]->ctr[CTR_wapp] / dcpm);
        fprintf(file, "Leap work (%zu):       %10.2f ms\n", i, workers[i]->ctr[CTR_lapp] / dcpm);
        fprintf(file, "Steal overhead (%zu):  %10.2f ms\n", i, (workers[i]->ctr[CTR_wstealsucc]+workers[i]->ctr[CTR_wsignal]) / dcpm);
        fprintf(file, "Leap overhead (%zu):   %10.2f ms\n", i, (workers[i]->ctr[CTR_lstealsucc]+workers[i]->ctr[CTR_lsignal]) / dcpm);
        fprintf(file, "Steal search (%zu):    %10.2f ms\n", i, (workers[i]->ctr[CTR_wsteal]-workers[i]->ctr[CTR_wstealsucc]-workers[i]->ctr[CTR_wsignal]) / dcpm);
        fprintf(file, "Leap search (%zu):     %10.2f ms\n", i, (workers[i]->ctr[CTR_lsteal]-workers[i]->ctr[CTR_lstealsucc]-workers[i]->ctr[CTR_lsignal]) / dcpm);
        fprintf(file, "Exit time (%zu):       %10.2f ms\n", i, workers[i]->ctr[CTR_close] / dcpm);
        fprintf(file, "\n");
    }

    fprintf(file, "Startup time (sum):    %10.2f ms\n", ctr_all[CTR_init] / dcpm);
    fprintf(file, "Steal work (sum):      %10.2f ms\n", ctr_all[CTR_wapp] / dcpm);
    fprintf(file, "Leap work (sum):       %10.2f ms\n", ctr_all[CTR_lapp] / dcpm);
    fprintf(file, "Steal overhead (sum):  %10.2f ms\n", (ctr_all[CTR_wstealsucc]+ctr_all[CTR_wsignal]) / dcpm);
    fprintf(file, "Leap overhead (sum):   %10.2f ms\n", (ctr_all[CTR_lstealsucc]+ctr_all[CTR_lsignal]) / dcpm);
    fprintf(file, "Steal search (sum):    %10.2f ms\n", (ctr_all[CTR_wsteal]-ctr_all[CTR_wstealsucc]-ctr_all[CTR_wsignal]) / dcpm);
    fprintf(file, "Leap search (sum):     %10.2f ms\n", (ctr_all[CTR_lsteal]-ctr_all[CTR_lstealsucc]-ctr_all[CTR_lsignal]) / dcpm);
    fprintf(file, "Exit time (sum):       %10.2f ms\n", ctr_all[CTR_close] / dcpm);
    fprintf(file, "\n" );
#endif
#endif
    return;
    (void)file;
}

void lace_exit()
{
    lace_time_event(workers[0], 2);

    more_work = 0;

    // Wait for others
    barrier_wait(&bar);

    barrier_destroy(&bar);

#if LACE_COUNT_EVENTS
    lace_count_report_file(stderr);
#endif
}

void (*lace_cb_stealing)(void);

void lace_set_callback(void (*cb)(void))
{
    lace_cb_stealing = cb;
}
