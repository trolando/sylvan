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

#include <errno.h>
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

#ifndef USE_HWLOC
#define USE_HWLOC 0
#endif

#if USE_HWLOC
#include <hwloc.h>
#endif

// public Worker data
static Worker **workers;
static size_t default_stacksize = 0; // set by lace_init
static size_t default_dqsize = 100000;

#if USE_HWLOC
static hwloc_topology_t topo;
static unsigned int n_nodes, n_cores, n_pus;
#endif

static int n_workers = 0;

// private Worker data (just for stats at end )
static WorkerP **workers_p;

// set to 0 when quitting
static int lace_quits = 0;

// for storing private Worker data
static pthread_attr_t worker_attr;
static pthread_key_t worker_key;

static pthread_cond_t wait_until_done = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t wait_until_done_mutex = PTHREAD_MUTEX_INITIALIZER;

struct lace_worker_init
{
    void* stack;
    size_t stacksize;
};

static struct lace_worker_init *workers_init;

lace_newframe_t lace_newframe;

WorkerP*
lace_get_worker()
{
    return (WorkerP*)pthread_getspecific(worker_key);
}

Task*
lace_get_head(WorkerP *self)
{
    Task *dq = self->dq;
    if (dq[0].thief == 0) return dq;
    if (dq[1].thief == 0) return dq+1;
    if (dq[2].thief == 0) return dq+2;

    size_t low = 2;
    size_t high = self->end - self->dq;

    for (;;) {
        if (low*2 >= high) {
            break;
        } else if (dq[low*2].thief == 0) {
            high=low*2;
            break;
        } else {
            low*=2;
        }
    }

    while (low < high) {
        size_t mid = low + (high-low)/2;
        if (dq[mid].thief == 0) high = mid;
        else low = mid + 1;
    }

    return dq+low;
}

size_t
lace_workers()
{
    return n_workers;
}

size_t
lace_default_stacksize()
{
    return default_stacksize;
}

#if LACE_PIE_TIMES
static uint64_t count_at_start, count_at_end;
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

#if USE_HWLOC
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

static barrier_t bar;

void
lace_init_worker(int worker, size_t dq_size)
{
    Worker *wt = NULL;
    WorkerP *w = NULL;

    if (dq_size == 0) dq_size = default_dqsize;

#if USE_HWLOC
    // Get our logical processor
    hwloc_obj_t pu = hwloc_get_obj_by_type(topo, HWLOC_OBJ_PU, worker % n_pus);

    // Pin our thread...
    hwloc_set_cpubind(topo, pu->cpuset, HWLOC_CPUBIND_THREAD);

    // Allocate memory on our node...
    lock_acquire();
    wt = (Worker *)hwloc_alloc_membind(topo, sizeof(Worker), pu->cpuset, HWLOC_MEMBIND_BIND, 0);
    w = (WorkerP *)hwloc_alloc_membind(topo, sizeof(WorkerP), pu->cpuset, HWLOC_MEMBIND_BIND, 0);
    if (wt == NULL || w == NULL || (w->dq = (Task*)hwloc_alloc_membind(topo, dq_size * sizeof(Task), pu->cpuset, HWLOC_MEMBIND_BIND, 0)) == NULL) {
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

    // Initialize private worker data
    w->_public = wt;
    w->end = w->dq + dq_size;
    w->split = w->dq;
    w->allstolen = 0;
    w->worker = worker;
    if (workers_init[worker].stack != 0) {
        w->stack_trigger = ((size_t)workers_init[worker].stack) + workers_init[worker].stacksize/20;
    } else {
        w->stack_trigger = 0;
    }

#if LACE_COUNT_EVENTS
    // Reset counters
    { int k; for (k=0; k<CTR_MAX; k++) w->ctr[k] = 0; }
#endif

    // Set pointers
    pthread_setspecific(worker_key, w);
    workers[worker] = wt;
    workers_p[worker] = w;

    // Synchronize with others
    barrier_wait(&bar);

#if LACE_PIE_TIMES
    w->time = gethrtime();
    w->level = 0;
#endif
}

#if defined(__APPLE__) && !defined(pthread_barrier_t)

typedef int pthread_barrierattr_t;
typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int count;
    int tripCount;
} pthread_barrier_t;

static int
pthread_barrier_init(pthread_barrier_t *barrier, const pthread_barrierattr_t *attr, unsigned int count)
{
    if(count == 0)
    {
        errno = EINVAL;
        return -1;
    }
    if(pthread_mutex_init(&barrier->mutex, 0) < 0)
    {
        return -1;
    }
    if(pthread_cond_init(&barrier->cond, 0) < 0)
    {
        pthread_mutex_destroy(&barrier->mutex);
        return -1;
    }
    barrier->tripCount = count;
    barrier->count = 0;

    return 0;
    (void)attr;
}

static int
pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    pthread_cond_destroy(&barrier->cond);
    pthread_mutex_destroy(&barrier->mutex);
    return 0;
}

static int
pthread_barrier_wait(pthread_barrier_t *barrier)
{
    pthread_mutex_lock(&barrier->mutex);
    ++(barrier->count);
    if(barrier->count >= barrier->tripCount)
    {
        barrier->count = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return 1;
    }
    else
    {
        pthread_cond_wait(&barrier->cond, &(barrier->mutex));
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

#endif // defined(__APPLE__) && !defined(pthread_barrier_t)

static pthread_barrier_t suspend_barrier;
static volatile int must_suspend = 0;

static inline void
lace_go_suspend()
{
    barrier_wait(&bar);
    pthread_barrier_wait(&suspend_barrier);
}

void
lace_suspend()
{
    must_suspend = 1;
    barrier_wait(&bar);
    must_suspend = 0;
}

void
lace_resume()
{
    pthread_barrier_wait(&suspend_barrier);
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

VOID_TASK_IMPL_0(lace_steal_random)
{
    Worker *victim = workers[(__lace_worker->worker + 1 + rng(&__lace_worker->seed, n_workers-1)) % n_workers];

    PR_COUNTSTEALS(__lace_worker, CTR_steal_tries);
    Worker *res = lace_steal(__lace_worker, __lace_dq_head, victim);
    if (res == LACE_NOWORK) {
        YIELD_NEWFRAME();
        if (must_suspend) lace_go_suspend();
    } else if (res == LACE_STOLEN) {
        PR_COUNTSTEALS(__lace_worker, CTR_steals);
    } else if (res == LACE_BUSY) {
        PR_COUNTSTEALS(__lace_worker, CTR_steal_busy);
    }
}

VOID_TASK_IMPL_1(lace_steal_random_loop, int*, quit)
{
    while (!(*(volatile int*)quit)) STEAL_RANDOM();
}

static lace_startup_cb main_cb;

static void*
lace_main_wrapper(void *arg)
{
    lace_init_worker(0, 0);
    WorkerP *self = lace_get_worker();

#if LACE_PIE_TIMES
    self->time = gethrtime();
#endif

    lace_time_event(self, 1);
    main_cb(self, self->dq, arg);
    lace_exit();
    pthread_cond_broadcast(&wait_until_done);

    return NULL;
}

VOID_TASK_IMPL_1(lace_steal_loop, int*, quit)
{
    // Determine who I am
    const int worker_id = __lace_worker->worker;

    // Prepare self, victim
    Worker ** const self = &workers[worker_id];
    Worker **victim = self;

#if LACE_PIE_TIMES
    __lace_worker->time = gethrtime();
#endif

    uint32_t seed = worker_id;
    unsigned int n = n_workers;
    int i=0;

    while(!(*(volatile int*)quit)) {
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

        PR_COUNTSTEALS(__lace_worker, CTR_steal_tries);
        Worker *res = lace_steal(__lace_worker, __lace_dq_head, *victim);
        if (res == LACE_NOWORK) {
            YIELD_NEWFRAME();
            if (must_suspend) lace_go_suspend();
        } else if (res == LACE_STOLEN) {
            PR_COUNTSTEALS(__lace_worker, CTR_steals);
        } else if (res == LACE_BUSY) {
            PR_COUNTSTEALS(__lace_worker, CTR_steal_busy);
        }
    }
}

static void*
lace_default_worker(void* arg)
{
    lace_init_worker((size_t)arg, 0);
    WorkerP *__lace_worker = lace_get_worker();
    Task *__lace_dq_head = __lace_worker->dq;
    lace_steal_loop(&lace_quits);
    lace_time_event(__lace_worker, 9);
    barrier_wait(&bar);
    return NULL;
}

pthread_t
lace_spawn_worker(int worker, size_t stacksize, void* (*fun)(void*), void* arg)
{
    // Determine stack size
    if (stacksize == 0) stacksize = default_stacksize;

    size_t pagesize = sysconf(_SC_PAGESIZE);
    stacksize = (stacksize + pagesize - 1) & ~(pagesize - 1); // ceil(stacksize, pagesize)

#if USE_HWLOC
    // Get our logical processor
    hwloc_obj_t pu = hwloc_get_obj_by_type(topo, HWLOC_OBJ_PU, worker % n_pus);

    // Allocate memory for the program stack
    lock_acquire();
    void *stack_location = hwloc_alloc_membind(topo, stacksize + pagesize, pu->cpuset, HWLOC_MEMBIND_BIND, 0);
    lock_release();
    if (stack_location == 0) {
        fprintf(stderr, "Lace error: Unable to allocate memory for the pthread stack!\n");
        exit(1);
    }
#else
    void *stack_location = mmap(NULL, stacksize + pagesize, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, 0, 0);
    if (stack_location == MAP_FAILED) {
        fprintf(stderr, "Lace error: Cannot allocate program stack, errno=%d!\n", errno);
        exit(1);
    }
#endif

    if (0 != mprotect(stack_location, pagesize, PROT_NONE)) {
        fprintf(stderr, "Lace error: Unable to protect the allocated program stack with a guard page!\n");
        exit(1);
    }
    stack_location = (uint8_t *)stack_location + pagesize; // skip protected page.
    if (0 != pthread_attr_setstack(&worker_attr, stack_location, stacksize)) {
        fprintf(stderr, "Lace error: Unable to set the pthread stack in Lace!\n");
        exit(1);
    }

    workers_init[worker].stack = stack_location;
    workers_init[worker].stacksize = stacksize;

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
#if USE_HWLOC
    int count = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_PU);
#elif defined(sched_getaffinity)
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
#if USE_HWLOC
    hwloc_topology_init(&topo);
    hwloc_topology_load(topo);

    n_nodes = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_NODE);
    n_cores = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_CORE);
    n_pus = hwloc_get_nbobjs_by_type(topo, HWLOC_OBJ_PU);
#endif

    // Initialize globals
    n_workers = n;
    if (n_workers == 0) n_workers = get_cpu_count();
    if (dqsize != 0) default_dqsize = dqsize;
    lace_quits = 0;

    // Create barrier for all workers
    barrier_init(&bar, n_workers);

    // Create suspend barrier
    pthread_barrier_init(&suspend_barrier, NULL, n_workers);

    // Allocate array with all workers
    if (posix_memalign((void**)&workers, LINE_SIZE, n_workers*sizeof(Worker*)) != 0 ||
        posix_memalign((void**)&workers_p, LINE_SIZE, n_workers*sizeof(WorkerP*)) != 0) {
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
        default_stacksize = 1048576; // 1 megabyte default
    }

#if USE_HWLOC
    fprintf(stderr, "Initializing Lace, %u nodes, %u cores, %u logical processors, %d workers.\n", n_nodes, n_cores, n_pus, n_workers);
#else
    fprintf(stderr, "Initializing Lace, %d workers.\n", n_workers);
#endif

    // Prepare lace_init structure
    workers_init = (struct lace_worker_init*)calloc(1, sizeof(struct lace_worker_init) * n_workers);

    lace_newframe.t = NULL;

#if LACE_PIE_TIMES
    // Initialize counters for pie times
    us_elapsed_start();
    count_at_start = gethrtime();
#endif
}

void
lace_startup(size_t stacksize, lace_startup_cb cb, void *arg)
{
    if (stacksize == 0) stacksize = default_stacksize;

    if (cb != 0) {
        fprintf(stderr, "Lace startup, creating %d worker threads with program stack %zu bytes.\n", n_workers, stacksize);
    } else if (n_workers == 1) {
        fprintf(stderr, "Lace startup, creating 0 worker threads.\n");
    } else {
        fprintf(stderr, "Lace startup, creating %d worker threads with program stack %zu bytes.\n", n_workers-1, stacksize);
    }

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
    int i;
    size_t j;

    for (i=0;i<n_workers;i++) {
        for (j=0;j<CTR_MAX;j++) {
            workers_p[i]->ctr[j] = 0;
        }
    }

#if LACE_PIE_TIMES
    for (i=0;i<n_workers;i++) {
        workers_p[i]->time = gethrtime();
        if (i != 0) workers_p[i]->level = 0;
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
    int i;
    size_t j;

    for (j=0;j<CTR_MAX;j++) ctr_all[j] = 0;
    for (i=0;i<n_workers;i++) {
        uint64_t *wctr = workers_p[i]->ctr;
        for (j=0;j<CTR_MAX;j++) {
            ctr_all[j] += wctr[j];
        }
    }

#if LACE_COUNT_TASKS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Tasks (%d): %zu\n", i, workers_p[i]->ctr[CTR_tasks]);
    }
    fprintf(file, "Tasks (sum): %zu\n", ctr_all[CTR_tasks]);
    fprintf(file, "\n");
#endif

#if LACE_COUNT_STEALS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Steals (%d): %zu good/%zu busy of %zu tries; leaps: %zu good/%zu busy of %zu tries\n", i,
            workers_p[i]->ctr[CTR_steals], workers_p[i]->ctr[CTR_steal_busy],
            workers_p[i]->ctr[CTR_steal_tries], workers_p[i]->ctr[CTR_leaps],
            workers_p[i]->ctr[CTR_leap_busy], workers_p[i]->ctr[CTR_leap_tries]);
    }
    fprintf(file, "Steals (sum): %zu good/%zu busy of %zu tries; leaps: %zu good/%zu busy of %zu tries\n", 
        ctr_all[CTR_steals], ctr_all[CTR_steal_busy],
        ctr_all[CTR_steal_tries], ctr_all[CTR_leaps],
        ctr_all[CTR_leap_busy], ctr_all[CTR_leap_tries]);
    fprintf(file, "\n");
#endif

#if LACE_COUNT_STEALS && LACE_COUNT_TASKS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Tasks per steal (%d): %zu\n", i,
            workers_p[i]->ctr[CTR_tasks]/(workers_p[i]->ctr[CTR_steals]+workers_p[i]->ctr[CTR_leaps]));
    }
    fprintf(file, "Tasks per steal (sum): %zu\n", ctr_all[CTR_tasks]/(ctr_all[CTR_steals]+ctr_all[CTR_leaps]));
    fprintf(file, "\n");
#endif

#if LACE_COUNT_SPLITS
    for (i=0;i<n_workers;i++) {
        fprintf(file, "Splits (%d): %zu shrinks, %zu grows, %zu outgoing requests\n", i,
            workers_p[i]->ctr[CTR_split_shrink], workers_p[i]->ctr[CTR_split_grow], workers_p[i]->ctr[CTR_split_req]);
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
        fprintf(file, "Startup time (%d):    %10.2f ms\n", i, workers_p[i]->ctr[CTR_init] / dcpm);
        fprintf(file, "Steal work (%d):      %10.2f ms\n", i, workers_p[i]->ctr[CTR_wapp] / dcpm);
        fprintf(file, "Leap work (%d):       %10.2f ms\n", i, workers_p[i]->ctr[CTR_lapp] / dcpm);
        fprintf(file, "Steal overhead (%d):  %10.2f ms\n", i, (workers_p[i]->ctr[CTR_wstealsucc]+workers_p[i]->ctr[CTR_wsignal]) / dcpm);
        fprintf(file, "Leap overhead (%d):   %10.2f ms\n", i, (workers_p[i]->ctr[CTR_lstealsucc]+workers_p[i]->ctr[CTR_lsignal]) / dcpm);
        fprintf(file, "Steal search (%d):    %10.2f ms\n", i, (workers_p[i]->ctr[CTR_wsteal]-workers_p[i]->ctr[CTR_wstealsucc]-workers_p[i]->ctr[CTR_wsignal]) / dcpm);
        fprintf(file, "Leap search (%d):     %10.2f ms\n", i, (workers_p[i]->ctr[CTR_lsteal]-workers_p[i]->ctr[CTR_lstealsucc]-workers_p[i]->ctr[CTR_lsignal]) / dcpm);
        fprintf(file, "Exit time (%d):       %10.2f ms\n", i, workers_p[i]->ctr[CTR_close] / dcpm);
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
    lace_time_event(lace_get_worker(), 2);

    lace_quits = 1;

    // Wait for others
    barrier_wait(&bar);

    barrier_destroy(&bar);

    pthread_barrier_destroy(&suspend_barrier);

#if LACE_COUNT_EVENTS
    lace_count_report_file(stderr);
#endif
}

void
lace_exec_in_new_frame(WorkerP *__lace_worker, Task *__lace_dq_head, Task *root)
{
    TailSplit old;
    uint8_t old_as;

    // save old tail, split, allstolen and initiate new frame
    {
        Worker *wt = __lace_worker->_public;

        old_as = wt->allstolen;
        wt->allstolen = 1;
        old.ts.split = wt->ts.ts.split;
        wt->ts.ts.split = 0;
        mfence();
        old.ts.tail = wt->ts.ts.tail;

        TailSplit ts_new;
        ts_new.ts.tail = __lace_dq_head - __lace_worker->dq;
        ts_new.ts.split = __lace_dq_head - __lace_worker->dq;
        wt->ts.v = ts_new.v;

        __lace_worker->split = __lace_dq_head;
        __lace_worker->allstolen = 1;
    }

    // wait until all workers are ready
    barrier_wait(&bar);

    // execute task
    root->f(__lace_worker, __lace_dq_head, root);
    compiler_barrier();

    // wait until all workers are back (else they may steal from previous frame)
    barrier_wait(&bar);

    // restore tail, split, allstolen
    {
        Worker *wt = __lace_worker->_public;
        wt->allstolen = old_as;
        wt->ts.v = old.v;
        __lace_worker->split = __lace_worker->dq + old.ts.split;
        __lace_worker->allstolen = old_as;
    }
}

VOID_TASK_IMPL_2(lace_steal_loop_root, Task*, t, int*, done)
{
    t->f(__lace_worker, __lace_dq_head, t);
    *done = 1;
}

VOID_TASK_2(lace_together_helper, Task*, t, volatile int*, finished)
{
    t->f(__lace_worker, __lace_dq_head, t);

    for (;;) {
        int f = *finished;
        if (cas(finished, f, f-1)) break;
    }

    while (*finished != 0) STEAL_RANDOM();
}

static void
lace_sync_and_exec(WorkerP *__lace_worker, Task *__lace_dq_head, Task *root)
{
    // wait until other workers have made a local copy
    barrier_wait(&bar);

    // one worker sets t to 0 again
    if (LACE_WORKER_ID == 0) lace_newframe.t = 0;
    // else while (*(volatile Task**)&lace_newframe.t != 0) {}

    // the above line is commented out since lace_exec_in_new_frame includes
    // a barrier_wait before the task is executed

    lace_exec_in_new_frame(__lace_worker, __lace_dq_head, root);
}

void
lace_yield(WorkerP *__lace_worker, Task *__lace_dq_head)
{
    // make a local copy of the task
    Task _t;
    memcpy(&_t, lace_newframe.t, sizeof(Task));

    // wait until all workers have made a local copy
    barrier_wait(&bar);

    // one worker sets t to 0 again
    if (LACE_WORKER_ID == 0) lace_newframe.t = 0;
    // else while (*(volatile Task**)&lace_newframe.t != 0) {}

    // the above line is commented out since lace_exec_in_new_frame includes
    // a barrier_wait before the task is executed

    lace_exec_in_new_frame(__lace_worker, __lace_dq_head, &_t);
}

void
lace_do_together(WorkerP *__lace_worker, Task *__lace_dq_head, Task *t)
{
    /* synchronization integer */
    int done = n_workers;

    /* wrap task in lace_together_helper */
    Task _t2;
    TD_lace_together_helper *t2 = (TD_lace_together_helper *)&_t2;
    t2->f = lace_together_helper_WRAP;
    t2->thief = THIEF_TASK;
    t2->d.args.arg_1 = t;
    t2->d.args.arg_2 = &done;

    while (!cas(&lace_newframe.t, 0, &_t2)) lace_yield(__lace_worker, __lace_dq_head);
    lace_sync_and_exec(__lace_worker, __lace_dq_head, &_t2);
}

void
lace_do_newframe(WorkerP *__lace_worker, Task *__lace_dq_head, Task *t)
{
    /* synchronization integer */
    int done = 0;

    /* wrap task in lace_steal_loop_root */
    Task _t2;
    TD_lace_steal_loop_root *t2 = (TD_lace_steal_loop_root *)&_t2;
    t2->f = lace_steal_loop_root_WRAP;
    t2->thief = THIEF_TASK;
    t2->d.args.arg_1 = t;
    t2->d.args.arg_2 = &done;

    /* and create the lace_steal_loop task for other workers */
    Task _s;
    TD_lace_steal_loop *s = (TD_lace_steal_loop *)&_s;
    s->f = &lace_steal_loop_WRAP;
    s->thief = THIEF_TASK;
    s->d.args.arg_1 = &done;

    compiler_barrier();

    while (!cas(&lace_newframe.t, 0, &_s)) lace_yield(__lace_worker, __lace_dq_head);
    lace_sync_and_exec(__lace_worker, __lace_dq_head, &_t2);
}
